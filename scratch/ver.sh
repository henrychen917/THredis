#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# ver.sh -- VERIFICATION night for the flip thrash guard (t-flipdamp).
#
# Resumable and gate-pausing by construction: every step is skipped when its output is already on
# file, and every step WAITS while the owner holds the box marker instead of aborting the chain
# (final.sh aborted mid-matrix on the marker and lost three regimes). Rebuild first: the binary
# changed after the matrix2 rows were taken, so those rows are a different server and are kept only
# as fd-matrix2-prev.csv.
#
# Steps, in decreasing order of what they decide:
#   S1  build + unit
#   S2  tests/flipctl.py -- the one gate row that FAILS on this branch, base vs fix, 2 runs each
#   S3  matrix3: 4 regimes x 3 rounds x {OFF a, OFF b, POST, PRE}, 40 s cells
#   S4  directed multi-key hold test (the defect, asserted)
#   S5  NON-VACUITY: boot at the WRONG split under mk and require the controller to still move
#   S6  instr/op + cycles/op at MATCHED rate: hot path (base fa0 vs fix fa0) and always-on (fa1)
#   S7  batteries, 2s and 1s (fused), both atomic settings
#   S8  differ gate at the ratio its sort suite requires (6:2)
#   S9  report
source /home/user/Projects/wt-flipdamp/scratch/lib.sh
cd "$WT"
LG(){ echo "$(date +%T) $*"; }
wait_gate(){ local n=0; while ! gate_ok; do [ $((n%10)) = 0 ] && LG "paused: $(intruders | tr '\n' ';')$([ -f "$SP/quiet.done" ] || echo 'quiet.done MISSING')"; n=$((n+1)); sleep 30; done; }
# 8-thread server geometry (--ratio 6:2, differ, batteries): server 52-55 + siblings, loadgen 56,57 + siblings.
SRV8=52,53,54,55,180,181,182,183; LG8=56,57,184,185
boot8(){ local SRV_CPUS=$SRV8; boot "$@"; }
CSV=$SP/fd-matrix3.csv

# ---- S1 build + unit ---------------------------------------------------------------------------
if [ ! -s "$SP/fd-build.txt" ]; then
  wait_gate
  taskset -c 52-57,180-185 make -j12 >"$SP/fd-build.txt" 2>&1; rc=$?
  LG "BUILD rc=$rc :: $(tail -2 "$SP/fd-build.txt" | tr '\n' ' ' | cut -c1-160)"
  [ $rc = 0 ] || { LG "BUILD FAILED -- stop"; exit 1; }
  ldd "$FIX_BIN" | grep -qi "asan\|ubsan" && LG "WARNING: sanitizer linked into the fix binary"
  # A pinned source is not a pinned binary (.make-settings has cached a sanitizer through make -B
  # before): name each ARM's binary by content, not by path.
  LG "ARMS $(sha256sum "$FIX_BIN" "$BASE_BIN" | cut -c1-16,66- | tr '\n' ' ')"
else LG "build: on file"; fi
if [ ! -s "$SP/fd-unit.txt" ]; then
  taskset -c 52-57,180-185 make unit >"$SP/fd-unit.txt" 2>&1
  LG "UNIT make unit rc=$? :: $(tail -3 "$SP/fd-unit.txt" | tr '\n' ' ' | cut -c1-200)"
fi

# ---- S2 tests/flipctl.py, base vs fix ----------------------------------------------------------
# The gate row "flip controller: ramp gate, hold, surge + mix re-maneuvers". It failed on the fix
# binary with a RAIL anchor (1:7) whose +22% "gain" was the driver's own ramp; base random-walks and
# settles on the best of several readings, so it is expected to pass. Two runs each: one pass and
# one fail is a flaky row, and that verdict changes what the failure means.
ctlrun(){ # ctlrun TAG BIN
  local tag=$1 bin=$2 pid rc out
  out=$SP/fd-ctl-$tag.txt   # NOT in the `local` above: local expands its args before assigning them
  [ -s "$out" ] && { LG "CTL $tag: on file :: $(grep -E '^ok:|AssertionError|^RC=' "$out" | tail -2 | tr '\n' ' ' | cut -c1-200)"; return 0; }
  wait_gate
  pid=$(boot8 "$bin" "$PORT_BAT" "ctl-$tag" --ratio 6:2 --atomic 0 --flip-auto 1 \
       ) || return 1
  taskset -c "$LG8" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" \
        --stable-seconds 30 >"$out" 2>&1
  rc=$?
  echo "RC=$rc" >>"$out"
  redis-cli -p "$PORT_BAT" debug flipctl >>"$out" 2>&1
  LG "CTL $tag rc=$rc :: $(grep -E 'anchored off-rail|^ok:|AssertionError' "$out" | head -2 | tr '\n' ' ' | cut -c1-240)"
  stop "$pid" "$PORT_BAT"
}
for r in 1 2; do ctlrun "base-$r" "$BASE_BIN"; ctlrun "fix-$r" "$FIX_BIN"; done

# ---- S3 the regime matrix ----------------------------------------------------------------------
[ -f "$SP/fd-matrix2.csv" ] && [ ! -f "$SP/fd-matrix2-prev.csv" ] && mv "$SP/fd-matrix2.csv" "$SP/fd-matrix2-prev.csv"
for wl in mk sk1:1 sk9:1 get; do
  have=$(awk -F, -v w="$wl" 'NR>1 && $3==w {n++} END{print n+0}' "$CSV" 2>/dev/null); have=${have:-0}
  full=$((have / 4)); need=$((3 - full))
  [ "$need" -le 0 ] && { LG "matrix $wl complete ($have rows)"; continue; }
  LG "matrix $wl: $have rows on file, running $need round(s)"
  ROUND_OFFSET=$full ./scratch/abw.sh "$CSV" 40 "$need" "$wl" \
      pol0a=$FIX_BIN:0 pol0b=$FIX_BIN:0 pol1=$FIX_BIN:1 base1=$BASE_BIN:1
done
LG "MATRIX3 COMPLETE"

# ---- S4 directed hold test (the defect itself) -------------------------------------------------
for spec in "pol-a:$FIX_BIN" "pol-b:$FIX_BIN" "base-a:$BASE_BIN"; do
  tag=${spec%%:*}; bin=${spec#*:}; out=$SP/fd-hold3-$tag.txt
  [ -s "$out" ] || { wait_gate; ./scratch/hold.sh "$bin" >"$out" 2>&1; }
  LG "HOLD $tag :: $(grep -E '^ok:|^anchored|AssertionError' "$out" | tail -2 | tr '\n' ' ' | cut -c1-260)"
done

# ---- S5 NON-VACUITY: the controller must still fire on a genuine load change --------------------
# A guard that never moves is a silent --flip-auto 0, and every zero-flip row above would be
# vacuous. Boot at 3:1, which the explicit-flip probe measured at ~190k against ~500k at 2:2 on
# this rig, and run the same mk load: --flip-auto 1 has to find the split and pay for the flip,
# --flip-auto 0 stays where it was booted.
firecell(){ # firecell TAG BIN FA RATIO SECS
  local tag=$1 bin=$2 fa=$3 ratio=$4 secs=$5 pid info out
  out=$SP/fd-fire-$tag.txt
  [ -s "$out" ] && { LG "FIRE $tag: on file :: $(cat "$out")"; return 0; }
  wait_gate
  pid=$(boot "$bin" "$PORT_SIG" "fire-$tag" --ratio "$ratio" --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  preload "$PORT_SIG"
  ( t0=$(date +%s); while :; do echo "$(( $(date +%s) - t0 )) $(redis-cli -p "$PORT_SIG" info flipctl 2>/dev/null | tr -d '\r' | grep -E '^flipctl_(state|anchor_io|anchor_ex|triggers):' | tr '\n' ' ') $(redis-cli -p "$PORT_SIG" info stats 2>/dev/null | tr -d '\r' | grep -E '^flip_(completed|clients_transferred):' | tr '\n' ' ')"; sleep 2; done ) >"$SP/fd-firetl-$tag.txt" 2>&1 &
  local tl=$!
  ./scratch/mk.sh "$PORT_SIG" "$secs" >"$SP/fd-firemt-$tag.txt" 2>&1
  kill "$tl" 2>/dev/null; wait "$tl" 2>/dev/null
  info=$(redis-cli -p "$PORT_SIG" info all 2>/dev/null | tr -d '\r')
  redis-cli -p "$PORT_SIG" debug flipctl >"$SP/fd-firedbg-$tag.txt" 2>&1
  local mt; mt=$(tr '\r' '\n' <"$SP/fd-firemt-$tag.txt")
  echo "$tag boot=$ratio fa=$fa rate=$(echo "$mt" | awk '/^Totals/{print $2}') p99=$(echo "$mt" | awk '/^Totals/{print $8}') flips=$(infog "$info" flip_completed) xfer=$(infog "$info" flip_clients_transferred) trig=$(infog "$info" flipctl_triggers) holds=$(infog "$info" flipctl_model_holds) anchor=$(infog "$info" flipctl_anchor_io):$(infog "$info" flipctl_anchor_ex) live=$(infog "$info" io_threads):$(infog "$info" ex_threads)" >"$out"
  LG "FIRE $(cat "$out")"
  stop "$pid" "$PORT_SIG"
}
firecell base-31-off "$BASE_BIN" 0 3:1 60
firecell base-31-on  "$BASE_BIN" 1 3:1 60
firecell fix-31-off  "$FIX_BIN"  0 3:1 60
firecell fix-31-on   "$FIX_BIN"  1 3:1 60
firecell fix-22-off  "$FIX_BIN"  0 2:2 60

# ---- S6 instr/op and cycles/op at a MATCHED rate -----------------------------------------------
# The always-on question: what does the controller cost when it is stable, and what did the ex-loop
# accounting change cost the hot path? instr/op counts idle spin, so the arms are rate-limited to
# the same offered load (memtier --rate-limiting is per connection) and the achieved rates are
# printed with the counts: a row whose rates do not match is not a comparison.
RL=${RL:-1200}
perfcell(){ # perfcell TAG BIN FA
  local tag=$1 bin=$2 fa=$3 pid c0 c1 out secs=55 win=35
  out=$SP/fd-perf-$tag.txt
  [ -s "$out" ] && { LG "PERF $tag: on file :: $(cat "$out")"; return 0; }
  wait_gate
  pid=$(boot "$bin" "$PORT_AB" "perf-$tag" --ratio 2:2 --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  preload "$PORT_AB"
  ( taskset -c "$LG_CPUS" memtier_benchmark -s 127.0.0.1 -p "$PORT_AB" --protocol=redis \
      -t "$LG_THREADS" -c "$LG_CONNS" --pipeline="$LG_PIPE" --rate-limiting="$RL" \
      --command="MGET __key__ __key__ __key__ __key__ __key__ __key__ __key__ __key__" --command-ratio=1 --command-key-pattern=R \
      --command="MSET __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__" --command-ratio=1 --command-key-pattern=R \
      -d 32 --key-minimum=1 --key-maximum=$KEYMAX --test-time="$secs" --distinct-client-seed --hide-histogram ) >"$SP/fd-perfmt-$tag.txt" 2>&1 &
  local load=$!
  sleep 12
  c0=$(redis-cli -p "$PORT_AB" info stats | tr -d '\r' | sed -n 's/^total_commands_processed://p')
  perf stat -e instructions,cycles,task-clock -x, -p "$pid" -o "$SP/fd-perfstat-$tag.txt" -- sleep "$win" 2>/dev/null
  c1=$(redis-cli -p "$PORT_AB" info stats | tr -d '\r' | sed -n 's/^total_commands_processed://p')
  wait "$load"
  local mt; mt=$(tr '\r' '\n' <"$SP/fd-perfmt-$tag.txt")
  python3 - "$SP/fd-perfstat-$tag.txt" "$c0" "$c1" "$tag" "$fa" \
      "$(echo "$mt" | awk '/^Totals/{print $2}')" "$win" >"$out" <<'PY'
import sys
path, c0, c1, tag, fa, rate, win = sys.argv[1:8]
vals = {}
for line in open(path):
    f = line.strip().split(",")
    if len(f) > 2 and f[2]:
        try: vals[f[2]] = float(f[0])
        except ValueError: pass
ops = int(c1) - int(c0)
ins, cyc, tc = vals.get("instructions", 0), vals.get("cycles", 0), vals.get("task-clock", 0)
print("%s fa=%s rate=%s window_cmds=%d instr/op=%.1f cycles/op=%.1f IPC=%.3f srv_cpu_s=%.2f" % (
    tag, fa, rate, ops, ins / ops if ops else 0, cyc / ops if ops else 0,
    ins / cyc if cyc else 0, tc / 1000.0))
PY
  LG "PERF $(cat "$out")"
  stop "$pid" "$PORT_AB"
}
for rep in 1 2; do
  perfcell "base0-$rep" "$BASE_BIN" 0
  perfcell "fix0-$rep"  "$FIX_BIN"  0
  perfcell "fix1-$rep"  "$FIX_BIN"  1
done

# ---- S7 batteries, both thread modes ------------------------------------------------------------
if [ ! -s "$SP/fd-bat3-flipttl.txt" ]; then
  wait_gate
  pid=$(boot8 "$FIX_BIN" "$PORT_BAT" bat3 --enable-debug-command yes) && {
    for t in flip flip_under_load flip_ttl; do
      case $t in flip_under_load) a="20";; *) a="";; esac
      taskset -c "$LG8" python3 tests/$t.py 127.0.0.1 "$PORT_BAT" $a >"$SP/fd-bat3-${t//_/}.txt" 2>&1
      LG "BAT 2s $t.py rc=$? :: $(tail -1 "$SP/fd-bat3-${t//_/}.txt" | cut -c1-140)"
    done
    taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" >"$SP/fd-bat3-spin.txt" 2>&1
    LG "BAT 2s spinprobe rc=$? :: $(tail -1 "$SP/fd-bat3-spin.txt" | cut -c1-140)"
    taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" --idle-only >"$SP/fd-bat3-idle.txt" 2>&1
    LG "BAT 2s idle-ceiling rc=$? :: $(tail -1 "$SP/fd-bat3-idle.txt" | cut -c1-140)"
    stop "$pid" "$PORT_BAT"; }
else LG "2s batteries: on file"; fi
if [ ! -s "$SP/fd-fused-done.txt" ]; then
  wait_gate
  for AT in 0 1; do
    pid=$(boot8 "$FIX_BIN" "$PORT_BAT" "fused3-$AT" --thread-mode 1s --atomic "$AT" --enable-debug-command yes) && {
      LG "FUSED atomic=$AT boot line: $(redis-cli -p "$PORT_BAT" info server | tr -d '\r' | grep -E '^thread_mode:|^overlap:' | tr '\n' ' ')"
      for t in s6 multi_exec edgeproto atomfix; do
        taskset -c "$LG8" python3 tests/$t.py 127.0.0.1 "$PORT_BAT" >"$SP/fd-fused-$t-$AT.txt" 2>&1
        LG "BAT 1s $t.py (atomic $AT) rc=$? :: $(tail -1 "$SP/fd-fused-$t-$AT.txt" | cut -c1-120)"
      done
      taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" >"$SP/fd-fused-spin-$AT.txt" 2>&1
      LG "BAT 1s spinprobe (atomic $AT) rc=$? :: $(tail -1 "$SP/fd-fused-spin-$AT.txt" | cut -c1-120)"
      stop "$pid" "$PORT_BAT"; }
  done
  LG "MODES: 1s --flip-auto 1 must be REFUSED: $(timeout 10 taskset -c "$SRV8" "$FIX_BIN" --port "$PORT_BAT" --save '' --thread-mode 1s --flip-auto 1 2>&1 | head -1)"
  pid=$(boot "$FIX_BIN" "$PORT_BAT" split3 --ratio 2:2 --flip-auto 1) && { LG "MODES: 2s --flip-auto 1 boots: $(redis-cli -p "$PORT_BAT" info flipctl | tr -d '\r' | grep flipctl_state)"; stop "$pid" "$PORT_BAT"; }
  date >"$SP/fd-fused-done.txt"
fi

# ---- S8 differ at the ratio the sort suite requires ---------------------------------------------
if [ ! -s "$SP/fd-differ3.txt" ]; then
  wait_gate
  taskset -c 52-57,180-185 tests/differ_gate.sh "$FIX_BIN" 8225 8226 "$SRV8" 6:2 >"$SP/fd-differ3.txt" 2>&1
  LG "DIFFER rc=$? :: $(tail -1 "$SP/fd-differ3.txt")"
else LG "differ: on file :: $(tail -1 "$SP/fd-differ3.txt")"; fi
grep -iE "FAIL" "$SP/fd-differ3.txt" | head -6

# ---- S9 report ----------------------------------------------------------------------------------
python3 scratch/report.py "$SP/fd-report.html" 2>&1 | tail -1
LG "ALL DONE"; touch "$SP/fd-ver.done"
