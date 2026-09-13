#!/bin/bash
# PRE-PUSH GATE for tomokv-cpp (pure 2s baseline).
#
#   tests/gate.sh quick   loopback only: build (release+ASAN), footprint locks, boot
#                         matrix, smoke, torture, RYOW, atomic torn/mixed-write/window gates,
#                         shutdown invariants, counter-fired assertions, idle-loop ceiling. Runs on
#                         any machine.
#   tests/gate.sh full    quick + torture-under-ASAN + the Redis 7.4 differential matrix + NIC
#                         regression cells vs tests/gate_refs.txt (the NIC cells need the 25GbE
#                         netns rig and its scratchpad binaries/procsafe helper).
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
TIER=${1:-quick}
PORT=${GATE_PORT:-7899}
CORES=${GATE_CORES:-0-7}
NCORES=$(taskset -c "$CORES" nproc)
if [ "$NCORES" -ge 8 ]; then GATE_RATIO=6:$((NCORES-6))
else GATE_RATIO=$(((NCORES+1)/2)):$((NCORES-(NCORES+1)/2)); fi
PASS=0; FAIL=0
SRV=0; SRVLOG=/dev/null
GLOBCASE_ORACLE=0; MMPID=0
# The pinned vanilla Redis 7.4 tree (source + built src/redis-server). The ACL-category generator
# reads its SOURCE in both tiers; the differential and globcase rows boot its BINARY in the full
# tier. The preflight below says exactly what is missing instead of a traceback in row 4.
export REDIS74_ROOT=${REDIS74_ROOT:-/tmp/claude-1000/redis74}
# Wall-time bound per battery (see py()). A hung battery used to hang the whole gate; now it is a
# red row that says TIMEOUT. 900 s is ~3x the slowest battery on the gate cores. flipctl.py is
# launched with its own explicit 600 s bound (it re-rolls a non-stationary stable hold; see there).
GATE_TEST_TIMEOUT=${GATE_TEST_TIMEOUT:-900}
# A battery that records a STRICT skip (a DEBUG hook it needed was denied) fails under the gate:
# a gate row must run the arm it exists for, never turn green by skipping it (tests/_lib.py).
export TOMO_GATE_STRICT=1
# Machine-readable ledger, one line per row: verdict, seconds since the previous row, label.
# It is the runtime-distribution instrument (which rows own the 45 minutes) and, when the count
# drifts, the diff against the previous run's ledger names the row that vanished or appeared.
LEDGER=${GATE_LEDGER:-/tmp/gate-ledger-$TIER.txt}
[ -f "$LEDGER" ] && mv -f "$LEDGER" "$LEDGER.prev"
: > "$LEDGER"
ROW_T=$(date +%s.%N)
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
EXPECT_QUICK=325
EXPECT_FULL=342                 # full without the optional NIC row.
say(){ printf '  %-52s %s\n' "$1" "$2"; }
ledger(){ # verdict label -> one ledger line; the elapsed column is wall time since the last row
  local now; now=$(date +%s.%N)
  printf '%s\t%s\t%s\n' "$1" "$(awk -v a="$ROW_T" -v b="$now" 'BEGIN{printf "%.1f", b-a}')" "$2" \
      >> "$LEDGER"
  ROW_T=$now
}
ok(){ say "$1" "ok"; PASS=$((PASS+1)); ledger ok "$1"; }
bad(){ say "$1" "FAIL${2:+ ($2)}"; FAIL=$((FAIL+1)); ledger FAIL "$1"; }
ledger_labels(){ cut -f3 "$1" | sed 's/[0-9][0-9]*/N/g' | sort; }
program_state(){
  local expect=$1 actual=$((PASS+FAIL))
  if [ "$actual" -eq "$expect" ]; then
    say "PROGRAM-STATE ledger ($actual/$expect checks)" "ok"
  else
    bad "PROGRAM-STATE ledger" "$actual/$expect checks"
    if [ -f "$LEDGER.prev" ]; then
      echo "  rows that differ from the previous $TIER ledger (< only in previous run, > only now):"
      diff <(ledger_labels "$LEDGER.prev") <(ledger_labels "$LEDGER") | grep '^[<>]' | sed 's/^/    /'
    fi
  fi
  echo "  ledger: $LEDGER (verdict / seconds since previous row / label); slowest rows:"
  sort -t "$(printf '\t')" -k2,2 -rn "$LEDGER" | head -12 \
      | awk -F '\t' '{printf "    %7.1fs  %-4s %s\n", $2, $1, $3}'
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
  local t0 now stopped=0
  t0=$(date +%s.%N)
  if [ "$SRV" -gt 0 ] 2>/dev/null && kill -0 "$SRV" 2>/dev/null; then
    kill -STOP "$SRV" 2>/dev/null && stopped=1
  fi
  quiet_note "quiet: waiting for $QUIET_FILE to be >$QUIET_MIN min old" \
      "($([ "$stopped" = 1 ] && echo "server $SRV stopped" || echo "no server up"))"
  until quiet_ok; do sleep 5; done
  [ "$stopped" = 1 ] && kill -CONT "$SRV" 2>/dev/null
  now=$(date +%s.%N)
  ROW_T=$(awk -v a="$ROW_T" -v b="$t0" -v c="$now" 'BEGIN{printf "%.9f", a + (c - b)}')
  quiet_note "quiet: resumed after $(awk -v b="$t0" -v c="$now" 'BEGIN{printf "%.0f", c - b}')s"
}
pausable(){ # run "$@" to completion (its exit status is ours); SIGSTOP its whole process group
            # whenever quiet_ok turns false and SIGCONT it when the file is back. Builds only.
  [ -n "$QUIET_FILE" ] || { "$@"; return $?; }
  quiet_wait
  local pid stopped=0 t0=0 paused=0 rc
  setsid "$@" &
  pid=$!; PAUSABLE_PID=$pid
  while kill -0 "$pid" 2>/dev/null; do
    if quiet_ok; then
      if [ "$stopped" = 1 ]; then
        kill -CONT -- "-$pid" 2>/dev/null; stopped=0
        paused=$(awk -v p="$paused" -v b="$t0" -v c="$(date +%s.%N)" \
                     'BEGIN{printf "%.3f", p + (c - b)}')
        quiet_note "quiet: build resumed"
      fi
    elif [ "$stopped" = 0 ]; then
      kill -STOP -- "-$pid" 2>/dev/null; stopped=1; t0=$(date +%s.%N)
      quiet_note "quiet: build paused until $QUIET_FILE is >$QUIET_MIN min old"
    fi
    sleep 2
  done
  wait "$pid"; rc=$?
  PAUSABLE_PID=0
  ROW_T=$(awk -v a="$ROW_T" -v p="$paused" 'BEGIN{printf "%.9f", a + p}')
  return $rc
}
py(){ # python3 battery wrapper: bounded wall time. A timeout or a self-skip (exit 3, see
      # tests/_lib.skip_all) is named in the row's log (callers redirect our stderr there) and
      # stays RED: a row that cannot run is a gate defect, not a pass. --foreground keeps the
      # battery in the terminal's process group so Ctrl-C reaches it too.
  local rc
  quiet_wait
  timeout --foreground "$GATE_TEST_TIMEOUT" python3 "$@"; rc=$?
  case $rc in
    124) echo "GATE: TIMEOUT -- battery killed after ${GATE_TEST_TIMEOUT}s" >&2;;
    3)   echo "GATE: battery SKIPPED ITSELF (exit 3): a gate row must run, not skip" >&2;;
  esac
  return $rc
}
cleanup(){ # EXIT/INT/TERM: never leave the server on $PORT, the globcase oracle, or memtier behind
  local p
  if [ "$PAUSABLE_PID" -gt 0 ] 2>/dev/null; then   # a build parked by pausable
    kill -CONT -- "-$PAUSABLE_PID" 2>/dev/null; kill -TERM -- "-$PAUSABLE_PID" 2>/dev/null
  fi
  [ "$SRV" -gt 0 ] 2>/dev/null && kill -CONT "$SRV" 2>/dev/null   # TERM needs a running target
  for p in "$SRV" "$GLOBCASE_ORACLE" "$MMPID"; do
    if [ "$p" -gt 0 ] 2>/dev/null && kill -0 "$p" 2>/dev/null; then
      kill -TERM "$p" 2>/dev/null; wait "$p" 2>/dev/null
    fi
  done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---- 0. preflight: tools, oracle tree, strays ------------------------------------------------
MISSING=
for tool in g++ make python3 redis-cli memtier_benchmark ss taskset timeout awk; do
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

# ---- 1. builds (the static_asserts on sizeof(Op)/sizeof(Client) gate here) -------------------
pausable make -j12 >/tmp/gate-build.txt 2>&1 \
    && ok "release build (+footprint locks)" || bad "release build" "see /tmp/gate-build.txt"
ASAN=/tmp/tomokv-gate-asan
pausable g++ -std=c++20 -O1 -g -fsanitize=address -march=native -pthread -I. \
    src/main.cc src/net/tls.cc src/core/*.cc src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc \
    -o $ASAN -luring -pthread -lssl -lcrypto 2>/tmp/gate-asan-build.txt \
    && ok "ASAN build" || bad "ASAN build" "see /tmp/gate-asan-build.txt"
quiet_wait
g++ -std=c++20 -O2 -I. tests/config_parser_test.cc -o /tmp/tomokv-config-parser-test \
    && /tmp/tomokv-config-parser-test \
    && ok "Redis config quoting + mid-value #" || bad "Redis config quoting + mid-value #"
# The flip-controller MODEL (fingerprint classes, ramp gate, count-noise rejection) has a unit
# test that nothing built; a model regression surfaced only in flipctl.py's live 2-5 min row.
g++ -std=c++20 -O2 -I. tests/flipctl_unit.cc src/core/flipctl.cc -o /tmp/tomokv-flipctl-unit \
    2>/tmp/gate-flipctl-unit.txt \
    && /tmp/tomokv-flipctl-unit >>/tmp/gate-flipctl-unit.txt 2>&1 \
    && ok "flip controller model unit" \
    || bad "flip controller model unit" "see /tmp/gate-flipctl-unit.txt"
g++ -std=c++20 -O2 -pthread -I. tests/foreign_read_safety_test.cc \
    -o /tmp/tomokv-foreign-read-safety-test \
    && /tmp/tomokv-foreign-read-safety-test \
    && ok "B+ counting-fingerprint filter unit" \
    || bad "B+ counting-fingerprint filter unit"
# THE RYOW WRITE RING, INCLUDING ITS ARMING TRANSIENT (DESIGN-RINGDIET.md). The connection records
# nothing until a local read arms it, so the reads that arrive with pre-arming writes still in
# flight must be DEMOTED -- and the fence that demotes them must be one-shot, or a 1:1 connection
# would never be served locally again. That is a per-frame ordering property of the ROB, so it is
# tested server-less and deterministically here rather than inferred from a live counter: the five
# directed arming cases plus a 200k-frame soak that starts UNARMED and crosses the transition, on
# top of the fourteen ring cases the sizing lane left (which this row also brings into the gate for
# the first time -- they were `make unit` only). 20 cases, one row.
g++ -std=c++20 -O2 -march=native -pthread -I. tests/read_local_write_ring_unit.cc \
    -o /tmp/tomokv-read-local-write-ring-unit 2>/tmp/gate-ring-unit.txt \
    && /tmp/tomokv-read-local-write-ring-unit >>/tmp/gate-ring-unit.txt 2>&1 \
    && ok "read-local write ring + arming transient unit" \
    || bad "read-local write ring + arming transient unit" "see /tmp/gate-ring-unit.txt"
if [ "$ORACLE_OK" = 1 ]; then
  python3 tools/gen_acl_categories.py --redis-root "$REDIS74_ROOT" \
      --check src/cmd/acl_categories_generated.h \
      && ok "generated Redis 7.4 ACL categories" || bad "generated Redis 7.4 ACL categories"
else
  bad "generated Redis 7.4 ACL categories" "oracle source tree missing at $REDIS74_ROOT"
fi

# ---- 2. boot matrix: deleted flags stay dead; live grammar boots ------------------------------
./build/tomokv --mode 3s      2>&1 | grep -q "unknown" && ok "reject --mode (flag deleted)" || bad "reject --mode (flag deleted)"
./build/tomokv --spread 4:4   2>&1 | grep -q "unknown"  && ok "reject --spread"    || bad "reject --spread"
./build/tomokv --nodes 2      2>&1 | grep -q "unknown"  && ok "reject --nodes"     || bad "reject --nodes"
./build/tomokv --ratio 4:4:2  2>&1 | grep -q "deleted"  && ok "reject 3-part ratio"|| bad "reject 3-part ratio"
./build/tomokv --conf /nonexistent-conf 2>&1 | grep -q "cannot open" && ok "reject missing conf" || bad "reject missing conf"
printf 'florb 1\n' > /tmp/gate-bad.conf
./build/tomokv /tmp/gate-bad.conf 2>&1 | grep -q "unknown argument" && ok "reject bad conf key" || bad "reject bad conf key"
printf 'aclfile /tmp/gate-users.acl\nuser alice on nopass ~* &* +@all\n' > /tmp/gate-acl-mixed.conf
./build/tomokv /tmp/gate-acl-mixed.conf 2>&1 | grep -q \
    "Configuring Redis with users defined in redis.conf and at the same setting an ACL file path is invalid" \
    && ok "reject aclfile + conf user lines" || bad "reject aclfile + conf user lines"

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
  SRVLOG=$(mktemp "/tmp/gate-srv-$tag.XXXXXX")
  taskset -c $CORES "$bin" --port $PORT --bind 127.0.0.1 --shards 16 "$@" > "$SRVLOG" 2>&1 &
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
stop(){ kill -TERM $SRV 2>/dev/null; wait $SRV 2>/dev/null; settle; }
shutdown_clean(){ python3 tests/shutdown_report.py "$SRVLOG" clean; }
shutdown_present(){ python3 tests/shutdown_report.py "$SRVLOG" present; }
shutdown_value(){ python3 tests/shutdown_report.py "$SRVLOG" get "$1"; }

# A registered command with no generated metadata row makes command_metadata_init fail, and the
# server then refuses to boot at all -- every row below goes red at once with no indication which
# command is at fault. Static, so it fires before any server starts.
py tests/cmdmeta_coverage.py >/tmp/gate-cmdmeta-coverage.txt 2>&1 \
    && ok "cmdmeta covers every registered command" \
    || bad "cmdmeta covers every registered command" "see /tmp/gate-cmdmeta-coverage.txt"

# ---- 3. correctness: smoke + torture + RYOW on the release build ------------------------------
boot ./build/tomokv --enable-debug-command yes || bad "release boot"
py tests/torture.py 127.0.0.1 $PORT >/tmp/gate-tort.txt 2>&1 \
    && ok "torture battery" || bad "torture battery" "see /tmp/gate-tort.txt"
py tests/ryow.py 127.0.0.1 $PORT >/tmp/gate-ryow.txt 2>&1 \
    && ok "RYOW battery" || bad "RYOW battery" "see /tmp/gate-ryow.txt"
py tests/acl_categories.py 127.0.0.1 $PORT >/tmp/gate-acl-categories.txt 2>&1 \
    && ok "ACL category runtime table" || bad "ACL category runtime table" "see /tmp/gate-acl-categories.txt"
py tests/acl.py 127.0.0.1 $PORT - >/tmp/gate-acl-nofile.txt 2>&1 \
    && ok "ACL LOAD/SAVE no-file errors" || bad "ACL LOAD/SAVE no-file errors" "see /tmp/gate-acl-nofile.txt"
# Idle-loop ceiling: owner-written counters remove scheduler/jiffy noise and name a spinning
# thread. The helper requires two fresh LBSIGNALS captures and a sampling iteration, so a missing
# DEBUG surface or cached/empty dump cannot turn this row green.
py tests/spinprobe.py "$PORT" "$SRV" --idle-only >/tmp/gate-idle-signals.txt 2>&1 \
    && ok "idle loop ceiling (LBSIGNALS, 1s)" \
    || bad "idle loop ceiling" "see /tmp/gate-idle-signals.txt"
stop
# shutdown invariants + fired counters, from the TERM dump
shutdown_clean \
    && ok "shutdown invariants (nothing stuck)" || bad "shutdown invariants"
D=$(shutdown_value wb.direct)
[ -n "$D" ] && [ "$D" -gt 0 ] && ok "direct-reply fired (direct=$D)" || bad "direct-reply fired"
R=$(shutdown_value work.dispatched)
E=$(shutdown_value work.executed)
[ -n "$R" ] && [ "$R" = "$E" ] && ok "dispatched==executed ($R)" || bad "dispatched==executed" "$R vs $E"

# Explicit ON boot plus the non-vacuous epoch-MVCC gates. atomic_torn includes its own OFF control,
# predecessor/promotion counters, overlapping writers, window liveness, and live CONFIG flips.
boot ./build/tomokv --atomic 1 --enable-debug-command yes || bad "atomic release boot"
py tests/atomic_torn.py 127.0.0.1 $PORT --release-build >/tmp/gate-atomic-torn.txt 2>&1 \
    && ok "atomic torn/window battery" || bad "atomic torn/window battery" "see /tmp/gate-atomic-torn.txt"
py tests/atomic_ryow.py 127.0.0.1 $PORT >/tmp/gate-atomic-ryow.txt 2>&1 \
    && ok "atomic RYOW/mixed-write battery" || bad "atomic RYOW/mixed-write battery" "see /tmp/gate-atomic-ryow.txt"
py tests/atomic_hazards.py 127.0.0.1 $PORT >/tmp/gate-atomic-hazards.txt 2>&1 \
    && ok "atomic owner-local hazard battery" \
    || bad "atomic owner-local hazard battery" "see /tmp/gate-atomic-hazards.txt"
stop
shutdown_clean \
    && ok "atomic shutdown invariants" || bad "atomic shutdown invariants"

# ---- feature batteries: every shipped feature's directed test, BOTH atomic settings -----------
# The gate accumulates a section per landed feature (owner rule). Each test is directed and
# asserts its own mechanisms fired; the boot covers multi/blocking/pubsub+sharded/lua/limits.
# ONE list, shared with the fused+armed leg below, so the two legs cannot drift apart: a battery
# added here runs on the armed lane too, and the ledger arithmetic counts it twice per atomic mode.
FEATURE_BATTERIES="s6 multi_exec blocking blockmulti stream streamgroups pubsub lua_scripting scriptsurf limits resp3 bitfield dumprestore zsetops geo climon climon2 tracking hexpire servertail lcs concur edgeproto edgeenc edgetime arity contarity cmdgap aclsel expwide infofix pushtear"
for AT in 0 1; do
  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
      || bad "feature battery boot (atomic $AT)"
  for t in $FEATURE_BATTERIES; do
    FEATURE_ARGS=()
    [ "$t" = stream ] && FEATURE_ARGS+=(--release-build)
    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-$t-$AT.txt 2>&1 \
        && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see /tmp/gate-$t-$AT.txt"
  done
  stop
  shutdown_clean \
      && ok "feature shutdown invariants (atomic $AT)" || bad "feature shutdown invariants (atomic $AT)"
done

# ---- FUSED mode: one boot per atomic mode, coarse three-stream production subset --------------
for AT in 0 1; do
  if boot_fused ./build/tomokv --atomic "$AT" --enable-debug-command yes; then
    FUSED_INFO=$(redis-cli -h 127.0.0.1 -p "$PORT" INFO server 2>/dev/null | tr -d '\r')
    FUSED_MODE=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^thread_mode://p')
    FUSED_OVERLAP=$(printf '%s\n' "$FUSED_INFO" | sed -n 's/^x_overlap://p')
    [ "$FUSED_MODE" = 1s ] && [ "$FUSED_OVERLAP" = 0 ] \
        && ok "fused boot line (atomic $AT)" \
        || bad "fused boot line (atomic $AT)" "wire mode=$FUSED_MODE overlap=$FUSED_OVERLAP"
  else
    bad "fused boot line (atomic $AT)" "server did not boot; see $SRVLOG"
  fi
  for t in s6 multi_exec edgeproto atomfix; do
    python3 "tests/$t.py" 127.0.0.1 "$PORT" >"/tmp/gate-fused-$t-$AT.txt" 2>&1 \
        && ok "fused $t battery (atomic $AT)" \
        || bad "fused $t battery (atomic $AT)" "see /tmp/gate-fused-$t-$AT.txt"
  done
  py tests/spinprobe.py "$PORT" "$SRV" >"/tmp/gate-fused-spinprobe-$AT.txt" 2>&1 \
      && ok "fused spinprobe battery (atomic $AT)" \
      || bad "fused spinprobe battery (atomic $AT)" \
             "see /tmp/gate-fused-spinprobe-$AT.txt"
  stop
  FUSED_REPORT_MODE=$(shutdown_value thread_mode)
  FUSED_REPORT_KIND=$(shutdown_value work.kind)
  shutdown_clean && [ "$FUSED_REPORT_MODE" = 1s ] && [ "$FUSED_REPORT_KIND" = fused ] \
      && ok "fused shutdown report (atomic $AT)" \
      || bad "fused shutdown report (atomic $AT)" \
             "mode=$FUSED_REPORT_MODE kind=$FUSED_REPORT_KIND; see $SRVLOG"
done

# ---- FUSED + read-local ARMED: the recommended read-heavy posture runs the whole feature list ----
# The fused boots above never arm the local read lane, so until this leg no gate row had run a
# feature battery on the path that serves every clean GET/MGET in that posture. The boot row asserts
# the WIRE says so: INFO server thread_mode:1s AND read_local:1 -- the effective lane state, not the
# knob, which CONFIG GET also echoes on a split boot where it is inert. The shutdown row is the
# leg's vacuity guard: the lane's own hit counters must have moved during the batteries, or the 32
# rows between ran on the owner path and proved nothing about the armed one.
for AT in 0 1; do
  if boot_fused ./build/tomokv --atomic "$AT" --read-local 1 --enable-debug-command yes; then
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
    py tests/$t.py 127.0.0.1 $PORT "${FEATURE_ARGS[@]}" >/tmp/gate-fusedarmed-$t-$AT.txt 2>&1 \
        && ok "fused+armed $t battery (atomic $AT)" \
        || bad "fused+armed $t battery (atomic $AT)" "see /tmp/gate-fusedarmed-$t-$AT.txt"
  done
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
done

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
    boot ./build/tomokv --atomic $AT \
        || bad "eviction $EVSEC boot (split, atomic $AT)"
    py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-split-$AT.txt 2>&1 \
        && ok "eviction $EVSEC battery (split, atomic $AT)" \
        || bad "eviction $EVSEC battery (split, atomic $AT)" "see /tmp/gate-evict-$EVSEC-split-$AT.txt"
    stop
    boot_fused ./build/tomokv --atomic $AT --read-local 1 \
        || bad "eviction $EVSEC boot (fused+armed, atomic $AT)"
    py tests/evict_battery.py $PORT $EVSEC >/tmp/gate-evict-$EVSEC-armed-$AT.txt 2>&1 \
        && ok "eviction $EVSEC battery (fused+armed, atomic $AT)" \
        || bad "eviction $EVSEC battery (fused+armed, atomic $AT)" "see /tmp/gate-evict-$EVSEC-armed-$AT.txt"
    stop
  done
done

# ---- B+ pending-atomic filter: negative keys stay local, touched keys lower whole commands -----
# DEBUG geometry makes false positives impossible in these arms: A/B share one physical shard but
# use distinct filter cells, as do P/C on the other participating owner. ATOMIC-COMMIT-DELAY holds
# A/P after raw install and atomic_commit_holds proves the reserved/unpublished window was sampled.
boot_fused ./build/tomokv --atomic 1 --read-local 1 \
    --enable-debug-command yes \
    || bad "B+ read-local purpose boot"
py tests/bplus.py 127.0.0.1 "$PORT" >/tmp/gate-bplus.txt 2>&1 \
    && ok "B+ held-group GET/MGET filter battery" \
    || bad "B+ held-group GET/MGET filter battery" "see /tmp/gate-bplus.txt"
# Lane admission on the same armed boot: 32 connections per fused thread each pipelining 64 GETs
# oversubscribe the 1024-entry lane; the excess must be deferred and re-parsed locally (counters
# fire), never demoted to an owner task (fallback_lane_full stays 0), with order/RYOW intact.
py tests/read_local_lane.py 127.0.0.1 "$PORT" >/tmp/gate-read-local-lane.txt 2>&1 \
    && ok "read-local lane admission battery" \
    || bad "read-local lane admission battery" "see /tmp/gate-read-local-lane.txt"
stop

# ---- ACL recheck over a CODED reply: exactly one reply per blocking command -------------------
# A blocking command's reply is discarded and replaced when the live ACL denies it at retire. A
# timeout answers "*-1" / "_", which are ReplyCode-carried, so a discard that clears only the byte
# buffer leaves the code standing and puts TWO replies on the wire. Its own boot in each thread
# mode: the battery creates an ACL user, and a live non-default user makes acl_active() true for
# everything else sharing the boot. Verified discriminating -- with Op::clear_reply() reverted to
# op.reply.clear() the four timeout rows fail and the rest pass.
boot ./build/tomokv --enable-debug-command yes || bad "ACL-recheck reply boot (2s)"
py tests/aclreply.py 127.0.0.1 "$PORT" >/tmp/gate-aclreply-2s.txt 2>&1 \
    && ok "ACL recheck one-reply battery (2s)" \
    || bad "ACL recheck one-reply battery (2s)" "see /tmp/gate-aclreply-2s.txt"
stop
boot_fused ./build/tomokv --enable-debug-command yes || bad "ACL-recheck reply boot (1s)"
py tests/aclreply.py 127.0.0.1 "$PORT" >/tmp/gate-aclreply-1s.txt 2>&1 \
    && ok "ACL recheck one-reply battery (1s)" \
    || bad "ACL recheck one-reply battery (1s)" "see /tmp/gate-aclreply-1s.txt"
stop

# ---- SORT's dynamic keys: exact production gate geometry, both atomic modes -------------------
# tests/sort.py rejects any boot other than 16 shards at 6:2, buckets candidate names with
# DEBUG SHARD, and requires concrete BY and GET keys plus STORE destination on the executor slot
# opposite the source. No same-owner or one-executor run can satisfy this row.
for AT in 0 1; do
  boot ./build/tomokv --ratio 6:2 --atomic "$AT" --enable-debug-command yes \
      || bad "cross-owner SORT boot (atomic $AT)"
  py tests/sort.py 127.0.0.1 "$PORT" >/tmp/gate-sort-$AT.txt 2>&1 \
      && ok "cross-owner SORT battery (atomic $AT)" \
      || bad "cross-owner SORT battery (atomic $AT)" "see /tmp/gate-sort-$AT.txt"
  stop
done

# ---- debug-surface batteries: these drive DEBUG subcommands, hence their own armed boot -------
for AT in 0 1; do
  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
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
    py tests/$t.py 127.0.0.1 $PORT >/tmp/gate-$t-$AT.txt 2>&1 \
        && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see /tmp/gate-$t-$AT.txt"
  done
  stop
done

# ---- cross-owner script bounds: production auto budgets and four cut slots per IO ----------
for XS_MODE in limit window; do
  boot ./build/tomokv --atomic 1 --enable-debug-command yes \
      || bad "xscript $XS_MODE control boot"
  py tests/xscript.py 127.0.0.1 $PORT "$XS_MODE" >/tmp/gate-xscript-$XS_MODE.txt 2>&1 \
      && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
             "see /tmp/gate-xscript-$XS_MODE.txt"
  stop
done

# ---- efficiency guards: each needs a boot geometry of its own, so each gets its own server ----
# Exact mechanism checks run on every build; timing budgets require the explicit release-build
# flag. A ratio of wall times is still a performance claim (see DESIGN-GATEHYGIENE.md).
# --shards 1 for the same reason as the borrow guard: the sidecar under test is per shard, and the
# battery asserts that precondition rather than quietly measuring a diluted one.
boot ./build/tomokv --shards 1 --enable-debug-command yes || bad "expire-index guard boot"
py tests/expireindex.py 127.0.0.1 $PORT --release-build >/tmp/gate-expireindex.txt 2>&1 \
    && ok "expire-index growth bound" || bad "expire-index growth bound" "see /tmp/gate-expireindex.txt"
stop

# one shard so every borrow lands in ONE registry (the quantity under test), and a small zc-min so
# an ordinary-sized value still takes the borrow path and pays registry cost.
boot ./build/tomokv --shards 1 --zc-min 64 --client-output-buffer-limit "normal 0 0 0" \
    --enable-debug-command yes || bad "borrow-registry guard boot"
py tests/borrow_registry.py 127.0.0.1 $PORT --release-build >/tmp/gate-borrow.txt 2>&1 \
    && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
stop

# Dispatch scaling's manual ownership geometry is retired; see MERGE.md and DESIGN-KNOBS.md.

# ---- Redis-wire DUMP/RESTORE survives the native snapshot/restart boundary -------------------
DUMPRESTORE_DIR=$(mktemp -d /tmp/gate-dumprestore.XXXXXX)
boot ./build/tomokv --atomic 1 --dir "$DUMPRESTORE_DIR" --dbfilename dumprestore.tomo \
    || bad "DUMP/RESTORE restart preparation boot"
py tests/dumprestore.py 127.0.0.1 $PORT prepare_restart \
    >/tmp/gate-dumprestore-restart.txt 2>&1 \
    && ok "DUMP/RESTORE prepare + native SAVE" \
    || bad "DUMP/RESTORE restart preparation" "see /tmp/gate-dumprestore-restart.txt"
stop
boot ./build/tomokv --atomic 1 --dir "$DUMPRESTORE_DIR" \
    --load "$DUMPRESTORE_DIR/dumprestore.tomo" \
    || bad "DUMP/RESTORE snapshot reload boot"
py tests/dumprestore.py 127.0.0.1 $PORT verify_restart \
    >>/tmp/gate-dumprestore-restart.txt 2>&1 \
    && ok "DUMP/RESTORE cross-restart round-trip" \
    || bad "DUMP/RESTORE cross-restart round-trip" "see /tmp/gate-dumprestore-restart.txt"
stop
shutdown_clean \
    && ok "DUMP/RESTORE restart shutdown invariants" \
    || bad "DUMP/RESTORE restart shutdown invariants"

# ---- auth + audit DEBUG (purpose-booted; each test asserts its gate actually opened) -----------
./build/tomokv --protected-mode maybe 2>&1 | grep -q "protected-mode wants" \
    && ok "reject bad protected-mode" || bad "reject bad protected-mode"
./build/tomokv --enable-debug-command maybe 2>&1 | grep -q "enable-debug-command wants" \
    && ok "reject bad enable-debug-command" || bad "reject bad enable-debug-command"
boot ./build/tomokv --requirepass gatepass || bad "auth purpose boot"
py tests/auth.py 127.0.0.1 $PORT gatepass >/tmp/gate-auth.txt 2>&1 \
    && ok "AUTH/HELLO/protected state machine" || bad "AUTH/HELLO/protected state machine" "see /tmp/gate-auth.txt"
stop
ACL_DIR=$(mktemp -d /tmp/gate-acl.XXXXXX)
ACL_FILE="$ACL_DIR/users.acl"
: > "$ACL_FILE"
boot ./build/tomokv --aclfile "$ACL_FILE" || bad "ACL purpose boot"
py tests/acl.py 127.0.0.1 $PORT "$ACL_FILE" >/tmp/gate-acl.txt 2>&1 \
    && ok "ACL battery (atomic off)" || bad "ACL battery (atomic off)" "see /tmp/gate-acl.txt"
stop
ACL_ATOMIC_FILE="$ACL_DIR/users-atomic.acl"
: > "$ACL_ATOMIC_FILE"
boot ./build/tomokv --aclfile "$ACL_ATOMIC_FILE" --atomic 1 || bad "ACL atomic purpose boot"
py tests/acl.py 127.0.0.1 $PORT "$ACL_ATOMIC_FILE" >/tmp/gate-acl-atomic.txt 2>&1 \
    && ok "ACL battery (atomic on)" || bad "ACL battery (atomic on)" "see /tmp/gate-acl-atomic.txt"
stop
DEBUG_DIR=$(mktemp -d /tmp/gate-debug.XXXXXX)
boot ./build/tomokv --enable-debug-command local --dir "$DEBUG_DIR" --dbfilename reload.tomo \
    || bad "DEBUG purpose boot"
py tests/debug.py 127.0.0.1 $PORT >/tmp/gate-debug.txt 2>&1 \
    && ok "DEBUG toggle/reload battery" || bad "DEBUG toggle/reload battery" "see /tmp/gate-debug.txt"
{ redis_cli_expect_ok FLUSHALL \
    && py tests/snap_typed_roundtrip.py $PORT build_save \
    && redis_cli_expect_ok DEBUG RELOAD \
    && py tests/snap_typed_roundtrip.py $PORT verify; } \
    >/tmp/gate-snap-typed.txt 2>&1 \
    && ok "typed snapshot round-trip incl stream" \
    || bad "typed snapshot round-trip incl stream" "see /tmp/gate-snap-typed.txt"
stop

# ---- snapshot data/sync engines: cut, typed round-trip, and typed preimage race ----------------
for NET_IO in epoll uring; do
  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${NET_IO}.XXXXXX")
  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
      --dir "$SNAP_DIR" --dbfilename cut.tomo \
      || bad "snapshot cut boot ($NET_IO)"
  py tests/snap_cut_battery.py "$PORT" save \
      >"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
      && ok "snapshot concurrent cut ($NET_IO)" \
      || bad "snapshot concurrent cut ($NET_IO)" \
             "see /tmp/gate-snapshot-cut-${NET_IO}.txt"
  stop
  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
      --dir "$SNAP_DIR" --load "$SNAP_DIR/cut.tomo" \
      || bad "snapshot cut reload boot ($NET_IO)"
  py tests/snap_cut_battery.py "$PORT" verify_cut \
      >>"/tmp/gate-snapshot-cut-${NET_IO}.txt" 2>&1 \
      && ok "snapshot cut reload ($NET_IO)" \
      || bad "snapshot cut reload ($NET_IO)" \
             "see /tmp/gate-snapshot-cut-${NET_IO}.txt"
  stop

  # The cut battery above writes only single keys, so it passes on a tree whose snapshot tears
  # every cross-shard atomic group. These two arms are the ones that can tell the difference:
  # generation-tagged groups, a live MGET reader that must stay clean throughout, and the
  # cuts_waited counter that separates a real group drain from a vacuous one. Both arms were
  # confirmed to FAIL on the pre-fix tree (44-53 and 10-13 torn groups per cut respectively).
  GROUP_DIR=$(mktemp -d "/tmp/gate-snapshot-groups-${NET_IO}.XXXXXX")
  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" --atomic 1 \
      --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
      || bad "atomic group cut boot ($NET_IO, atomic 1)"
  py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" mset 5 \
      >"/tmp/gate-snapshot-groups-mset-${NET_IO}.txt" 2>&1 \
      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-mset-${NET_IO}.txt" \
      && ok "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
      || bad "snapshot never tears a cross-shard MSET group ($NET_IO, atomic 1)" \
             "see /tmp/gate-snapshot-groups-mset-${NET_IO}.txt"
  stop
  # CONTROL ARM: the DEFAULT --atomic 0. EXEC force-admits a group at either setting, so a
  # transaction is atomic to readers here too and the file must agree with them.
  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" --atomic 0 \
      --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
      || bad "atomic group cut boot ($NET_IO, atomic 0)"
  py tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" exec 3 \
      >"/tmp/gate-snapshot-groups-exec-${NET_IO}.txt" 2>&1 \
      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-exec-${NET_IO}.txt" \
      && ok "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
      || bad "snapshot never tears a MULTI/EXEC group ($NET_IO, default atomic 0)" \
             "see /tmp/gate-snapshot-groups-exec-${NET_IO}.txt"
  stop

  TYPED_DIR=$(mktemp -d "/tmp/gate-snapshot-typed-${NET_IO}.XXXXXX")
  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
      --dir "$TYPED_DIR" --dbfilename typed.tomo \
      || bad "typed snapshot boot ($NET_IO)"
  py tests/snap_typed_roundtrip.py "$PORT" build_save \
      >"/tmp/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
      && ok "typed snapshot save ($NET_IO)" \
      || bad "typed snapshot save ($NET_IO)" \
             "see /tmp/gate-snapshot-typed-${NET_IO}.txt"
  stop
  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
      --dir "$TYPED_DIR" --load "$TYPED_DIR/typed.tomo" \
      || bad "typed snapshot reload boot ($NET_IO)"
  py tests/snap_typed_roundtrip.py "$PORT" verify \
      >>"/tmp/gate-snapshot-typed-${NET_IO}.txt" 2>&1 \
      && ok "typed snapshot reload ($NET_IO)" \
      || bad "typed snapshot reload ($NET_IO)" \
             "see /tmp/gate-snapshot-typed-${NET_IO}.txt"
  stop

  RACE_DIR=$(mktemp -d "/tmp/gate-snapshot-race-${NET_IO}.XXXXXX")
  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
      --dir "$RACE_DIR" --dbfilename race.tomo --save '' --enable-debug-command yes \
      || bad "typed snapshot race boot ($NET_IO)"
  py tests/snap_typed_race.py "$PORT" race "$RACE_DIR/race.tomo" \
      >"/tmp/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
      && grep -q 'PREIMAGE-FIRED PASS' "/tmp/gate-snapshot-race-${NET_IO}.txt" \
      && grep -q 'SNAPSHOT-TICKET-ORACLE PASS' "/tmp/gate-snapshot-race-${NET_IO}.txt" \
      && ok "typed snapshot preimage race ($NET_IO)" \
      || bad "typed snapshot preimage race ($NET_IO)" \
             "see /tmp/gate-snapshot-race-${NET_IO}.txt"
  stop
  boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
      --dir "$RACE_DIR" --load "$RACE_DIR/race.tomo.cut" \
      || bad "typed snapshot race reload boot ($NET_IO)"
  py tests/snap_typed_race.py "$PORT" verify "$RACE_DIR/race.tomo.oracle.json" \
      >>"/tmp/gate-snapshot-race-${NET_IO}.txt" 2>&1 \
      && ok "typed snapshot race reload ($NET_IO)" \
      || bad "typed snapshot race reload ($NET_IO)" \
             "see /tmp/gate-snapshot-race-${NET_IO}.txt"
  stop
done

# ---- notify lane: integrated owner/retire seams plus both live atomic settings -----------------
boot ./build/tomokv --notify-keyspace-events KEAmn --enable-debug-command yes \
    || bad "feature battery boot + notify CLI knob"   # armed: multi_exec.py needs DEBUG SHARD
                                                       # to locate a same-owner key pair, and it
                                                       # FAILS rather than skips without it
py tests/multi_exec.py 127.0.0.1 $PORT >/tmp/gate-multi.txt 2>&1 \
    && ok "MULTI feature battery" || bad "MULTI feature battery" "see /tmp/gate-multi.txt"
py tests/blocking.py 127.0.0.1 $PORT >/tmp/gate-blocking.txt 2>&1 \
    && ok "blocking feature battery" || bad "blocking feature battery" "see /tmp/gate-blocking.txt"
py tests/pubsub.py 127.0.0.1 $PORT >/tmp/gate-pubsub.txt 2>&1 \
    && ok "pubsub feature battery" || bad "pubsub feature battery" "see /tmp/gate-pubsub.txt"
py tests/lua_scripting.py 127.0.0.1 $PORT >/tmp/gate-lua.txt 2>&1 \
    && ok "Lua feature battery" || bad "Lua feature battery" "see /tmp/gate-lua.txt"
py tests/notify.py 127.0.0.1 $PORT >/tmp/gate-notify.txt 2>&1 \
    && ok "keyspace notification battery (atomic 0/1)" \
    || bad "keyspace notification battery" "see /tmp/gate-notify.txt"
stop
shutdown_clean \
    && ok "feature battery shutdown invariants" || bad "feature battery shutdown invariants"

# ---- FLIP lane: runtime thread reshaping is a shipped feature and gates like one ---------------
# Four rows, each guarding a failure class that actually shipped once:
#   flip.py            the 542-check state battery (incl. same-split no-op moves NOTHING)
#   flip_under_load    value-verified traffic + flips: no drops, no BUSY, no regressions
#   flip_ttl           TTL state AND expiry events survive the shard/client moves a flip performs
#   saturated flips    a one-shot quiesce snapshot once refused ~every flip under 512-conn
#                      pipelined load while 8-conn tests sailed through; this row flips under
#                      real saturation and asserts every one APPLIES (live split == requested)
boot ./build/tomokv --enable-debug-command yes || bad "flip battery boot"
py tests/flip.py 127.0.0.1 $PORT >/tmp/gate-flip.txt 2>&1 \
    && ok "FLIP state battery" || bad "FLIP state battery" "see /tmp/gate-flip.txt"
py tests/flip_under_load.py 127.0.0.1 $PORT 20 >/tmp/gate-flip-load.txt 2>&1 \
    && ok "FLIP under verified load" || bad "FLIP under verified load" "see /tmp/gate-flip-load.txt"
py tests/flip_ttl.py 127.0.0.1 $PORT >/tmp/gate-flip-ttl.txt 2>&1 \
    && ok "FLIP TTL + expiry events" || bad "FLIP TTL + expiry events" "see /tmp/gate-flip-ttl.txt"
py tests/spinprobe.py $PORT "$SRV" >/tmp/gate-spinprobe.txt 2>&1 \
    && ok "partial-frame conn parks (no io spin)" || bad "partial-frame conn parks (no io spin)" "see /tmp/gate-spinprobe.txt"
stop
boot ./build/tomokv --ratio 6:2 --atomic 0 --enable-debug-command yes --flip-auto 1 \
     || bad "flipctl boot"
# 300 -> 600 s: the stable-hold row now MEASURES its precondition (the driver's own command rate
# must be stationary by the controller's own band) before its assertion window opens, and re-rolls
# a hold whose load left that band up to three times inside its own wall budget before failing it.
# The typical run is unchanged; the worst case adds the re-rolls plus their re-anchor waits.
timeout 600 python3 tests/flipctl.py --host 127.0.0.1 --port $PORT --stable-seconds 30 \
    >/tmp/gate-flipctl.txt 2>&1 \
    && ok "flip controller: ramp gate, hold, surge + mix re-maneuvers" \
    || bad "flip controller: ramp gate, hold, surge + mix re-maneuvers" \
           "see /tmp/gate-flipctl.txt"
stop
boot ./build/tomokv --enable-debug-command yes || bad "flip battery reboot"
quiet_wait
(
  taskset -c "$CORES" memtier_benchmark -s 127.0.0.1 -p $PORT --protocol=redis -t 8 -c 32 \
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
    { [ "$OUT" = "OK" ] && [ "$LIVE" = "$TIO" ]; } || { SATOK=0; echo "flip $TIO refused/missed: '$OUT' live=$LIVE" >>/tmp/gate-flip-sat.txt; }
  done
  kill -9 $MTPID 2>/dev/null
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
  || bad "FLIP applies under saturated load" "see /tmp/gate-flip-sat.txt"
stop
# The saturation row kill -9s its memtier, so 256 aborted connections drain at shutdown -- the
# invariant line lands a beat after stop returns. Poll instead of racing it.
FLIPSHUT=0
for _ in $(seq 50); do
  shutdown_clean \
      && { FLIPSHUT=1; break; }
  sleep 0.1
done
[ "$FLIPSHUT" = 1 ] && ok "flip battery shutdown invariants" \
    || bad "flip battery shutdown invariants"

# ---- atomic multi-key throughput floor -------------------------------------------------------
# A 9x per-op tax on the atomic chain-read path (tripwire armed by --enable-debug-command alone)
# once passed this gate 236/236: nothing here drove MGET/MSET through the resolver and asserted a
# rate. Floor = 120k on the oversubscribed gate cores; healthy measures ~600k (5x margin), the
# regression class this catches lands under 70k. Boots the harness posture on purpose -- debug
# enabled, tripwire NOT armed -- because that is the posture every bench and gate row runs in.
boot ./build/tomokv --atomic 1 --enable-debug-command yes || bad "atomic mm floor boot"
MM_K8="__key__ __key__ __key__ __key__ __key__ __key__ __key__ __key__"
MM_M8="__key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__"
quiet_wait
taskset -c "$CORES" memtier_benchmark -s 127.0.0.1 -p $PORT --protocol=redis -t 4 -c 16 \
  --pipeline=16 --command="MGET $MM_K8" --command-ratio=9 --command-key-pattern=R \
  --command="MSET $MM_M8" --command-ratio=1 --command-key-pattern=R -d 64 \
  --key-minimum=1 --key-maximum=200000 --test-time=20 --hide-histogram >/dev/null 2>&1 &
MMPID=$!
sleep 8
MM_C0=$(redis-cli -p $PORT info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p')
sleep 6
MM_C1=$(redis-cli -p $PORT info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p')
kill -9 $MMPID 2>/dev/null; wait $MMPID 2>/dev/null
MM_RATE=$(( (${MM_C1:-0} - ${MM_C0:-0}) / 6 ))
[ "$MM_RATE" -ge 120000 ] \
    && ok "atomic MGET/MSET floor (${MM_RATE}/s >= 120k)" \
    || bad "atomic MGET/MSET floor" "measured ${MM_RATE}/s < 120000/s"
# Same boot: the explicit arm surface answers OK both ways (observation itself is priced by the
# floor row above -- if arming ever becomes the boot default again, that row fails, not this one).
TWARM=$(redis-cli -p $PORT debug tripwire arm 2>&1)
TWDIS=$(redis-cli -p $PORT debug tripwire disarm 2>&1)
{ [ "$TWARM" = "OK" ] && [ "$TWDIS" = "OK" ]; } \
    && ok "tripwire arm/disarm round-trip" \
    || bad "tripwire arm/disarm round-trip" "arm='$TWARM' disarm='$TWDIS'"
# Same boot: pipelined same-connection program order (the seed-19 divergence). Pre-fix this
# answered stale in ~74% of iterations, so 400 iterations are a decisive non-vacuous roll.
timeout 120 python3 tests/pipeorder.py $PORT 400 >/tmp/gate-pipeorder.txt 2>&1 \
    && ok "pipelined same-conn program order (400 rolls)" \
    || bad "pipelined same-conn program order" "see /tmp/gate-pipeorder.txt"
stop

# ---- AOF boot/replay + non-vacuous DEBUG LOADAOF ---------------------------------------------
for NET_IO in epoll uring; do
for AOF_ATOMIC in 0 1; do
AOF_DIR=$(mktemp -d "/tmp/gate-aof-${NET_IO}-atomic${AOF_ATOMIC}.XXXXXX")
AOF_STATE=$AOF_DIR/state.json
boot ./build/tomokv --protected-mode no --atomic "$AOF_ATOMIC" \
    --appendonly yes --appendfsync no \
    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_DIR" \
    || bad "AOF purpose boot ($NET_IO, atomic $AOF_ATOMIC)"
py tests/knobs.py 127.0.0.1 "$PORT" "$NET_IO" "$AOF_ATOMIC" \
    >"/tmp/gate-knobs-${NET_IO}-${AOF_ATOMIC}.txt" 2>&1 \
    && ok "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)" \
    || bad "configuration reduction + actual geometry ($NET_IO, atomic $AOF_ATOMIC)"
py tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
    && py tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($NET_IO, atomic $AOF_ATOMIC)" \
           "see /tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
AOF_PRE_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
AOF_WRITTEN=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_records_written://p')
[ -n "$AOF_WRITTEN" ] && [ "$AOF_WRITTEN" -gt 0 ] \
    && ok "AOF writer fired (records=$AOF_WRITTEN)" || bad "AOF writer fired"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
settle
boot ./build/tomokv --protected-mode no --appendonly yes --appendfsync no \
    --atomic "$AOF_ATOMIC" --net-io "$NET_IO" \
    --enable-debug-command yes --dir "$AOF_DIR" \
    || bad "AOF replay boot ($NET_IO, atomic $AOF_ATOMIC)"
py tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt 2>&1 \
    && ok "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
    || bad "AOF process-restart script replay ($NET_IO, atomic $AOF_ATOMIC)" \
           "see /tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt"
AOF_POST_MODEL=$(py tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$NET_IO-$AOF_ATOMIC.txt)
[ -n "$AOF_PRE_MODEL" ] && [ "$AOF_PRE_MODEL" = "$AOF_POST_MODEL" ] \
    && ok "AOF native snapshot streams byte-exact" || bad "AOF native snapshot streams byte-exact"
AOF_REPLAYED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_replayed_records://p')
AOF_SKIPPED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_groups_skipped_on_replay://p')
[ -n "$AOF_REPLAYED" ] && [ "$AOF_REPLAYED" -gt 0 ] && [ -n "$AOF_SKIPPED" ] \
    && ok "AOF replay fired (records=$AOF_REPLAYED skipped=$AOF_SKIPPED)" || bad "AOF replay fired"
stop
done

# ---- AOF atomic-group bracketing + directed interrupted-process recovery ---------------------
AOF_GROUP_DIR=$(mktemp -d "/tmp/gate-aof-group-${NET_IO}.XXXXXX")
AOF_GROUP_STATE=$AOF_GROUP_DIR/state.json
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
    || bad "AOF group purpose boot ($NET_IO)"
py tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
    >/tmp/gate-aof-group.txt 2>&1 \
    && ok "AOF directed group interruption fired" \
    || bad "AOF directed group interruption" "see /tmp/gate-aof-group.txt"
wait $SRV 2>/dev/null
settle
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
    --net-io "$NET_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
    || bad "AOF group recovery boot ($NET_IO)"
py tests/aof_torn_group.py 127.0.0.1 $PORT verify "$AOF_GROUP_STATE" \
    >>/tmp/gate-aof-group.txt 2>&1 \
    && py tests/aof_torn_group.py 127.0.0.1 $PORT scan \
       "$AOF_GROUP_DIR/appendonlydir/appendonly.aof.1.incr.tomo" \
       >>/tmp/gate-aof-group.txt 2>&1 \
    && ok "AOF atomic-group recovery + writer order" \
    || bad "AOF atomic-group recovery + writer order" "see /tmp/gate-aof-group.txt"
stop
shutdown_clean \
    && ok "AOF group shutdown invariants" || bad "AOF group shutdown invariants"

# ---- AOF sync policies, reply gate, idle sync, and durability-window recovery ----------------
AOF_ALWAYS_DIR=$(mktemp -d "/tmp/gate-aof-always-${NET_IO}.XXXXXX")
AOF_ALWAYS_STATE=$AOF_ALWAYS_DIR/state.json
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
    || bad "AOF always purpose boot ($NET_IO)"
py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_ALWAYS_STATE" always 512 \
    >/tmp/gate-aof-always.txt 2>&1 \
    && ok "AOF always sync + reply gate fired" \
    || bad "AOF always sync + reply gate" "see /tmp/gate-aof-always.txt"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
settle
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
    --net-io "$NET_IO" --dir "$AOF_ALWAYS_DIR" \
    || bad "AOF always recovery boot ($NET_IO)"
py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_ALWAYS_STATE" always 512 \
    >>/tmp/gate-aof-always.txt 2>&1 \
    && ok "AOF always acknowledged-prefix recovery" \
    || bad "AOF always acknowledged-prefix recovery" "see /tmp/gate-aof-always.txt"
stop

AOF_EVERY_DIR=$(mktemp -d "/tmp/gate-aof-everysec-${NET_IO}.XXXXXX")
AOF_EVERY_STATE=$AOF_EVERY_DIR/state.json
AOF_EVERY_FILE=$AOF_EVERY_DIR/appendonlydir/appendonly.aof.1.incr.tomo
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
    || bad "AOF everysec purpose boot ($NET_IO)"
py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_EVERY_STATE" everysec 512 \
    >/tmp/gate-aof-everysec.txt 2>&1 \
    && ok "AOF everysec write gate + idle sync fired" \
    || bad "AOF everysec write gate + idle sync" "see /tmp/gate-aof-everysec.txt"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
AOF_EVERY_SIZE=$(stat -c %s "$AOF_EVERY_FILE" 2>/dev/null || echo 0)
case "$AOF_EVERY_FILE" in
  "$AOF_EVERY_DIR"/appendonlydir/*)
    [ "$AOF_EVERY_SIZE" -gt 7 ] && truncate -s $((AOF_EVERY_SIZE-7)) "$AOF_EVERY_FILE" ;;
  *) bad "AOF everysec tail target" ;;
esac
settle
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
    --net-io "$NET_IO" --dir "$AOF_EVERY_DIR" \
    || bad "AOF everysec recovery boot ($NET_IO)"
py tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
    >>/tmp/gate-aof-everysec.txt 2>&1 \
    && grep -q "AOF warning: truncated AOF tail" "$SRVLOG" \
    && ok "AOF everysec durability window + tail warning" \
    || bad "AOF everysec durability window + tail warning" "see /tmp/gate-aof-everysec.txt"
stop

AOF_NO_DIR=$(mktemp -d "/tmp/gate-aof-no-sync-${NET_IO}.XXXXXX")
boot ./build/tomokv --protected-mode no --atomic 0 --appendonly yes --appendfsync no \
    --net-io "$NET_IO" --dir "$AOF_NO_DIR" \
    || bad "AOF no-sync purpose boot ($NET_IO)"
py tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
    >/tmp/gate-aof-no-sync.txt 2>&1 \
    && ok "AOF no-sync bypassed sync + reply gate" \
    || bad "AOF no-sync bypass" "see /tmp/gate-aof-no-sync.txt"
stop

quiet_wait
NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
    >/tmp/gate-aof-rewrite.txt 2>&1 \
    && ok "AOF rewrite atomic/stage/corruption matrix" \
    || bad "AOF rewrite matrix" "see /tmp/gate-aof-rewrite.txt"

quiet_wait
NET_IO=$NET_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
    >/tmp/gate-aof-rewrite-trigger.txt 2>&1 \
    && ok "AOF rewrite triggers + observability matrix" \
    || bad "AOF rewrite triggers" "see /tmp/gate-aof-rewrite-trigger.txt"

AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${NET_IO}.XXXXXX")
boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
    --appendonly no --dir "$AOF_OFF_DIR" \
    || bad "AOF-off negative-control boot"
AOF_OFF_SEED=$(redis-cli -h 127.0.0.1 -p $PORT SET aof-negative-control must-disappear 2>&1 |
    tr -d '\r')
AOF_OFF_PRE_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\r')
[ "$AOF_OFF_SEED" = OK ] && [ "$AOF_OFF_PRE_SIZE" = 1 ] \
    && ok "AOF-off negative-control seed landed" \
    || bad "AOF-off negative-control seed" "SET=$AOF_OFF_SEED DBSIZE=$AOF_OFF_PRE_SIZE"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
settle
boot ./build/tomokv --protected-mode no --net-io "$NET_IO" \
    --appendonly no --dir "$AOF_OFF_DIR" \
    || bad "AOF-off negative-control reboot"
AOF_OFF_SIZE=$(redis-cli -h 127.0.0.1 -p $PORT DBSIZE 2>/dev/null | tr -d '\r')
[ "$AOF_OFF_SIZE" = 0 ] && [ ! -e "$AOF_OFF_DIR/appendonlydir" ] \
    && ok "AOF-off negative control lost data" || bad "AOF-off negative control"
stop
done

# ---- AOF physical framing: a control frame must never land inside a large record --------------
# A gate run's AOF replay boot exited with "AOF control record interleaves a large record": the
# writer flushed a ready GCMT at the top of a writer pass without checking that a large record
# still held the physical stream. Recovery truncates the file from that large record's first byte,
# so a control frame inside it is discardable -- and the loader refuses to start on the whole file.
# Syscall persistence under epoll: that is where the defect was demonstrated (11 of 114 runs of the AOF
# battery, 0 of 117 on uring) and where the window is entered reliably enough for the row to prove
# its mechanism fired. The battery FAILS on a build with the guard removed (6 of 6).
for AOF_FRAME_ATOMIC in 0 1; do
AOF_FRAME_DIR=$(mktemp -d "/tmp/gate-aof-frameorder-atomic${AOF_FRAME_ATOMIC}.XXXXXX")
boot ./build/tomokv --protected-mode no --atomic "$AOF_FRAME_ATOMIC" --appendonly yes \
    --appendfsync no --net-io epoll --auto-aof-rewrite-percentage 0 \
    --enable-debug-command yes --dir "$AOF_FRAME_DIR" \
    || bad "AOF frame-order purpose boot (atomic $AOF_FRAME_ATOMIC)"
py tests/aof_frame_order.py 127.0.0.1 $PORT "$AOF_FRAME_DIR/appendonlydir" \
    >/tmp/gate-aof-frameorder-$AOF_FRAME_ATOMIC.txt 2>&1 \
    && ok "AOF control frame never inside a large record (atomic $AOF_FRAME_ATOMIC)" \
    || bad "AOF control frame never inside a large record (atomic $AOF_FRAME_ATOMIC)" \
           "see /tmp/gate-aof-frameorder-$AOF_FRAME_ATOMIC.txt"
stop
done

# ---- TLS memory-BIO transport: independent listener, auth matrix, parser/teardown fences -------
TLS_PORT=$((PORT+1))
TLS_DIR=$(mktemp -d /tmp/gate-tls.XXXXXX)
py tests/tls.py --generate "$TLS_DIR" >/tmp/gate-tls-generate.txt 2>&1 \
    && ok "TLS ephemeral CA/server/client certificates" \
    || bad "TLS certificate generation" "see /tmp/gate-tls-generate.txt"

./build/tomokv --port 0 --tls-port "$TLS_PORT" --tls-key-file "$TLS_DIR/server.key" \
    --tls-auth-clients no 2>&1 | grep -q "tls-port requires tls-cert-file" \
    && ok "reject TLS listener without certificate" || bad "reject missing TLS certificate"
./build/tomokv --port 0 --tls-port "$TLS_PORT" --tls-cert-file "$TLS_DIR/server.crt" \
    --tls-key-file "$TLS_DIR/server.key" --tls-auth-clients no --tls-protocols SSLv3 \
    2>&1 | grep -q "Invalid tls-protocols" \
    && ok "reject invalid tls-protocols" || bad "reject invalid tls-protocols"
./build/tomokv --port 0 --tls-port "$TLS_PORT" --tls-cert-file "$TLS_DIR/server.crt" \
    --tls-key-file "$TLS_DIR/server.key" --tls-auth-clients no --tls-ciphers NOT-A-CIPHER \
    2>&1 | grep -q "Failed to configure tls-ciphers" \
    && ok "reject invalid tls-ciphers" || bad "reject invalid tls-ciphers"

tlsboot(){ # auth-mode [extra TLS knobs]
  local auth=$1; shift
  quiet_wait
  SRV=0; SRVLOG=/dev/null
  guard_port "$PORT"
  guard_port "$TLS_PORT"
  SRVLOG=$(mktemp /tmp/gate-tls-srv.XXXXXX)
  taskset -c $CORES ./build/tomokv --port "$PORT" --tls-port "$TLS_PORT" \
      --bind 127.0.0.1 --shards 16 --ratio "$GATE_RATIO" --protected-mode no \
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
py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" yes --plain-port "$PORT" \
    >/tmp/gate-tls-yes.txt 2>&1 \
    && ok "TLS client-auth yes matrix" || bad "TLS client-auth yes matrix" "see /tmp/gate-tls-yes.txt"
stop
shutdown_clean \
    && ok "TLS yes shutdown invariants" || bad "TLS yes shutdown invariants"
# (tls_ktls_active is a GAUGE — 0 at clean shutdown by design — so kTLS engagement is asserted
# live below, on the optional-mode boot, not from this shutdown dump.)

tlsboot optional || bad "TLS client-auth optional purpose boot"
py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" optional --plain-port "$PORT" \
    >/tmp/gate-tls-optional.txt 2>&1 \
    && ok "TLS client-auth optional matrix" \
    || bad "TLS client-auth optional matrix" "see /tmp/gate-tls-optional.txt"
# Live kTLS engagement proof on the default boot: a plain TLS client connects and
# must see itself counted in the active gauge. Client-auth 'optional' permits a cert-less client.
python3 - "$TLS_PORT" "$TLS_DIR" <<'PYEOF' >/tmp/gate-ktls-live.txt 2>&1 \
    && ok "kTLS engaged live (default boot)" || bad "kTLS engaged live" "see /tmp/gate-ktls-live.txt"
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
shutdown_clean \
    && ok "TLS optional shutdown invariants" || bad "TLS optional shutdown invariants"

# Exercise userspace fallback through the retained cipher grammar. TLS 1.2 uses CBC; TLS 1.3
# uses AES-256, outside this implementation's AES-128 TLS 1.3 RX installer. Counters must fire.
tlsboot no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers ECDHE-RSA-AES256-SHA384 \
    --tls-ciphersuites TLS_AES_256_GCM_SHA384 --tls-prefer-server-ciphers yes \
    || bad "TLS coexistence purpose boot"
py tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" no --plain-port "$PORT" --full \
    --expect-ktls no >/tmp/gate-tls-full.txt 2>&1 \
    && ok "TLS pipeline/torn-record/coexistence battery" \
    || bad "TLS correctness battery" "see /tmp/gate-tls-full.txt"
stop
shutdown_clean \
    && ok "TLS shutdown invariants" || bad "TLS shutdown invariants"
[ "$(shutdown_value wb.send_errors)" = 0 ] \
    && ok "TLS application send path error-free" || bad "TLS send errors"
TLS_ACCEPTS=$(shutdown_value tls.accepts)
TLS_FREED=$(shutdown_value tls.connections_freed)
TLS_ZC=$(shutdown_value tls.zc_suppressed)
[ -n "$TLS_ACCEPTS" ] && [ "$TLS_ACCEPTS" -gt 0 ] && [ "$TLS_ACCEPTS" = "$TLS_FREED" ] \
    && ok "TLS connection slots all freed ($TLS_FREED/$TLS_ACCEPTS)" \
    || bad "TLS connection-slot cleanup" "$TLS_FREED/$TLS_ACCEPTS"
[ -n "$TLS_ZC" ] && [ "$TLS_ZC" -gt 0 ] \
    && ok "TLS zc borrow gates fired (suppressed=$TLS_ZC)" || bad "TLS zc borrow gates fired"

if [ "$TIER" = quick ]; then
  program_state "$EXPECT_QUICK"
  echo; echo "GATE(quick): $PASS ok, $FAIL FAIL"; [ $FAIL -eq 0 ] || exit 1; exit 0
fi

# ---- 4. full tier: torture under ASAN ---------------------------------------------------------
boot $ASAN --atomic 1 --enable-debug-command yes || bad "ASAN boot"
py tests/torture.py 127.0.0.1 $PORT >/tmp/gate-tort-asan.txt 2>&1 \
    && ok "torture under ASAN" || bad "torture under ASAN"
py tests/ryow.py 127.0.0.1 $PORT >/tmp/gate-ryow-asan.txt 2>&1 \
    && ok "RYOW under ASAN" || bad "RYOW under ASAN"
# Deliberately omit --release-build: coverage/safety checks remain mandatory, including the
# derived-window witnesses; promotion timing prints without scoring its release budget.
py tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn-asan.txt 2>&1 \
    && ok "atomic torn/window under ASAN" || bad "atomic torn/window under ASAN"
# --no-rate-assertions: the battery's overlap section makes one claim about SPEED (pipelining 24
# atomic groups beats 24 serial round trips by >10%). ASAN does not slow the two arms by the same
# factor, so that ratio inverts here on a correct build -- measured pipe 21,005/s vs serial
# 24,202/s in a full-gate run whose every correctness check passed, on a build that passed 4 of 4
# standalone. The mechanism half of the same section (all 24 admitted, multiple groups observed in flight,
# so a younger cross-key group WAS in flight while an older one decided) still runs on this tier,
# and the measured rates are still printed in the log.
py tests/atomic_ryow.py 127.0.0.1 $PORT --no-rate-assertions \
    >/tmp/gate-atomic-ryow-asan.txt 2>&1 \
    && ok "atomic RYOW under ASAN" || bad "atomic RYOW under ASAN"
stop
# "No ASAN report" is a finding only if the ASAN server actually ran to its shutdown dump; an
# empty log (boot failed) has no ASAN text either and used to pass this row vacuously.
if grep -q "ERROR: AddressSanitizer" "$SRVLOG"; then bad "ASAN clean" "see $SRVLOG"
elif shutdown_present; then ok "ASAN clean"
else bad "ASAN clean" "ASAN server never reached its shutdown dump; see $SRVLOG"; fi

# ---- CLIENT REPLY OFF/SKIP + cross-shard MGET on a zero-copy boot: suppressed replies leave nothing on the
# wire and release their borrows (regression for the partial-array leak fixed on the netwb lane) ------------
for RA in 0 1; do
  boot ./build/tomokv --shards 64 --zc-min 64 --atomic $RA --enable-debug-command yes || bad "replyoff boot (atomic $RA)"
  py tests/replyoff_xshard.py 127.0.0.1 $PORT >/tmp/gate-replyoff-$RA.txt 2>&1 \
      && ok "CLIENT REPLY OFF/SKIP cross-shard MGET wire silence (atomic $RA)" \
      || bad "CLIENT REPLY OFF/SKIP cross-shard MGET wire silence (atomic $RA)" "see /tmp/gate-replyoff-$RA.txt"
  stop
done

# ---- 4b. full tier: zero-copy borrow lifetime (release+ASAN) ----------------------------------
zcboot(){
  quiet_wait
  tools/quietcheck.sh "${GATE_CORES:-192-199}" "$PORT" 2>/tmp/gate-quiet.err || {
    sleep 3
    tools/quietcheck.sh "${GATE_CORES:-192-199}" "$PORT" 2>>/tmp/gate-quiet.err || {
      echo "boot preflight: cores/port not quiet: $(tail -1 /tmp/gate-quiet.err)"; return 1; }
  }
  SRV=0; SRVLOG=/dev/null
  guard_port "$PORT"
  SRVLOG=$(mktemp /tmp/gate-srv-zc.XXXXXX)
  timeout 900 taskset -c $CORES "$1" --port $PORT --bind 127.0.0.1 --shards 16 --ratio $GATE_RATIO \
      --zc-min 16384 > "$SRVLOG" 2>&1 &
  SRV=$!
  for _ in $(seq 50); do
    if ! kill -0 "$SRV" 2>/dev/null; then wait "$SRV" 2>/dev/null; return 1; fi
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2; done
  return 1
}
zcboot ./build/tomokv || bad "zc boot"
py tests/zc.py 127.0.0.1 $PORT >/tmp/gate-zc.txt 2>&1     && ok "zc borrow battery" || bad "zc borrow battery" "see /tmp/gate-zc.txt"
stop
ZS=$(shutdown_value wb.zc_sends)
[ -n "$ZS" ] && [ "$ZS" -gt 0 ] && ok "zc fired (zc_sends=$ZS)" || bad "zc fired"
zcboot $ASAN || bad "zc ASAN boot"
py tests/zc.py 127.0.0.1 $PORT >/tmp/gate-zc-asan.txt 2>&1     && ok "zc borrow battery under ASAN" || bad "zc borrow battery under ASAN"
stop
if grep -q "ERROR: AddressSanitizer" "$SRVLOG"; then bad "zc ASAN clean" "see $SRVLOG"
elif shutdown_present; then ok "zc ASAN clean"
else bad "zc ASAN clean" "ASAN server never reached its shutdown dump; see $SRVLOG"; fi

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
RLDBG=/tmp/tomokv-gate-rlcachedbg
pausable g++ -std=c++20 -O2 -g -march=native -pthread -DTOMO_JEMALLOC -DTOMO_RL_CACHE_DEBUG -I. \
    src/main.cc src/net/tls.cc src/core/*.cc src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc \
    -o $RLDBG -ljemalloc -luring -pthread -lssl -lcrypto -lm 2>/tmp/gate-rlcachedbg-build.txt \
    && ok "read-local ownership-invariant build" \
    || bad "read-local ownership-invariant build" "see /tmp/gate-rlcachedbg-build.txt"
quiet_wait
# More shards than workers permit ownership movement. The automatic balancer must move shards;
# the battery's existing positive-movement assertion remains mandatory under the derived policy.
if boot_fused $RLDBG --shards 64 --atomic 1 --read-local 1 --enable-debug-command yes; then
  # The battery carries its own three non-vacuity checks: the armed lane served reads, the block
  # cache actually held blocks, and -- the precondition for this whole class of defect -- the load
  # balancer MOVED shards during the run.
  py tests/rlcache_churn.py 127.0.0.1 $PORT "${GATE_RLCACHE_SECONDS:-25}" 48 \
      >/tmp/gate-rlcache-churn.txt 2>&1 \
      && ok "armed block-cache churn battery" \
      || bad "armed block-cache churn battery" "see /tmp/gate-rlcache-churn.txt"
  stop
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

# ---- 4c. full tier: byte-exact differential matrix against pinned vanilla Redis 7.4 ----------
# The helper discovers the ordinary suites from differ.py's gens registry, adds the two special
# early-exit suites, and runs serially because this gate owns only one target/oracle port pair.
DIFFER_ORACLE_PORT=${GATE_DIFFER_ORACLE_PORT:-$((PORT+1))}
quiet_wait
if GATE_DIFFER_ORACLE_CORES=${GATE_DIFFER_ORACLE_CORES:-$CORES} \
    tests/differ_gate.sh ./build/tomokv "$PORT" "$DIFFER_ORACLE_PORT" "$CORES" "$GATE_RATIO"; then
  ok "Redis 7.4 differential matrix"
else
  bad "Redis 7.4 differential matrix"
fi

# ---- 4c-bis. the same matrix in the ARMED-FUSED geometry ---------------------------------------
# The row above boots the target split, with the read-local lane disarmed, which is the shape the
# gate has always used -- and which structurally cannot reach the fused read-local parse and
# resolve arms at all. A defect that needs `--thread-mode fused --read-local 1` is therefore
# invisible to it, which is how a wire-visible MSETNX/EXEC divergence sat on shipped code (the
# lane that found it had to run the matrix by hand in this geometry). This row boots the same
# binary the other way and runs the same matrix; differ_gate.sh's own non-vacuity check refuses to
# report a pass unless read_local_hits actually advanced, so an accidentally-disarmed boot is a
# FAIL rather than a quiet green.
if GATE_DIFFER_GEOMETRY=armed-fused \
    GATE_DIFFER_ORACLE_CORES=${GATE_DIFFER_ORACLE_CORES:-$CORES} \
    tests/differ_gate.sh ./build/tomokv "$PORT" "$DIFFER_ORACLE_PORT" "$CORES" "$GATE_RATIO"; then
  ok "Redis 7.4 differential matrix (armed fused + read-local)"
else
  bad "Redis 7.4 differential matrix (armed fused + read-local)"
fi

# ---- 4d. full tier: glob / scan-cursor grammar parity -----------------------------------------
# The differential matrix above generates COMMANDS, so it only reaches the glob patterns its
# generators happen to emit. This row drives the specific patterns where the five former glob copies
# disagreed with redis -- case sensitivity, [!x], reversed and high-bit ranges, unterminated classes
# -- plus the cursor and ACL LOG count grammars. It carries its own negative control: the ACL rows
# are permission-WIDENING checks, which every allow-path test passes by construction.
GLOBCASE_ORACLE_PORT=${GATE_GLOBCASE_ORACLE_PORT:-$((PORT+2))}
if boot ./build/tomokv; then
  taskset -c ${GATE_DIFFER_ORACLE_CORES:-$CORES} \
      "${GATE_DIFFER_ORACLE_BIN:-$REDIS74_ROOT/src/redis-server}" \
      --port "$GLOBCASE_ORACLE_PORT" --bind 127.0.0.1 --save '' --appendonly no \
      >/tmp/gate-globcase-oracle.txt 2>&1 &
  GLOBCASE_ORACLE=$!
  for _ in $(seq 60); do
    (exec 3<>/dev/tcp/127.0.0.1/$GLOBCASE_ORACLE_PORT) 2>/dev/null && break; sleep 0.25
  done
  py tests/globcase.py 127.0.0.1 "$PORT" 127.0.0.1 "$GLOBCASE_ORACLE_PORT" \
      >/tmp/gate-globcase.txt 2>&1 \
      && ok "glob/scan grammar parity vs Redis 7.4" \
      || bad "glob/scan grammar parity vs Redis 7.4" "see /tmp/gate-globcase.txt"
  kill -TERM $GLOBCASE_ORACLE 2>/dev/null; wait $GLOBCASE_ORACLE 2>/dev/null
  stop
else
  bad "glob/scan grammar parity vs Redis 7.4" "target boot failed"
fi

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
  ( set -u
    . tests/niclib.sh; . "$SPD/procsafe.sh"
    NIC_PORT=6380; NIC_CLI_BIN=$SPD/bins/cli; BL_LOGDIR=$(mktemp -d)
    nic_assert_link || exit 9
    nic_tune >/dev/null 2>&1 || true
    CPP=$(pwd)/build/tomokv; KMAX=2000000
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
    done < tests/gate_refs.txt
    nic_kill_srv $NIC_PORT || RC=1
    exit $RC
  )
  case $? in
    0) NIC_CHECKED=1; ok "NIC regression cells (all within -3%)";;
    9) say "NIC regression cells" "SKIPPED (no rig)";;
    *) NIC_CHECKED=1; bad "NIC regression cells";;
  esac
else
  say "NIC regression cells" "SKIPPED (no rig/refs)"
fi

program_state "$((EXPECT_FULL+NIC_CHECKED))"
echo
[ "$NIC_CHECKED" -eq 1 ] \
    && echo "GATE(full): $PASS ok, $FAIL FAIL" \
    || echo "GATE(full, no perf tier): $PASS ok, $FAIL FAIL"
[ $FAIL -eq 0 ] || exit 1
