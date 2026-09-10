#!/bin/bash
# RELEASE GATE for tomokv-cpp (split/fused correctness and performance).
#
#   tests/gate.sh         iteration (default): ALL full correctness + the smoke ABBA subset.
#   tests/gate.sh iteration   same default tier; the subset saves only regression cells.
#   tests/gate.sh push    full correctness + ALL regression cells, required for push/release.
#   tests/gate.sh release same as push. `full` is the legacy alias for the same complete gate.
#   tests/gate.sh quick   legacy correctness-only diagnostic: build (release+ASAN), footprint locks, boot
#                         matrix, smoke, torture, RYOW, atomic torn/mixed-write/window gates,
#                         shutdown invariants, counter-fired feature matrix, idle-loop ceiling. Runs on
#                         any machine with at least 16 physical cores.
#   Complete gates add torture-under-ASAN + the Redis 7.4 differential matrix and mandatory ABBA
#                         (the last pushed binary against
#                         the candidate, same session, same box, threshold derived from the
#                         reference's own spread; a missing reference SKIPS LOUDLY and stays red)
#                         + NIC regression cells vs tests/gate_refs.txt (the NIC cells need the 25GbE
#                         netns rig and its scratchpad binaries/procsafe helper).
#
#   Resource options (CPU lists accept ranges and commas):
#     --server-cores LIST  --server-smt LIST  --load-cores LIST  --load-smt LIST
#     --ports FIRST-LAST  --reference-binary PATH  --candidate-binary PATH
#   Omitted physical ranges use the available topology (at most 128 physical cores). Server SMT
#   is enabled only when explicitly supplied. Correctness uses eight physical cores per slot and at
#   least two separate physical load cores: modest protocol traffic is sufficient for these rows.
#   ABBA runs after all correctness children are reaped, keeps at most 32 physical server cores,
#   and gives all remaining selected physical cores plus their available SMT siblings to load.
#   Explicit --load-smt (including an empty string) overrides that automatic ABBA sibling set.
#   A load CPU must never share a physical core with a server CPU. --subset smoke is forbidden
#   for push/release/full; iteration can opt into full, and perf can select either diagnostically.
#   --candidate-binary bypasses only the release build; instrumented source builds still run.
#   Such external binaries remain valid perf/iteration diagnostics but cannot earn a source receipt.
#   Push/release/full also require a source/binary-bound local receipt. GATE_RECEIPT_BASELINE
#   selects a trusted full ledger; GATE_RECEIPT_NULL selects a recent full byte-identical ABBA
#   control. Defaults come only from a previous certified receipt. On first use, every check still
#   runs; missing baseline/null evidence withholds the receipt and makes the final gate nonzero.
#   The owner reviews that completed full ledger before explicitly using it as the next baseline.
#
#   Every feature battery runs on three boots: split (both atomic modes), fused, and fused with the
#   read-local lane ARMED (--read-local 1, the recommended read-heavy production posture; that leg
#   is the only one on which a clean GET/MGET is served by the lane instead of an owner).
#
#   GATE_QUIET_FILE=<path> (opt-in, for a shared box): nothing that starts CPU work -- a battery, a
#   boot, a build, a load generator -- is launched unless that file exists and is older than
#   GATE_QUIET_MINUTES (3). A running battery always finishes; a build is SIGSTOPped; the idle server
#   is SIGSTOPped while waiting; waited time is taken out of the ledger's per-row seconds and logged
#   to $LEDGER.pauses. GATE_LEDGER overrides the ledger path.
#
# The vacuous-validation rule is load-bearing here: every section proves its mechanism FIRED
# (counters, accepts, direct>0), not merely that nothing crashed. A gate that can pass while
# testing nothing is worse than no gate.
set -u
cd "$(dirname "$0")/.."
GATE_SELF_TEST=0
for gate_arg in "$@"; do
  case "$gate_arg" in
    -h|--help|--json) exec python3 tests/gateplan.py "$@";;
    --self-test) GATE_SELF_TEST=1;;
  esac
done
if [ "$GATE_SELF_TEST" = 1 ]; then
  if [ "${1:-iteration}" = perf ]; then
    shift
    exec python3 tests/abbagate.py "$@"
  fi
  exec python3 tests/gateplan.py "$@"
fi
GATE_STARTED=$SECONDS
PLAN=$(python3 tests/gateplan.py "$@") || exit $?
eval "$PLAN"
ALL_BUILD_CORES=$BUILD_CORES
mkdir -p "$PWD/build" || exit 2
RUN_DIR=$(mktemp -d "$PWD/build/gate-run.XXXXXX") || exit 2
phase(){ printf '%s\t%s\n' "$1" "$(date +%s.%N)" >> "$RUN_DIR/phases.tsv"; }
phase begin
printf '%s\n' "$PLAN" > "$RUN_DIR/plan.sh"
export TMPDIR="$RUN_DIR/main"
mkdir -p "$TMPDIR" "$RUN_DIR/jobs" "$PWD/build/gate-cache"
CORES=${SLOT_CORES[0]}; LOAD_CORES=${SLOT_LOAD_CORES[0]}; PORT=${SLOT_PORTS[0]}
GATE_RATIO=${GATE_RATIO:-6:2}; NCORES=8
export GATE_CANDIDATE_BINARY="$CANDIDATE_BINARY"
# Pin the coordinator so every otherwise unpinned Python/CLI child inherits the load allocation.
# Server boot helpers explicitly replace this affinity with their eight physical server cores.
set_slot(){
  local slot=$1
  CORES=${SLOT_CORES[$slot]}; LOAD_CORES=${SLOT_LOAD_CORES[$slot]}; PORT=${SLOT_PORTS[$slot]}
  export GATE_CORES="$CORES" GATE_LOAD_CORES="$LOAD_CORES" GATE_PORT="$PORT"
  export GATE_FEATURE_PORT="$PORT" GATE_SPARE_PORT="$((PORT+2))"
  export GATE_DIFFER_ORACLE_PORT="$((PORT+1))" GATE_GLOBCASE_ORACLE_PORT="$((PORT+2))"
  export GATE_DIFFER_ORACLE_CORES="$CORES"
  taskset -pc "$LOAD_CORES" "$BASHPID" >/dev/null
}
set_slot 0
printf 'GATE(%s): %s\n  artifacts: %s\n' "$GATE_PURPOSE" "$PLAN_HEADER" "$RUN_DIR"
if [ "$TIER" = perf ]; then
  # An omitted candidate means the current tree in every tier. Otherwise a perf-only invocation
  # could measure stale release objects after a source edit and still label them the candidate.
  if [ "$BUILD_CANDIDATE" = 1 ]; then
    # ABBA checks this opt-in sharing signal itself, but compilation now precedes that check.
    # Keep its same absent/recent-file rejection before any compiler starts.
    if [ -n "${GATE_QUIET_FILE:-}" ]; then
      python3 - "$GATE_QUIET_FILE" "${GATE_QUIET_MINUTES:-3}" <<'PY' || exit 3
from pathlib import Path
import sys, time
quiet = Path(sys.argv[1])
age = time.time() - quiet.stat().st_mtime if quiet.exists() else -1
if age < 60 * float(sys.argv[2]):
    print(f'GATE(perf): quiet file {quiet} is absent or too recent; candidate build not started', file=sys.stderr)
    sys.exit(3)
PY
    fi
    if ! taskset -c "$BUILD_CORES" make -j"$BUILD_JOBS" >"$RUN_DIR/release-build.log" 2>&1; then
      cat "$RUN_DIR/release-build.log" >&2
      echo "GATE(perf): candidate build failed" >&2
      exit 1
    fi
  fi
  exec python3 tests/abbagate.py "${ABBA_ARGS[@]}"
fi
PASS=0; FAIL=0
SRV=0; SRVLOG=/dev/null
GLOBCASE_ORACLE=0; MMPID=0; ABBA_PID=0
# The pinned vanilla Redis 7.4 tree (source + built src/redis-server). The ACL-category generator
# reads its SOURCE in both tiers; the differential and globcase rows boot its BINARY in the full
# tier. The preflight below says exactly what is missing instead of a traceback in row 4.
export REDIS74_ROOT=${REDIS74_ROOT:-/tmp/claude-1000/redis74}
# A battery that records a STRICT skip (a DEBUG hook it needed was denied) fails under the gate:
# a gate row must run the arm it exists for, never turn green by skipping it (tests/_lib.py).
export TOMO_GATE_STRICT=1
# Ledger columns are verdict, the row's own elapsed seconds, and stable identity,
# assembled in source order. Compare identities/verdicts after projecting out duration.
# The sidecar retains observed labels/counters; families.tsv measures whole worker jobs.
LEDGER=${GATE_LEDGER:-$PWD/build/gate-ledger-$GATE_PURPOSE.txt}
TIMINGS="$LEDGER.timings"
ROW_T=$(date +%s.%N)
ROW_HISTORY=${GATE_HISTORY:-$PWD/.gate-history/rows}
ROW_RUN_ID="$GATE_PURPOSE:${RUN_DIR##*/}"
export GATE_RUN_ID="$ROW_RUN_ID"
RECEIPT_REQUIRED=0; RECEIPT_START=
case "$GATE_PURPOSE" in
  push|release|full)
    RECEIPT_REQUIRED=1
    # A missing baseline must not abort before correctness or silently skip performance. The
    # helper records a withheld bootstrap state; only a reviewed prior full ledger can arm it.
    RECEIPT_START=$(python3 tests/gate_receipt.py begin --run-id "$ROW_RUN_ID" --tier "$GATE_PURPOSE" \
        --expected-ledger "${GATE_RECEIPT_BASELINE:-$PWD/.gate-history/receipts/baselines/full.tsv}" \
        --allow-missing-baseline --nic-auto 2>"$RUN_DIR/receipt-begin.log") || RECEIPT_START=
    cat "$RUN_DIR/receipt-begin.log" >&2
    ;;
esac
# Freeze the reviewed baseline before rotating outputs: an explicit GATE_RECEIPT_BASELINE may
# name this very ledger from the previous run. Reading it after truncation would discard the
# owner's bootstrap evidence. The manifest keeps exact labels and its digest before any writes.
[ -f "$LEDGER" ] && mv -f "$LEDGER" "$LEDGER.prev"
[ ! -f "$TIMINGS" ] || mv -f "$TIMINGS" "$TIMINGS.prev"
: > "$LEDGER"; : > "$TIMINGS"
ROW_PLAN="$RUN_DIR/row-timeouts.json"
HISTORY_ARGS=()
[ ! -s "$TIMINGS.prev" ] || HISTORY_ARGS+=(--import-ledger "$TIMINGS.prev")
python3 tests/gate_history.py prepare --history "$ROW_HISTORY" "${HISTORY_ARGS[@]}" \
    --output "$ROW_PLAN" || exit 2
# Expected check counts. These are the whole point of the ledger row: a battery that silently
# stops running drops the count and turns the gate red instead of quietly shrinking coverage.
# Bump them DELIBERATELY when rows are added, and say which rows in the commit message.
# 152 -> 172: the s6 oracle battery, concur, execiso and xacct, each under both atomic modes.
# 172 -> 178: snapcut added two cross-shard snapshot-cut rows per persist-io engine, and execfix
# added its battery under both atomic modes.
# 178 -> 184: edgeproto, edgeenc and edgetime batteries joined the feature loop (both modes each).
# 184 -> 189: cross-owner scripts add their battery under both atomic modes plus three control
# boots (off / byte-limit / window), each needing its own knob value and so its own server.
# 189 -> 192: the three efficiency guards, each on its own boot geometry.
# 192 -> 194: the AOF frame-order battery, on persist-io normal under both atomic modes.
# 194 -> 207: arity, blockmulti, cmdgap, multires and xmove were shipped but not invoked; aclsel,
# cmdmeta and expwide joined the feature loop; cmdmeta coverage is a new static row. Each feature
# battery runs under both atomic modes, so one battery is two rows.
# 207 -> 211: contarity exercises XGROUP/XINFO under the feature boot's 16-shard, >=2-executor
# geometry and infofix checks measured INFO telemetry; each runs in both atomic modes.
# 211 -> 213: cross-owner SORT has its required 16-shard, 6:2 geometry under both atomic modes.
# 213 -> 215: pushtear proves out-of-band frames cannot splice borrowed replies and requires the
# segmented/deferred and zero-copy counters to fire, under both atomic modes.
# 215 -> 224: nine rows landed without a history line (two sibling lanes each claimed 207 -> 209
# and the merged history double-counted). The totals were recounted statically at 239/250 in
# AUDIT-TESTS.md; the per-row ledger file ($LEDGER) is what keeps this list honest from here on.
# 224 -> 236: fused mode adds one boot-line assertion plus five directed batteries (s6,
# multi_exec, edgeproto, atomfix, spinprobe) under each of the two atomic modes: 2 * (1 + 5) = 12.
# 236 -> 238: atomic MGET/MSET throughput floor + tripwire arm/disarm round-trip (a 9x armed-by-
# default tax on the atomic chain read once passed this gate with nothing measuring the path).
# 238 -> 239: pipelined same-connection program order (seed-19 divergence; 74% stale pre-fix).
# 239 -> 240: the flip-controller model unit test (tests/flipctl_unit.cc) as a static build row;
# it compiled clean but nothing built or ran it (AUDIT-TESTS F15). Full: 250 -> 251.
# 240 -> 241: atomic_hazards locks the S1 FLUSH read-cut and S2 XREAD touched-key fixes on the
# existing atomic-on, DEBUG-enabled 2s boot. Full: 251 -> 252.
# 241 -> 243: B+ adds the counting-fingerprint representation unit plus the deterministic
# held-group GET/MGET filter battery. Full: 252 -> 254.
# 243 -> 245 quick: each fused boot now proves its schema-1 final report is present, clean, and
# labels itself as fused instead of relying only on the live INFO assertion. Full: 254 -> 256.
# 256 -> 258 full: the CLIENT REPLY OFF/SKIP cross-shard MGET zero-copy rows (both atomic modes).
# 245 -> 246 quick, 258 -> 259 full: the armed local-read lane-admission battery on the B+ boot
# (deferral, never demotion, when connections x window exceed the 1024-entry lane; P128.md).
# 245 -> 313 quick, 258 -> 326 full: the FUSED + read-local ARMED leg. Fused alone never arms the
# local read lane, so no row had ever run a feature battery on the path that serves every clean
# GET/MGET in the recommended read-heavy posture (30 of 32 passed there on first contact; expwide
# S1 and climon2 NO-TOUCH did not). Per atomic mode: one boot-line row (INFO thread_mode:1s AND
# read_local:1), the same 32 feature batteries as the split loop, and one shutdown row (clean
# report, 1s/fused, AND the lane's hit counters moved): 2 * (1 + 32 + 1) = 68 rows, both tiers.
# 313 -> 321 quick, 326 -> 334 full: eviction accounting rows. tests/evict_battery.py had no gate
# row on any boot; its lfu section failed on the armed lane because lane-served reads never touched
# the LFU/LRU metadata (hot keys were evicted FIRST). lfu and the new lruclock section, each on its
# own fresh boot, on the split and the fused+armed boots, both atomic modes: 2 * 2 * 2 = 8 rows.
# Merged: 245 base + 1 lane-admission row + 76 fused+armed and eviction rows = 322 quick;
# 258 base + 1 + 76 = 335 full. The two additions are disjoint: the lane-admission battery is its
# own row on the B+ boot, not a member of the 32-battery feature list the fused+armed loop runs.
# 245 -> 247 quick (258 -> 260 full): multirace joins the armed debug-surface loop under both
# atomic modes. An MSETNX aborted by a key on another owner had already installed its candidate,
# and a MULTI/EXEC the same connection sent behind it read that withdrawn value through the
# store's connection-scoped RYOW overlay and committed a clone of it.
# 260 -> 261 full: a SECOND differential matrix in the armed-fused geometry (--thread-mode fused
# --read-local 1). The canonical row boots split with the read-local lane disarmed, so it cannot
# reach that lane at all; the armed run carries its own non-vacuity check on read_local_hits.
# ARITHMETIC FOR THE MERGE. This lane (t-multirace) contributes +2 quick and +3 full: two
# multirace battery rows (one per atomic mode, quick and full alike) and one full-only armed-fused
# differ row. It branched from 245/258. A sibling lane (t-replycode) contributes +2 FULL-only from
# the same base, i.e. 245/260. Whichever lands second resolves this hunk by ADDING both lanes'
# deltas to the shared base rather than by taking one side: 245+2 / 258+2+3 = 247/263.
# Merged: 322 quick + 2 (multirace: the multi_exec order-late row per atomic mode) = 324;
# 335 full + 3 = 338. Additions are disjoint from the fused+armed and lane-admission rows.
# 258 -> 260 full: reply codes make a blocking timeout's "*-1"/"_" a CODE rather than bytes, so the
# ACL retire-recheck's discard has a second representation to drop; one battery per thread mode.
# Merged: the aclreply battery adds one row per thread mode and both run BEFORE the quick-tier exit,
# so quick is 324 + 2 = 326 and full is 338 + 2 = 340. (Corrected 2026-09-07: the first resolution read
# the branch deltas 245/260 over 245/258 as full-only and left quick at 324, so every quick gate failed
# its own ledger row by exactly 2 while the full tier was right.)
# 340 -> 343 full: the armed-write block cache's ownership laws (DESIGN-P0REPLY.md). A shard that
# changed owner kept its read-local retire sink pointing at the OLD owner's QSBR ring and block
# cache until the destination's own later pass rebound it, so the new owner wrote through another
# thread's unlocked free list. Three rows: the -DTOMO_RL_CACHE_DEBUG build, the churn battery on it
# (which carries its own non-vacuity checks, including that the balancer MOVED shards), and the
# invariant check over that server's log.
# COUNTED BY LINE, NOT BY INFERENCE (which is what produced the 324/326 correction above): the
# quick tier exits at the `[ "$TIER" = quick ]` block, and all three rows are emitted after it, so
# they are full-only. Quick is unchanged at 326; full is 340 + 3 = 343.
# 324 -> 325 quick (340 -> 341 full): the RYOW write-ring unit joins the static rows, carrying the
# arm-on-demand transient cases (t-ringdiet, DESIGN-RINGDIET.md). One row in BOTH tiers, because it
# is server-less and costs under two seconds; the arithmetic is 324+1 / 340+1 and it is disjoint
# from every boot-geometry loop above.
# Merged 2026-09-07: the ring unit is one row in BOTH tiers (server-less, under two seconds), so
# quick is 326 + 1 = 327 and full is 343 + 1 = 344. Counted by line: the ring row is emitted with
# the static rows, far above the quick-tier exit, and the P0 rows stay below it.
# Measured on the 2026-09-10 full run: 418 rows before the quick-tier exit, 467 total. This run
# then removed the 32-row stored-reference loopback tier and added two ABBA rows -- the serverless
# negative control BEFORE the quick exit (quick +1, full +1) and the mandatory headline result
# AFTER it (full +1). 418+1 = 419 quick; 467-32+2 = 437 full.
EXPECT_QUICK=419
EXPECT_FULL=437                 # full without the optional NIC row.
say(){ printf '  %-52s %s\n' "$1" "$2"; }
canonical_label(){ sed -E \
      -e 's/(direct|hits|records|skipped|suppressed|zc_sends)=[0-9]+/\1=N/g' \
      -e 's/(dispatched==executed) \([0-9]+\)/\1 (N)/' \
      -e 's/(atomic MGET\/MSET floor) \([0-9]+\/s/\1 (N\/s/' \
      -e 's/(TLS connection slots all freed) \([0-9]+\/[0-9]+\)/\1 (N\/N)/'; }
# Explicit scopes begin before the row's work, including compound shell assertions. A
# watchdog bounds the WHOLE row rather than only Python: compiler, redis-cli, pipelines,
# shell loops and waits all count. On expiry the family stops red; later rows are visibly
# unreached through the existing completion/count checks. No timeout becomes a skip.
ROW_ID=; ROW_WATCHDOG=0; ROW_EXPIRED=0; ROW_START=0; ROW_PAUSED=0
ROW_WATCH_SEQUENCE=0; ROW_WATCHDOG_START=0; ROW_WATCH_PARENT=0; ROW_WATCH_PARENT_START=0
row_clock(){
  # Kernel uptime is monotonic and has 10 ms resolution. A wall-clock/NTP step
  # must neither manufacture an expiry nor extend a hung row's measured history.
  # Bash reads it directly, avoiding another Python startup for every endpoint.
  read -r ROW_NOW row_idle < /proc/uptime || exit 2
}
row_watch(){
  local parent_stat parent_start remaining parent_pid=$BASHPID
  read -r parent_stat < "/proc/$BASHPID/stat"
  parent_stat=${parent_stat##*) }; read -ra parent_fields <<< "$parent_stat"
  parent_start=${parent_fields[19]}
  ROW_WATCH_PARENT=$parent_pid; ROW_WATCH_PARENT_START=$parent_start
  ROW_WATCH_SEQUENCE=$((${ROW_WATCH_SEQUENCE:-0}+1))
  ROW_WATCH_GENERATION="$parent_pid.$parent_start.$ROW_WATCH_SEQUENCE"
  ROW_WATCH_CANCEL="${ROW_MARKER%.json}.$ROW_WATCH_GENERATION.cancel"
  ROW_WATCH_RECEIPT="${ROW_MARKER%.json}.$ROW_WATCH_GENERATION.cancelled"
  row_clock
  remaining=$(awk -v budget="$ROW_TIMEOUT" -v start="$ROW_START" -v now="$ROW_NOW" -v paused="$ROW_PAUSED" \
      'BEGIN {v=budget-(now-start-paused); print (v>0?v:0.001)}')
  python3 tests/gate_history.py watch --pid "$parent_pid" --parent-start "$parent_start" \
      --seconds "$remaining" --marker "$ROW_MARKER" --generation "$ROW_WATCH_GENERATION" \
      --cancel-request "$ROW_WATCH_CANCEL" --cancel-receipt "$ROW_WATCH_RECEIPT" &
  ROW_WATCHDOG=$!
  ROW_WATCHDOG_START=0
  if { read -r parent_stat < "/proc/$ROW_WATCHDOG/stat"; } 2>/dev/null; then
    parent_stat=${parent_stat##*) }; read -ra parent_fields <<< "$parent_stat"
    [ "${parent_fields[1]}" != "$parent_pid" ] || ROW_WATCHDOG_START=${parent_fields[19]}
  fi
}
row_watch_probe(){
  local state fields
  ROW_WATCH_STATE=gone
  if { read -r state < "/proc/$ROW_WATCHDOG/stat"; } 2>/dev/null; then
    state=${state##*) }; read -ra fields <<< "$state"
    if [ "${fields[1]}" != "${ROW_WATCH_PARENT:-0}" ] ||
        [ "${fields[19]}" != "${ROW_WATCHDOG_START:-0}" ] ||
        [ "$BASHPID" != "${ROW_WATCH_PARENT:-0}" ]; then
      ROW_WATCH_STATE=mismatch
    else ROW_WATCH_STATE=${fields[0]}
    fi
  fi
}
row_watch_clock(){
  local uptime idle whole fraction
  read -r uptime idle < /proc/uptime || return 1
  whole=${uptime%.*}; fraction=${uptime#*.}00
  ROW_WATCH_CLOCK=$((10#$whole*100+10#${fraction:0:2}))
}
row_watch_receipt(){
  local line expected lines=()
  [ -f "${ROW_WATCH_RECEIPT:-}" ] || return 1
  mapfile -t lines < "$ROW_WATCH_RECEIPT"
  [ "${#lines[@]}" = 1 ] || return 1
  IFS= read -r line < "$ROW_WATCH_RECEIPT" || return 1
  printf -v expected 'CANCELLED\t%s\t%s\t%s\t%s\t%s' "$ROW_WATCH_PARENT" "$ROW_WATCH_PARENT_START" \
      "$ROW_WATCHDOG" "$ROW_WATCHDOG_START" "$ROW_WATCH_GENERATION"
  [ "$line" = "$expected" ]
}
row_unwatch(){
  local watcher_rc=0 until expired=0 forced=0 can_wait=1
  if [ "$ROW_WATCHDOG" -gt 0 ]; then
    # Captured 2026-09-10: TERM in Bash's fork/exec transition was lost, leaving three
    # Python watchers sleeping for 900s while their parents waited forever. An EXIT-owner
    # guard prevents false publication but cannot make that first signal reliable.
    # Publish a generation-specific request before TERM; a child that starts afterwards
    # acknowledges it. Pause/rearm generations never share cancellation evidence.
    row_watch_probe
    if [ "$ROW_WATCH_STATE" != gone ] && [ "$ROW_WATCH_STATE" != Z ] &&
        [ "$ROW_WATCH_STATE" != mismatch ] && [ ! -f "$ROW_MARKER" ]; then
      if ! { printf 'CANCEL\t%s\t%s\t%s\t%s\t%s\n' "$ROW_WATCH_PARENT" "$ROW_WATCH_PARENT_START" \
          "$ROW_WATCHDOG" "$ROW_WATCHDOG_START" "$ROW_WATCH_GENERATION" > "$ROW_WATCH_CANCEL.tmp" &&
          mv "$ROW_WATCH_CANCEL.tmp" "$ROW_WATCH_CANCEL"; }; then ROW_MONITOR_FAILED=1; fi
      kill -TERM "$ROW_WATCHDOG" 2>/dev/null || :
    fi
    row_watch_clock || { ROW_MONITOR_FAILED=1; return 1; }
    until=$((ROW_WATCH_CLOCK+500)) # Match watch()'s existing five-second cleanup grace.
    while :; do
      row_watch_probe
      case "$ROW_WATCH_STATE" in
        gone|Z) break;;
        mismatch)
          ROW_MONITOR_FAILED=1; can_wait=0
          say "$ROW_ID" 'FAIL (row watchdog PID/start or direct-parent identity changed)'
          break;;
      esac
      row_watch_clock || { ROW_MONITOR_FAILED=1; can_wait=0; break; }
      if [ -f "$ROW_MARKER" ] && [ "$expired" = 0 ]; then
        # Expiry owns descendant reclamation. Allow its full five seconds plus one
        # second for marker publication/scheduling; never TERM/KILL it immediately.
        expired=1; until=$((ROW_WATCH_CLOCK+600))
      fi
      if [ "$ROW_WATCH_CLOCK" -ge "$until" ]; then
        forced=1; ROW_MONITOR_FAILED=1
        say "$ROW_ID" 'FAIL (row watchdog cancellation/expiry cleanup exceeded its bounded grace)'
        # This exceptional path pays for a pidfd helper so PID reuse across inspection
        # and SIGKILL cannot touch another process. Forced cleanup can never turn green.
        python3 tests/gate_history.py signal-child --pid "$ROW_WATCHDOG" --start "$ROW_WATCHDOG_START" \
            --parent "$ROW_WATCH_PARENT" --parent-start "$ROW_WATCH_PARENT_START" || :
        row_watch_clock || { can_wait=0; break; }
        until=$((ROW_WATCH_CLOCK+100))
        while :; do
          row_watch_probe
          case "$ROW_WATCH_STATE" in gone|Z) break;; mismatch) can_wait=0; break;; esac
          row_watch_clock || { can_wait=0; break; }
          [ "$ROW_WATCH_CLOCK" -lt "$until" ] || { can_wait=0; break; }
          sleep .01
        done
        break
      fi
      sleep .01
    done
    # A published timeout owns its five-second kill escalation. Let it finish so a
    # TERM-ignoring child cannot survive the correctness barrier into performance.
    # wait is reached only after this exact child is gone/zombie, never while it runs.
    if [ "$can_wait" = 1 ]; then
      wait "$ROW_WATCHDOG" 2>/dev/null || watcher_rc=$?
    else
      watcher_rc=255; ROW_MONITOR_FAILED=1
      say "$ROW_ID" 'FAIL (row watchdog remains live/unowned; refusing an unbounded wait)'
    fi
    if [ "$watcher_rc" != 143 ] &&
        { [ "$watcher_rc" != 0 ] || { [ ! -f "$ROW_MARKER" ] && ! row_watch_receipt; }; }; then
      ROW_MONITOR_FAILED=1
      say "$ROW_ID" "FAIL (row watchdog exited unexpectedly: $watcher_rc)"
    fi
    ROW_WATCHDOG=0
    [ "$forced" = 0 ] && [ "$can_wait" = 1 ] || return 1
  fi
}
row_begin(){
  if [ -n "$ROW_ID" ]; then
    bad "$ROW_ID" "row scope was replaced without a verdict"
    exit 2
  fi
  quiet_wait
  ROW_ID=$(printf '%s\n' "$1" | canonical_label)
  ROW_HISTORY_ID="$ROW_ID${2:+ [$2]}"
  local budget
  budget=$(python3 tests/gate_history.py budget --plan "$ROW_PLAN" --label "$ROW_HISTORY_ID") || exit 2
  IFS=$'\t' read -r ROW_TIMEOUT ROW_MEDIAN ROW_BASIS <<< "$budget"
  # The whole ABBA matrix is a single historical gate row. Until it has exact
  # history, its conservative budget must accommodate the full escalation matrix.
  # This bound is not a license to accept partial results; ABBA still scores all cells.
  if [ "$ROW_MEDIAN" = - ] && [ "$ROW_ID" = 'headline ABBA vs last pushed binary' ]; then
    ROW_TIMEOUT=43200; ROW_BASIS=no-history-full-matrix-conservative-default
  fi
  ROW_MARKER="$TMPDIR/row-timeout-$BASHPID.json"
  rm -f "$ROW_MARKER"
  row_clock
  ROW_EXPIRED=0; ROW_MONITOR_FAILED=0; ROW_PAUSED=0; ROW_START=$ROW_NOW
  trap row_timeout USR1
  row_watch
  printf '  row budget: %ss; median=%ss; %s; %s\n' "$ROW_TIMEOUT" "$ROW_MEDIAN" "$ROW_BASIS" "$ROW_ID"
}
row_timeout(){
  trap '' USR1
  ROW_EXPIRED=1
  bad "${ROW_ID:-row watchdog}" "TIMEOUT after ${ROW_TIMEOUT:-?}s; median=${ROW_MEDIAN:--}s; ${ROW_BASIS:-unknown}"
  exit 124
}
row_finish(){
  row_clock
  local now=$ROW_NOW
  row_unwatch
  ROW_SECONDS=$(awk -v start="$ROW_START" -v now="$now" -v paused="$ROW_PAUSED" \
      'BEGIN {v=now-start-paused; printf "%.6f", (v>0?v:0)}')
  if [ -f "$ROW_MARKER" ] || awk -v elapsed="$ROW_SECONDS" -v budget="$ROW_TIMEOUT" 'BEGIN {exit !(elapsed>=budget)}'; then
    ROW_EXPIRED=1
  fi
}
row_save_history(){
  local verdict=$1 scored=${2:-1} timed=() identity=()
  [ "$ROW_EXPIRED" != 1 ] || { verdict=FAIL; timed=(--timed-out); }
  [ "${ROW_MONITOR_FAILED:-0}" = 0 ] || verdict=FAIL
  [ "$scored" != 1 ] || identity=(--ledger-label "$ROW_ID")
  python3 tests/gate_history.py record --history "$ROW_HISTORY" --run-id "$ROW_RUN_ID" \
      --label "$ROW_HISTORY_ID" --seconds "$ROW_SECONDS" --verdict "$verdict" \
      --scored "$scored" "${identity[@]}" "${timed[@]}"
}
ledger(){
  local verdict=$1 label=$2 scored=${3:-1} identity duration
  if [ -n "$ROW_ID" ]; then
    row_finish
    identity=$ROW_ID; duration=$ROW_SECONDS
    [ "${ROW_MONITOR_FAILED:-0}" = 0 ] || verdict=FAIL
    if [ "$ROW_EXPIRED" = 1 ]; then
      verdict=FAIL
      say "$identity" "FAIL (TIMEOUT ${ROW_TIMEOUT}s; median=${ROW_MEDIAN}s; $ROW_BASIS)"
    fi
    row_save_history "$verdict" "$scored" || exit 2
  else
    # Infrastructure failures (boot/worker/count) are red even without a normal row
    # scope. An unscoped SUCCESS is a harness defect, never manufactured zero timing.
    verdict=FAIL; duration=0
    identity=$(printf '%s\n' "$label" | canonical_label)
    [ "$1" != ok ] || say "$identity" 'FAIL (row was not timed: missing row_begin)'
  fi
  printf '%s\t%s\t%s\n' "$verdict" "$duration" "$identity" >> "$LEDGER"
  printf '%s\t%s\t%s\n' "$verdict" "$duration" "$label" >> "$TIMINGS"
  if [ "$verdict" = ok ]; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
  ROW_ID=; ROW_EXPIRED=0
  trap - USR1
}
ok(){ say "$1" "ok"; ledger ok "$1"; }
bad(){ say "$1" "FAIL${2:+ ($2)}"; ledger FAIL "$1"; }
ledger_labels(){
  # Read the previous three-column format too when a caller keeps its old GATE_LEDGER path.
  awk -F '\t' 'NF >= 3 {print $3; next} {print $2}' "$1" | canonical_label
}
program_state(){
  local expect=$1 actual=$((PASS+FAIL))
  if [ "$actual" -eq "$expect" ]; then
    say "PROGRAM-STATE ledger ($actual/$expect checks)" "ok"
  else
    bad "PROGRAM-STATE ledger" "$actual/$expect checks"
    if [ -f "$LEDGER.prev" ]; then
      echo "  rows that differ from the previous $GATE_PURPOSE ledger (< only in previous run, > only now):"
      diff <(ledger_labels "$LEDGER.prev") <(ledger_labels "$LEDGER") | grep '^[<>]' | sed 's/^/    /'
    fi
  fi
  echo "  ledger: $LEDGER (verdict / own-row seconds / stable label); timings: $TIMINGS; slowest rows:"
  sort -t "$(printf '\t')" -k2,2 -rn "$TIMINGS" | head -12 \
      | awk -F '\t' '{printf "    %7.1fs  %-4s %s\n", $2, $1, $3}'
  # Every observation remains in the durable corpus, including failures and
  # timeouts. A previous failure is evidence to investigate, never an anecdote
  # erased by this run's green verdict or a make clean.
  if python3 tests/gate_history.py report --history "$ROW_HISTORY" > "$RUN_DIR/verdict-history.txt"; then
    cat "$RUN_DIR/verdict-history.txt"
  else
    bad 'verdict history report' "could not read $ROW_HISTORY"
  fi
}
redis_cli_expect_ok(){
  local reply
  reply=$(redis-cli -h 127.0.0.1 -p "$PORT" "$@" 2>&1 | tr -d '\r')
  [ "$reply" = OK ] || { printf 'unexpected redis-cli reply to %s: %s\n' "$*" "$reply" >&2; return 1; }
}
# Cooperative box sharing (opt-in, see header). quiet_ok is the whole rule; its two callers are
# quiet_wait (before anything that starts CPU work) and pausable (around a build -- the one kind
# of row long enough to need stopping mid-flight and free of timing semantics).
QUIET_FILE=${GATE_QUIET_FILE:-}
QUIET_MIN=${GATE_QUIET_MINUTES:-3}
PAUSABLE_PID=0
quiet_ok(){ [ -z "$QUIET_FILE" ] || [ -n "$(find "$QUIET_FILE" -mmin +"$QUIET_MIN" 2>/dev/null)" ]; }
quiet_note(){ printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LEDGER.pauses" >&2; }
quiet_wait(){ # block until quiet_ok. The live server is SIGSTOPped meanwhile (an idle fused loop
              # still spins on cores an owner measurement may share); the waited time leaves the
              # ledger's next row, which measures rows, not the box.
  quiet_ok && return 0
  local t0 now stopped=0 row_was_active=0
  if [ -n "$ROW_ID" ]; then row_unwatch; row_was_active=1; fi
  row_clock; t0=$ROW_NOW
  if [ "$SRV" -gt 0 ] 2>/dev/null && kill -0 "$SRV" 2>/dev/null; then
    kill -STOP "$SRV" 2>/dev/null && stopped=1
  fi
  quiet_note "quiet: waiting for $QUIET_FILE to be >$QUIET_MIN min old" \
      "($([ "$stopped" = 1 ] && echo "server $SRV stopped" || echo "no server up"))"
  until quiet_ok; do sleep 5; done
  [ "$stopped" = 1 ] && kill -CONT "$SRV" 2>/dev/null
  row_clock; now=$ROW_NOW
  ROW_T=$(awk -v a="$ROW_T" -v b="$t0" -v c="$now" 'BEGIN{printf "%.9f", a + (c - b)}')
  if [ "$row_was_active" = 1 ]; then
    ROW_PAUSED=$(awk -v p="$ROW_PAUSED" -v b="$t0" -v c="$now" 'BEGIN{print p+c-b}')
    row_watch
  fi
  quiet_note "quiet: resumed after $(awk -v b="$t0" -v c="$now" 'BEGIN{printf "%.0f", c - b}')s"
}
signal_owned_tree(){ # signal only the recorded build PID and its descendants, never an argv match
  python3 - "$@" <<'PY'
import os, signal, sys
signum=getattr(signal, 'SIG'+sys.argv[1]); root=int(sys.argv[2]); processes={}
for entry in os.scandir('/proc'):
    if not entry.name.isdigit(): continue
    try:
        fields=open(entry.path+'/stat').read().rsplit(')',1)[1].split()
        processes[int(entry.name)]=(int(fields[1]), fields[19])
    except (OSError,ValueError): pass
owned={root} if root in processes else set()
while True:
    more={pid for pid,(parent,_) in processes.items() if parent in owned}-owned
    if not more: break
    owned.update(more)
for pid in sorted(owned, reverse=signum != signal.SIGCONT):
    if pid in (os.getpid(),os.getppid()): continue
    try:
        # PID reuse after the snapshot never grants ownership of the replacement process.
        current=open(f'/proc/{pid}/stat').read().rsplit(')',1)[1].split()
        if current[19] == processes[pid][1]: os.kill(pid,signum)
    except (OSError,ValueError): pass
PY
}
pausable(){
  local dependency_scope=0 dependency_rc dependency_identity
  if [ -z "$ROW_ID" ]; then
    dependency_identity=$(python3 tests/gate_history.py dependency-label -- "$@") || return 2
    row_begin "$dependency_identity"
    dependency_scope=1
  fi
  pausable_body "$@"; dependency_rc=$?
  if [ "$dependency_scope" = 1 ]; then
    row_finish
    if [ "$ROW_EXPIRED" = 1 ] || [ "${ROW_MONITOR_FAILED:-0}" != 0 ]; then
      bad "$ROW_ID" 'build dependency deadline/monitor failed'
      exit 124
    fi
    # Hidden prerequisite work is timed evidence too, but adds no passing gate row. Its
    # dependent rows retain their original build/readiness assertions and expected counts.
    if [ "$dependency_rc" = 0 ]; then row_save_history ok 0 || exit 2
    else row_save_history FAIL 0 || exit 2; fi
    ROW_ID=; trap - USR1
  fi
  return "$dependency_rc"
}
pausable_body(){ # run "$@" to completion; pause/resume its owned descendant PIDs for quiet-file waits
  [ -n "$QUIET_FILE" ] || { "$@"; return $?; }
  quiet_wait
  local pid stopped=0 t0=0 paused=0 rc now
  setsid "$@" &
  pid=$!; PAUSABLE_PID=$pid
  while kill -0 "$pid" 2>/dev/null; do
    if quiet_ok; then
      if [ "$stopped" = 1 ]; then
        signal_owned_tree CONT "$pid"; stopped=0
        row_clock; now=$ROW_NOW
        if [ -n "$ROW_ID" ]; then
          ROW_PAUSED=$(awk -v p="$ROW_PAUSED" -v b="$t0" -v c="$now" 'BEGIN{print p+c-b}')
          row_watch
        fi
        paused=$(awk -v p="$paused" -v b="$t0" -v c="$now" \
                     'BEGIN{printf "%.3f", p + (c - b)}')
        quiet_note "quiet: build resumed"
      fi
    elif [ "$stopped" = 0 ]; then
      [ -z "$ROW_ID" ] || row_unwatch
      signal_owned_tree STOP "$pid"; stopped=1; row_clock; t0=$ROW_NOW
      quiet_note "quiet: build paused until $QUIET_FILE is >$QUIET_MIN min old"
    fi
    sleep 2
  done
  wait "$pid"; rc=$?
  PAUSABLE_PID=0
  ROW_T=$(awk -v a="$ROW_T" -v p="$paused" 'BEGIN{printf "%.9f", a + p}')
  return $rc
}
py(){ # All callers belong to an explicit whole-row watchdog scope.
  local rc
  quiet_wait
  python3 "$@"; rc=$?
  [ "$rc" != 3 ] || echo 'GATE: battery SKIPPED ITSELF: a gate row must run, not skip' >&2
  return "$rc"
}

reap_children(){
  local owner_pid=$BASHPID owner_start p rc=0
  owner_start=$(python3 tests/gate_history.py identity --pid "$owner_pid") || return 1
  # The helper is a direct child and validates parent PID/start identity before walking
  # descendants. CONT/TERM/grace/KILL bounds paused builds and TERM-ignoring teardown.
  # Only after every owned process has stopped can wait() safely reap direct children.
  python3 tests/gate_history.py reap-tree --pid "$owner_pid" --parent-start "$owner_start" || rc=$?
  # Exit3 means all processes stopped only after forced KILL: reap them, then fail loudly.
  # Other failures may retain a live D-state child, so an unbounded wait is forbidden.
  [ "$rc" = 0 ] || [ "$rc" = 3 ] || return "$rc"
  for p in "${WORKER_PIDS[@]}" "${PAUSABLE_PID:-0}" "${ABBA_PID:-0}" \
           "${SRV:-0}" "${GLOBCASE_ORACLE:-0}" "${MMPID:-0}"; do
    [ "$p" -le 0 ] || wait "$p" 2>/dev/null || :
  done
  WORKER_PIDS=( ); PAUSABLE_PID=0; ABBA_PID=0; SRV=0; GLOBCASE_ORACLE=0; MMPID=0
  return "$rc"
}
publish_abba(){
  [ "${ABBA_PENDING:-0}" = 1 ] || return 0
  LEDGER=$ABBA_LEDGER; TIMINGS=$ABBA_TIMINGS
  { head -n "$ABBA_PREFIX_ROWS" "$LEDGER"; cat "$RUN_DIR/abba.ledger";
    tail -n +"$((ABBA_PREFIX_ROWS+1))" "$LEDGER"; } > "$RUN_DIR/assembled.ledger" || return 1
  mv "$RUN_DIR/assembled.ledger" "$LEDGER" || return 1
  cat "$RUN_DIR/abba.timings" >> "$TIMINGS" || return 1
  ABBA_PENDING=0
}
cleanup(){ # EXIT/INT/TERM: publication and teardown remain bounded outside normal row scopes.
  [ "$BASHPID" = "$CLEANUP_OWNER" ] || return 0
  row_unwatch
  publish_abba || return 1
  reap_children
}
CLEANUP_OWNER=$BASHPID
trap 'cleanup || exit 1' EXIT
trap 'exit 130' INT TERM

port_listeners(){ # pids bound to a port, listening or not yet accepting (ss names the owner)
  ss -H -ltnp "sport = :$1" 2>/dev/null | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u \
      | paste -sd, -
}
guard_port(){ # a bound listener OR an accepting peer on the port ends the gate, naming the pid
  local owners; owners=$(port_listeners "$1")
  if [ -n "$owners" ] || (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null; then
    say "port $1 pre-boot guard" \
        "FAIL (already listening${owners:+; pid=$owners} -- a leftover server; kill it and re-run)"
    exit 1
  fi
}
settle(){ # after a server is gone: wait for $PORT to stop accepting, then a short settle.
  # Replaces a fixed `sleep 5` (x55 per run, ~4.6 min). Not load-bearing: the listener sets
  # SO_REUSEADDR+SO_REUSEPORT (src/core/io_loop.h:167) and `wait` already reaped the process.
  # GATE_STOP_SETTLE=5 restores the old timing if a row ever turns out to depend on it -- which
  # would itself be a finding worth a NOTES line.
  for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null || break; sleep 0.1; done
  sleep "${GATE_STOP_SETTLE:-1}"
}
launch(){ # logtag binary args... -> pid in $SRV, log in $SRVLOG; waits up to 30 s for the port
  local tag=$1 bin=$2; shift 2
  quiet_wait
  SRV=0; SRVLOG=/dev/null
  guard_port "$PORT"
  [ -x "$bin" ] || { say "boot ($tag)" "FAIL ($bin is not an executable)"; return 1; }
  SRVLOG=$(mktemp "$TMPDIR/gate-srv-$tag.XXXXXX")
  # Every ordinary boot gets an empty persistence directory. Explicit recovery arms override it
  # through their later --dir argument; a previous battery's SAVE must not become this one's input.
  local boot_dir
  boot_dir=$(mktemp -d "$TMPDIR/gate-data-$tag.XXXXXX") || return 1
  taskset -c $CORES "$bin" --port $PORT --bind 127.0.0.1 --shards 16 --dir "$boot_dir" "$@" > "$SRVLOG" 2>&1 &
  SRV=$!
  # 30s, not 10s: the AOF replay boot replays its file BEFORE it listens, and on a box shared
  # with other lanes that overran a 10s deadline and turned six AOF rows red with no defect behind
  # them. A generous deadline costs nothing when the server is quick — the loop exits on connect.
  for _ in $(seq 150); do
    if ! kill -0 "$SRV" 2>/dev/null; then
      wait "$SRV" 2>/dev/null; boot_log_tail "$tag" "exited before it listened"; return 1
    fi
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2; done
  boot_log_tail "$tag" "never accepted on $PORT within 30s"
  return 1
}
boot_log_tail(){ # a boot that failed is a defect report, not a filename: SHOW the server's words.
                 # Tonight three boot failures were triaged twice over because the row said only
                 # "see $SRVLOG" and the log was rotated by the next row's mktemp before anyone
                 # opened it.
  printf 'GATE: boot (%s) %s -- last 25 lines of %s:\n' "$1" "$2" "$SRVLOG" >&2
  tail -n 25 "$SRVLOG" 2>/dev/null | sed 's/^/GATE|   /' >&2
}
boot(){ local bin=$1; shift; launch main "$bin" --ratio $GATE_RATIO "$@"; }
boot_fused(){ # deliberately omits --ratio, which fused mode rejects
  local bin=$1; shift; launch fused "$bin" --thread-mode fused "$@"; }
stop(){
  if [ "$SRV" -gt 0 ]; then
    kill -TERM "$SRV" 2>/dev/null
    if ! timeout --kill-after=1 30 tail --sleep-interval=.1 --pid="$SRV" -f /dev/null; then
      kill -KILL "$SRV" 2>/dev/null
      bad 'server shutdown' "TIMEOUT after 30s waiting for owned PID $SRV"
    fi
    wait "$SRV" 2>/dev/null; SRV=0
  fi
  settle
}
shutdown_clean(){ python3 tests/shutdown_report.py "$SRVLOG" clean; }
shutdown_present(){ python3 tests/shutdown_report.py "$SRVLOG" present; }
shutdown_value(){ python3 tests/shutdown_report.py "$SRVLOG" get "$1"; }


# Each worker owns a server/load/port slot for its entire lifetime. A locked queue chooses the
# next job; completion never writes the shared ledger. collect_job emits fragments in the source
# order below, including on failure. One slot uses the same worker/collector path as twelve.
WORKER_PIDS=()
JOB_NAMES=()
# ---- feature batteries: every shipped feature's directed test, BOTH atomic settings -----------
# The gate accumulates a section per landed feature (owner rule). Each test is directed and
# asserts its own mechanisms fired; the boot covers multi/blocking/pubsub+sharded/lua/limits.
# ONE list, shared with the fused+armed leg below, so the two legs cannot drift apart: a battery
# added here runs on the armed lane too, and the ledger arithmetic counts it twice per atomic mode.
FEATURE_BATTERIES="s6 multi_exec blocking blockmulti stream streamgroups pubsub lua_scripting scriptsurf limits resp3 bitfield dumprestore zsetops geo climon climon2 tracking hexpire servertail lcs concur edgeproto edgeenc edgetime arity contarity cmdgap aclsel expwide infofix pushtear netcmd"
feature_split_job(){
  local AT=$1 t FEATURE_ARGS
  boot "$CANDIDATE_BINARY" --atomic $AT --enable-debug-command yes \
      || bad "feature battery boot (atomic $AT)"
  for t in $FEATURE_BATTERIES; do
    FEATURE_ARGS=()
    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
    row_begin "$t battery (atomic $AT)"
    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >$TMPDIR/gate-$t-$AT.txt 2>&1 \
        && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see $TMPDIR/gate-$t-$AT.txt"
  done
  stop
  row_begin "feature shutdown invariants (atomic $AT)"
  shutdown_clean \
      && ok "feature shutdown invariants (atomic $AT)" || bad "feature shutdown invariants (atomic $AT)"
}

# ---- FUSED + read-local ARMED: the recommended read-heavy posture runs the whole feature list ----
# The fused boots above never arm the local read lane, so until this leg no gate row had run a
# feature battery on the path that serves every clean GET/MGET in that posture. The boot row asserts
# the WIRE says so: INFO server thread_mode:1s AND read_local:1 -- the effective lane state, not the
# knob, which CONFIG GET also echoes on a split boot where it is inert. The shutdown row is the
# leg's vacuity guard: the lane's own hit counters must have moved during the batteries, or the 32
# rows between ran on the owner path and proved nothing about the armed one.
feature_armed_job(){
  local AT=$1 t FEATURE_ARGS ARMED_INFO ARMED_MODE ARMED_RL ARMED_HITS ARMED_REPORT_MODE ARMED_REPORT_KIND
  row_begin "fused+armed boot line (atomic $AT)"
  if boot_fused "$CANDIDATE_BINARY" --atomic "$AT" --read-local 1 --enable-debug-command yes; then
    ARMED_INFO=$(redis-cli -h 127.0.0.1 -p "$PORT" INFO server 2>/dev/null | tr -d '\r')
    ARMED_MODE=$(printf '%s\n' "$ARMED_INFO" | sed -n 's/^thread_mode://p')
    ARMED_RL=$(printf '%s\n' "$ARMED_INFO" | sed -n 's/^read_local://p')
    [ "$ARMED_MODE" = 1s ] && [ "$ARMED_RL" = 1 ] \
        && ok "fused+armed boot line (atomic $AT)" \
        || bad "fused+armed boot line (atomic $AT)" "wire mode=$ARMED_MODE read_local=$ARMED_RL"
  else
    bad "fused+armed boot line (atomic $AT)" "server did not boot; see $SRVLOG"
  fi
  for t in $FEATURE_BATTERIES; do
    FEATURE_ARGS=()
    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
    row_begin "fused+armed $t battery (atomic $AT)"
    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >$TMPDIR/gate-fusedarmed-$t-$AT.txt 2>&1 \
        && ok "fused+armed $t battery (atomic $AT)" \
        || bad "fused+armed $t battery (atomic $AT)" "see $TMPDIR/gate-fusedarmed-$t-$AT.txt"
  done
  row_begin "fused+armed shutdown report + lane fired (hits=N, atomic $AT)"
  ARMED_HITS=$(redis-cli -h 127.0.0.1 -p "$PORT" INFO stats 2>/dev/null | tr -d '\r' \
      | awk -F: '/^read_local_keyspace_hits:|^read_local_mget_local_hits:/{s+=$2} END{print s+0}')
  stop
  ARMED_REPORT_MODE=$(shutdown_value thread_mode)
  ARMED_REPORT_KIND=$(shutdown_value work.kind)
  shutdown_clean && [ "$ARMED_REPORT_MODE" = 1s ] && [ "$ARMED_REPORT_KIND" = fused ] \
      && [ -n "$ARMED_HITS" ] && [ "$ARMED_HITS" -gt 0 ] \
      && ok "fused+armed shutdown report + lane fired (hits=$ARMED_HITS, atomic $AT)" \
      || bad "fused+armed shutdown report + lane fired (atomic $AT)" \
             "mode=$ARMED_REPORT_MODE kind=$ARMED_REPORT_KIND hits=$ARMED_HITS; see $SRVLOG"
}

job_label(){
  case "$1" in
    feature-split-*) echo "s6 battery (atomic ${1##*-})";;
    feature-armed-*) echo "fused+armed boot line (atomic ${1##*-})";;
    differ-split) echo 'Redis 7.4 differential matrix';;
    differ-armed) echo 'Redis 7.4 differential matrix (armed fused + read-local)';;
    differ-split-[01]) echo "Redis 7.4 differential part (split, atomic ${1##*-})";;
    differ-armed-[01]) echo "Redis 7.4 differential part (armed fused, atomic ${1##*-})";;
    differ-equivalence) echo 'mode equivalence part (all execution modes and knobs)';;
    flipctl) echo 'flip controller: ramp gate, hold, surge + mix re-maneuvers';;
    evict-*) local kind section mode atomic
      IFS=- read -r kind section mode atomic <<< "$1"
      [ "$mode" != armed ] || mode=fused+armed
      echo "eviction $section battery ($mode, atomic $atomic)";;
    feature-cell-*) echo "feature ${1#feature-cell-}";;
    *) echo "correctness family $1";;
  esac
}
job_body(){
  local name=$1 label; label=$(job_label "$name")
  case "$name" in
    feature-cell-*) job_feature_cell "$name";;
    snapshot-*) job_snapshot "$name";;
    aof-*) job_aof "$name";;
    debug-*) job_debug "$name";;
    fused-*) job_fused "$name";;
    feature-split-*) feature_split_job "${name##*-}";;
    feature-armed-*) feature_armed_job "${name##*-}";;
    differ-*)
      export GATE_DIFFER_GEOMETRY=split
      export GATE_DIFFER_OUT="$TMPDIR/differ"
      export GATE_DIFFER_PART="${name#differ-}" GATE_DIFFER_PLAN="$RUN_DIR/differ-plan.json"
      [[ "$name" != differ-armed-* ]] || export GATE_DIFFER_GEOMETRY=armed-fused
      row_begin "$label"
      # Private rows retain their watchdog and verdict evidence. Only the two complete folds
      # below are scored publicly; all atomic lifetimes and every equivalence stream are required.
      if tests/differ_gate.sh "$CANDIDATE_BINARY" "$PORT" "$((PORT+1))" "$CORES" "$GATE_RATIO"; then
        say "$label" ok; ledger ok "$label" 0
      else
        say "$label" FAIL; ledger FAIL "$label" 0
      fi;;
    evict-*)
      local kind section mode atomic
      IFS=- read -r kind section mode atomic <<< "$name"
      if [ "$mode" = split ]; then
        boot "$CANDIDATE_BINARY" --atomic "$atomic" || bad "eviction $section boot (split, atomic $atomic)"
      else
        boot_fused "$CANDIDATE_BINARY" --atomic "$atomic" --read-local 1 \
            || bad "eviction $section boot (fused+armed, atomic $atomic)"
      fi
      row_begin "$label"
      py tests/evict_battery.py "$PORT" "$section" >"$TMPDIR/battery.log" 2>&1 \
          && ok "$label" || bad "$label" "see $TMPDIR/battery.log"
      stop;;
    flipctl)
      boot "$CANDIDATE_BINARY" --ratio 6:2 --atomic 0 --enable-debug-command yes --flip-auto 1 \
          || bad 'flipctl boot'
      # Retain the original bounded stationary-hold re-rolls and every directed assertion.
      row_begin "$label"
      python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT" --stable-seconds 30 \
          >"$TMPDIR/battery.log" 2>&1 && ok "$label" || bad "$label" "see $TMPDIR/battery.log"
      stop;;
    *) "job_$name";;
  esac
}
job_recover(){
  local name=$1 slot=$2 rc=$3 dir="$RUN_DIR/jobs/$1" label now
  [ ! -f "$dir/done" ] || return 0
  mkdir -p "$dir"
  touch "$dir/ledger" "$dir/timings" # Preserve earlier verdicts if the finalizer was interrupted.
  label=$(job_label "$name")
  if ! grep -q $'^FAIL\t' "$dir/ledger"; then
    # Unreached infrastructure has no measured row scope; zero is explicit, and must
    # never enter exact timing history. Both ledger formats retain their three columns.
    printf 'FAIL\t0\t%s\n' "$label" >> "$dir/ledger"
    printf 'FAIL\t0\t%s\n' "$label" >> "$dir/timings"
  fi
  printf 'GATE: family %s did not publish completion (exit %s); partial rows retained\n' \
      "$name" "$rc" >> "$dir/output.log"
  [ "$rc" != 0 ] || rc=1
  now=$(date +%s.%N)
  [ -f "$dir/family.tsv" ] || printf '%s\t%s\t%s\t%s\n' "$name" "$slot" "$now" "$now" > "$dir/family.tsv"
  printf '%s\t%s\t%s\n' "$rc" \
      "$(awk -F '\t' '$1=="ok"{n++} END{print n+0}' "$dir/ledger")" \
      "$(awk -F '\t' '$1=="FAIL"{n++} END{print n+0}' "$dir/ledger")" > "$dir/done.tmp"
  mv "$dir/done.tmp" "$dir/done"
}
job_finalize(){
  # A fast row can cancel its watchdog after fork but before Python exec. Bash may then
  # run the inherited EXIT trap in that child (captured 2026-09-10), while its parent correctly
  # waits status143. Only the worker may publish its ledger/done or reclaim its children.
  # Keep row_unwatch's unexpected-status checks: this fixes ownership, not the verdict rule.
  [ "$BASHPID" = "$CLEANUP_OWNER" ] || return 0
  local rc=$1 ended
  trap - EXIT INT TERM
  if [ -n "$ROW_ID" ]; then
    bad "$ROW_ID" "family exited $rc before this row produced a verdict"
    [ "$rc" != 0 ] || rc=1
  fi
  if ! cleanup; then
    bad "$(job_label "$name")" "owned process teardown did not complete"
    rc=1
  fi
  if [ "$rc" != 0 ] && ! grep -q $'^FAIL\t' "$LEDGER"; then
    bad "$(job_label "$name")" "worker exited $rc after its rows"
  fi
  # EXIT is the publication point even for row_timeout's exit124. Preserve every partial
  # row (including its own duration) and publish the actual counts after bounded cleanup.
  PASS=$(awk -F '\t' '$1=="ok"{n++} END{print n+0}' "$LEDGER")
  FAIL=$(awk -F '\t' '$1=="FAIL"{n++} END{print n+0}' "$LEDGER")
  ended=$(date +%s.%N)
  printf '%s\t%s\t%s\t%s\n' "$name" "$slot" "$started" "$ended" > "$TMPDIR/family.tsv"
  printf '%s\t%s\t%s\n' "$rc" "$PASS" "$FAIL" > "$TMPDIR/done.tmp"
  mv "$TMPDIR/done.tmp" "$TMPDIR/done"
  exit "$rc"
}
run_job(){ (
  local name=$1 slot=$2 started=$EPOCHREALTIME ended rc CLEANUP_OWNER=$BASHPID
  WORKER_PIDS=(); SRV=0; GLOBCASE_ORACLE=0; MMPID=0; PAUSABLE_PID=0
  PASS=0; FAIL=0; ROW_ID=; ROW_WATCHDOG=0; ROW_EXPIRED=0
  export TMPDIR="$RUN_DIR/jobs/$name"
  mkdir -p "$TMPDIR"
  LEDGER="$TMPDIR/ledger"; TIMINGS="$TMPDIR/timings"
  : > "$LEDGER"; : > "$TIMINGS"
  trap 'job_finalize "$?" >>"$TMPDIR/output.log" 2>&1' EXIT
  trap 'exit 130' INT TERM
  set_slot "$slot"
  BUILD_CORES="$CORES,$LOAD_CORES"
  BUILD_JOBS=$(taskset -c "$BUILD_CORES" nproc)
  export PARBUILD_JOBS="$BUILD_JOBS"
  quiet_wait
  started=$(date +%s.%N); ROW_T=$started
  job_body "$name" >"$TMPDIR/output.log" 2>&1
  rc=$?
  exit "$rc"
); }
start_workers(){
  phase parallel-begin
  # Serialization is limited to binary publication, shared release-object writes, one job per
  # CPU/three-port slot, and the operations inside a shared boot/recovery chain. All files written
  # by independent batteries are private TMPDIR/mktemp paths; the coordinator alone owns ledgers.
  # TLS uses PORT+1, nested servertail uses PORT+2, and oracle boots use these same reserved ports.
  # NIC's global namespace/interface changes and every regression measurement follow the final
  # join barrier. atomic_batteries also waits for every other family: its RYOW row contains
  # a performance assertion, so it is an explicit exception to parallel correctness work.
  # Correctness traffic is modest and stays on each slot's two or more physical load cores.
  # Compilers use that slot's server+load cores. The release build alone unlocks release jobs;
  # ASAN and standalone units do not delay boots, and full-only builds start immediately too.
  JOB_NAMES=(release asan core_tsan_build waits_tsan_build)
  [ "$TIER" != full ] || JOB_NAMES+=(rldbg)
  JOB_NAMES+=(config_unit flip_unit filter_unit ring_unit reorder_unit storage_units
              production_units acl_metadata cmd_metadata abba_selftest)
  # Start long waits and whole boot families early; short jobs occupy the slots they release.
  if [ "$TIER" = full ]; then
    # Each target already rebooted between atomic modes. Those four independent lifetimes can
    # occupy separate CPU/port/TMPDIR slots; seeds and the accumulated-state MULTI repeats cannot.
    # Plan failure remains loud and makes every required differential fold red, while unrelated
    # correctness jobs still run and retain their evidence.
    python3 tests/differ_fanout.py plan --run "${ROW_RUN_ID:-$RUN_DIR}" --output "$RUN_DIR/differ-plan.json" \
        --repeats "${GATE_DIFFER_MULTI_REPEATS:-4}" || true
    JOB_NAMES+=(differ-split-0 differ-split-1 differ-armed-0 differ-armed-1 differ-equivalence)
  fi
  local atomic mode section slot FM FR FO FQ FF
  for section in lruclock lfu; do
    for atomic in 0 1; do for mode in split armed; do JOB_NAMES+=("evict-$section-$mode-$atomic"); done; done
  done
  JOB_NAMES+=(flipctl)
  for atomic in 0 1; do JOB_NAMES+=("feature-split-$atomic" "feature-armed-$atomic"); done
  JOB_NAMES+=(aof-epoll aof-uring snapshot-epoll snapshot-uring debug-0 debug-1
              core_units atomic_units netcmd_units boot_grammar wait_units readonly
              release_batteries atomic_batteries bplus acl_recheck sort script_bounds
              efficiency dump_restore auth notify flip flip_saturated atomic_floor
              aof_frame tls fused-0 fused-1)
  [ "$TIER" != full ] || JOB_NAMES+=(asan_batteries replyoff zc rlcache globcase)
  for FM in 1s 2s; do
    for FR in 0 1; do for FO in 0 1; do for FQ in 0 1; do for FF in 0 1; do
      JOB_NAMES+=("feature-cell-$FM-$FR-$FO-$FQ-$FF")
    done; done; done; done
  done
  JOB_NAMES+=(feature-cell-split-home-min feature-cell-fused-home-max-nopin feature-cell-split-shards-auto)
  mkdir -p "$RUN_DIR/claimed"
  for ((slot=0; slot<GATE_SLOTS; slot++)); do
    (
      WORKER_PIDS=(); SRV=0; GLOBCASE_ORACLE=0; MMPID=0; PAUSABLE_PID=0
      trap 'exit 130' INT TERM
      while :; do
        exec {queue_fd}>"$RUN_DIR/queue.lock"
        flock "$queue_fd"
        selected=; pending=0
        for name in "${JOB_NAMES[@]}"; do
          [ ! -d "$RUN_DIR/claimed/$name" ] || continue
          pending=1
          if job_ready "$name"; then
            selected=$name
            mkdir "$RUN_DIR/claimed/$name"
            break
          fi
        done
        flock -u "$queue_fd"; exec {queue_fd}>&-
        if [ -n "$selected" ]; then
          run_job "$selected" "$slot"
          job_recover "$selected" "$slot" "$?"
        elif [ "$pending" = 0 ]; then break
        else sleep 0.2
        fi
      done
    ) &
    WORKER_PIDS+=("$!")
  done
}
collect_differ_group(){
  local group=$1 name dir p live row verdict duration label
  local parts=("differ-$group-0" "differ-$group-1")
  [ "$group" != split ] || parts+=(differ-equivalence)
  label=$(job_label "differ-$group")
  for name in "${parts[@]}"; do
    dir="$RUN_DIR/jobs/$name"
    while [ ! -f "$dir/done" ]; do
      live=0
      for p in "${WORKER_PIDS[@]}"; do kill -0 "$p" 2>/dev/null && live=1; done
      [ "$live" = 1 ] || break
      sleep 0.2
    done
    [ ! -f "$dir/output.log" ] || cat "$dir/output.log"
  done
  # Fold only after every named child's normal finalizer has published its real exit and cleanup
  # result. The helper revalidates ordered leg inventories and coverage, including failed-seed
  # replays; a missing/unreached child is one failed original row, never a smaller passing matrix.
  if ! row=$(python3 tests/differ_fanout.py fold --plan "$RUN_DIR/differ-plan.json" --group "$group" \
      --run-directory "$RUN_DIR" --row-plan "$ROW_PLAN" --row-history "$ROW_HISTORY" --row-run "$ROW_RUN_ID"); then
    bad "$label" "incomplete differential children; see $RUN_DIR/jobs/differ-*"
    return
  fi
  IFS=$'\t' read -r verdict duration label <<< "$row"
  printf '%s\n' "$row" >> "$LEDGER"
  printf '%s\n' "$row" >> "$TIMINGS"
  say "$label" "$verdict (${duration}s across concurrent children)"
  if [ "$verdict" = ok ]; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi
}
collect_job(){
  case "$1" in differ-split|differ-armed) collect_differ_group "${1#differ-}"; return;; esac
  local name=$1 dir="$RUN_DIR/jobs/$1" live p job_rc job_pass job_fail
  while [ ! -f "$dir/done" ]; do
    live=0
    for p in "${WORKER_PIDS[@]}"; do kill -0 "$p" 2>/dev/null && live=1; done
    [ "$live" = 1 ] || break
    sleep 0.2
  done
  if [ ! -s "$dir/ledger" ] || [ ! -f "$dir/done" ]; then
    bad "$(job_label "$name")" "worker did not complete; see $dir"
    return
  fi
  read -r job_rc job_pass job_fail < "$dir/done"
  if ! [[ "$job_rc" =~ ^[0-9]+$ && "$job_pass" =~ ^[0-9]+$ && "$job_fail" =~ ^[0-9]+$ ]]; then
    bad "$(job_label "$name")" "malformed worker completion; see $dir"
    return
  fi
  # A child can print success and then fail (including during teardown). Its process result is
  # evidence too: never let a previously emitted green fragment swallow that failure.
  if [ "$job_rc" -ne 0 ] && ! grep -q $'^FAIL\t' "$dir/ledger"; then
    cat "$dir/output.log"
    bad "$(job_label "$name")" "worker exited $job_rc after its rows; see $dir"
    return
  fi
  cat "$dir/output.log"
  cat "$dir/ledger" >> "$LEDGER"; cat "$dir/timings" >> "$TIMINGS"
  PASS=$((PASS + $(awk -F '\t' '$1=="ok"{n++} END{print n+0}' "$dir/ledger")))
  FAIL=$((FAIL + $(awk -F '\t' '$1=="FAIL"{n++} END{print n+0}' "$dir/ledger")))
  ROW_T=$(date +%s.%N)
}
join_workers(){
  local p
  for p in "${WORKER_PIDS[@]}"; do wait "$p" || bad 'correctness worker completion'; done
  WORKER_PIDS=()
  BUILD_CORES=$ALL_BUILD_CORES
  cat "$RUN_DIR"/jobs/*/family.tsv > "$RUN_DIR/families.tsv"
}
stop_workers(){ reap_children; }


# The bodies below preserve every same-server sequence and persistence recovery chain. Only
# dispatch moved: collection at the original tier/source positions remains the ledger contract.
ASAN="$PWD/build/gate-cache/tomokv-asan"
RLDBG="$PWD/build/gate-cache/tomokv-rlcachedbg"
CORE_TSAN="$PWD/build/gate-cache/core-concurrency-tsan"
WAITS_TSAN="$PWD/build/gate-cache/waits-unit-tsan"
FEATURE_OUTPUT=${GATE_FEATURE_OUTPUT:-$(mktemp -d "$PWD/build/gate-feature.XXXXXX")}

reject_boot(){
  local conf=() directory
  # A broken rejection may accidentally start a server. Its affinity, bind, data directory and
  # lifetime must still obey this slot. Config-file paths remain first, as the parser requires.
  if [[ "$1" != --* ]]; then conf=("$1"); shift; fi
  directory=$(mktemp -d "$TMPDIR/reject-boot.XXXXXX") || return 1
  timeout --kill-after=5 10 taskset -c "$CORES" "$CANDIDATE_BINARY" "${conf[@]}" \
      --port "$PORT" --bind 127.0.0.1 --shards 16 --ratio "$GATE_RATIO" --dir "$directory" "$@"
}

store_build(){
  local variant=${1:-} flags=${CXXFLAGS-'-std=c++20 -O2 -g -Wall -Wextra -march=native -pthread'}
  # Match the three Makefile recipes, including the later -O1 override for TSan. Their former
  # single compiler invocation serialized three large translation units on every cache miss.
  # Reuse the header-aware cache and keep compilation on this slot while other batteries run.
  if [ "$variant" = tsan ]; then
    flags+=' -O1 -fsanitize=thread -fno-omit-frame-pointer -no-pie'
  fi
  flags+=' -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST'
  [ "$variant" != sidecar ] || flags+=' -DTOMO_TTL_DEADLINE_SIDECAR=1'
  flags+=' -I.'
  pausable taskset -c "$CORES,$LOAD_CORES" tests/parbuild.sh \
      "$PWD/build/store-regression${variant:+-$variant}" \
      "$PWD/build/gate-cache/store${variant:+-$variant}-objects" \
      "$flags" '-Wl,--gc-sections' \
      tests/store_regression.cc src/cmd/t_hash.cc src/cmd/t_hash_ttl.cc
}

job_release(){
if [ "$BUILD_CANDIDATE" = 1 ]; then
  row_begin "release build (+footprint locks)"
  pausable taskset -c "$BUILD_CORES" make -j"$BUILD_JOBS" >$TMPDIR/gate-build.txt 2>&1 \
    && ok "release build (+footprint locks)" || bad "release build" "see $TMPDIR/gate-build.txt"
else
  row_begin "release build (+footprint locks)" external-candidate
  [ -x "$CANDIDATE_BINARY" ] && ok "release build (+footprint locks)" \
      || bad "release build" "candidate is not executable: $CANDIDATE_BINARY"
fi
if [ "$FAIL" = 0 ] && [ -n "$RECEIPT_START" ]; then
  # Publish the binary binding before this worker's done marker unlocks release batteries.
  # A binding problem withholds certification; it does not delete or bypass any gate row.
  # An external executable has no demonstrated relationship to this source tree. Its full
  # diagnostic run remains useful, but only our normal source build can obtain a push receipt.
  if [ "$BUILD_CANDIDATE" = 1 ]; then
    python3 tests/gate_receipt.py bind --start "$RECEIPT_START" --candidate "$CANDIDATE_BINARY" \
        >"$RUN_DIR/receipt-bind.log" 2>&1 || cat "$RUN_DIR/receipt-bind.log" >&2
  else
    echo 'GATE RECEIPT WITHHELD: --candidate-binary skips the source build; all diagnostic rows still run, but external bytes cannot certify this source tree' \
        >"$RUN_DIR/receipt-bind.log"
    cat "$RUN_DIR/receipt-bind.log" >&2
  fi
fi
}

job_asan(){
# 42 translation units: compiled in parallel with a header-aware object cache rather than in one
# serial g++, which cost 365s and was the largest row in the gate.
row_begin "ASAN build"
pausable taskset -c "$BUILD_CORES" tests/parbuild.sh $ASAN "$PWD/build/gate-cache/obj-asan" \
    "-std=c++20 -O1 -g -fsanitize=address -march=native -pthread -I." \
    "-luring -pthread -lssl -lcrypto" \
    src/main.cc src/net/tls.cc src/core/*.cc src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc 2>$TMPDIR/gate-asan-build.txt \
    && ok "ASAN build" || bad "ASAN build" "see $TMPDIR/gate-asan-build.txt"
}

job_config_unit(){
quiet_wait
row_begin "Redis config quoting + mid-value #"
g++ -std=c++20 -O2 -I. tests/config_parser_test.cc -o $TMPDIR/tomokv-config-parser-test \
    && taskset -c "$CORES" $TMPDIR/tomokv-config-parser-test \
    && ok "Redis config quoting + mid-value #" || bad "Redis config quoting + mid-value #"
}

job_flip_unit(){
# The flip-controller MODEL (fingerprint classes, ramp gate, count-noise rejection) has a unit
# test that nothing built; a model regression surfaced only in flipctl.py's live 2-5 min row.
row_begin "flip controller model unit"
g++ -std=c++20 -O2 -I. tests/flipctl_unit.cc src/core/flipctl.cc -o $TMPDIR/tomokv-flipctl-unit \
    2>$TMPDIR/gate-flipctl-unit.txt \
    && $TMPDIR/tomokv-flipctl-unit >>$TMPDIR/gate-flipctl-unit.txt 2>&1 \
    && ok "flip controller model unit" \
    || bad "flip controller model unit" "see $TMPDIR/gate-flipctl-unit.txt"
}

job_filter_unit(){
row_begin "B+ counting-fingerprint filter unit"
g++ -std=c++20 -O2 -pthread -I. tests/foreign_read_safety_test.cc \
    -o $TMPDIR/tomokv-foreign-read-safety-test \
    && $TMPDIR/tomokv-foreign-read-safety-test \
    && ok "B+ counting-fingerprint filter unit" \
    || bad "B+ counting-fingerprint filter unit"
}

job_ring_unit(){
# THE RYOW WRITE RING, INCLUDING ITS ARMING TRANSIENT (DESIGN-RINGDIET.md). The connection records
# nothing until a local read arms it, so the reads that arrive with pre-arming writes still in
# flight must be DEMOTED -- and the fence that demotes them must be one-shot, or a 1:1 connection
# would never be served locally again. That is a per-frame ordering property of the ROB, so it is
# tested server-less and deterministically here rather than inferred from a live counter: the five
# directed arming cases plus a 200k-frame soak that starts UNARMED and crosses the transition, on
# top of the fourteen ring cases the sizing lane left (which this row also brings into the gate for
# the first time -- they were `make unit` only). 20 cases, one row.
row_begin "read-local write ring + arming transient unit"
g++ -std=c++20 -O2 -march=native -pthread -I. tests/read_local_write_ring_unit.cc \
    -o $TMPDIR/tomokv-read-local-write-ring-unit 2>$TMPDIR/gate-ring-unit.txt \
    && $TMPDIR/tomokv-read-local-write-ring-unit >>$TMPDIR/gate-ring-unit.txt 2>&1 \
    && ok "read-local write ring + arming transient unit" \
    || bad "read-local write ring + arming transient unit" "see $TMPDIR/gate-ring-unit.txt"
}

job_core_units(){
# SURVIVING core concurrency regressions. Eight rows, all ABOVE the quick-tier exit.
# Each selection asserts its hazardous state; ASAN/UBSAN and bounded interleaving hooks
# make a broken mechanism fail. The fixture starts no server and opens no listener.
CORE_UNIT_READY=0
unit_ready core-concurrency-unit && CORE_UNIT_READY=1
for core_row in watch scheduler lifetime drain route snapshot config notify; do
  row_begin "core concurrency $core_row"
  quiet_wait
  if [ "$CORE_UNIT_READY" = 1 ] && \
      ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
      taskset -c "$CORES" ./build/core-concurrency-unit "$core_row" \
          >"$TMPDIR/gate-core-$core_row.txt" 2>&1 && \
      tsan_unit "$CORE_TSAN" core-concurrency-tsan \
          "PASS core concurrency $core_row (state assertions fired)" "$core_row"; then
    ok "core concurrency $core_row"
  else
    bad "core concurrency $core_row" "see $TMPDIR/gate-core-$core_row.txt, $TMPDIR/tsan-core-concurrency-tsan-$core_row.log, and $RUN_DIR/jobs/production_units/build.log and $RUN_DIR/jobs/core_tsan_build/build.log"
  fi
done
}

job_reorder_unit(){
# REORDER.md: one row in BOTH tiers (before the quick exit). Real published ROB tasks drive the
# production scheduler at 32/128 capacity. Exact non-identity permutations prove it fired; ASAN
# and UBSAN make undersized scratch and an invalid occupancy shift fail, never skip or time out green.
row_begin "reorder mechanism + 32/128-task geometry battery"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -pthread -I. tests/reorder_unit.cc \
    -o $TMPDIR/tomokv-reorder-unit 2>$TMPDIR/gate-reorder-unit.txt \
    && $TMPDIR/tomokv-reorder-unit >>$TMPDIR/gate-reorder-unit.txt 2>&1 \
    && ok "reorder mechanism + 32/128-task geometry battery" \
    || bad "reorder mechanism + 32/128-task geometry battery" "see $TMPDIR/gate-reorder-unit.txt"
}

job_storage_units(){
# Twelve storage regressions, all BEFORE the quick-tier exit. The hash reaper is production code;
# store-boundary spies make held epochs, capture cursors, eviction and allocation failures exact.
# No case skips. Build failure makes every dependent row red. EXPECT constants are maintainer-owned.
STORE_REGRESSION_BUILT=0; STORE_TSAN_BUILT=0; STORE_SIDECAR_BUILT=0
store_build >$TMPDIR/gate-store-build.txt 2>&1 & store_pid=$!
store_build tsan >$TMPDIR/gate-store-flags-build.txt 2>&1 & store_tsan_pid=$!
store_build sidecar >$TMPDIR/gate-store-sidecar.txt 2>&1 & store_sidecar_pid=$!
wait "$store_pid" && STORE_REGRESSION_BUILT=1
wait "$store_tsan_pid" && STORE_TSAN_BUILT=1
wait "$store_sidecar_pid" && STORE_SIDECAR_BUILT=1
for STORE_CASE in unlinked randomkey rehash rollback snapshot-eviction flags aof-eviction intents imported-hash field-index-failure hash-bytes; do
  row_begin "storage $STORE_CASE regression"
  STORE_RUN=(./build/store-regression "$STORE_CASE")
  STORE_CASE_BUILT=$STORE_REGRESSION_BUILT
  if [ "$STORE_CASE" = flags ]; then
    # Only this process disables address randomization: GCC TSan otherwise collides with the
    # host's mappings before main. A runtime race report or unavailable TSan is a red row.
    STORE_CASE_BUILT=0
    [ "$STORE_TSAN_BUILT" = 1 ] && STORE_CASE_BUILT=1
    STORE_RUN=(setarch x86_64 -R ./build/store-regression-tsan flags)
  fi
  quiet_wait
  if [ "$STORE_CASE_BUILT" = 1 ] && \
      "${STORE_RUN[@]}" \
          >$TMPDIR/gate-store-$STORE_CASE.txt 2>&1; then
    ok "storage $STORE_CASE regression"
  else
    bad "storage $STORE_CASE regression" "see $TMPDIR/gate-store-build.txt, $TMPDIR/gate-store-$STORE_CASE-build.txt and $TMPDIR/gate-store-$STORE_CASE.txt"
  fi
done
row_begin "storage deadline-sidecar regression"
[ "$STORE_SIDECAR_BUILT" = 1 ] \
    && ./build/store-regression-sidecar deadline-sidecar \
        >>$TMPDIR/gate-store-sidecar.txt 2>&1 \
    && ok "storage deadline-sidecar regression" \
    || bad "storage deadline-sidecar regression" "see $TMPDIR/gate-store-sidecar.txt"
}

job_atomic_units(){
# SURVIVING.md's atomic lane: one build and fifteen named, deterministic defect rows.
# Counted by line: all sixteen are ABOVE the quick-tier exit, so both tiers gain sixteen.
# EXPECT_QUICK/EXPECT_FULL are deliberately left to the maintainer (see FIXES-ATOMICS.md).
row_begin "atomic survivors unit build"
unit_ready atomic-survivors-unit \
    && ok "atomic survivors unit build" \
    || bad "atomic survivors unit build" "see $RUN_DIR/jobs/production_units/build.log"
for defect in admission closure script_keys rename_overlay write_latest script_apply \
              lua_conversion watch_parent watch_cycle mset_arity watch_oom lua_lines \
              library_limit stage_flag instruction_limit; do
  quiet_wait
  row_begin "atomic survivor: $defect"
  taskset -c "$CORES" ./build/atomic-survivors-unit "$defect" \
      >"$TMPDIR/gate-atomic-survivors-$defect.txt" 2>&1 \
      && ok "atomic survivor: $defect" \
      || bad "atomic survivor: $defect" "see $TMPDIR/gate-atomic-survivors-$defect.txt"
done
}

job_netcmd_units(){
# Surviving networking/command audit: deterministic serverless failure states, both tiers.
# notify-retry and flush prepare a four-thread topology. Workers inherit their small load
# slot, so every unit case runs on the assigned eight-CPU server slot instead.
row_begin "netcmd regression build"
unit_ready netcmd-unit \
    && ok "netcmd regression build" || bad "netcmd regression build" "see $RUN_DIR/jobs/production_units/build.log"
for NETCMD_CASE in streams zpop notify-oom notify-retry flush output pubsub receive config; do
  row_begin "netcmd $NETCMD_CASE regression"
  taskset -c "$CORES" ./build/netcmd-unit "$NETCMD_CASE" >$TMPDIR/gate-netcmd-$NETCMD_CASE.txt 2>&1 \
      && ok "netcmd $NETCMD_CASE regression" \
      || bad "netcmd $NETCMD_CASE regression" "see $TMPDIR/gate-netcmd-$NETCMD_CASE.txt"
done
}

job_acl_metadata(){
if [ "$ORACLE_OK" = 1 ]; then
  row_begin "generated Redis 7.4 ACL categories"
  python3 tools/gen_acl_categories.py --redis-root "$REDIS74_ROOT" \
      --check src/cmd/acl_categories_generated.h \
      && ok "generated Redis 7.4 ACL categories" || bad "generated Redis 7.4 ACL categories"
else
  bad "generated Redis 7.4 ACL categories" "oracle source tree missing at $REDIS74_ROOT"
fi
}

job_boot_grammar(){
row_begin "reject --mode (flag deleted)"
reject_boot --mode 3s      2>&1 | grep -q "unknown" && ok "reject --mode (flag deleted)" || bad "reject --mode (flag deleted)"
row_begin "reject --spread"
reject_boot --spread 4:4   2>&1 | grep -q "unknown"  && ok "reject --spread"    || bad "reject --spread"
row_begin "reject --nodes"
reject_boot --nodes 2      2>&1 | grep -q "unknown"  && ok "reject --nodes"     || bad "reject --nodes"
row_begin "reject 3-part ratio"
reject_boot --ratio 4:4:2  2>&1 | grep -q "deleted"  && ok "reject 3-part ratio"|| bad "reject 3-part ratio"
row_begin "reject missing conf"
reject_boot /nonexistent-conf 2>&1 | grep -q "cannot open" && ok "reject missing conf" || bad "reject missing conf"
printf 'florb 1\n' > $TMPDIR/gate-bad.conf
row_begin "reject bad conf key"
reject_boot $TMPDIR/gate-bad.conf 2>&1 | grep -q "unknown argument" && ok "reject bad conf key" || bad "reject bad conf key"
printf 'aclfile %s\nuser alice on nopass ~* &* +@all\n' "$TMPDIR/gate-users.acl" > $TMPDIR/gate-acl-mixed.conf
row_begin "reject aclfile + conf user lines"
reject_boot $TMPDIR/gate-acl-mixed.conf 2>&1 | grep -q \
    "Configuring Redis with users defined in redis.conf and at the same setting an ACL file path is invalid" \
    && ok "reject aclfile + conf user lines" || bad "reject aclfile + conf user lines"
}

job_cmd_metadata(){
# A registered command with no generated metadata row makes command_metadata_init fail, and the
# server then refuses to boot at all -- every row below goes red at once with no indication which
# command is at fault. Static, so it fires before any server starts.
row_begin "cmdmeta covers every registered command"
py tests/cmdmeta_coverage.py >$TMPDIR/gate-cmdmeta-coverage.txt 2>&1 \
    && ok "cmdmeta covers every registered command" \
    || bad "cmdmeta covers every registered command" "see $TMPDIR/gate-cmdmeta-coverage.txt"
}

job_wait_units(){
# MERGEWAITS.md: six rows, all BEFORE the quick-tier exit. Build/boot failure belongs to its
# dependent row, so it cannot change the count. The retirement row intentionally includes lazy
# expiry: cx-waits alone still fails that broader no-quiescence-wait law. Never skip/xfail it.
row_begin "waits config publication + admission unit"
unit_ready waits-unit \
    && taskset -c "$CORES" ./build/waits-unit >>$TMPDIR/gate-waits-unit.txt 2>&1 \
    && tsan_unit "$WAITS_TSAN" waits-unit-tsan "waits unit: PASS" \
    && ok "waits config publication + admission unit" \
    || bad "waits config publication + admission unit" "see $TMPDIR/gate-waits-unit.txt, $TMPDIR/tsan-waits-unit-tsan.log, and $RUN_DIR/jobs/waits_tsan_build/build.log"
row_begin "reads never wait for retirement quiescence"
unit_ready rehash-waits-unit \
    && taskset -c "$CORES" ./build/rehash-waits-unit retirement \
        >>$TMPDIR/gate-rehash-waits-unit.txt 2>&1 \
    && ok "reads never wait for retirement quiescence" \
    || bad "reads never wait for retirement quiescence" "see $TMPDIR/gate-rehash-waits-unit.txt"
}

job_readonly(){
for WAIT_MODE in split fused; do
  for WAIT_LOCAL in 0 1; do
    row_begin "read-only resize $WAIT_MODE read-local=$WAIT_LOCAL"
    WAIT_BOOTED=0
    if [ "$WAIT_MODE" = split ]; then
      boot "$CANDIDATE_BINARY" --atomic 1 --read-local "$WAIT_LOCAL" --overlap 0 \
          --flip-auto 0 --enable-debug-command yes && WAIT_BOOTED=1
    else
      boot_fused "$CANDIDATE_BINARY" --atomic 1 --read-local "$WAIT_LOCAL" --overlap 0 \
          --flip-auto 0 --enable-debug-command yes && WAIT_BOOTED=1
    fi
    WAIT_OK=0
    WAIT_LOG="$TMPDIR/gate-rehash-readonly-$WAIT_MODE-$WAIT_LOCAL.txt"
    if [ "$WAIT_BOOTED" = 1 ] && py tests/rehash_readonly.py 127.0.0.1 "$PORT" "$WAIT_MODE" "$WAIT_LOCAL" >"$WAIT_LOG" 2>&1; then
      WAIT_OK=1
    fi
    if [ "$SRV" -gt 0 ]; then stop; fi
    if [ "$WAIT_OK" = 1 ] && shutdown_clean >>"$WAIT_LOG" 2>&1; then
      ok "read-only resize $WAIT_MODE read-local=$WAIT_LOCAL"
    else
      bad "read-only resize $WAIT_MODE read-local=$WAIT_LOCAL" "see $WAIT_LOG and $SRVLOG"
    fi
  done
done
}

job_release_batteries(){
boot "$CANDIDATE_BINARY" --enable-debug-command yes || bad "release boot"
row_begin "torture battery"
py tests/torture.py 127.0.0.1 $PORT >$TMPDIR/gate-tort.txt 2>&1 \
    && ok "torture battery" || bad "torture battery" "see $TMPDIR/gate-tort.txt"
row_begin "RYOW battery"
py tests/ryow.py 127.0.0.1 $PORT >$TMPDIR/gate-ryow.txt 2>&1 \
    && ok "RYOW battery" || bad "RYOW battery" "see $TMPDIR/gate-ryow.txt"
row_begin "ACL category runtime table"
py tests/acl_categories.py 127.0.0.1 $PORT >$TMPDIR/gate-acl-categories.txt 2>&1 \
    && ok "ACL category runtime table" || bad "ACL category runtime table" "see $TMPDIR/gate-acl-categories.txt"
row_begin "ACL LOAD/SAVE no-file errors"
py tests/acl.py 127.0.0.1 $PORT - >$TMPDIR/gate-acl-nofile.txt 2>&1 \
    && ok "ACL LOAD/SAVE no-file errors" || bad "ACL LOAD/SAVE no-file errors" "see $TMPDIR/gate-acl-nofile.txt"
# Idle-loop ceiling: owner-written counters remove scheduler/jiffy noise and name a spinning
# thread. The helper requires two fresh LBSIGNALS captures and a sampling iteration, so a missing
# DEBUG surface or cached/empty dump cannot turn this row green.
row_begin "idle loop ceiling (LBSIGNALS, 1s)"
py tests/spinprobe.py "$PORT" "$SRV" --idle-only >$TMPDIR/gate-idle-signals.txt 2>&1 \
    && ok "idle loop ceiling (LBSIGNALS, 1s)" \
    || bad "idle loop ceiling" "see $TMPDIR/gate-idle-signals.txt"
stop
# shutdown invariants + fired counters, from the TERM dump
row_begin "shutdown invariants (nothing stuck)"
shutdown_clean \
    && ok "shutdown invariants (nothing stuck)" || bad "shutdown invariants"
row_begin "direct-reply fired (direct=N)"
D=$(shutdown_value wb.direct)
[ -n "$D" ] && [ "$D" -gt 0 ] && ok "direct-reply fired (direct=$D)" || bad "direct-reply fired"
row_begin "dispatched==executed (N)"
R=$(shutdown_value work.dispatched)
E=$(shutdown_value work.executed)
[ -n "$R" ] && [ "$R" = "$E" ] && ok "dispatched==executed ($R)" || bad "dispatched==executed" "$R vs $E"
}

job_atomic_batteries(){
# Explicit ON boot plus the non-vacuous epoch-MVCC gates. atomic_torn includes its own OFF control,
# predecessor/promotion counters, overlapping writers, window liveness, and live CONFIG flips.
boot "$CANDIDATE_BINARY" --atomic 1 --enable-debug-command yes || bad "atomic release boot"
row_begin "atomic torn/window battery"
py tests/atomic_torn.py 127.0.0.1 $PORT --release-build >$TMPDIR/gate-atomic-torn.txt 2>&1 \
    && ok "atomic torn/window battery" || bad "atomic torn/window battery" "see $TMPDIR/gate-atomic-torn.txt"
row_begin "atomic RYOW/mixed-write battery"
py tests/atomic_ryow.py 127.0.0.1 $PORT >$TMPDIR/gate-atomic-ryow.txt 2>&1 \
    && ok "atomic RYOW/mixed-write battery" || bad "atomic RYOW/mixed-write battery" "see $TMPDIR/gate-atomic-ryow.txt"
row_begin "atomic owner-local hazard battery"
py tests/atomic_hazards.py 127.0.0.1 $PORT >$TMPDIR/gate-atomic-hazards.txt 2>&1 \
    && ok "atomic owner-local hazard battery" \
    || bad "atomic owner-local hazard battery" "see $TMPDIR/gate-atomic-hazards.txt"
stop
row_begin "atomic shutdown invariants"
shutdown_clean \
    && ok "atomic shutdown invariants" || bad "atomic shutdown invariants"
}

job_fused(){
# ---- FUSED mode: one boot per atomic mode, coarse three-stream production subset --------------
local AT=${1##*-}
  row_begin "fused boot line (atomic $AT)"
  if boot_fused "$CANDIDATE_BINARY" --atomic "$AT" --enable-debug-command yes; then
    FUSED_INFO=$(redis-cli -h 127.0.0.1 -p "$PORT" INFO server 2>/dev/null | tr -d '\r')
    FUSED_MODE=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^thread_mode://p')
    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^overlap://p')
    [ "$FUSED_MODE" = 1s ] && [ "$FUSED_OVERLAP" = 0 ] \
        && ok "fused boot line (atomic $AT)" \
        || bad "fused boot line (atomic $AT)" "wire mode=$FUSED_MODE overlap=$FUSED_OVERLAP"
  else
    bad "fused boot line (atomic $AT)" "server did not boot; see $SRVLOG"
  fi
  for t in s6 multi_exec edgeproto atomfix; do
    row_begin "fused $t battery (atomic $AT)"
    python3 "tests/$t.py" 127.0.0.1 "$PORT" >"$TMPDIR/gate-fused-$t-$AT.txt" 2>&1 \
        && ok "fused $t battery (atomic $AT)" \
        || bad "fused $t battery (atomic $AT)" "see $TMPDIR/gate-fused-$t-$AT.txt"
  done
  row_begin "fused spinprobe battery (atomic $AT)"
  py tests/spinprobe.py "$PORT" "$SRV" >"$TMPDIR/gate-fused-spinprobe-$AT.txt" 2>&1 \
      && ok "fused spinprobe battery (atomic $AT)" \
      || bad "fused spinprobe battery (atomic $AT)" \
             "see $TMPDIR/gate-fused-spinprobe-$AT.txt"
  row_begin "fused shutdown report (atomic $AT)"
  stop
  FUSED_REPORT_MODE=$(shutdown_value thread_mode)
  FUSED_REPORT_KIND=$(shutdown_value work.kind)
  shutdown_clean && [ "$FUSED_REPORT_MODE" = 1s ] && [ "$FUSED_REPORT_KIND" = fused ] \
      && ok "fused shutdown report (atomic $AT)" \
      || bad "fused shutdown report (atomic $AT)" \
             "mode=$FUSED_REPORT_MODE kind=$FUSED_REPORT_KIND; see $SRVLOG"
}

job_bplus(){
# ---- B+ pending-atomic filter: negative keys stay local, touched keys lower whole commands -----
# DEBUG geometry makes false positives impossible in these arms: A/B share one physical shard but
# use distinct filter cells, as do P/C on the other participating owner. ATOMIC-COMMIT-DELAY holds
# A/P after raw install and atomic_commit_holds proves the reserved/unpublished window was sampled.
boot_fused "$CANDIDATE_BINARY" --atomic 1 --read-local 1 \
    --enable-debug-command yes \
    || bad "B+ read-local purpose boot"
row_begin "B+ held-group GET/MGET filter battery"
py tests/bplus.py 127.0.0.1 "$PORT" >$TMPDIR/gate-bplus.txt 2>&1 \
    && ok "B+ held-group GET/MGET filter battery" \
    || bad "B+ held-group GET/MGET filter battery" "see $TMPDIR/gate-bplus.txt"
# Lane admission on the same armed boot: 32 connections per fused thread each pipelining 64 GETs
# oversubscribe the 1024-entry lane; the excess must be deferred and re-parsed locally (counters
# fire), never demoted to an owner task (fallback_lane_full stays 0), with order/RYOW intact.
row_begin "read-local lane admission battery"
py tests/read_local_lane.py 127.0.0.1 "$PORT" >$TMPDIR/gate-read-local-lane.txt 2>&1 \
    && ok "read-local lane admission battery" \
    || bad "read-local lane admission battery" "see $TMPDIR/gate-read-local-lane.txt"
stop
}

job_acl_recheck(){
# ---- ACL recheck over a CODED reply: exactly one reply per blocking command -------------------
# A blocking command's reply is discarded and replaced when the live ACL denies it at retire. A
# timeout answers "*-1" / "_", which are ReplyCode-carried, so a discard that clears only the byte
# buffer leaves the code standing and puts TWO replies on the wire. Its own boot in each thread
# mode: the battery creates an ACL user, and a live non-default user makes acl_active() true for
# everything else sharing the boot. Verified discriminating -- with Op::clear_reply() reverted to
# op.reply.clear() the four timeout rows fail and the rest pass.
boot "$CANDIDATE_BINARY" --enable-debug-command yes || bad "ACL-recheck reply boot (2s)"
row_begin "ACL recheck one-reply battery (2s)"
py tests/aclreply.py 127.0.0.1 "$PORT" >$TMPDIR/gate-aclreply-2s.txt 2>&1 \
    && ok "ACL recheck one-reply battery (2s)" \
    || bad "ACL recheck one-reply battery (2s)" "see $TMPDIR/gate-aclreply-2s.txt"
stop
boot_fused "$CANDIDATE_BINARY" --enable-debug-command yes || bad "ACL-recheck reply boot (1s)"
row_begin "ACL recheck one-reply battery (1s)"
py tests/aclreply.py 127.0.0.1 "$PORT" >$TMPDIR/gate-aclreply-1s.txt 2>&1 \
    && ok "ACL recheck one-reply battery (1s)" \
    || bad "ACL recheck one-reply battery (1s)" "see $TMPDIR/gate-aclreply-1s.txt"
stop
}

job_sort(){
# ---- SORT's dynamic keys: exact production gate geometry, both atomic modes -------------------
# tests/sort.py rejects any boot other than 16 shards at 6:2, buckets candidate names with
# DEBUG SHARD, and requires concrete BY and GET keys plus STORE destination on the executor slot
# opposite the source. No same-owner or one-executor run can satisfy this row.
for AT in 0 1; do
  boot "$CANDIDATE_BINARY" --ratio 6:2 --atomic "$AT" --enable-debug-command yes \
      || bad "cross-owner SORT boot (atomic $AT)"
  row_begin "cross-owner SORT battery (atomic $AT)"
  py tests/sort.py 127.0.0.1 "$PORT" >$TMPDIR/gate-sort-$AT.txt 2>&1 \
      && ok "cross-owner SORT battery (atomic $AT)" \
      || bad "cross-owner SORT battery (atomic $AT)" "see $TMPDIR/gate-sort-$AT.txt"
  stop
done
}

job_debug(){
# ---- debug-surface batteries: these drive DEBUG subcommands, hence their own armed boot -------
local AT=${1##*-}
  boot "$CANDIDATE_BINARY" --atomic $AT --enable-debug-command yes \
      || bad "debug-surface boot (atomic $AT)"
  # scriptatomic needs the armed boot for its cross-shard section (DEBUG SHARD proves the group
  # really spans owners; ATOMIC-COMMIT-DELAY / ATOMIC-READ-DELAY widen the window). It flips
  # `atomic` itself as well, so it covers both modes from either boot.
  # execatomic needs the armed boot too: DEBUG SHARD proves its reads really fan out over more
  # than one owner, and ATOMIC-FANOUT-DEFER parks all but the lead fragment of a cross-shard read
  # so a whole transaction can commit inside the fan-out on demand. It flips `atomic` itself, so
  # either boot covers both modes.
  # execiso is the in-EXEC half of the same story and needs the same armed boot: DEBUG SHARD proves
  # its reads fan out over more than one owner, and ATOMIC-FANOUT-DEFER now parks MULTI-child
  # fragments too, so a foreign transaction can be made to commit BETWEEN two fragments of one
  # in-EXEC read on demand. It flips `atomic` itself, so either boot covers both modes. Its armed
  # arms all assert atomic_exec_read_cuts advanced, so the row cannot print PASS on a run where the
  # transaction never entered the read-cut machinery.
  # execfix needs the armed boot for DEBUG SHARD: without it a "two-owner transaction" arm could
  # silently be a same-owner arm, and every write-loss row below would pass for the wrong reason.
  # It flips `atomic` itself, so either boot covers both modes. Its rows assert counters as well as
  # data -- atomic_predecessor_reads must stay 0 (the resolver never answered from a parked
  # predecessor) and atomic_gauge_underflows must stay 0 (the store returned exactly the version
  # bytes it charged) -- and a build that cannot report either counter FAILS rather than passing.
  # xscript needs the armed boot for the same reason plus one of its own: SCRIPT-STAGE-DEFER parks
  # every cross-owner gather except the coordinator's AFTER the reservation sub-wave has armed each
  # declared key and the cut is chosen, which is the only way to land a plain write inside that
  # window on demand. Its counters (script_keys_armed / script_write_tickets_forced /
  # script_group_occ_retries) are what make the reservation falsifiable rather than merely present.
  # multirace is the withdrawn-candidate half of multires and needs the same armed boot: DEBUG
  # SHARD is what proves the aborting MSETNX's blocker really lives on an owner other than its
  # victims', without which the install-then-veto race cannot occur and every round would pass for
  # the wrong reason. Its armed cases assert atomic_exec_order_holds advanced, so the row cannot
  # print PASS on a run where no transaction fragment ever met an undecided same-connection unit,
  # and its committed-MSETNX control asserts read-your-own-writes still crosses the two units --
  # a "fix" that merely hid the group's candidate would fail there.
  for t in lbsignals slowlog atomfix scriptatomic execatomic execiso execfix multires multirace session_monotonic xacct xmove xscript; do
    FEATURE_ARGS=()
    [ "$t" = xmove ] && FEATURE_ARGS+=(--release-build)
    row_begin "$t battery (atomic $AT)"
    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >$TMPDIR/gate-$t-$AT.txt 2>&1 \
        && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see $TMPDIR/gate-$t-$AT.txt"
  done
  stop
}

job_script_bounds(){
# ---- cross-owner script bounds: production auto budgets and four cut slots per IO ----------
for XS_MODE in limit window; do
  boot "$CANDIDATE_BINARY" --atomic 1 --enable-debug-command yes \
      || bad "xscript $XS_MODE control boot"
  row_begin "xscript $XS_MODE control"
  py tests/xscript.py 127.0.0.1 $PORT "$XS_MODE" >$TMPDIR/gate-xscript-$XS_MODE.txt 2>&1 \
      && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
             "see $TMPDIR/gate-xscript-$XS_MODE.txt"
  stop
done
}

job_efficiency(){
# ---- efficiency guards: each needs a boot geometry of its own, so each gets its own server ----
# Exact mechanism checks run on every build; timing budgets require the explicit release-build
# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md).
# --shards 1 for the same reason as the borrow guard: the sidecar under test is per shard, and the
# battery asserts that precondition rather than quietly measuring a diluted one.
boot "$CANDIDATE_BINARY" --shards 1 --enable-debug-command yes || bad "expire-index guard boot"
row_begin "expire-index growth bound"
py tests/expireindex.py 127.0.0.1 $PORT --release-build >$TMPDIR/gate-expireindex.txt 2>&1 \
    && ok "expire-index growth bound" || bad "expire-index growth bound" "see $TMPDIR/gate-expireindex.txt"
stop

# one shard so every borrow lands in ONE registry (the quantity under test), and a small zc-min so
# an ordinary-sized value still takes the borrow path and pays registry cost.
boot "$CANDIDATE_BINARY" --shards 1 --zc-min 64 --client-output-buffer-limit "normal 0 0 0" \
    --enable-debug-command yes || bad "borrow-registry guard boot"
row_begin "borrow-registry growth bound"
py tests/borrow_registry.py 127.0.0.1 $PORT --release-build >$TMPDIR/gate-borrow.txt 2>&1 \
    && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see $TMPDIR/gate-borrow.txt"
stop

# This row restores the original manual ownership geometry: two real owners plus empty fillers.
# It boots its own arms; the maintainer runs it on the quiet box with the rest of the gate.
quiet_wait
row_begin "cross-shard dispatch scaling"
XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN="$CANDIDATE_BINARY" bash tests/xshard_dispatch_scale.sh \
    >$TMPDIR/gate-xds.txt 2>&1 \
    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see $TMPDIR/gate-xds.txt"
}

job_dump_restore(){
# ---- Redis-wire DUMP/RESTORE survives the native snapshot/restart boundary -------------------
DUMPRESTORE_DIR=$(mktemp -d $TMPDIR/gate-dumprestore.XXXXXX)
boot "$CANDIDATE_BINARY" --atomic 1 --dir "$DUMPRESTORE_DIR" --dbfilename dumprestore.tomo \
    || bad "DUMP/RESTORE restart preparation boot"
row_begin "DUMP/RESTORE prepare + native SAVE"
py tests/dumprestore.py 127.0.0.1 $PORT prepare_restart \
    >$TMPDIR/gate-dumprestore-restart.txt 2>&1 \
    && ok "DUMP/RESTORE prepare + native SAVE" \
    || bad "DUMP/RESTORE restart preparation" "see $TMPDIR/gate-dumprestore-restart.txt"
stop
boot "$CANDIDATE_BINARY" --atomic 1 --dir "$DUMPRESTORE_DIR" \
    --dbfilename dumprestore.tomo \
    || bad "DUMP/RESTORE snapshot reload boot"
row_begin "DUMP/RESTORE cross-restart round-trip"
py tests/dumprestore.py 127.0.0.1 $PORT verify_restart \
    >>$TMPDIR/gate-dumprestore-restart.txt 2>&1 \
    && ok "DUMP/RESTORE cross-restart round-trip" \
    || bad "DUMP/RESTORE cross-restart round-trip" "see $TMPDIR/gate-dumprestore-restart.txt"
stop
row_begin "DUMP/RESTORE restart shutdown invariants"
shutdown_clean \
    && ok "DUMP/RESTORE restart shutdown invariants" \
    || bad "DUMP/RESTORE restart shutdown invariants"
}

job_auth(){
# ---- auth + audit DEBUG (purpose-booted; each test asserts its gate actually opened) -----------
row_begin "reject bad protected-mode"
reject_boot --protected-mode maybe 2>&1 | grep -q "protected-mode wants" \
    && ok "reject bad protected-mode" || bad "reject bad protected-mode"
row_begin "reject bad enable-debug-command"
reject_boot --enable-debug-command maybe 2>&1 | grep -q "enable-debug-command wants" \
    && ok "reject bad enable-debug-command" || bad "reject bad enable-debug-command"
boot "$CANDIDATE_BINARY" --requirepass gatepass || bad "auth purpose boot"
row_begin "AUTH/HELLO/protected state machine"
py tests/auth.py 127.0.0.1 $PORT gatepass >$TMPDIR/gate-auth.txt 2>&1 \
    && ok "AUTH/HELLO/protected state machine" || bad "AUTH/HELLO/protected state machine" "see $TMPDIR/gate-auth.txt"
stop
ACL_DIR=$(mktemp -d $TMPDIR/gate-acl.XXXXXX)
ACL_FILE="$ACL_DIR/users.acl"
: > "$ACL_FILE"
boot "$CANDIDATE_BINARY" --aclfile "$ACL_FILE" || bad "ACL purpose boot"
row_begin "ACL battery (atomic off)"
py tests/acl.py 127.0.0.1 $PORT "$ACL_FILE" >$TMPDIR/gate-acl.txt 2>&1 \
    && ok "ACL battery (atomic off)" || bad "ACL battery (atomic off)" "see $TMPDIR/gate-acl.txt"
stop
ACL_ATOMIC_FILE="$ACL_DIR/users-atomic.acl"
: > "$ACL_ATOMIC_FILE"
boot "$CANDIDATE_BINARY" --aclfile "$ACL_ATOMIC_FILE" --atomic 1 || bad "ACL atomic purpose boot"
row_begin "ACL battery (atomic on)"
py tests/acl.py 127.0.0.1 $PORT "$ACL_ATOMIC_FILE" >$TMPDIR/gate-acl-atomic.txt 2>&1 \
    && ok "ACL battery (atomic on)" || bad "ACL battery (atomic on)" "see $TMPDIR/gate-acl-atomic.txt"
stop
DEBUG_DIR=$(mktemp -d $TMPDIR/gate-debug.XXXXXX)
boot "$CANDIDATE_BINARY" --enable-debug-command local --dir "$DEBUG_DIR" --dbfilename reload.tomo \
    || bad "DEBUG purpose boot"
row_begin "DEBUG toggle/reload battery"
py tests/debug.py 127.0.0.1 $PORT >$TMPDIR/gate-debug.txt 2>&1 \
    && ok "DEBUG toggle/reload battery" || bad "DEBUG toggle/reload battery" "see $TMPDIR/gate-debug.txt"
row_begin "typed snapshot round-trip incl stream"
{ redis_cli_expect_ok FLUSHALL \
    && py tests/snap_typed_roundtrip.py $PORT build_save \
    && redis_cli_expect_ok DEBUG RELOAD \
    && py tests/snap_typed_roundtrip.py $PORT verify; } \
    >$TMPDIR/gate-snap-typed.txt 2>&1 \
    && ok "typed snapshot round-trip incl stream" \
    || bad "typed snapshot round-trip incl stream" "see $TMPDIR/gate-snap-typed.txt"
stop
}

job_snapshot(){
# ---- snapshot data/sync engines: cut, typed round-trip, and typed preimage race ----------------
local NET_IO=${1##*-}
  SNAP_DIR=$(mktemp -d "$TMPDIR/gate-snapshot-${NET_IO}.XXXXXX")
  boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" \
      --dir "$SNAP_DIR" --dbfilename cut.tomo \
      || bad "snapshot cut boot ($NET_IO)"
  row_begin "snapshot concurrent cut ($NET_IO)"
  py tests/snap_cut_battery.py "$PORT" save \
      >"$TMPDIR/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
      && ok "snapshot concurrent cut ($NET_IO)" \
      || bad "snapshot concurrent cut ($NET_IO)" \
             "see $TMPDIR/gate-snapshot-cut-${NET_IO}.txt"
  stop
  boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" \
      --dir "$SNAP_DIR" --dbfilename cut.tomo \
      || bad "snapshot cut reload boot ($NET_IO)"
  row_begin "snapshot cut reload ($NET_IO)"
  py tests/snap_cut_battery.py "$PORT" verify_cut \
      >>"$TMPDIR/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
      && ok "snapshot cut reload ($NET_IO)" \
      || bad "snapshot cut reload ($NET_IO)" \
             "see $TMPDIR/gate-snapshot-cut-${NET_IO}.txt"
  stop

  # The cut battery above writes only single keys, so it passes on a tree whose snapshot tears
  # every cross-shard atomic group. These two arms are the ones that can tell the difference:
  # generation-tagged groups, a live MGET reader that must stay clean throughout, and the
  # cuts_waited counter that separates a real group drain from a vacuous one. Both arms were
  # confirmed to FAIL on the pre-fix tree (44-53 and 10-13 torn groups per cut respectively).
  GROUP_DIR=$(mktemp -d "$TMPDIR/gate-snapshot-groups-${NET_IO}.XXXXXX")
  boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" --atomic 1 \
      --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
      || bad "atomic group cut boot ($NET_IO, atomic 1)"
  row_begin "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)"
  py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" mset 5 \
      >"$TMPDIR/gate-snapshot-groups-mset-${NET_IO}.txt" 2>&1 \
      && grep -q "ATOMIC_GROUP_CUT PASS" "$TMPDIR/gate-snapshot-groups-mset-${NET_IO}.txt" \
      && ok "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
      || bad "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
             "see $TMPDIR/gate-snapshot-groups-mset-${NET_IO}.txt"
  stop
  # CONTROL ARM: the DEFAULT --atomic 0. EXEC force-admits a group at either setting, so a
  # transaction is atomic to readers here too and the file must agree with them.
  boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" --atomic 0 \
      --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
      || bad "atomic group cut boot ($NET_IO, atomic 0)"
  row_begin "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)"
  py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" exec 3 \
      >"$TMPDIR/gate-snapshot-groups-exec-${NET_IO}.txt" 2>&1 \
      && grep -q "ATOMIC_GROUP_CUT PASS" "$TMPDIR/gate-snapshot-groups-exec-${NET_IO}.txt" \
      && ok "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
      || bad "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
             "see $TMPDIR/gate-snapshot-groups-exec-${NET_IO}.txt"
  stop

  TYPED_DIR=$(mktemp -d "$TMPDIR/gate-snapshot-typed-${NET_IO}.XXXXXX")
  boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" \
      --dir "$TYPED_DIR" --dbfilename typed.tomo \
      || bad "typed snapshot boot ($NET_IO)"
  row_begin "typed snapshot save ($NET_IO)"
  py tests/snap_typed_roundtrip.py "$PORT" build_save \
      >"$TMPDIR/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
      && ok "typed snapshot save ($NET_IO)" \
      || bad "typed snapshot save ($NET_IO)" \
             "see $TMPDIR/gate-snapshot-typed-${NET_IO}.txt"
  stop
  boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" \
      --dir "$TYPED_DIR" --dbfilename typed.tomo \
      || bad "typed snapshot reload boot ($NET_IO)"
  row_begin "typed snapshot reload ($NET_IO)"
  py tests/snap_typed_roundtrip.py "$PORT" verify \
      >>"$TMPDIR/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
      && ok "typed snapshot reload ($NET_IO)" \
      || bad "typed snapshot reload ($NET_IO)" \
             "see $TMPDIR/gate-snapshot-typed-${NET_IO}.txt"
  stop

  RACE_DIR=$(mktemp -d "$TMPDIR/gate-snapshot-race-${NET_IO}.XXXXXX")
  boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" \
      --dir "$RACE_DIR" --dbfilename race.tomo --save '' --enable-debug-command yes \
      || bad "typed snapshot race boot ($NET_IO)"
  row_begin "typed snapshot preimage race ($NET_IO)"
  py tests/snap_typed_race.py "$PORT" race "$RACE_DIR/race.tomo" \
      >"$TMPDIR/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
      && grep -q 'PREIMAGE-FIRED PASS' "$TMPDIR/gate-snapshot-race-${NET_IO}.txt" \
      && grep -q 'SNAPSHOT-TICKET-ORACLE PASS' "$TMPDIR/gate-snapshot-race-${NET_IO}.txt" \
      && ok "typed snapshot preimage race ($NET_IO)" \
      || bad "typed snapshot preimage race ($NET_IO)" \
             "see $TMPDIR/gate-snapshot-race-${NET_IO}.txt"
  stop
  boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" \
      --dir "$RACE_DIR" --dbfilename race.tomo.cut \
      || bad "typed snapshot race reload boot ($NET_IO)"
  row_begin "typed snapshot race reload ($NET_IO)"
  py tests/snap_typed_race.py "$PORT" verify "$RACE_DIR/race.tomo.oracle.json" \
      >>"$TMPDIR/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
      && ok "typed snapshot race reload ($NET_IO)" \
      || bad "typed snapshot race reload ($NET_IO)" \
             "see $TMPDIR/gate-snapshot-race-${NET_IO}.txt"
  stop
}

job_notify(){
# ---- notify lane: integrated owner/retire seams plus both live atomic settings -----------------
boot "$CANDIDATE_BINARY" --notify-keyspace-events KEAmn --enable-debug-command yes \
    || bad "feature battery boot + notify CLI knob"   # armed: multi_exec.py needs DEBUG SHARD
                                                       # to locate a same-owner key pair, and it
                                                       # FAILS rather than skips without it
row_begin "MULTI feature battery"
py tests/multi_exec.py 127.0.0.1 $PORT >$TMPDIR/gate-multi.txt 2>&1 \
    && ok "MULTI feature battery" || bad "MULTI feature battery" "see $TMPDIR/gate-multi.txt"
row_begin "blocking feature battery"
py tests/blocking.py 127.0.0.1 $PORT >$TMPDIR/gate-blocking.txt 2>&1 \
    && ok "blocking feature battery" || bad "blocking feature battery" "see $TMPDIR/gate-blocking.txt"
row_begin "pubsub feature battery"
py tests/pubsub.py 127.0.0.1 $PORT >$TMPDIR/gate-pubsub.txt 2>&1 \
    && ok "pubsub feature battery" || bad "pubsub feature battery" "see $TMPDIR/gate-pubsub.txt"
row_begin "Lua feature battery"
py tests/lua_scripting.py 127.0.0.1 $PORT >$TMPDIR/gate-lua.txt 2>&1 \
    && ok "Lua feature battery" || bad "Lua feature battery" "see $TMPDIR/gate-lua.txt"
row_begin "keyspace notification battery (atomic 0/1)"
py tests/notify.py 127.0.0.1 $PORT >$TMPDIR/gate-notify.txt 2>&1 \
    && ok "keyspace notification battery (atomic 0/1)" \
    || bad "keyspace notification battery" "see $TMPDIR/gate-notify.txt"
stop
row_begin "feature battery shutdown invariants"
shutdown_clean \
    && ok "feature battery shutdown invariants" || bad "feature battery shutdown invariants"
}

job_flip(){
# ---- FLIP lane: runtime thread reshaping is a shipped feature and gates like one ---------------
# Four rows, each guarding a failure class that actually shipped once:
#   flip.py            the 542-check state battery (incl. same-split no-op moves NOTHING)
#   flip_under_load    value-verified traffic + flips: no drops, no BUSY, no regressions
#   flip_ttl           TTL state AND expiry events survive the shard/client moves a flip performs
#   saturated flips    a one-shot quiesce snapshot once refused ~every flip under 512-conn
#                      pipelined load while 8-conn tests sailed through; this row flips under
#                      real saturation and asserts every one APPLIES (live split == requested)
boot "$CANDIDATE_BINARY" --enable-debug-command yes || bad "flip battery boot"
row_begin "FLIP state battery"
py tests/flip.py 127.0.0.1 $PORT >$TMPDIR/gate-flip.txt 2>&1 \
    && ok "FLIP state battery" || bad "FLIP state battery" "see $TMPDIR/gate-flip.txt"
row_begin "FLIP under verified load"
py tests/flip_under_load.py 127.0.0.1 $PORT 20 >$TMPDIR/gate-flip-load.txt 2>&1 \
    && ok "FLIP under verified load" || bad "FLIP under verified load" "see $TMPDIR/gate-flip-load.txt"
row_begin "FLIP TTL + expiry events"
py tests/flip_ttl.py 127.0.0.1 $PORT >$TMPDIR/gate-flip-ttl.txt 2>&1 \
    && ok "FLIP TTL + expiry events" || bad "FLIP TTL + expiry events" "see $TMPDIR/gate-flip-ttl.txt"
row_begin "partial-frame conn parks (no io spin)"
py tests/spinprobe.py $PORT "$SRV" >$TMPDIR/gate-spinprobe.txt 2>&1 \
    && ok "partial-frame conn parks (no io spin)" || bad "partial-frame conn parks (no io spin)" "see $TMPDIR/gate-spinprobe.txt"
stop
}

job_flip_saturated(){
boot "$CANDIDATE_BINARY" --enable-debug-command yes || bad "flip battery reboot"
quiet_wait
row_begin "FLIP applies under saturated load"
(
  taskset -c "$LOAD_CORES" memtier_benchmark -s 127.0.0.1 -p $PORT --protocol=redis -t 8 -c 32 \
    --pipeline=16 --ratio=1:1 --key-pattern=R:R --key-minimum=1 --key-maximum=200000 -d 64 \
    --test-time=25 --distinct-client-seed --hide-histogram >/dev/null 2>&1 &
  MTPID=$!
  sleep 3
  SATOK=1
  TOTAL=$(redis-cli -p $PORT flip 2>/dev/null | paste - - | awk '/live_io/{a=$2} /live_ex/{b=$2} END{print a+b}')
  for FR in 25 75 50; do
    TIO=$(( TOTAL * FR / 100 )); [ "$TIO" -lt 2 ] && TIO=2; [ "$TIO" -gt $((TOTAL-2)) ] && TIO=$((TOTAL-2))
    OUT=$(redis-cli -p $PORT flip $TIO $((TOTAL-TIO)) 2>&1); sleep 1
    LIVE=$(redis-cli -p $PORT flip 2>/dev/null | paste - - | awk '/live_io/{print $2; exit}')
    { [ "$OUT" = "OK" ] && [ "$LIVE" = "$TIO" ]; } || { SATOK=0; echo "flip $TIO refused/missed: '$OUT' live=$LIVE" >>$TMPDIR/gate-flip-sat.txt; }
  done
  kill -9 $MTPID 2>/dev/null; wait "$MTPID" 2>/dev/null
  # The 256 SIGKILLed connections must REAP before shutdown or the drain line reports them as
  # live (observed: live_conns=256 on the oversubscribed gate cores, where io threads are starved
  # at the moment of the kill). Waiting for reaping keeps the shutdown invariant meaningful: dead
  # clients that never reap would be a real defect and still fail the row.
  for _ in $(seq 100); do
    LEFT=$(redis-cli -p $PORT info clients 2>/dev/null | tr -d '\r' | sed -n 's/^connected_clients://p')
    [ -n "$LEFT" ] && [ "$LEFT" -le 2 ] && break
    sleep 0.1
  done
  exit $((1 - SATOK))
) && ok "FLIP applies under saturated load" \
  || bad "FLIP applies under saturated load" "see $TMPDIR/gate-flip-sat.txt"
stop
# The saturation row kill -9s its memtier, so 256 aborted connections drain at shutdown -- the
# invariant line lands a beat after stop returns. Poll instead of racing it.
row_begin "flip battery shutdown invariants"
FLIPSHUT=0
for _ in $(seq 50); do
  shutdown_clean \
      && { FLIPSHUT=1; break; }
  sleep 0.1
done
[ "$FLIPSHUT" = 1 ] && ok "flip battery shutdown invariants" \
    || bad "flip battery shutdown invariants"
}

job_atomic_floor(){
# ---- atomic multi-key throughput floor -------------------------------------------------------
# A 9x per-op tax on the atomic chain-read path (tripwire armed by --enable-debug-command alone)
# once passed this gate 236/236: nothing here drove MGET/MSET through the resolver and asserted a
# rate. Floor = 120k on the oversubscribed gate cores; healthy measures ~600k (5x margin), the
# regression class this catches lands under 70k. Boots the harness posture on purpose -- debug
# enabled, tripwire NOT armed -- because that is the posture every bench and gate row runs in.
boot "$CANDIDATE_BINARY" --atomic 1 --enable-debug-command yes || bad "atomic mm floor boot"
row_begin "atomic MGET/MSET floor (N/s >= 120k)"
MM_K8="__key__ __key__ __key__ __key__ __key__ __key__ __key__ __key__"
MM_M8="__key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__"
quiet_wait
taskset -c "$LOAD_CORES" memtier_benchmark -s 127.0.0.1 -p $PORT --protocol=redis -t 4 -c 16 \
  --pipeline=16 --command="MGET $MM_K8" --command-ratio=9 --command-key-pattern=R \
  --command="MSET $MM_M8" --command-ratio=1 --command-key-pattern=R -d 64 \
  --key-minimum=1 --key-maximum=200000 --test-time=20 --hide-histogram >/dev/null 2>&1 &
MMPID=$!
sleep 8
MM_C0=$(redis-cli -p $PORT info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p')
sleep 6
MM_C1=$(redis-cli -p $PORT info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p')
kill -9 $MMPID 2>/dev/null; wait $MMPID 2>/dev/null; MMPID=0
MM_RATE=$(( (${MM_C1:-0} - ${MM_C0:-0}) / 6 ))
[ "$MM_RATE" -ge 120000 ] \
    && ok "atomic MGET/MSET floor (${MM_RATE}/s >= 120k)" \
    || bad "atomic MGET/MSET floor" "measured ${MM_RATE}/s < 120000/s"
# Same boot: the explicit arm surface answers OK both ways (observation itself is priced by the
# floor row above -- if arming ever becomes the boot default again, that row fails, not this one).
row_begin "tripwire arm/disarm round-trip"
TWARM=$(redis-cli -p $PORT debug tripwire arm 2>&1)
TWDIS=$(redis-cli -p $PORT debug tripwire disarm 2>&1)
{ [ "$TWARM" = "OK" ] && [ "$TWDIS" = "OK" ]; } \
    && ok "tripwire arm/disarm round-trip" \
    || bad "tripwire arm/disarm round-trip" "arm='$TWARM' disarm='$TWDIS'"
# Same boot: pipelined same-connection program order (the seed-19 divergence). Pre-fix this
# answered stale in ~74% of iterations, so 400 iterations are a decisive non-vacuous roll.
row_begin "pipelined same-conn program order (400 rolls)"
python3 tests/pipeorder.py $PORT 400 >$TMPDIR/gate-pipeorder.txt 2>&1 \
    && ok "pipelined same-conn program order (400 rolls)" \
    || bad "pipelined same-conn program order" "see $TMPDIR/gate-pipeorder.txt"
stop
}

job_aof(){
# ---- AOF boot/replay + non-vacuous DEBUG LOADAOF ---------------------------------------------
local NET_IO=${1##*-}
for AOF_ATOMIC in 0 1; do
AOF_DIR=$(mktemp -d "$TMPDIR/gate-aof-${NET_IO}-atomic${AOF_ATOMIC}.XXXXXX")
AOF_STATE=$AOF_DIR/state.json
boot "$CANDIDATE_BINARY" --protected-mode no --atomic "$AOF_ATOMIC" \
    --appendonly yes --appendfsync no \
    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_DIR" \
    || bad "AOF purpose boot ($NET_IO, atomic $AOF_ATOMIC)"
row_begin "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)"
py tests/knobs.py 127.0.0.1 "$PORT" "$NET_IO" "$AOF_ATOMIC" \
    >"$TMPDIR/gate-knobs-${NET_IO}-${AOF_ATOMIC}.txt" 2>&1 \
    && ok "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)" \
    || bad "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)"
row_begin "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)"
py tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >$TMPDIR/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
    && py tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>$TMPDIR/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
           "see $TMPDIR/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
row_begin "AOF writer fired (records=N)"
AOF_PRE_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>$TMPDIR/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
AOF_WRITTEN=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_records_written://p')
[ -n "$AOF_WRITTEN" ] && [ "$AOF_WRITTEN" -gt 0 ] \
    && ok "AOF writer fired (records=$AOF_WRITTEN)" || bad "AOF writer fired"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
settle
boot "$CANDIDATE_BINARY" --protected-mode no --appendonly yes --appendfsync no \
    --atomic "$AOF_ATOMIC" --net-io "$NET_IO" \
    --enable-debug-command yes --dir "$AOF_DIR" \
    || bad "AOF replay boot ($NET_IO, atomic $AOF_ATOMIC)"
row_begin "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)"
py tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>$TMPDIR/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
    && ok "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
    || bad "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
           "see $TMPDIR/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
row_begin "AOF native snapshot streams byte-exact"
AOF_POST_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>$TMPDIR/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
[ -n "$AOF_PRE_MODEL" ] && [ "$AOF_PRE_MODEL" = "$AOF_POST_MODEL" ] \
    && ok "AOF native snapshot streams byte-exact" || bad "AOF native snapshot streams byte-exact"
row_begin "AOF replay fired (records=N skipped=N)"
AOF_REPLAYED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_replayed_records://p')
AOF_SKIPPED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_groups_skipped_on_replay://p')
[ -n "$AOF_REPLAYED" ] && [ "$AOF_REPLAYED" -gt 0 ] && [ -n "$AOF_SKIPPED" ] \
    && ok "AOF replay fired (records=$AOF_REPLAYED skipped=$AOF_SKIPPED)" || bad "AOF replay fired"
stop
done

# ---- AOF atomic-group bracketing + directed interrupted-process recovery ---------------------
AOF_GROUP_DIR=$(mktemp -d "$TMPDIR/gate-aof-group-${NET_IO}.XXXXXX")
AOF_GROUP_STATE=$AOF_GROUP_DIR/state.json
boot "$CANDIDATE_BINARY" --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
    || bad "AOF group purpose boot ($NET_IO)"
row_begin "AOF directed group interruption fired"
py tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
    >$TMPDIR/gate-aof-group.txt 2>&1 \
    && ok "AOF directed group interruption fired" \
    || bad "AOF directed group interruption" "see $TMPDIR/gate-aof-group.txt"
wait $SRV 2>/dev/null
settle
boot "$CANDIDATE_BINARY" --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
    || bad "AOF group recovery boot ($NET_IO)"
row_begin "AOF atomic-group recovery + writer order"
py tests/aof_torn_group.py 127.0.0.1 $PORT verify "$AOF_GROUP_STATE" \
    >>$TMPDIR/gate-aof-group.txt 2>&1 \
    && py tests/aof_torn_group.py 127.0.0.1 $PORT scan \
       "$AOF_GROUP_DIR/appendonlydir/appendonly.aof.1.incr.tomo" \
       >>$TMPDIR/gate-aof-group.txt 2>&1 \
    && ok "AOF atomic-group recovery + writer order" \
    || bad "AOF atomic-group recovery + writer order" "see $TMPDIR/gate-aof-group.txt"
stop
row_begin "AOF group shutdown invariants"
shutdown_clean \
    && ok "AOF group shutdown invariants" || bad "AOF group shutdown invariants"

# ---- AOF sync policies, reply gate, idle sync, and durability-window recovery ----------------
AOF_ALWAYS_DIR=$(mktemp -d "$TMPDIR/gate-aof-always-${NET_IO}.XXXXXX")
AOF_ALWAYS_STATE=$AOF_ALWAYS_DIR/state.json
boot "$CANDIDATE_BINARY" --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
    || bad "AOF always purpose boot ($NET_IO)"
row_begin "AOF always sync + reply gate fired"
py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_ALWAYS_STATE" always 512 \
    >$TMPDIR/gate-aof-always.txt 2>&1 \
    && ok "AOF always sync + reply gate fired" \
    || bad "AOF always sync + reply gate" "see $TMPDIR/gate-aof-always.txt"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
settle
boot "$CANDIDATE_BINARY" --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
    || bad "AOF always recovery boot ($NET_IO)"
row_begin "AOF always acknowledged-prefix recovery"
py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_ALWAYS_STATE" always 512 \
    >>$TMPDIR/gate-aof-always.txt 2>&1 \
    && ok "AOF always acknowledged-prefix recovery" \
    || bad "AOF always acknowledged-prefix recovery" "see $TMPDIR/gate-aof-always.txt"
stop

AOF_EVERY_DIR=$(mktemp -d "$TMPDIR/gate-aof-everysec-${NET_IO}.XXXXXX")
AOF_EVERY_STATE=$AOF_EVERY_DIR/state.json
AOF_EVERY_FILE=$AOF_EVERY_DIR/appendonlydir/appendonly.aof.1.incr.tomo
boot "$CANDIDATE_BINARY" --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
    || bad "AOF everysec purpose boot ($NET_IO)"
row_begin "AOF everysec write gate + idle sync fired"
py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_EVERY_STATE" everysec 512 \
    >$TMPDIR/gate-aof-everysec.txt 2>&1 \
    && ok "AOF everysec write gate + idle sync fired" \
    || bad "AOF everysec write gate + idle sync" "see $TMPDIR/gate-aof-everysec.txt"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
AOF_EVERY_SIZE=$(stat -c %s "$AOF_EVERY_FILE" 2>/dev/null || echo 0)
case "$AOF_EVERY_FILE" in
  "$AOF_EVERY_DIR"/appendonlydir/*)
    [ "$AOF_EVERY_SIZE" -gt 7 ] && truncate -s $((AOF_EVERY_SIZE-7)) "$AOF_EVERY_FILE" ;;
  *) bad "AOF everysec tail target" ;;
esac
settle
boot "$CANDIDATE_BINARY" --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
    || bad "AOF everysec recovery boot ($NET_IO)"
row_begin "AOF everysec durability window + tail warning"
py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
    >>$TMPDIR/gate-aof-everysec.txt 2>&1 \
    && grep -q "AOF warning: truncated AOF tail" "$SRVLOG" \
    && ok "AOF everysec durability window + tail warning" \
    || bad "AOF everysec durability window + tail warning" "see $TMPDIR/gate-aof-everysec.txt"
stop

AOF_NO_DIR=$(mktemp -d "$TMPDIR/gate-aof-no-sync-${NET_IO}.XXXXXX")
boot "$CANDIDATE_BINARY" --protected-mode no --atomic 0 --appendonly yes --appendfsync no \
    --net-io "$NET_IO" --dir "$AOF_NO_DIR" \
    || bad "AOF no-sync purpose boot ($NET_IO)"
row_begin "AOF no-sync bypassed sync + reply gate"
py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
    >$TMPDIR/gate-aof-no-sync.txt 2>&1 \
    && ok "AOF no-sync bypassed sync + reply gate" \
    || bad "AOF no-sync bypass" "see $TMPDIR/gate-aof-no-sync.txt"
stop

quiet_wait
row_begin "AOF rewrite atomic/stage/corruption matrix"
NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
    >$TMPDIR/gate-aof-rewrite.txt 2>&1 \
    && ok "AOF rewrite atomic/stage/corruption matrix" \
    || bad "AOF rewrite matrix" "see $TMPDIR/gate-aof-rewrite.txt"

quiet_wait
row_begin "AOF rewrite triggers + observability matrix"
NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
    >$TMPDIR/gate-aof-rewrite-trigger.txt 2>&1 \
    && ok "AOF rewrite triggers + observability matrix" \
    || bad "AOF rewrite triggers" "see $TMPDIR/gate-aof-rewrite-trigger.txt"

AOF_OFF_DIR=$(mktemp -d "$TMPDIR/gate-aof-off-${NET_IO}.XXXXXX")
boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" \
    --appendonly no --dir "$AOF_OFF_DIR" \
    || bad "AOF-off negative-control boot"
row_begin "AOF-off negative-control seed landed"
AOF_OFF_SEED=$(redis-cli -h 127.0.0.1 -p $PORT SET aof-negative-control must-disappear 2>&1 |
    tr -d '\r')
AOF_OFF_PRE_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\r')
[ "$AOF_OFF_SEED" = OK ] && [ "$AOF_OFF_PRE_SIZE" = 1 ] \
    && ok "AOF-off negative-control seed landed" \
    || bad "AOF-off negative-control seed" "SET=$AOF_OFF_SEED DBSIZE=$AOF_OFF_PRE_SIZE"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
settle
boot "$CANDIDATE_BINARY" --protected-mode no --net-io "$NET_IO" \
    --appendonly no --dir "$AOF_OFF_DIR" \
    || bad "AOF-off negative-control reboot"
row_begin "AOF-off negative control lost data"
AOF_OFF_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\r')
[ "$AOF_OFF_SIZE" = 0 ] && [ ! -e "$AOF_OFF_DIR/appendonlydir" ] \
    && ok "AOF-off negative control lost data" || bad "AOF-off negative control"
stop
}

job_aof_frame(){
# ---- AOF physical framing: a control frame must never land inside a large record --------------
# A gate run's AOF replay boot exited with "AOF control record interleaves a large record": the
# writer flushed a ready GCMT at the top of a writer pass without checking that a large record
# still held the physical stream. Recovery truncates the file from that large record's first byte,
# so a control frame inside it is discardable -- and the loader refuses to start on the whole file.
# Syscall persistence under epoll: that is where the defect was demonstrated (11 of 114 runs of the AOF
# battery, 0 of 117 on uring) and where the window is entered reliably enough for the row to prove
# its mechanism fired. The battery FAILS on a build with the guard removed (6 of 6).
for AOF_FRAME_ATOMIC in 0 1; do
AOF_FRAME_DIR=$(mktemp -d "$TMPDIR/gate-aof-frameorder-atomic${AOF_FRAME_ATOMIC}.XXXXXX")
boot "$CANDIDATE_BINARY" --protected-mode no --atomic "$AOF_FRAME_ATOMIC" --appendonly yes \
    --appendfsync no --net-io epoll --auto-aof-rewrite-percentage 0 \
    --enable-debug-command yes --dir "$AOF_FRAME_DIR" \
    || bad "AOF frame-order purpose boot (atomic $AOF_FRAME_ATOMIC)"
row_begin "AOF control frame never inside a large record (atomic $AOF_FRAME_ATOMIC)"
py tests/aof_frame_order.py 127.0.0.1 $PORT "$AOF_FRAME_DIR/appendonlydir" \
    >$TMPDIR/gate-aof-frameorder-$AOF_FRAME_ATOMIC.txt 2>&1 \
    && ok "AOF control frame never inside a large record (atomic $AOF_FRAME_ATOMIC)" \
    || bad "AOF control frame never inside a large record (atomic $AOF_FRAME_ATOMIC)" \
           "see $TMPDIR/gate-aof-frameorder-$AOF_FRAME_ATOMIC.txt"
stop
done
}

job_tls(){
# ---- TLS memory-BIO transport: independent listener, auth matrix, parser/teardown fences -------
TLS_PORT=$((PORT+1))
TLS_DIR=$(mktemp -d $TMPDIR/gate-tls.XXXXXX)
row_begin "TLS ephemeral CA/server/client certificates"
py tests/tls.py --generate "$TLS_DIR" >$TMPDIR/gate-tls-generate.txt 2>&1 \
    && ok "TLS ephemeral CA/server/client certificates" \
    || bad "TLS certificate generation" "see $TMPDIR/gate-tls-generate.txt"

row_begin "reject TLS listener without certificate"
reject_boot --port 0 --tls-port "$TLS_PORT" --tls-key-file "$TLS_DIR/server.key" \
    --tls-auth-clients no 2>&1 | grep -q "tls-port requires tls-cert-file" \
    && ok "reject TLS listener without certificate" || bad "reject missing TLS certificate"
row_begin "reject invalid tls-protocols"
reject_boot --port 0 --tls-port "$TLS_PORT" --tls-cert-file "$TLS_DIR/server.crt" \
    --tls-key-file "$TLS_DIR/server.key" --tls-auth-clients no --tls-protocols SSLv3 \
    2>&1 | grep -q "Invalid tls-protocols" \
    && ok "reject invalid tls-protocols" || bad "reject invalid tls-protocols"
row_begin "reject invalid tls-ciphers"
reject_boot --port 0 --tls-port "$TLS_PORT" --tls-cert-file "$TLS_DIR/server.crt" \
    --tls-key-file "$TLS_DIR/server.key" --tls-auth-clients no --tls-ciphers NOT-A-CIPHER \
    2>&1 | grep -q "Failed to configure tls-ciphers" \
    && ok "reject invalid tls-ciphers" || bad "reject invalid tls-ciphers"

tlsboot(){ # auth-mode [extra TLS knobs]
  local auth=$1; shift
  quiet_wait
  SRV=0; SRVLOG=/dev/null
  guard_port "$PORT"
  guard_port "$TLS_PORT"
  SRVLOG=$(mktemp $TMPDIR/gate-tls-srv.XXXXXX)
  taskset -c $CORES "$CANDIDATE_BINARY" --port "$PORT" --tls-port "$TLS_PORT" \
      --bind 127.0.0.1 --shards 16 --ratio "$GATE_RATIO" --protected-mode no --dir "$TLS_DIR" \
      --tls-cert-file "$TLS_DIR/server.crt" --tls-key-file "$TLS_DIR/server.key" \
      --tls-ca-cert-file "$TLS_DIR/ca.crt" --tls-auth-clients "$auth" "$@" \
      >"$SRVLOG" 2>&1 &
  SRV=$!
  for _ in $(seq 50); do
    if ! kill -0 "$SRV" 2>/dev/null; then wait "$SRV" 2>/dev/null; return 1; fi
    if (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && \
       (exec 4<>/dev/tcp/127.0.0.1/$TLS_PORT) 2>/dev/null; then return 0; fi
    sleep 0.2
  done
  return 1
}

tlsboot yes || bad "TLS client-auth yes purpose boot"
row_begin "TLS client-auth yes matrix"
py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" yes --plain-port "$PORT" \
    >$TMPDIR/gate-tls-yes.txt 2>&1 \
    && ok "TLS client-auth yes matrix" || bad "TLS client-auth yes matrix" "see $TMPDIR/gate-tls-yes.txt"
stop
row_begin "TLS yes shutdown invariants"
shutdown_clean \
    && ok "TLS yes shutdown invariants" || bad "TLS yes shutdown invariants"
# (tls_ktls_active is a GAUGE — 0 at clean shutdown by design — so kTLS engagement is asserted
# live below, on the optional-mode boot, not from this shutdown dump.)

tlsboot optional || bad "TLS client-auth optional purpose boot"
row_begin "TLS client-auth optional matrix"
py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" optional --plain-port "$PORT" \
    >$TMPDIR/gate-tls-optional.txt 2>&1 \
    && ok "TLS client-auth optional matrix" \
    || bad "TLS client-auth optional matrix" "see $TMPDIR/gate-tls-optional.txt"
# Live kTLS engagement proof on the default boot: a plain TLS client connects and
# must see itself counted in the active gauge. Client-auth 'optional' permits a cert-less client.
row_begin "kTLS engaged live (default boot)"
python3 - "$TLS_PORT" "$TLS_DIR" <<'PYEOF' >$TMPDIR/gate-ktls-live.txt 2>&1 \
    && ok "kTLS engaged live (default boot)" || bad "kTLS engaged live" "see $TMPDIR/gate-ktls-live.txt"
import socket, ssl, sys, time
port, certdir = int(sys.argv[1]), sys.argv[2]
ctx = ssl.create_default_context(cafile=f"{certdir}/ca.crt")
ctx.check_hostname = False
s = ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=5))
s.sendall(b"INFO STATS\r\n"); time.sleep(0.4)
d = s.recv(1 << 20).decode(errors="replace")
line = [l for l in d.split("\r\n") if l.startswith("tls_ktls_active:")]
assert line, "no tls_ktls_active in INFO STATS: " + d[:200]
assert int(line[0].split(":")[1]) >= 1, "kTLS did not engage: " + line[0]
print("KTLS_LIVE_OK", line[0])
PYEOF
stop
row_begin "TLS optional shutdown invariants"
shutdown_clean \
    && ok "TLS optional shutdown invariants" || bad "TLS optional shutdown invariants"

# Exercise userspace fallback through the retained cipher grammar. TLS 1.2 uses CBC; TLS 1.3
# uses AES-256, outside this implementation's AES-128 TLS 1.3 RX installer. Counters must fire.
tlsboot no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers ECDHE-RSA-AES256-SHA384 \
    --tls-ciphersuites TLS_AES_256_GCM_SHA384 --tls-prefer-server-ciphers yes \
    || bad "TLS coexistence purpose boot"
row_begin "TLS pipeline/torn-record/coexistence battery"
py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" no --plain-port "$PORT" --full \
    --expect-ktls no >$TMPDIR/gate-tls-full.txt 2>&1 \
    && ok "TLS pipeline/torn-record/coexistence battery" \
    || bad "TLS correctness battery" "see $TMPDIR/gate-tls-full.txt"
stop
row_begin "TLS shutdown invariants"
shutdown_clean \
    && ok "TLS shutdown invariants" || bad "TLS shutdown invariants"
row_begin "TLS application send path error-free"
[ "$(shutdown_value wb.send_errors)" = 0 ] \
    && ok "TLS application send path error-free" || bad "TLS send errors"
row_begin "TLS connection slots all freed (N/N)"
TLS_ACCEPTS=$(shutdown_value tls.accepts)
TLS_FREED=$(shutdown_value tls.connections_freed)
TLS_ZC=$(shutdown_value tls.zc_suppressed)
[ -n "$TLS_ACCEPTS" ] && [ "$TLS_ACCEPTS" -gt 0 ] && [ "$TLS_ACCEPTS" = "$TLS_FREED" ] \
    && ok "TLS connection slots all freed ($TLS_FREED/$TLS_ACCEPTS)" \
    || bad "TLS connection-slot cleanup" "$TLS_FREED/$TLS_ACCEPTS"
row_begin "TLS zc borrow gates fired (suppressed=$TLS_ZC)"
[ -n "$TLS_ZC" ] && [ "$TLS_ZC" -gt 0 ] \
    && ok "TLS zc borrow gates fired (suppressed=$TLS_ZC)" || bad "TLS zc borrow gates fired"
}

job_feature_cell(){
  local FEATURE_CELL=${1#feature-cell-}
  row_begin "feature $FEATURE_CELL"
  py tests/feature_gate.py --cell "$FEATURE_CELL" --binary "$CANDIDATE_BINARY" \
        --server-cpus "$CORES" --load-cpus "${GATE_LOAD_CORES:-96-127}" --ratio "$GATE_RATIO" \
        --port "$PORT" --output "$FEATURE_OUTPUT" \
        >"$FEATURE_OUTPUT/$FEATURE_CELL.log" 2>&1 \
        && ok "feature $FEATURE_CELL" \
        || bad "feature $FEATURE_CELL" "see $FEATURE_OUTPUT/$FEATURE_CELL.log"
}

job_abba_selftest(){
# The ABBA tier's own decision logic, saturation rules and rejection paths, exercised serverless so
# a broken comparator is caught on any machine and before any measurement is trusted.
row_begin "ABBA comparison + saturation negative controls"
py tests/abbagate.py --self-test > $TMPDIR/gate-abbagate-unit.txt 2>&1 \
    && py tests/background_environment_test.py >> $TMPDIR/gate-abbagate-unit.txt 2>&1 \
    && py tests/gate_history.py self-test >> $TMPDIR/gate-abbagate-unit.txt 2>&1 \
    && ok "ABBA comparison + saturation negative controls" \
    || bad "ABBA comparison + saturation negative controls" "see $TMPDIR/gate-abbagate-unit.txt"
}

job_asan_batteries(){
# ---- 4. full tier: torture under ASAN ---------------------------------------------------------
boot $ASAN --atomic 1 --enable-debug-command yes || bad "ASAN boot"
row_begin "torture under ASAN"
py tests/torture.py 127.0.0.1 $PORT >$TMPDIR/gate-tort-asan.txt 2>&1 \
    && ok "torture under ASAN" || bad "torture under ASAN"
row_begin "RYOW under ASAN"
py tests/ryow.py 127.0.0.1 $PORT >$TMPDIR/gate-ryow-asan.txt 2>&1 \
    && ok "RYOW under ASAN" || bad "RYOW under ASAN"
# Deliberately omit --release-build: coverage/safety checks remain mandatory, including the
# derived-window witnesses; promotion timing prints without scoring its release budget.
row_begin "atomic torn/window under ASAN"
py tests/atomic_torn.py 127.0.0.1 $PORT >$TMPDIR/gate-atomic-torn-asan.txt 2>&1 \
    && ok "atomic torn/window under ASAN" || bad "atomic torn/window under ASAN"
# --no-rate-assertions: the battery's overlap section makes one claim about SPEED (pipelining 24
# atomic groups beats 24 serial round trips by >10%). ASAN does not slow the two arms by the same
# factor, so that ratio inverts here on a correct build -- measured pipe 21,005/s vs serial
# 24,202/s in a full-gate run whose every correctness check passed, on a build that passed 4 of 4
# standalone. The mechanism half of the same section (all 24 admitted, multiple groups observed in flight,
# so a younger cross-key group WAS in flight while an older one decided) still runs on this tier,
# and the measured rates are still printed in the log.
row_begin "atomic RYOW under ASAN"
py tests/atomic_ryow.py 127.0.0.1 $PORT --no-rate-assertions \
    >$TMPDIR/gate-atomic-ryow-asan.txt 2>&1 \
    && ok "atomic RYOW under ASAN" || bad "atomic RYOW under ASAN"
stop
# "No ASAN report" is a finding only if the ASAN server actually ran to its shutdown dump; an
# empty log (boot failed) has no ASAN text either and used to pass this row vacuously.
row_begin "ASAN clean"
if grep -q "ERROR: AddressSanitizer" "$SRVLOG"; then bad "ASAN clean" "see $SRVLOG"
elif shutdown_present; then ok "ASAN clean"
else bad "ASAN clean" "ASAN server never reached its shutdown dump; see $SRVLOG"; fi
}

job_replyoff(){
# ---- CLIENT REPLY OFF/SKIP + cross-shard MGET on a zero-copy boot: suppressed replies leave nothing on the
# wire and release their borrows (regression for the partial-array leak fixed on the netwb lane) ------------
for RA in 0 1; do
  boot "$CANDIDATE_BINARY" --shards 64 --zc-min 64 --atomic $RA --enable-debug-command yes || bad "replyoff boot (atomic $RA)"
  row_begin "CLIENT REPLY OFF/SKIP cross-shard MGET wire silence (atomic $RA)"
  py tests/replyoff_xshard.py 127.0.0.1 $PORT >$TMPDIR/gate-replyoff-$RA.txt 2>&1 \
      && ok "CLIENT REPLY OFF/SKIP cross-shard MGET wire silence (atomic $RA)" \
      || bad "CLIENT REPLY OFF/SKIP cross-shard MGET wire silence (atomic $RA)" "see $TMPDIR/gate-replyoff-$RA.txt"
  stop
done
}

job_zc(){
# ---- 4b. full tier: zero-copy borrow lifetime (release+ASAN) ----------------------------------
zcboot(){
  quiet_wait
  tools/quietcheck.sh "${GATE_CORES:-192-199}" "$PORT" 2>$TMPDIR/gate-quiet.err || {
    sleep 3
    tools/quietcheck.sh "${GATE_CORES:-192-199}" "$PORT" 2>>$TMPDIR/gate-quiet.err || {
      echo "boot preflight: cores/port not quiet: $(tail -1 $TMPDIR/gate-quiet.err)"; return 1; }
  }
  SRV=0; SRVLOG=/dev/null
  guard_port "$PORT"
  SRVLOG=$(mktemp $TMPDIR/gate-srv-zc.XXXXXX)
  taskset -c $CORES "$1" --port $PORT --bind 127.0.0.1 --shards 16 --ratio $GATE_RATIO \
      --dir "$(mktemp -d "$TMPDIR/zc-data.XXXXXX")" --zc-min 16384 > "$SRVLOG" 2>&1 &
  SRV=$!
  for _ in $(seq 50); do
    if ! kill -0 "$SRV" 2>/dev/null; then wait "$SRV" 2>/dev/null; return 1; fi
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2; done
  return 1
}
zcboot "$CANDIDATE_BINARY" || bad "zc boot"
row_begin "zc borrow battery"
py tests/zc.py 127.0.0.1 $PORT >$TMPDIR/gate-zc.txt 2>&1     && ok "zc borrow battery" || bad "zc borrow battery" "see $TMPDIR/gate-zc.txt"
stop
row_begin "zc fired (zc_sends=N)"
ZS=$(shutdown_value wb.zc_sends)
[ -n "$ZS" ] && [ "$ZS" -gt 0 ] && ok "zc fired (zc_sends=$ZS)" || bad "zc fired"
zcboot $ASAN || bad "zc ASAN boot"
row_begin "zc borrow battery under ASAN"
py tests/zc.py 127.0.0.1 $PORT >$TMPDIR/gate-zc-asan.txt 2>&1     && ok "zc borrow battery under ASAN" || bad "zc borrow battery under ASAN"
stop
row_begin "zc ASAN clean"
if grep -q "ERROR: AddressSanitizer" "$SRVLOG"; then bad "zc ASAN clean" "see $SRVLOG"
elif shutdown_present; then ok "zc ASAN clean"
else bad "zc ASAN clean" "ASAN server never reached its shutdown dump; see $SRVLOG"; fi
}

job_rldbg(){
# ---- 4b-bis. full tier: the armed-write block cache's ownership laws ---------------------------
# THE ROW THAT WOULD HAVE CAUGHT THE P0 (DESIGN-P0REPLY.md). A shard's read-local retire sink names
# two structures belonging to ONE thread and protected by nothing else: the owner's single-producer
# QSBR retire ring and the owner's unlocked block-cache free list. When a shard changed owner the
# sink was rebound lazily, on the destination's own later executor pass, so for a window the new
# owner wrote through the old owner's ring and free list -- and the damage surfaced far away, as
# either KvBlockCache::put's abort or a wild free-list head in an unrelated SET.
#
# Nothing in the gate could see it. The differential matrix compares REPLIES; it never rewrites one
# key often enough to cycle a block through put -> grace -> take, and its key distribution is flat
# enough that the load balancer moves no shards at all. So this row supplies both missing halves:
# a debug build that states the ownership laws as assertions, and traffic that forces shard moves.
row_begin "read-local ownership-invariant build"
pausable taskset -c "$BUILD_CORES" tests/parbuild.sh $RLDBG "$PWD/build/gate-cache/obj-rlcachedbg" \
    "-std=c++20 -O2 -g -march=native -pthread -DTOMO_JEMALLOC -DTOMO_RL_CACHE_DEBUG -I." \
    "-ljemalloc -luring -pthread -lssl -lcrypto -lm" \
    src/main.cc src/net/tls.cc src/core/*.cc src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc 2>$TMPDIR/gate-rlcachedbg-build.txt \
    && ok "read-local ownership-invariant build" \
    || bad "read-local ownership-invariant build" "see $TMPDIR/gate-rlcachedbg-build.txt"
}

job_rlcache(){
quiet_wait
# More shards than workers permit ownership movement. The automatic balancer must move shards;
# the battery's existing positive-movement assertion remains mandatory under the derived policy.
if boot_fused $RLDBG --shards 64 --atomic 1 --read-local 1 --enable-debug-command yes; then
  # The battery carries its own three non-vacuity checks: the armed lane served reads, the block
  # cache actually held blocks, and -- the precondition for this whole class of defect -- the load
  # balancer MOVED shards during the run.
  row_begin "armed block-cache churn battery"
  py tests/rlcache_churn.py 127.0.0.1 $PORT "${GATE_RLCACHE_SECONDS:-25}" 48 \
      >$TMPDIR/gate-rlcache-churn.txt 2>&1 \
      && ok "armed block-cache churn battery" \
      || bad "armed block-cache churn battery" "see $TMPDIR/gate-rlcache-churn.txt"
  stop
  row_begin "read-local ownership invariants"
  if grep -q 'RLSINK-VIOLATION\|RLCACHE-VIOLATION\|RLRING-VIOLATION' "$SRVLOG"; then
    bad "read-local ownership invariants" "see $SRVLOG"
  elif shutdown_present; then
    ok "read-local ownership invariants"
  else
    bad "read-local ownership invariants" "server never reached its shutdown dump; see $SRVLOG"
  fi
else
  bad "armed block-cache churn battery" "boot failed; see $SRVLOG"
  bad "read-local ownership invariants" "boot failed"
fi
}

job_globcase(){
# ---- 4d. full tier: glob / scan-cursor grammar parity -----------------------------------------
# The differential matrix above generates COMMANDS, so it only reaches the glob patterns its
# generators happen to emit. This row drives the specific patterns where the five former glob copies
# disagreed with redis -- case sensitivity, [!x], reversed and high-bit ranges, unterminated classes
# -- plus the cursor and ACL LOG count grammars. It carries its own negative control: the ACL rows
# are permission-WIDENING checks, which every allow-path test passes by construction.
GLOBCASE_ORACLE_PORT=${GATE_GLOBCASE_ORACLE_PORT:-$((PORT+2))}
if boot "$CANDIDATE_BINARY"; then
  taskset -c ${GATE_DIFFER_ORACLE_CORES:-$CORES} \
      "${GATE_DIFFER_ORACLE_BIN:-$REDIS74_ROOT/src/redis-server}" \
      --port "$GLOBCASE_ORACLE_PORT" --bind 127.0.0.1 --save '' --appendonly no --dir "$(mktemp -d "$TMPDIR/glob-data.XXXXXX")" \
      >$TMPDIR/gate-globcase-oracle.txt 2>&1 &
  GLOBCASE_ORACLE=$!
  for _ in $(seq 60); do
    (exec 3<>/dev/tcp/127.0.0.1/$GLOBCASE_ORACLE_PORT) 2>/dev/null && break; sleep 0.25
  done
  row_begin "glob/scan grammar parity vs Redis 7.4"
  py tests/globcase.py 127.0.0.1 "$PORT" 127.0.0.1 "$GLOBCASE_ORACLE_PORT" \
      >$TMPDIR/gate-globcase.txt 2>&1 \
      && ok "glob/scan grammar parity vs Redis 7.4" \
      || bad "glob/scan grammar parity vs Redis 7.4" "see $TMPDIR/gate-globcase.txt"
  kill -TERM $GLOBCASE_ORACLE 2>/dev/null; wait $GLOBCASE_ORACLE 2>/dev/null; GLOBCASE_ORACLE=0
  stop
else
  bad "glob/scan grammar parity vs Redis 7.4" "target boot failed"
fi
}

# TSan complements the existing ASAN/UBSAN controls; neither replaces the other's assertions.
# All core dependencies are instrumented in an isolated cache. Linking release objects here
# would leave command/owner accesses invisible, while sharing the ASAN cache would mix runtimes.
# One compile mode across every TU also gives inline test hooks identical definitions everywhere.
# These builds own no ledger row: the existing eight core rows and waits row require both runs.
job_core_tsan_build(){
  local source sources=()
  mkdir -p "$RUN_DIR/unit-ready"
  for source in src/net/tls.cc src/core/*.cc src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc; do
    [ "$source" = src/core/genthread.cc ] || sources+=("$source")
  done
  pausable taskset -c "$BUILD_CORES" tests/parbuild.sh "$CORE_TSAN" \
      "$PWD/build/gate-cache/obj-core-tsan" \
      '-std=c++20 -O1 -g -march=native -pthread -fsanitize=thread -fno-omit-frame-pointer -no-pie -DTOMO_CORE_CONCURRENCY_TEST -I.' \
      '-luring -pthread -lssl -lcrypto -lm' tests/core_concurrency_unit.cc "${sources[@]}" \
      >"$TMPDIR/build.log" 2>&1 && : > "$RUN_DIR/unit-ready/core-concurrency-tsan"
}
job_waits_tsan_build(){
  mkdir -p "$RUN_DIR/unit-ready"
  pausable taskset -c "$BUILD_CORES" tests/parbuild.sh "$WAITS_TSAN" \
      "$PWD/build/gate-cache/obj-waits-tsan" \
      '-std=c++20 -O1 -g -march=native -pthread -fsanitize=thread -fno-omit-frame-pointer -no-pie -I.' \
      '-pthread' tests/waits_unit.cc >"$TMPDIR/build.log" 2>&1 \
      && : > "$RUN_DIR/unit-ready/waits-unit-tsan"
}
tsan_unit(){
  local binary=$1 ready=$2 witness=$3 rc log
  shift 3
  log="$TMPDIR/tsan-$ready${1:+-$1}.log"
  [ -f "$RUN_DIR/unit-ready/$ready" ] || {
    echo "TSAN FAIL: $ready build did not complete; see $RUN_DIR/jobs/*tsan_build/build.log" >&2
    return 1
  }
  # Match the existing storage-TSAN address-map workaround, and keep all runtime reports fatal.
  # Do not inherit caller suppression options. A platform/runtime failure is a red row, never skip.
  # Existing fixture arming assertions, waits alarm and core interleaving bounds are unchanged.
  TSAN_OPTIONS=halt_on_error=1:exitcode=66 timeout --foreground 60 \
      setarch x86_64 -R taskset -c "$CORES" "$binary" "$@" >"$log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ] || grep -q 'ThreadSanitizer' "$log" || ! grep -Fxq "$witness" "$log"; then
    echo "TSAN FAIL: $ready ${*:-} exit=$rc; runtime report, unavailable runtime, or missing state witness; see $log" >&2
    return 1
  fi
}

job_production_units(){
  local target
  mkdir -p "$RUN_DIR/unit-ready"
  pausable taskset -c "$BUILD_CORES" make -k -j"$BUILD_JOBS" \
      build/core-concurrency-unit build/atomic-survivors-unit build/netcmd-unit \
      build/waits-unit build/rehash-waits-unit >"$TMPDIR/build.log" 2>&1
  # -q verifies prerequisites as well as output existence: a failed compile cannot reuse a stale
  # executable. Each dependent historical row owns the failure; this helper adds no gate row.
  for target in core-concurrency-unit atomic-survivors-unit netcmd-unit waits-unit rehash-waits-unit; do
    make -q "build/$target" && : > "$RUN_DIR/unit-ready/$target"
  done
  return 0
}
unit_ready(){
  [ -f "$RUN_DIR/unit-ready/$1" ] || {
    echo "unit $1 did not build; see $RUN_DIR/jobs/production_units/build.log" >&2
    return 1
  }
}

job_dependencies(){
  local dependency
  case "$1" in
    atomic_batteries)
      # A measured 3 ms scheduling pause alone flips atomic_ryow's unchanged 24-command
      # rate assertion. Preserve the entire boot/battery chain and every assertion, but
      # finish all other gate-owned correctness work before this performance check boots.
      # JOB_NAMES is complete before dispatch and no other family depends on this one.
      for dependency in "${JOB_NAMES[@]}"; do
        [ "$dependency" = atomic_batteries ] || printf '%s\n' "$dependency"
      done;;
    release|asan|rldbg|core_tsan_build|waits_tsan_build|config_unit|flip_unit|filter_unit|ring_unit|reorder_unit|storage_units|acl_metadata|cmd_metadata|abba_selftest) ;;
    core_units) echo 'production_units core_tsan_build';;
    wait_units) echo 'production_units waits_tsan_build';;
    atomic_units|netcmd_units) echo production_units;;
    asan_batteries) echo asan;;
    zc) echo 'release asan';;
    rlcache) echo rldbg;;
    *) echo release;;
  esac
}
job_ready(){
  local dependency
  for dependency in $(job_dependencies "$1"); do
    [ -f "$RUN_DIR/jobs/$dependency/done" ] || return 1
  done
  return 0
}

# ---- 0. preflight: tools, oracle tree, strays ------------------------------------------------
MISSING=
for tool in g++ make python3 redis-cli memtier_benchmark ss taskset timeout awk setarch flock; do
  command -v "$tool" >/dev/null 2>&1 || MISSING="$MISSING $tool"
done
[ -z "$MISSING" ] || {
  echo "GATE PREFLIGHT: missing tools:$MISSING -- every row that needs them would fail for that reason alone"
  exit 2; }
ORACLE_MISSING=
for f in src/server.h src/acl.c src/commands.def; do
  [ -f "$REDIS74_ROOT/$f" ] || ORACLE_MISSING="$ORACLE_MISSING $f"
done
if [ "$TIER" = full ]; then
  for f in src/redis-server src/redis-cli; do
    [ -x "$REDIS74_ROOT/$f" ] || ORACLE_MISSING="$ORACLE_MISSING $f(executable)"
  done
fi
ORACLE_OK=1
if [ -n "$ORACLE_MISSING" ]; then
  ORACLE_OK=0
  echo "GATE PREFLIGHT: $REDIS74_ROOT is not a built vanilla Redis 7.4 checkout (missing:$ORACLE_MISSING)."
  echo "  Expected: symlink /tmp/claude-1000/redis74 -> <redis 7.4 source tree with src/ built>,"
  echo "  or export REDIS74_ROOT=/path/to/redis-7.4."
  if [ "$TIER" = full ]; then
    echo "  The full tier's differential and globcase rows cannot run against nothing; fix it and re-run."
    exit 2
  fi
  echo "  The 'generated Redis 7.4 ACL categories' row FAILS with this reason; every other row runs."
fi
STRAYS=$(pgrep -x tomokv 2>/dev/null | paste -sd, -)
[ -z "$STRAYS" ] || say "stray tomokv processes on this box (pids $STRAYS)" \
    "WARN (other lanes? they share CPU with the timed rows; the port guard covers only $PORT)"
for reserved_port in "${SLOT_PORTS[@]}"; do
  for reserved_offset in 0 1 2; do guard_port "$((reserved_port+reserved_offset))"; done
done

# ---- 1. builds (the static_asserts on sizeof(Op)/sizeof(Client) gate here) -------------------
start_workers
collect_job release

collect_job asan
collect_job config_unit

collect_job flip_unit

collect_job filter_unit

collect_job ring_unit

collect_job core_units

collect_job reorder_unit

collect_job storage_units

collect_job atomic_units

collect_job netcmd_units

collect_job acl_metadata

# ---- 2. boot matrix: deleted flags stay dead; live grammar boots ------------------------------
collect_job boot_grammar

collect_job cmd_metadata

# ---- 3. correctness: smoke + torture + RYOW on the release build ------------------------------
collect_job wait_units

collect_job readonly

collect_job release_batteries

collect_job atomic_batteries

# Whole split boots run independently; their internal battery sequence remains unchanged.
for AT in 0 1; do collect_job "feature-split-$AT"; done

for AT in 0 1; do collect_job "fused-$AT"; done

# Whole armed boots run independently; collect their rows in the original atomic-mode order.
for AT in 0 1; do collect_job "feature-armed-$AT"; done

# ---- eviction accounting on both read paths: owner-served (split) and lane-served (fused+armed) --
# tests/evict_battery.py needs one FRESH boot per section (it sets maxmemory itself and has no
# FLUSHALL). lfu proves OBJECT FREQ rises for ordinary reads and stays put for CLIENT NO-TOUCH
# reads, plus hot-set survival under pressure; lruclock ages all cohorts across one fixed 256 s
# production bucket (a bounded wait of up to 260 s per boot) before testing the read paths.
# On the armed boot both sections also assert the reads they measure were LANE-served -- a key
# kept hot only by lane reads was never touched before ExLoopT::note_local_read_access and so was
# evicted FIRST, which is the inverse of the policy the boot asked for.
for AT in 0 1; do
  for EVSEC in lfu lruclock; do
    collect_job "evict-$EVSEC-split-$AT"
    collect_job "evict-$EVSEC-armed-$AT"
  done
done

collect_job bplus

collect_job acl_recheck

collect_job sort

for AT in 0 1; do collect_job "debug-$AT"; done

collect_job script_bounds

collect_job efficiency

collect_job dump_restore

collect_job auth

for NET_IO in epoll uring; do collect_job "snapshot-$NET_IO"; done

collect_job notify

collect_job flip

collect_job flipctl
collect_job flip_saturated

collect_job atomic_floor

for NET_IO in epoll uring; do collect_job "aof-$NET_IO"; done

collect_job aof_frame

collect_job tls

# ---- A. mandatory feature matrix (35 rows, BEFORE the quick-tier exit) -----------------------
# No inherited feature defaults: feature_gate.py sets --thread-mode {1s,2s}, --read-local {0,1},
# --overlap {0,1}, --reorder {0,1}, --flip-auto {0,1} in the full 32-way product, and explicitly
# sets both values of --atomic, --key-lb, --client-lb across it. The last three rows exercise
# --shards {-1,1,16,256}, --ratio, --place, --shard-home (including empty owners), and --no-pin.
# The eight fused + flip-auto=1 cells must refuse the exact documented unsupported combination;
# all other cells must boot and fire their mechanisms. These existing refusal checks remain
# mandatory: accepting the combination or refusing for another reason is a failure. The Python
# inventory assertion refuses a missing value/product entry. See GATES.md.
for FM in 1s 2s; do
  for FR in 0 1; do for FO in 0 1; do for FQ in 0 1; do for FF in 0 1; do
    FEATURE_CELL=$FM-$FR-$FO-$FQ-$FF
    collect_job "feature-cell-$FEATURE_CELL"
  done; done; done; done
done
for FEATURE_CELL in split-home-min fused-home-max-nopin split-shards-auto; do
  collect_job "feature-cell-$FEATURE_CELL"
done

collect_job abba_selftest

if [ "$TIER" = quick ]; then
  join_workers
  phase end
  program_state "$EXPECT_QUICK"
  echo; echo "GATE(quick): $PASS ok, $FAIL FAIL (wall $((SECONDS-GATE_STARTED))s)"; [ $FAIL -eq 0 ] || exit 1; exit 0
fi

# ---- B. mandatory headline performance: ABBA vs the LAST PUSHED BINARY ------------------------
# Replaces the stored-reference tier that stood here. A stored rate goes stale the moment the
# kernel, compiler, microcode or machine changes, and this tree's reference file was pinned to a
# kernel that no longer runs -- which made every verdict it produced provisional and left the tier
# permanently UNARMED, i.e. a guaranteed red row carrying no information. The reference is now the
# last pushed build itself, measured in THIS session on THIS box, interleaved A/B/B/A, with the
# failure threshold derived from the reference's own observed spread rather than a fixed
# percentage. A missing reference is a loud SKIP that still cannot exit green.
# Rationale and the full contract: the module docstring in tests/abbagate.py.
# Reserve this canonical row position; measurement starts only after the correctness barrier.
ABBA_PREFIX_ROWS=$(wc -l < "$LEDGER")

collect_job asan_batteries

collect_job replyoff

collect_job zc

collect_job rldbg

collect_job rlcache

# ---- 4c. full tier: byte-exact differential matrix against pinned vanilla Redis 7.4 ----------
# The four atomic-mode lifetimes and mode-equivalence job use independent slots. Each lifetime
# retains its entire ordered suite/seed chain and repeats; this fold still emits one original row.
collect_job differ-split

# ---- 4c-bis. the same matrix in the ARMED-FUSED geometry ---------------------------------------
# The row above boots the target split, with the read-local lane disarmed, which is the shape the
# gate has always used -- and which structurally cannot reach the fused read-local parse and
# resolve arms at all. A defect that needs `--thread-mode fused --read-local 1` is therefore
# invisible to it, which is how a wire-visible MSETNX/EXEC divergence sat on shipped code (the
# lane that found it had to run the matrix by hand in this geometry). This row boots the same
# binary the other way and runs the same matrix; differ_gate.sh's own non-vacuity check refuses to
# report a pass unless read_local_hits actually advanced, so an accidentally-disarmed boot is a
# FAIL rather than a quiet green.
collect_job differ-armed

collect_job globcase

# Every worker has reaped its servers before this barrier returns. No build, correctness
# driver, or background server can overlap ABBA. Its row is spliced into its historical position.
join_workers
phase abba-begin
ABBA_LEDGER=$LEDGER
ABBA_TIMINGS=$TIMINGS
ABBA_PENDING=1
LEDGER="$RUN_DIR/abba.ledger"; TIMINGS="$RUN_DIR/abba.timings"; : > "$LEDGER"; : > "$TIMINGS"
ROW_T=$(date +%s.%N)
quiet_wait
# Resolve the actual parser's output option, including caller-supplied abbreviations/paths.
# Recording one explicit destination makes the receipt consume this run's result, never a glob
# that could select another lane's or a previous run's results.json.
ABBA_OUTPUT=$(python3 - "$RUN_DIR/abba" "${ABBA_ARGS[@]}" <<'PY'
from pathlib import Path
import sys
default = Path(sys.argv.pop(1))
sys.path.insert(0, 'tests')
from abbagate import parse_args
print((parse_args().output or default).resolve())
PY
) || exit 2
# Bash defers a TERM trap while waiting for a foreground external command. An explicit wait on
# our tracked background child is interruptible, so stopping the gate reaches ABBA's cleanup.
ABBA_HISTORY_CONTEXT=$(python3 tests/gate_history.py abba-context -- "${ABBA_ARGS[@]}") || exit 2
row_begin "headline ABBA vs last pushed binary" "$ABBA_HISTORY_CONTEXT"
# The row watcher is this driver's sibling, not its child. Exempt only the declared exact
# watcher identity; gate_quiet independently verifies its script, direct controller parent,
# and --pid/--parent-start arguments on every sample. All other gate children stay foreign.
ABBA_WATCH_START=missing
if read -r ABBA_WATCH_STAT < "/proc/$ROW_WATCHDOG/stat"; then
  ABBA_WATCH_STAT=${ABBA_WATCH_STAT##*) }; read -ra ABBA_WATCH_FIELDS <<< "$ABBA_WATCH_STAT"
  ABBA_WATCH_START=${ABBA_WATCH_FIELDS[19]}
fi
GATE_QUIET_WATCHDOG="$ROW_WATCHDOG:$ABBA_WATCH_START" \
  GATE_ABBA_BACKGROUND_ENVIRONMENT="${GATE_ABBA_BACKGROUND_ENVIRONMENT:-}" \
  python3 tests/abbagate.py "${ABBA_ARGS[@]}" --output "$ABBA_OUTPUT" &
ABBA_PID=$!
wait "$ABBA_PID"
ABBA_RC=$?
ABBA_PID=0
case "$ABBA_RC" in
  0) ok "headline ABBA vs last pushed binary";;
  3) bad "headline ABBA vs last pushed binary" "no trusted comparison PASS (partial diagnostic or skipped); see ABBA output";;
  *) bad "headline ABBA vs last pushed binary" "see ABBA output and results.json";;
esac

phase abba-end
publish_abba || exit 2
ROW_T=$(date +%s.%N)

# ---- 5. full tier: NIC regression cells vs pinned refs ----------------------------------------
SPD=${GATE_SCRATCH:-/tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad}
NIC_CHECKED=0
if [ -f tests/niclib.sh ] && [ -f "$SPD/procsafe.sh" ] && [ -f tests/gate_refs.txt ]; then
  # The refs are only comparable on the kernel they were measured on (the 2026-09-02 kernel
  # change shifted race geometry box-wide). A mismatch is a WARN, never a re-pin: re-pinning is
  # an owner action on the box, and the WARN is what says the -3% verdict below is provisional.
  REF_KERNEL=$(sed -n 's/^# *kernel: *//p' tests/gate_refs.txt | head -1)
  if [ -n "$REF_KERNEL" ] && [ "$REF_KERNEL" != "$(uname -r)" ]; then
    say "NIC refs kernel pin" \
        "WARN (refs pinned on '$REF_KERNEL', running $(uname -r): re-pin on the box before trusting a -3% verdict)"
  fi
  row_begin "NIC regression cells (all within -3%)"
  ( set -u
    . tests/niclib.sh; . "$SPD/procsafe.sh"
    NIC_PORT=$PORT; NIC_CLI_BIN=$SPD/bins/cli; BL_LOGDIR=$(mktemp -d)
    nic_assert_link || exit 9
    # These stored-reference cells keep their original thread, ratio, shard and load geometry.
    # Reuse the pinned CPU IDs when allowed; otherwise remap the same physical CPU counts into
    # the phase's pools. Explicit SMT remains unused here because adding it would change those
    # pinned geometries. Validate EVERY cell before booting any: a smaller budget must not run a
    # partial reference matrix or escape its allocation to reach the 32-server/64-load cell.
    python3 - "$PERF_SERVER_CORES" "$PERF_LOAD_CORES" tests/gate_refs.txt \
        > "$BL_LOGDIR/refs.tsv" <<'PY' || exit 1
import sys
from pathlib import Path
sys.path.insert(0, 'tests')
from gateplan import cpu_string, parse_cpu_range, validate_axes

try:
    pools = validate_axes(sys.argv[1], '', sys.argv[2], '')
    rows = []
    for lineno, line in enumerate(Path(sys.argv[3]).read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        fields = line.split()
        if len(fields) != 10:
            raise ValueError(f'{sys.argv[3]}:{lineno}: expected ten reference fields')
        for field, axis in ((1, 'server_cores'), (5, 'load_cores')):
            pinned = parse_cpu_range(fields[field])
            allowed = pools[axis]
            if len(allowed) < len(pinned):
                raise ValueError(f'{fields[0]} needs {len(pinned)} {axis.replace("_", " ")} '
                                 f'but the supplied measurement pool has {len(allowed)}; '
                                 'the reference geometry cannot fit this budget')
            chosen = pinned if set(pinned).issubset(allowed) else allowed[:len(pinned)]
            if chosen != pinned:
                print(f'  NIC {fields[0]} {axis}: {cpu_string(pinned)} -> {cpu_string(chosen)}; '
                      'WARN stored reference is provisional after CPU remapping '
                      '(LLC/IRQ locality may differ; no reference was re-pinned)', file=sys.stderr)
            fields[field] = cpu_string(chosen)
        rows.append('\t'.join(fields))
    if not rows:
        raise ValueError('NIC reference matrix contains no cells')
    print('\n'.join(rows))
except (ValueError, OSError) as exc:
    print(f'  NIC resource preflight FAIL: {exc}', file=sys.stderr)
    sys.exit(1)
PY
    nic_tune >/dev/null 2>&1 || true
    CPP="$CANDIDATE_BINARY"; KMAX=2000000
    run_cell(){ # name cores ratio shards pipe lg t conns ratio_rw
      nic_kill_srv $NIC_PORT || return 1
      # --protected-mode no: protected mode (vanilla-compat: no bind check) denies non-local
      # peers when no password is set — which is every NIC cell.
      NIC_SRV_CORES=$2 nic_boot "gate_$1" "$CPP" --port $NIC_PORT --bind $NIC_SRV_IP --ratio $3 --shards $4 --protected-mode no || return 1
      NIC_TO=1200 NIC_LG_CORES=$6 nic_memtier -t 16 -c 4 --pipeline=32 --ratio=1:0 --key-pattern=P:P \
          --key-minimum=1 --key-maximum=$KMAX -n allkeys -d 64 >/dev/null 2>&1
      local f=$BL_LOGDIR/g_$1.log
      NIC_TO=90 NIC_LG_CORES=$6 nic_memtier -t $7 -c $(( $8 / $7 )) --pipeline=$5 --ratio=$9 \
          --key-pattern=R:R --key-minimum=1 --key-maximum=$KMAX --test-time=15 -d 64 \
          --distinct-client-seed > "$f" 2>&1
      tr '\r' '\n' < "$f" | grep -E '^Totals' | tail -1 | awk '{print $2}'
    }
    RC=0
    while read -r name cores ratio shards pipe lg t conns rw ref; do
      case "$name" in \#*|"") continue;; esac
      if ! got=$(run_cell "$name" "$cores" "$ratio" "$shards" "$pipe" "$lg" "$t" "$conns" "$rw"); then
        printf '  regression %-28s teardown/boot/cell FAIL\n' "$name"
        RC=1
        continue
      fi
      python3 - "$name" "$got" "$ref" <<'PY' || RC=1
import sys
name, got, ref = sys.argv[1], float(sys.argv[2] or 0), float(sys.argv[3])
d = (got - ref) / ref * 100
status = "ok" if d >= -3.0 else "FAIL"
print(f"  regression {name:<28} {got/1e6:.2f}M vs ref {ref/1e6:.2f}M ({d:+.1f}%)  {status}")
sys.exit(0 if d >= -3.0 else 1)
PY
    done < "$BL_LOGDIR/refs.tsv"
    nic_kill_srv $NIC_PORT || RC=1
    exit $RC
  )
  case $? in
    0) NIC_CHECKED=1; ok "NIC regression cells (all within -3%)";;
    9) row_unwatch; ROW_ID=; say "NIC regression cells" "SKIPPED (no rig)";;
    *) NIC_CHECKED=1; bad "NIC regression cells";;
  esac
else
  say "NIC regression cells" "SKIPPED (no rig/refs)"
fi

phase end
program_state "$((EXPECT_FULL+NIC_CHECKED))"
GATE_CLEANUP_RC=0
cleanup || GATE_CLEANUP_RC=$?
RECEIPT_RC=0
if [ "$RECEIPT_REQUIRED" = 1 ]; then
  # All correctness, ABBA, optional NIC work and owned-child cleanup precede completion. The
  # receipt is an additional certification condition, not a counted row or a weakened EXPECT.
  RECEIPT_COORDINATOR_ARGS=()
  [ -z "$RECEIPT_START" ] || RECEIPT_COORDINATOR_ARGS+=(--start "$RECEIPT_START")
  python3 tests/gate_receipt.py coordinator "${RECEIPT_COORDINATOR_ARGS[@]}" \
      --run-id "$ROW_RUN_ID" --tier "$GATE_PURPOSE" --passed "$PASS" --failed "$FAIL" \
      --abba-rc "$ABBA_RC" --cleanup-rc "$GATE_CLEANUP_RC" --nic "$NIC_CHECKED" \
      --output "$RUN_DIR/gate-result.json" || RECEIPT_RC=1
  if [ -n "$RECEIPT_START" ] && [ "$RECEIPT_RC" = 0 ]; then
    # ABBA freezes its accepted standing control here, including an explicit --null-result
    # override. Re-reading a mutable default could certify or reject against another control.
    python3 tests/gate_receipt.py finish --start "$RECEIPT_START" --gate-result "$RUN_DIR/gate-result.json" \
        --ledger "$LEDGER" --observations "$ROW_HISTORY/row-observations.jsonl" \
        --abba-result "$ABBA_OUTPUT/results.json" \
        --null-result "$ABBA_OUTPUT/null-control.json" || RECEIPT_RC=1
  else
    echo "GATE RECEIPT WITHHELD: start/binding evidence was unavailable; full gate work has completed; see $RUN_DIR/receipt-begin.log" >&2
    RECEIPT_RC=1
  fi
fi
echo
echo "GATE($GATE_PURPOSE): $PASS ok, $FAIL FAIL (ABBA rc=$ABBA_RC, NIC checked=$NIC_CHECKED, cleanup rc=$GATE_CLEANUP_RC, receipt required=$RECEIPT_REQUIRED rc=$RECEIPT_RC, wall $((SECONDS-GATE_STARTED))s)"
[ "$FAIL" -eq 0 ] && [ "$GATE_CLEANUP_RC" -eq 0 ] && [ "$RECEIPT_RC" -eq 0 ] || exit 1
