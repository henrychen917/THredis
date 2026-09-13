#!/bin/bash
# PRE-PUSH GATE for tomokv-cpp (pure 2s baseline).
#
#   tests/gate.sh quick   loopback only, ~3 min: build (release+ASAN), footprint locks, boot
#                         matrix, smoke, torture, RYOW, atomic torn/mixed-write/window gates,
#                         shutdown invariants, counter-fired assertions, idle-CPU ceiling. Runs on
#                         any machine.
#   tests/gate.sh full    quick + torture-under-ASAN + the Redis 7.4 differential matrix + NIC
#                         regression cells vs tests/gate_refs.txt (the NIC cells need the 25GbE
#                         netns rig and its scratchpad binaries/procsafe helper).
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
# geometry, while infofix proves INFO rows are measured rather than placeholders; both run in
# both atomic modes. Their sibling lanes each claimed 207 -> 209, so the merged ledger owes four.
# 211 -> 213: pushtear proves out-of-band frames cannot splice borrowed replies and requires the
# segmented/deferred and zero-copy counters to fire, under both atomic modes.
EXPECT_QUICK=217
EXPECT_FULL=228                 # full without the optional NIC row.
say(){ printf '  %-52s %s\n' "$1" "$2"; }
ok(){ say "$1" "ok"; PASS=$((PASS+1)); }
bad(){ say "$1" "FAIL${2:+ ($2)}"; FAIL=$((FAIL+1)); }
program_state(){
  local expect=$1 actual=$((PASS+FAIL))
  [ "$actual" -eq "$expect" ] \
      && say "PROGRAM-STATE ledger ($actual/$expect checks)" "ok" \
      || bad "PROGRAM-STATE ledger" "$actual/$expect checks"
}
redis_cli_expect_ok(){
  local reply
  reply=$(redis-cli -h 127.0.0.1 -p "$PORT" "$@" 2>&1 | tr -d '\r')
  [ "$reply" = OK ] || { printf 'unexpected redis-cli reply to %s: %s\n' "$*" "$reply" >&2; return 1; }
}

# ---- 1. builds (the static_asserts on sizeof(Op)/sizeof(Client) gate here) -------------------
make -j12 >/dev/null 2>&1 && ok "release build (+footprint locks)" || bad "release build"
ASAN=/tmp/tomokv-gate-asan
g++ -std=c++20 -O1 -g -fsanitize=address -march=native -pthread -I. \
    src/main.cc src/net/tls.cc src/cmd/*.cc src/snapshot/*.cc src/persist/*.cc \
    -o $ASAN -luring -pthread -lssl -lcrypto 2>/dev/null \
    && ok "ASAN build" || bad "ASAN build"
g++ -std=c++20 -O2 -I. tests/config_parser_test.cc -o /tmp/tomokv-config-parser-test \
    && /tmp/tomokv-config-parser-test \
    && ok "Redis config quoting + mid-value #" || bad "Redis config quoting + mid-value #"
python3 tools/gen_acl_categories.py --redis-root "${REDIS74_ROOT:-/tmp/claude-1000/redis74}" \
    --check src/cmd/acl_categories_generated.h \
    && ok "generated Redis 7.4 ACL categories" || bad "generated Redis 7.4 ACL categories"

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

boot(){ # binary -> pid ; server log to $SRVLOG
  local bin=$1; shift
  SRV=0; SRVLOG=/dev/null
  (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null \
      && { say "port $PORT pre-boot guard" "FAIL (already accepting)"; exit 1; }
  SRVLOG=$(mktemp /tmp/gate-srv.XXXXXX)
  taskset -c $CORES "$bin" --port $PORT --bind 127.0.0.1 --shards 16 --ratio $GATE_RATIO "$@" \
      > "$SRVLOG" 2>&1 &
  SRV=$!
  # 30s, not 10s: the AOF replay boot replays its file BEFORE it listens, and on a box shared
  # with other lanes that overran a 10s deadline and turned six AOF rows red with no defect behind
  # them. A generous deadline costs nothing when the server is quick — the loop exits on connect.
  for _ in $(seq 150); do ./build/tomokv --help >/dev/null 2>&1
    if ! kill -0 "$SRV" 2>/dev/null; then wait "$SRV" 2>/dev/null; return 1; fi
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2; done
  return 1
}
stop(){ kill -TERM $SRV 2>/dev/null; wait $SRV 2>/dev/null; sleep 5; }

# A registered command with no generated metadata row makes command_metadata_init fail, and the
# server then refuses to boot at all -- every row below goes red at once with no indication which
# command is at fault. Static, so it fires before any server starts.
python3 tests/cmdmeta_coverage.py >/tmp/gate-cmdmeta-coverage.txt 2>&1 \
    && ok "cmdmeta covers every registered command" \
    || bad "cmdmeta covers every registered command" "see /tmp/gate-cmdmeta-coverage.txt"

# ---- 3. correctness: smoke + torture + RYOW on the release build ------------------------------
boot ./build/tomokv || bad "release boot"
python3 tests/../tests/torture.py 127.0.0.1 $PORT >/tmp/gate-tort.txt 2>&1 \
    && ok "torture battery" || bad "torture battery" "see /tmp/gate-tort.txt"
python3 tests/ryow.py 127.0.0.1 $PORT >/tmp/gate-ryow.txt 2>&1 \
    && ok "RYOW battery" || bad "RYOW battery" "see /tmp/gate-ryow.txt"
python3 tests/acl_categories.py 127.0.0.1 $PORT >/tmp/gate-acl-categories.txt 2>&1 \
    && ok "ACL category runtime table" || bad "ACL category runtime table" "see /tmp/gate-acl-categories.txt"
python3 tests/acl.py 127.0.0.1 $PORT - >/tmp/gate-acl-nofile.txt 2>&1 \
    && ok "ACL LOAD/SAVE no-file errors" || bad "ACL LOAD/SAVE no-file errors" "see /tmp/gate-acl-nofile.txt"
# idle-CPU ceiling: after the batteries, an idle server must not burn cores
C0=$(awk '{print $14+$15}' /proc/$SRV/stat 2>/dev/null); sleep 5
C1=$(awk '{print $14+$15}' /proc/$SRV/stat 2>/dev/null)
J=
[ -n "$C0" ] && [ -n "$C1" ] && J=$((C1-C0))
# J must exist: a missing process sample is not a zero-jiffy measurement.
[ -n "$J" ] && [ "$J" -lt 200 ] \
    && ok "idle CPU ceiling ($J jiffies/5s)" \
    || bad "idle CPU ceiling" "${J:-measurement missing} jiffies/5s"
stop
# shutdown invariants + fired counters, from the TERM dump
grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
    && ok "shutdown invariants (nothing stuck)" || bad "shutdown invariants"
D=$(grep -oE "direct=[0-9]+" "$SRVLOG" | head -1 | cut -d= -f2)
[ -n "$D" ] && [ "$D" -gt 0 ] && ok "direct-reply fired (direct=$D)" || bad "direct-reply fired"
R=$(grep -oE "dispatched=[0-9]+" "$SRVLOG" | cut -d= -f2)
E=$(grep -oE "executed=[0-9]+"  "$SRVLOG" | cut -d= -f2)
[ -n "$R" ] && [ "$R" = "$E" ] && ok "dispatched==executed ($R)" || bad "dispatched==executed" "$R vs $E"

# Explicit ON boot plus the non-vacuous epoch-MVCC gates. atomic_torn includes its own OFF control,
# predecessor/promotion counters, overlapping writers, window liveness, and live CONFIG flips.
boot ./build/tomokv --atomic 1 || bad "atomic release boot"
python3 tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn.txt 2>&1 \
    && ok "atomic torn/window battery" || bad "atomic torn/window battery" "see /tmp/gate-atomic-torn.txt"
python3 tests/atomic_ryow.py 127.0.0.1 $PORT >/tmp/gate-atomic-ryow.txt 2>&1 \
    && ok "atomic RYOW/mixed-write battery" || bad "atomic RYOW/mixed-write battery" "see /tmp/gate-atomic-ryow.txt"
stop
grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
    && ok "atomic shutdown invariants" || bad "atomic shutdown invariants"

# ---- feature batteries: every shipped feature's directed test, BOTH atomic settings -----------
# The gate accumulates a section per landed feature (owner rule). Each test is directed and
# asserts its own mechanisms fired; the boot covers multi/blocking/pubsub+sharded/lua/limits.
for AT in 0 1; do
  boot ./build/tomokv --atomic $AT --enable-debug-command yes \
      || bad "feature battery boot (atomic $AT)"
  for t in s6 multi_exec blocking blockmulti stream streamgroups pubsub lua_scripting scriptsurf limits resp3 bitfield dumprestore zsetops geo climon climon2 tracking hexpire servertail lcs concur edgeproto edgeenc edgetime arity contarity cmdgap aclsel expwide infofix pushtear; do
    python3 tests/$t.py 127.0.0.1 $PORT >/tmp/gate-$t-$AT.txt 2>&1 \
        && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see /tmp/gate-$t-$AT.txt"
  done
  stop
  grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
      && ok "feature shutdown invariants (atomic $AT)" || bad "feature shutdown invariants (atomic $AT)"
done

# ---- SORT's dynamic keys: exact production gate geometry, both atomic modes -------------------
# tests/sort.py rejects any boot other than 16 shards at 6:2, buckets candidate names with
# DEBUG SHARD, and requires concrete BY and GET keys plus STORE destination on the executor slot
# opposite the source. No same-owner or one-executor run can satisfy this row.
for AT in 0 1; do
  boot ./build/tomokv --ratio 6:2 --atomic "$AT" --enable-debug-command yes \
      || bad "cross-owner SORT boot (atomic $AT)"
  python3 tests/sort.py 127.0.0.1 "$PORT" >/tmp/gate-sort-$AT.txt 2>&1 \
      && ok "cross-owner SORT battery (atomic $AT)" \
      || bad "cross-owner SORT battery (atomic $AT)" "see /tmp/gate-sort-$AT.txt"
  stop
done

# ---- debug-surface batteries: these drive DEBUG subcommands, hence their own armed boot -------
for AT in 0 1; do
  boot ./build/tomokv --atomic $AT --enable-debug-command yes || bad "debug-surface boot (atomic $AT)"
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
  for t in lbsignals slowlog atomfix scriptatomic execatomic execiso execfix multires session_monotonic xacct xmove xscript; do
    python3 tests/$t.py 127.0.0.1 $PORT >/tmp/gate-$t-$AT.txt 2>&1 \
        && ok "$t battery (atomic $AT)" || bad "$t battery (atomic $AT)" "see /tmp/gate-$t-$AT.txt"
  done
  stop
done

# ---- cross-owner script control boots: each needs its own knob value, hence its own server ----
# off:    script-crossshard-max-bytes 0 must reproduce today's CROSSSLOT byte-for-byte and leave
#         every cross counter at zero (the feature is off ⇒ nothing is allocated).
# limit:  a tiny staging budget must refuse before RUN and leave the declared values untouched.
# window: one cut slot per IO thread must refuse the overflow with -BUSY, counted exactly, with
#         the same activations issued one at a time as the zero control.
for XS in "off:--script-crossshard-max-bytes 0" \
          "limit:--script-crossshard-max-bytes 300" \
          "window:--script-crossshard-cut-slots 1"; do
  XS_MODE=${XS%%:*}; XS_ARGS=${XS#*:}
  boot ./build/tomokv --atomic 1 --enable-debug-command yes $XS_ARGS \
      || bad "xscript $XS_MODE control boot"
  python3 tests/xscript.py 127.0.0.1 $PORT "$XS_MODE" >/tmp/gate-xscript-$XS_MODE.txt 2>&1 \
      && ok "xscript $XS_MODE control" || bad "xscript $XS_MODE control" \
             "see /tmp/gate-xscript-$XS_MODE.txt"
  stop
done

# ---- efficiency guards: each needs a boot geometry of its own, so each gets its own server ----
# These assert a RATIO or a growth bound, never an absolute time, and each was verified to FAIL on
# the binary from before its fix (see NOTES-XPERF2.md). They shipped unwired, which is the same
# coverage-that-does-not-exist problem the gate lane fixed elsewhere.
# --shards 1 for the same reason as the borrow guard: the sidecar under test is per shard, and the
# battery asserts that precondition rather than quietly measuring a diluted one.
boot ./build/tomokv --shards 1 --enable-debug-command yes || bad "expire-index guard boot"
python3 tests/expireindex.py 127.0.0.1 $PORT >/tmp/gate-expireindex.txt 2>&1 \
    && ok "expire-index growth bound" || bad "expire-index growth bound" "see /tmp/gate-expireindex.txt"
stop

# one shard so every borrow lands in ONE registry (the quantity under test), and a small zc-min so
# an ordinary-sized value still takes the borrow path and pays registry cost.
boot ./build/tomokv --shards 1 --zc-min 64 --client-output-buffer-limit "normal 0 0 0" \
    --enable-debug-command yes || bad "borrow-registry guard boot"
python3 tests/borrow_registry.py 127.0.0.1 $PORT >/tmp/gate-borrow.txt 2>&1 \
    && ok "borrow-registry growth bound" || bad "borrow-registry growth bound" "see /tmp/gate-borrow.txt"
stop

# this one boots its own arms at two thread counts, so it takes the port/cores rather than a server
XDS_PORT=$PORT XDS_CPUS=$CORES XDS_BIN=./build/tomokv bash tests/xshard_dispatch_scale.sh \
    >/tmp/gate-xds.txt 2>&1 \
    && ok "cross-shard dispatch scaling" || bad "cross-shard dispatch scaling" "see /tmp/gate-xds.txt"

# ---- Redis-wire DUMP/RESTORE survives the native snapshot/restart boundary -------------------
DUMPRESTORE_DIR=$(mktemp -d /tmp/gate-dumprestore.XXXXXX)
boot ./build/tomokv --atomic 1 --dir "$DUMPRESTORE_DIR" --dbfilename dumprestore.tomo \
    || bad "DUMP/RESTORE restart preparation boot"
python3 tests/dumprestore.py 127.0.0.1 $PORT prepare_restart \
    >/tmp/gate-dumprestore-restart.txt 2>&1 \
    && ok "DUMP/RESTORE prepare + native SAVE" \
    || bad "DUMP/RESTORE restart preparation" "see /tmp/gate-dumprestore-restart.txt"
stop
boot ./build/tomokv --atomic 1 --dir "$DUMPRESTORE_DIR" \
    --load "$DUMPRESTORE_DIR/dumprestore.tomo" \
    || bad "DUMP/RESTORE snapshot reload boot"
python3 tests/dumprestore.py 127.0.0.1 $PORT verify_restart \
    >>/tmp/gate-dumprestore-restart.txt 2>&1 \
    && ok "DUMP/RESTORE cross-restart round-trip" \
    || bad "DUMP/RESTORE cross-restart round-trip" "see /tmp/gate-dumprestore-restart.txt"
stop
grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
    && ok "DUMP/RESTORE restart shutdown invariants" \
    || bad "DUMP/RESTORE restart shutdown invariants"

# ---- auth + audit DEBUG (purpose-booted; each test asserts its gate actually opened) -----------
./build/tomokv --protected-mode maybe 2>&1 | grep -q "protected-mode wants" \
    && ok "reject bad protected-mode" || bad "reject bad protected-mode"
./build/tomokv --enable-debug-command maybe 2>&1 | grep -q "enable-debug-command wants" \
    && ok "reject bad enable-debug-command" || bad "reject bad enable-debug-command"
boot ./build/tomokv --requirepass gatepass || bad "auth purpose boot"
python3 tests/auth.py 127.0.0.1 $PORT gatepass >/tmp/gate-auth.txt 2>&1 \
    && ok "AUTH/HELLO/protected state machine" || bad "AUTH/HELLO/protected state machine" "see /tmp/gate-auth.txt"
stop
ACL_DIR=$(mktemp -d /tmp/gate-acl.XXXXXX)
ACL_FILE="$ACL_DIR/users.acl"
: > "$ACL_FILE"
boot ./build/tomokv --aclfile "$ACL_FILE" || bad "ACL purpose boot"
python3 tests/acl.py 127.0.0.1 $PORT "$ACL_FILE" >/tmp/gate-acl.txt 2>&1 \
    && ok "ACL battery (atomic off)" || bad "ACL battery (atomic off)" "see /tmp/gate-acl.txt"
stop
ACL_ATOMIC_FILE="$ACL_DIR/users-atomic.acl"
: > "$ACL_ATOMIC_FILE"
boot ./build/tomokv --aclfile "$ACL_ATOMIC_FILE" --atomic 1 || bad "ACL atomic purpose boot"
python3 tests/acl.py 127.0.0.1 $PORT "$ACL_ATOMIC_FILE" >/tmp/gate-acl-atomic.txt 2>&1 \
    && ok "ACL battery (atomic on)" || bad "ACL battery (atomic on)" "see /tmp/gate-acl-atomic.txt"
stop
DEBUG_DIR=$(mktemp -d /tmp/gate-debug.XXXXXX)
boot ./build/tomokv --enable-debug-command local --dir "$DEBUG_DIR" --dbfilename reload.tomo \
    || bad "DEBUG purpose boot"
python3 tests/debug.py 127.0.0.1 $PORT >/tmp/gate-debug.txt 2>&1 \
    && ok "DEBUG toggle/reload battery" || bad "DEBUG toggle/reload battery" "see /tmp/gate-debug.txt"
{ redis_cli_expect_ok FLUSHALL \
    && python3 tests/snap_typed_roundtrip.py $PORT build_save \
    && redis_cli_expect_ok DEBUG RELOAD \
    && python3 tests/snap_typed_roundtrip.py $PORT verify; } \
    >/tmp/gate-snap-typed.txt 2>&1 \
    && ok "typed snapshot round-trip incl stream" \
    || bad "typed snapshot round-trip incl stream" "see /tmp/gate-snap-typed.txt"
stop

# ---- snapshot data/sync engines: cut, typed round-trip, and typed preimage race ----------------
for PERSIST_IO in normal uring; do
  SNAP_DIR=$(mktemp -d "/tmp/gate-snapshot-${PERSIST_IO}.XXXXXX")
  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
      --dir "$SNAP_DIR" --dbfilename cut.tomo \
      || bad "snapshot cut boot ($PERSIST_IO)"
  python3 tests/snap_cut_battery.py "$PORT" save \
      >"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
      && ok "snapshot concurrent cut ($PERSIST_IO)" \
      || bad "snapshot concurrent cut ($PERSIST_IO)" \
             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
  stop
  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
      --dir "$SNAP_DIR" --load "$SNAP_DIR/cut.tomo" \
      || bad "snapshot cut reload boot ($PERSIST_IO)"
  python3 tests/snap_cut_battery.py "$PORT" verify_cut \
      >>"/tmp/gate-snapshot-cut-${PERSIST_IO}.txt" 2>&1 \
      && ok "snapshot cut reload ($PERSIST_IO)" \
      || bad "snapshot cut reload ($PERSIST_IO)" \
             "see /tmp/gate-snapshot-cut-${PERSIST_IO}.txt"
  stop

  # The cut battery above writes only single keys, so it passes on a tree whose snapshot tears
  # every cross-shard atomic group. These two arms are the ones that can tell the difference:
  # generation-tagged groups, a live MGET reader that must stay clean throughout, and the
  # cuts_waited counter that separates a real group drain from a vacuous one. Both arms were
  # confirmed to FAIL on the pre-fix tree (44-53 and 10-13 torn groups per cut respectively).
  GROUP_DIR=$(mktemp -d "/tmp/gate-snapshot-groups-${PERSIST_IO}.XXXXXX")
  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" --atomic 1 \
      --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
      || bad "atomic group cut boot ($PERSIST_IO, atomic 1)"
  python3 tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" mset 5 \
      >"/tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt" 2>&1 \
      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt" \
      && ok "snapshot never tears a cross-shard MSET group ($PERSIST_IO, atomic 1)" \
      || bad "snapshot never tears a cross-shard MSET group ($PERSIST_IO, atomic 1)" \
             "see /tmp/gate-snapshot-groups-mset-${PERSIST_IO}.txt"
  stop
  # CONTROL ARM: the DEFAULT --atomic 0. EXEC force-admits a group at either setting, so a
  # transaction is atomic to readers here too and the file must agree with them.
  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" --atomic 0 \
      --enable-debug-command yes --dir "$GROUP_DIR" --dbfilename groups.tomo \
      || bad "atomic group cut boot ($PERSIST_IO, atomic 0)"
  python3 tests/snap_cut_battery.py "$PORT" atomic_groups "$GROUP_DIR/groups.tomo" exec 3 \
      >"/tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt" 2>&1 \
      && grep -q "ATOMIC_GROUP_CUT PASS" "/tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt" \
      && ok "snapshot never tears a MULTI/EXEC group ($PERSIST_IO, default atomic 0)" \
      || bad "snapshot never tears a MULTI/EXEC group ($PERSIST_IO, default atomic 0)" \
             "see /tmp/gate-snapshot-groups-exec-${PERSIST_IO}.txt"
  stop

  TYPED_DIR=$(mktemp -d "/tmp/gate-snapshot-typed-${PERSIST_IO}.XXXXXX")
  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
      --dir "$TYPED_DIR" --dbfilename typed.tomo \
      || bad "typed snapshot boot ($PERSIST_IO)"
  python3 tests/snap_typed_roundtrip.py "$PORT" build_save \
      >"/tmp/gate-snapshot-typed-${PERSIST_IO}.txt" 2>&1 \
      && ok "typed snapshot save ($PERSIST_IO)" \
      || bad "typed snapshot save ($PERSIST_IO)" \
             "see /tmp/gate-snapshot-typed-${PERSIST_IO}.txt"
  stop
  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
      --dir "$TYPED_DIR" --load "$TYPED_DIR/typed.tomo" \
      || bad "typed snapshot reload boot ($PERSIST_IO)"
  python3 tests/snap_typed_roundtrip.py "$PORT" verify \
      >>"/tmp/gate-snapshot-typed-${PERSIST_IO}.txt" 2>&1 \
      && ok "typed snapshot reload ($PERSIST_IO)" \
      || bad "typed snapshot reload ($PERSIST_IO)" \
             "see /tmp/gate-snapshot-typed-${PERSIST_IO}.txt"
  stop

  RACE_DIR=$(mktemp -d "/tmp/gate-snapshot-race-${PERSIST_IO}.XXXXXX")
  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
      --dir "$RACE_DIR" --dbfilename race.tomo \
      || bad "typed snapshot race boot ($PERSIST_IO)"
  python3 tests/snap_typed_race.py "$PORT" race \
      >"/tmp/gate-snapshot-race-${PERSIST_IO}.txt" 2>&1 \
      && grep -q 'PREIMAGE-FIRED PASS' "/tmp/gate-snapshot-race-${PERSIST_IO}.txt" \
      && ok "typed snapshot preimage race ($PERSIST_IO)" \
      || bad "typed snapshot preimage race ($PERSIST_IO)" \
             "see /tmp/gate-snapshot-race-${PERSIST_IO}.txt"
  stop
  boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
      --dir "$RACE_DIR" --load "$RACE_DIR/race.tomo" \
      || bad "typed snapshot race reload boot ($PERSIST_IO)"
  python3 tests/snap_typed_race.py "$PORT" verify \
      >>"/tmp/gate-snapshot-race-${PERSIST_IO}.txt" 2>&1 \
      && ok "typed snapshot race reload ($PERSIST_IO)" \
      || bad "typed snapshot race reload ($PERSIST_IO)" \
             "see /tmp/gate-snapshot-race-${PERSIST_IO}.txt"
  stop
done

# ---- notify lane: integrated owner/retire seams plus both live atomic settings -----------------
boot ./build/tomokv --notify-keyspace-events KEAmn --enable-debug-command yes \
    || bad "feature battery boot + notify CLI knob"   # armed: multi_exec.py needs DEBUG SHARD
                                                       # to locate a same-owner key pair, and it
                                                       # FAILS rather than skips without it
python3 tests/multi_exec.py 127.0.0.1 $PORT >/tmp/gate-multi.txt 2>&1 \
    && ok "MULTI feature battery" || bad "MULTI feature battery" "see /tmp/gate-multi.txt"
python3 tests/blocking.py 127.0.0.1 $PORT >/tmp/gate-blocking.txt 2>&1 \
    && ok "blocking feature battery" || bad "blocking feature battery" "see /tmp/gate-blocking.txt"
python3 tests/pubsub.py 127.0.0.1 $PORT >/tmp/gate-pubsub.txt 2>&1 \
    && ok "pubsub feature battery" || bad "pubsub feature battery" "see /tmp/gate-pubsub.txt"
python3 tests/lua_scripting.py 127.0.0.1 $PORT >/tmp/gate-lua.txt 2>&1 \
    && ok "Lua feature battery" || bad "Lua feature battery" "see /tmp/gate-lua.txt"
python3 tests/notify.py 127.0.0.1 $PORT >/tmp/gate-notify.txt 2>&1 \
    && ok "keyspace notification battery (atomic 0/1)" \
    || bad "keyspace notification battery" "see /tmp/gate-notify.txt"
stop
grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
    && ok "feature battery shutdown invariants" || bad "feature battery shutdown invariants"

# ---- AOF boot/replay + non-vacuous DEBUG LOADAOF ---------------------------------------------
for PERSIST_IO in normal uring; do
for AOF_ATOMIC in 0 1; do
AOF_DIR=$(mktemp -d "/tmp/gate-aof-${PERSIST_IO}-atomic${AOF_ATOMIC}.XXXXXX")
AOF_STATE=$AOF_DIR/state.json
boot ./build/tomokv --protected-mode no --atomic "$AOF_ATOMIC" \
    --appendonly yes --appendfsync no \
    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_DIR" \
    || bad "AOF purpose boot ($PERSIST_IO, atomic $AOF_ATOMIC)"
PERSIST_GET=$(redis-cli -h 127.0.0.1 -p $PORT --raw CONFIG GET persist-io 2>/dev/null |
    tail -1)
PERSIST_SET=$(redis-cli -h 127.0.0.1 -p $PORT CONFIG SET persist-io "$PERSIST_IO" 2>&1)
[ "$PERSIST_GET" = "$PERSIST_IO" ] && printf '%s' "$PERSIST_SET" | grep -q immutable \
    && ok "persist-io surface + immutable ($PERSIST_IO)" \
    || bad "persist-io surface + immutable ($PERSIST_IO)"
python3 tests/aof.py 127.0.0.1 $PORT populate "$AOF_STATE" >/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
    && python3 tests/aof.py 127.0.0.1 $PORT loadaof "$AOF_STATE" >>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
    && ok "AOF byte-exact + script groups + DEBUG LOADAOF ($PERSIST_IO, atomic $AOF_ATOMIC)" \
    || bad "AOF byte-exact + script groups + DEBUG LOADAOF ($PERSIST_IO, atomic $AOF_ATOMIC)" \
           "see /tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt"
AOF_PRE_MODEL=$(python3 tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt)
AOF_WRITTEN=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_records_written://p')
[ -n "$AOF_WRITTEN" ] && [ "$AOF_WRITTEN" -gt 0 ] \
    && ok "AOF writer fired (records=$AOF_WRITTEN)" || bad "AOF writer fired"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
sleep 5
boot ./build/tomokv --protected-mode no --appendonly yes --appendfsync no \
    --atomic "$AOF_ATOMIC" --persist-io "$PERSIST_IO" \
    --enable-debug-command yes --dir "$AOF_DIR" \
    || bad "AOF replay boot ($PERSIST_IO, atomic $AOF_ATOMIC)"
python3 tests/aof.py 127.0.0.1 $PORT verify "$AOF_STATE" >>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt 2>&1 \
    && ok "AOF process-restart script replay ($PERSIST_IO, atomic $AOF_ATOMIC)" \
    || bad "AOF process-restart script replay ($PERSIST_IO, atomic $AOF_ATOMIC)" \
           "see /tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt"
AOF_POST_MODEL=$(python3 tests/aof.py 127.0.0.1 $PORT snapshot "$AOF_DIR/dump.tomo" 2>>/tmp/gate-aof-$PERSIST_IO-$AOF_ATOMIC.txt)
[ -n "$AOF_PRE_MODEL" ] && [ "$AOF_PRE_MODEL" = "$AOF_POST_MODEL" ] \
    && ok "AOF native snapshot streams byte-exact" || bad "AOF native snapshot streams byte-exact"
AOF_REPLAYED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_replayed_records://p')
AOF_SKIPPED=$(redis-cli -h 127.0.0.1 -p $PORT INFO Persistence 2>/dev/null \
    | tr -d '\r' | sed -n 's/^aof_groups_skipped_on_replay://p')
[ -n "$AOF_REPLAYED" ] && [ "$AOF_REPLAYED" -gt 0 ] && [ -n "$AOF_SKIPPED" ] \
    && ok "AOF replay fired (records=$AOF_REPLAYED skipped=$AOF_SKIPPED)" || bad "AOF replay fired"
stop
sleep 5
done

# ---- AOF atomic-group bracketing + directed interrupted-process recovery ---------------------
AOF_GROUP_DIR=$(mktemp -d "/tmp/gate-aof-group-${PERSIST_IO}.XXXXXX")
AOF_GROUP_STATE=$AOF_GROUP_DIR/state.json
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
    || bad "AOF group purpose boot ($PERSIST_IO)"
python3 tests/aof_torn_group.py 127.0.0.1 $PORT prepare "$AOF_GROUP_STATE" \
    >/tmp/gate-aof-group.txt 2>&1 \
    && ok "AOF directed group interruption fired" \
    || bad "AOF directed group interruption" "see /tmp/gate-aof-group.txt"
wait $SRV 2>/dev/null
sleep 5
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync no \
    --persist-io "$PERSIST_IO" --enable-debug-command yes --dir "$AOF_GROUP_DIR" \
    || bad "AOF group recovery boot ($PERSIST_IO)"
python3 tests/aof_torn_group.py 127.0.0.1 $PORT verify "$AOF_GROUP_STATE" \
    >>/tmp/gate-aof-group.txt 2>&1 \
    && python3 tests/aof_torn_group.py 127.0.0.1 $PORT scan \
       "$AOF_GROUP_DIR/appendonlydir/appendonly.aof.1.incr.tomo" \
       >>/tmp/gate-aof-group.txt 2>&1 \
    && ok "AOF atomic-group recovery + writer order" \
    || bad "AOF atomic-group recovery + writer order" "see /tmp/gate-aof-group.txt"
stop
grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
    && ok "AOF group shutdown invariants" || bad "AOF group shutdown invariants"
sleep 5

# ---- AOF sync policies, reply gate, idle sync, and durability-window recovery ----------------
AOF_ALWAYS_DIR=$(mktemp -d "/tmp/gate-aof-always-${PERSIST_IO}.XXXXXX")
AOF_ALWAYS_STATE=$AOF_ALWAYS_DIR/state.json
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
    --persist-io "$PERSIST_IO" --dir "$AOF_ALWAYS_DIR" \
    || bad "AOF always purpose boot ($PERSIST_IO)"
python3 tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_ALWAYS_STATE" always 512 \
    >/tmp/gate-aof-always.txt 2>&1 \
    && ok "AOF always sync + reply gate fired" \
    || bad "AOF always sync + reply gate" "see /tmp/gate-aof-always.txt"
kill -KILL $SRV 2>/dev/null
wait $SRV 2>/dev/null
sleep 5
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync always \
    --persist-io "$PERSIST_IO" --dir "$AOF_ALWAYS_DIR" \
    || bad "AOF always recovery boot ($PERSIST_IO)"
python3 tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_ALWAYS_STATE" always 512 \
    >>/tmp/gate-aof-always.txt 2>&1 \
    && ok "AOF always acknowledged-prefix recovery" \
    || bad "AOF always acknowledged-prefix recovery" "see /tmp/gate-aof-always.txt"
stop
sleep 5

AOF_EVERY_DIR=$(mktemp -d "/tmp/gate-aof-everysec-${PERSIST_IO}.XXXXXX")
AOF_EVERY_STATE=$AOF_EVERY_DIR/state.json
AOF_EVERY_FILE=$AOF_EVERY_DIR/appendonlydir/appendonly.aof.1.incr.tomo
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
    --persist-io "$PERSIST_IO" --dir "$AOF_EVERY_DIR" \
    || bad "AOF everysec purpose boot ($PERSIST_IO)"
python3 tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_EVERY_STATE" everysec 512 \
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
sleep 5
boot ./build/tomokv --protected-mode no --atomic 1 --appendonly yes --appendfsync everysec \
    --persist-io "$PERSIST_IO" --dir "$AOF_EVERY_DIR" \
    || bad "AOF everysec recovery boot ($PERSIST_IO)"
python3 tests/aof_fsync.py 127.0.0.1 $PORT verify "$AOF_EVERY_STATE" everysec 512 \
    >>/tmp/gate-aof-everysec.txt 2>&1 \
    && grep -q "AOF warning: truncated AOF tail" "$SRVLOG" \
    && ok "AOF everysec durability window + tail warning" \
    || bad "AOF everysec durability window + tail warning" "see /tmp/gate-aof-everysec.txt"
stop
sleep 5

AOF_NO_DIR=$(mktemp -d "/tmp/gate-aof-no-sync-${PERSIST_IO}.XXXXXX")
boot ./build/tomokv --protected-mode no --atomic 0 --appendonly yes --appendfsync no \
    --persist-io "$PERSIST_IO" --dir "$AOF_NO_DIR" \
    || bad "AOF no-sync purpose boot ($PERSIST_IO)"
python3 tests/aof_fsync.py 127.0.0.1 $PORT populate "$AOF_NO_DIR/state.json" no 128 \
    >/tmp/gate-aof-no-sync.txt 2>&1 \
    && ok "AOF no-sync bypassed sync + reply gate" \
    || bad "AOF no-sync bypass" "see /tmp/gate-aof-no-sync.txt"
stop
sleep 5

PERSIST_IO=$PERSIST_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_matrix.sh \
    >/tmp/gate-aof-rewrite.txt 2>&1 \
    && ok "AOF rewrite atomic/stage/corruption matrix" \
    || bad "AOF rewrite matrix" "see /tmp/gate-aof-rewrite.txt"

PERSIST_IO=$PERSIST_IO GATE_PORT=$PORT GATE_CORES=$CORES tests/aof_rewrite_trigger_matrix.sh \
    >/tmp/gate-aof-rewrite-trigger.txt 2>&1 \
    && ok "AOF rewrite triggers + observability matrix" \
    || bad "AOF rewrite triggers" "see /tmp/gate-aof-rewrite-trigger.txt"

AOF_OFF_DIR=$(mktemp -d "/tmp/gate-aof-off-${PERSIST_IO}.XXXXXX")
boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
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
sleep 5
boot ./build/tomokv --protected-mode no --persist-io "$PERSIST_IO" \
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
# persist-io normal only: that is where the defect was demonstrated (11 of 114 runs of the AOF
# battery, 0 of 117 on uring) and where the window is entered reliably enough for the row to prove
# its mechanism fired. The battery FAILS on a build with the guard removed (6 of 6).
for AOF_FRAME_ATOMIC in 0 1; do
AOF_FRAME_DIR=$(mktemp -d "/tmp/gate-aof-frameorder-atomic${AOF_FRAME_ATOMIC}.XXXXXX")
boot ./build/tomokv --protected-mode no --atomic "$AOF_FRAME_ATOMIC" --appendonly yes \
    --appendfsync no --persist-io normal --auto-aof-rewrite-percentage 0 \
    --enable-debug-command yes --dir "$AOF_FRAME_DIR" \
    || bad "AOF frame-order purpose boot (atomic $AOF_FRAME_ATOMIC)"
python3 tests/aof_frame_order.py 127.0.0.1 $PORT "$AOF_FRAME_DIR/appendonlydir" \
    >/tmp/gate-aof-frameorder-$AOF_FRAME_ATOMIC.txt 2>&1 \
    && ok "AOF control frame never inside a large record (atomic $AOF_FRAME_ATOMIC)" \
    || bad "AOF control frame never inside a large record (atomic $AOF_FRAME_ATOMIC)" \
           "see /tmp/gate-aof-frameorder-$AOF_FRAME_ATOMIC.txt"
stop
done

# ---- TLS memory-BIO transport: independent listener, auth matrix, parser/teardown fences -------
TLS_PORT=$((PORT+1))
TLS_DIR=$(mktemp -d /tmp/gate-tls.XXXXXX)
python3 tests/tls.py --generate "$TLS_DIR" >/tmp/gate-tls-generate.txt 2>&1 \
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
  SRV=0; SRVLOG=/dev/null
  (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null \
      && { say "TLS plain port $PORT pre-boot guard" "FAIL (already accepting)"; exit 1; }
  (exec 3<>/dev/tcp/127.0.0.1/$TLS_PORT) 2>/dev/null \
      && { say "TLS port $TLS_PORT pre-boot guard" "FAIL (already accepting)"; exit 1; }
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
python3 tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" yes --plain-port "$PORT" \
    >/tmp/gate-tls-yes.txt 2>&1 \
    && ok "TLS client-auth yes matrix" || bad "TLS client-auth yes matrix" "see /tmp/gate-tls-yes.txt"
stop
grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
    && ok "TLS yes shutdown invariants" || bad "TLS yes shutdown invariants"
# (tls_ktls_active is a GAUGE — 0 at clean shutdown by design — so kTLS engagement is asserted
# live below, on the optional-mode boot, not from this shutdown dump.)

tlsboot optional || bad "TLS client-auth optional purpose boot"
python3 tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" optional --plain-port "$PORT" \
    >/tmp/gate-tls-optional.txt 2>&1 \
    && ok "TLS client-auth optional matrix" \
    || bad "TLS client-auth optional matrix" "see /tmp/gate-tls-optional.txt"
# Live kTLS engagement proof on the default (tls-ktls yes) boot: a plain TLS client connects and
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
grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
    && ok "TLS optional shutdown invariants" || bad "TLS optional shutdown invariants"

# The full battery runs FORCED-FALLBACK so the userspace arm's mechanisms (zc suppression, memory
# BIO paths) keep their non-vacuous proofs; the default-ktls arm is proven separately below.
tlsboot no --tls-ktls no --tls-protocols "TLSv1.2 TLSv1.3" --tls-ciphers DEFAULT \
    --tls-ciphersuites TLS_AES_256_GCM_SHA384 --tls-prefer-server-ciphers yes \
    || bad "TLS coexistence purpose boot"
python3 tests/tls.py 127.0.0.1 "$TLS_PORT" "$TLS_DIR" no --plain-port "$PORT" --full \
    --expect-ktls no >/tmp/gate-tls-full.txt 2>&1 \
    && ok "TLS pipeline/torn-record/coexistence battery" \
    || bad "TLS correctness battery" "see /tmp/gate-tls-full.txt"
stop
grep -q "stuck: live_conns=0 rob_not_quiesced=0 unsent_bytes_pending=0" "$SRVLOG" \
    && ok "TLS shutdown invariants" || bad "TLS shutdown invariants"
grep -qE "wb: .* err=0 " "$SRVLOG" \
    && ok "TLS application send path error-free" || bad "TLS send errors"
TLS_ACCEPTS=$(sed -n 's/^tls: accepts=\([0-9][0-9]*\).*/\1/p' "$SRVLOG")
TLS_FREED=$(sed -n 's/^tls: .* freed=\([0-9][0-9]*\).*/\1/p' "$SRVLOG")
TLS_ZC=$(sed -n 's/^tls: .* zc_suppressed=\([0-9][0-9]*\).*/\1/p' "$SRVLOG")
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
boot $ASAN --atomic 1 || bad "ASAN boot"
python3 tests/torture.py 127.0.0.1 $PORT >/tmp/gate-tort-asan.txt 2>&1 \
    && ok "torture under ASAN" || bad "torture under ASAN"
python3 tests/ryow.py 127.0.0.1 $PORT >/tmp/gate-ryow-asan.txt 2>&1 \
    && ok "RYOW under ASAN" || bad "RYOW under ASAN"
python3 tests/atomic_torn.py 127.0.0.1 $PORT >/tmp/gate-atomic-torn-asan.txt 2>&1 \
    && ok "atomic torn/window under ASAN" || bad "atomic torn/window under ASAN"
python3 tests/atomic_ryow.py 127.0.0.1 $PORT >/tmp/gate-atomic-ryow-asan.txt 2>&1 \
    && ok "atomic RYOW under ASAN" || bad "atomic RYOW under ASAN"
stop
grep -q "ERROR: AddressSanitizer" "$SRVLOG" && bad "ASAN clean" || ok "ASAN clean"

# ---- 4b. full tier: zero-copy borrow lifetime (release+ASAN) ----------------------------------
zcboot(){
  SRV=0; SRVLOG=/dev/null
  (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null \
      && { say "port $PORT pre-boot guard" "FAIL (already accepting)"; exit 1; }
  SRVLOG=$(mktemp /tmp/gate-srv.XXXXXX)
  timeout 900 taskset -c $CORES "$1" --port $PORT --bind 127.0.0.1 --shards 16 --ratio $GATE_RATIO       --zc-min 16384 > "$SRVLOG" 2>&1 &
  SRV=$!
  for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2; done
  return 1
}
zcboot ./build/tomokv || bad "zc boot"
python3 tests/zc.py 127.0.0.1 $PORT >/tmp/gate-zc.txt 2>&1     && ok "zc borrow battery" || bad "zc borrow battery" "see /tmp/gate-zc.txt"
stop
ZS=$(grep -oE "zc_sends=[0-9]+" "$SRVLOG" | cut -d= -f2)
[ -n "$ZS" ] && [ "$ZS" -gt 0 ] && ok "zc fired (zc_sends=$ZS)" || bad "zc fired"
zcboot $ASAN || bad "zc ASAN boot"
python3 tests/zc.py 127.0.0.1 $PORT >/tmp/gate-zc-asan.txt 2>&1     && ok "zc borrow battery under ASAN" || bad "zc borrow battery under ASAN"
stop
grep -q "ERROR: AddressSanitizer" "$SRVLOG" && bad "zc ASAN clean" || ok "zc ASAN clean"

# ---- 4c. full tier: byte-exact differential matrix against pinned vanilla Redis 7.4 ----------
# The helper discovers the ordinary suites from differ.py's gens registry, adds the two special
# early-exit suites, and runs serially because this gate owns only one target/oracle port pair.
DIFFER_ORACLE_PORT=${GATE_DIFFER_ORACLE_PORT:-$((PORT+1))}
if GATE_DIFFER_ORACLE_CORES=${GATE_DIFFER_ORACLE_CORES:-$CORES} \
    tests/differ_gate.sh ./build/tomokv "$PORT" "$DIFFER_ORACLE_PORT" "$CORES" "$GATE_RATIO"; then
  ok "Redis 7.4 differential matrix"
else
  bad "Redis 7.4 differential matrix"
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
      "${GATE_DIFFER_ORACLE_BIN:-${REDIS74_ROOT:-/tmp/claude-1000/redis74}/src/redis-server}" \
      --port "$GLOBCASE_ORACLE_PORT" --bind 127.0.0.1 --save '' --appendonly no \
      >/tmp/gate-globcase-oracle.txt 2>&1 &
  GLOBCASE_ORACLE=$!
  for _ in $(seq 60); do
    (exec 3<>/dev/tcp/127.0.0.1/$GLOBCASE_ORACLE_PORT) 2>/dev/null && break; sleep 0.25
  done
  python3 tests/globcase.py 127.0.0.1 "$PORT" 127.0.0.1 "$GLOBCASE_ORACLE_PORT" \
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
