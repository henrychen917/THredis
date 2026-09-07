#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# red.sh -- the REDESIGN chain (t-flipdamp, 2026-09-06): cost gate + variance window + outcome loop.
# Resumable and gate-pausing: every step is skipped when its output is on file and WAITS while the
# owner holds the box marker. Re-run `scratch/red.sh` after any kill.
#   S1  build + unit + arm digests (guard binary preserved at $GUARD_BIN before the first make)
#   S2  directed test: tests/flip_cost_gate.py (cost gate refuses / pays / induced miss) x2
#   S3  directed hold test (the defect regime, asserted) x2
#   S4  matrix4: 4 regimes x 3 rounds x {OFF a, OFF b, REDESIGN, GUARD, BASE}, 40 s cells
#   S5  non-vacuity: boot at the WRONG split (3:1 of 4; 7:1 of 8) -> must move ONCE and land
#   S6  instr/op + cycles/op at a MATCHED rate: base0, red0, red1, guard1 (always-on cost)
#   S7  gate row tests/flipctl.py on the redesign x2
#   S8  batteries 2s + 1s (both atomic) + modes;  S9 differ at 6:2;  S10 report
source /home/user/Projects/wt-flipdamp/scratch/lib.sh
cd "$WT"
GUARD_BIN=$SP/fd-tomokv-guard-d84031d2f
RED_BIN=$FIX_BIN
LG(){ echo "$(date +%T) $*"; }
wait_gate(){ local n=0; while ! gate_ok; do [ $((n%10)) = 0 ] && LG "paused: $(intruders | tr '\n' ';')$([ -f "$SP/quiet.done" ] || echo 'quiet.done MISSING')"; n=$((n+1)); sleep 30; done; }
SRV8=52,53,54,55,180,181,182,183; LG8=56,57,184,185
boot8(){ local SRV_CPUS=$SRV8; boot "$@"; }
CSV=$SP/fd-matrix4.csv

# ---- S1 build + unit ---------------------------------------------------------------------------
if [ ! -s "$SP/fd-red-build.txt" ]; then
  wait_gate
  [ -s "$GUARD_BIN" ] || { LG "guard binary missing at $GUARD_BIN -- stop"; exit 1; }
  taskset -c 52-57,180-185 make -j12 >"$SP/fd-red-build.txt" 2>&1; rc=$?
  LG "BUILD rc=$rc :: $(grep -E 'error|warning' "$SP/fd-red-build.txt" | head -3 | tr '\n' ' ' | cut -c1-300)"
  [ $rc = 0 ] || { LG "BUILD FAILED -- stop"; rm -f "$SP/fd-red-build.txt"; exit 1; }
  ldd "$RED_BIN" | grep -qi "asan\|ubsan" && LG "WARNING: sanitizer linked into the redesign binary"
  LG "ARMS $(sha256sum "$RED_BIN" "$GUARD_BIN" "$BASE_BIN" | cut -c1-16,66- | tr '\n' ' ')"
fi
if [ ! -s "$SP/fd-red-unit.txt" ]; then
  wait_gate
  taskset -c 52-57,180-185 make unit >"$SP/fd-red-unit.txt" 2>&1; rc=$?
  LG "UNIT rc=$rc :: $(tail -3 "$SP/fd-red-unit.txt" | tr '\n' ' ' | cut -c1-200)"
  [ $rc = 0 ] || { LG "UNIT FAILED -- stop"; rm -f "$SP/fd-red-unit.txt"; exit 1; }
fi

# ---- S2 directed test: cost gate + outcome loop --------------------------------------------------
for r in 1 2; do
  out=$SP/fd-costgate-$r.txt
  [ -s "$out" ] && { LG "COSTGATE $r: on file :: $(grep -E '^ok:|AssertionError|^RC=' "$out" | tail -2 | tr '\n' ' ' | cut -c1-200)"; continue; }
  wait_gate
  pid=$(boot "$RED_BIN" "$PORT_HOLD" "costgate-$r" --ratio 3:1 --shards 64 --atomic 1 --flip-auto 1) || continue
  taskset -c "$LG_CPUS" timeout 600 python3 tests/flip_cost_gate.py 127.0.0.1 "$PORT_HOLD" >"$out" 2>&1; rc=$?
  echo "RC=$rc" >>"$out"
  redis-cli -p "$PORT_HOLD" debug flipctl >>"$out" 2>&1
  LG "COSTGATE $r rc=$rc :: $(grep -E '^[1-4]\. |^ok:|AssertionError' "$out" | tr '\n' ' ' | cut -c1-400)"
  stop "$pid" "$PORT_HOLD"
done

# ---- S3 directed hold test (the defect itself) -------------------------------------------------
for tag in red-a red-b; do
  out=$SP/fd-hold4-$tag.txt
  [ -s "$out" ] || { wait_gate; ./scratch/hold.sh "$RED_BIN" >"$out" 2>&1; }
  LG "HOLD $tag :: $(grep -E '^ok:|^anchored|AssertionError' "$out" | tail -2 | tr '\n' ' ' | cut -c1-260)"
done

# ---- S4 the regime matrix (same-binary null first: pol0a vs pol0b are the redesign binary, fa=0) --
for wl in mk sk1:1 sk9:1 get; do
  have=$(awk -F, -v w="$wl" 'NR>1 && $3==w {n++} END{print n+0}' "$CSV" 2>/dev/null); have=${have:-0}
  full=$((have / 5)); need=$((3 - full))
  [ "$need" -le 0 ] && { LG "matrix $wl complete ($have rows)"; continue; }
  LG "matrix $wl: $have rows on file, running $need round(s)"
  ROUND_OFFSET=$full ./scratch/abr.sh "$CSV" 40 "$need" "$wl" \
      pol0a=$RED_BIN:0 pol0b=$RED_BIN:0 red1=$RED_BIN:1 guard1=$GUARD_BIN:1 base1=$BASE_BIN:1
done
LG "MATRIX4 COMPLETE"

# ---- S5 NON-VACUITY: boot at the WRONG split, the controller must move ONCE and land -------------
firecell(){ # firecell TAG BIN FA RATIO SECS [SRVCPUS LGCPUS LGTHREADS]
  local tag=$1 bin=$2 fa=$3 ratio=$4 secs=$5 srv=${6:-$SRV_CPUS} lg=${7:-$LG_CPUS} lgt=${8:-$LG_THREADS} pid info out
  out=$SP/fd-fire4-$tag.txt
  [ -s "$out" ] && { LG "FIRE $tag: on file :: $(cat "$out")"; return 0; }
  wait_gate
  pid=$(SRV_CPUS=$srv boot "$bin" "$PORT_SIG" "fire4-$tag" --ratio "$ratio" --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  LG_CPUS=$lg preload "$PORT_SIG"
  ( t0=$(date +%s); while :; do echo "$(( $(date +%s) - t0 )) $(redis-cli -p "$PORT_SIG" info flipctl 2>/dev/null | tr -d '\r' | grep -E '^flipctl_(state|anchor_io|anchor_ex|triggers|model_last_decision):' | tr '\n' ' ') $(redis-cli -p "$PORT_SIG" info stats 2>/dev/null | tr -d '\r' | grep -E '^flip_(completed|clients_transferred):' | tr '\n' ' ') $(redis-cli -p "$PORT_SIG" info server 2>/dev/null | tr -d '\r' | grep -E '^(io|ex)_threads:' | tr '\n' ' ')"; sleep 2; done ) >"$SP/fd-fire4tl-$tag.txt" 2>&1 &
  local tl=$!
  LG_CPUS=$lg LG_THREADS=$lgt ./scratch/mk.sh "$PORT_SIG" "$secs" >"$SP/fd-fire4mt-$tag.txt" 2>&1
  kill "$tl" 2>/dev/null; wait "$tl" 2>/dev/null
  info=$(redis-cli -p "$PORT_SIG" info all 2>/dev/null | tr -d '\r')
  redis-cli -p "$PORT_SIG" debug flipctl >"$SP/fd-fire4dbg-$tag.txt" 2>&1
  local mt; mt=$(tr '\r' '\n' <"$SP/fd-fire4mt-$tag.txt")
  echo "$tag boot=$ratio fa=$fa rate=$(echo "$mt" | awk '/^Totals/{print $2}') p99=$(echo "$mt" | awk '/^Totals/{print $8}') flips=$(infog "$info" flip_completed) xfer=$(infog "$info" flip_clients_transferred) trig=$(infog "$info" flipctl_triggers) holds=$(infog "$info" flipctl_model_holds) anchor=$(infog "$info" flipctl_anchor_io):$(infog "$info" flipctl_anchor_ex) live=$(infog "$info" io_threads):$(infog "$info" ex_threads) decision=$(infog "$info" flipctl_model_last_decision) kappa=$(infog "$info" flipctl_model_kappa) misses=$(infog "$info" flipctl_model_misses) clientcost=$(infog "$info" flipctl_client_cost) lost=$(infog "$info" flipctl_last_flip_lost)" >"$out"
  LG "FIRE $(cat "$out")"
  stop "$pid" "$PORT_SIG"
}
firecell red-31-off   "$RED_BIN"   0 3:1 60
firecell red-31-on    "$RED_BIN"   1 3:1 60
firecell guard-31-on  "$GUARD_BIN" 1 3:1 60
firecell base-31-on   "$BASE_BIN"  1 3:1 60
firecell red-22-off   "$RED_BIN"   0 2:2 60
firecell red8-71-off  "$RED_BIN"   0 7:1 60 "$SRV8" "$LG8" 4
firecell red8-71-on   "$RED_BIN"   1 7:1 60 "$SRV8" "$LG8" 4
firecell guard8-71-on "$GUARD_BIN" 1 7:1 60 "$SRV8" "$LG8" 4
firecell red8-44-off  "$RED_BIN"   0 4:4 60 "$SRV8" "$LG8" 4

# ---- S6 instr/op and cycles/op at a MATCHED rate -----------------------------------------------
RL=${RL:-1200}
perfcell(){ # perfcell TAG BIN FA
  local tag=$1 bin=$2 fa=$3 pid c0 c1 out secs=55 win=35
  out=$SP/fd-perf4-$tag.txt
  [ -s "$out" ] && { LG "PERF $tag: on file :: $(cat "$out")"; return 0; }
  wait_gate
  pid=$(boot "$bin" "$PORT_AB" "perf4-$tag" --ratio 2:2 --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  preload "$PORT_AB"
  ( taskset -c "$LG_CPUS" memtier_benchmark -s 127.0.0.1 -p "$PORT_AB" --protocol=redis \
      -t "$LG_THREADS" -c "$LG_CONNS" --pipeline="$LG_PIPE" --rate-limiting="$RL" \
      --command="MGET __key__ __key__ __key__ __key__ __key__ __key__ __key__ __key__" --command-ratio=1 --command-key-pattern=R \
      --command="MSET __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__" --command-ratio=1 --command-key-pattern=R \
      -d 32 --key-minimum=1 --key-maximum=$KEYMAX --test-time="$secs" --distinct-client-seed --hide-histogram ) >"$SP/fd-perf4mt-$tag.txt" 2>&1 &
  local load=$!
  sleep 12
  c0=$(redis-cli -p "$PORT_AB" info stats | tr -d '\r' | sed -n 's/^total_commands_processed://p')
  perf stat -e instructions,cycles,task-clock -x, -p "$pid" -o "$SP/fd-perf4stat-$tag.txt" -- sleep "$win" 2>/dev/null
  c1=$(redis-cli -p "$PORT_AB" info stats | tr -d '\r' | sed -n 's/^total_commands_processed://p')
  wait "$load"
  local mt; mt=$(tr '\r' '\n' <"$SP/fd-perf4mt-$tag.txt")
  local ctl; ctl=$(redis-cli -p "$PORT_AB" info flipctl | tr -d '\r' | grep -E '^flipctl_(state|model_last_decision):' | tr '\n' ' ')
  python3 - "$SP/fd-perf4stat-$tag.txt" "$c0" "$c1" "$tag" "$fa" \
      "$(echo "$mt" | awk '/^Totals/{print $2}')" "$win" "$ctl" >"$out" <<'PY'
import sys
path, c0, c1, tag, fa, rate, win, ctl = sys.argv[1:9]
vals = {}
for line in open(path):
    f = line.strip().split(",")
    if len(f) > 2 and f[2]:
        try: vals[f[2]] = float(f[0])
        except ValueError: pass
ops = int(c1) - int(c0)
ins, cyc, tc = vals.get("instructions", 0), vals.get("cycles", 0), vals.get("task-clock", 0)
print("%s fa=%s rate=%s window_cmds=%d instr/op=%.1f cycles/op=%.1f IPC=%.3f srv_cpu_s=%.2f %s" % (
    tag, fa, rate, ops, ins / ops if ops else 0, cyc / ops if ops else 0,
    ins / cyc if cyc else 0, tc / 1000.0, ctl))
PY
  LG "PERF $(cat "$out")"
  stop "$pid" "$PORT_AB"
}
for rep in 1 2; do
  perfcell "base0-$rep"  "$BASE_BIN"  0
  perfcell "red0-$rep"   "$RED_BIN"   0
  perfcell "red1-$rep"   "$RED_BIN"   1
  perfcell "guard1-$rep" "$GUARD_BIN" 1
done

# ---- S7 the gate row, tests/flipctl.py ---------------------------------------------------------
ctlrun(){ # ctlrun TAG BIN
  local tag=$1 bin=$2 pid rc out
  out=$SP/fd-ctl4-$tag.txt
  [ -s "$out" ] && { LG "CTL $tag: on file :: $(grep -E '^ok:|AssertionError|^RC=' "$out" | tail -2 | tr '\n' ' ' | cut -c1-200)"; return 0; }
  wait_gate
  pid=$(boot8 "$bin" "$PORT_BAT" "ctl4-$tag" --ratio 6:2 --atomic 0 --flip-auto 1 \
       ) || return 1
  taskset -c "$LG8" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" \
        --stable-seconds 30 >"$out" 2>&1
  rc=$?
  echo "RC=$rc" >>"$out"
  redis-cli -p "$PORT_BAT" debug flipctl >>"$out" 2>&1
  LG "CTL $tag rc=$rc :: $(grep -E 'anchored off-rail|^ok:|AssertionError' "$out" | head -2 | tr '\n' ' ' | cut -c1-240)"
  stop "$pid" "$PORT_BAT"
}
for r in 1 2; do ctlrun "red-$r" "$RED_BIN"; done

# ---- S8 batteries, both thread modes ------------------------------------------------------------
if [ ! -s "$SP/fd-bat4-flipttl.txt" ]; then
  wait_gate
  pid=$(boot8 "$RED_BIN" "$PORT_BAT" bat4 --enable-debug-command yes) && {
    for t in flip flip_under_load flip_ttl; do
      case $t in flip_under_load) a="20";; *) a="";; esac
      taskset -c "$LG8" python3 tests/$t.py 127.0.0.1 "$PORT_BAT" $a >"$SP/fd-bat4-${t//_/}.txt" 2>&1
      LG "BAT 2s $t.py rc=$? :: $(tail -1 "$SP/fd-bat4-${t//_/}.txt" | cut -c1-140)"
    done
    taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" >"$SP/fd-bat4-spin.txt" 2>&1
    LG "BAT 2s spinprobe rc=$? :: $(tail -1 "$SP/fd-bat4-spin.txt" | cut -c1-140)"
    taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" --idle-only >"$SP/fd-bat4-idle.txt" 2>&1
    LG "BAT 2s idle-ceiling rc=$? :: $(tail -1 "$SP/fd-bat4-idle.txt" | cut -c1-140)"
    stop "$pid" "$PORT_BAT"; }
else LG "2s batteries: on file"; fi
if [ ! -s "$SP/fd-fused4-done.txt" ]; then
  wait_gate
  for AT in 0 1; do
    pid=$(boot8 "$RED_BIN" "$PORT_BAT" "fused4-$AT" --thread-mode 1s --atomic "$AT" --enable-debug-command yes) && {
      LG "FUSED atomic=$AT boot: $(redis-cli -p "$PORT_BAT" info server | tr -d '\r' | grep -E '^thread_mode:|^overlap:' | tr '\n' ' ')"
      for t in s6 multi_exec edgeproto atomfix; do
        taskset -c "$LG8" python3 tests/$t.py 127.0.0.1 "$PORT_BAT" >"$SP/fd-fused4-$t-$AT.txt" 2>&1
        LG "BAT 1s $t.py (atomic $AT) rc=$? :: $(tail -1 "$SP/fd-fused4-$t-$AT.txt" | cut -c1-120)"
      done
      taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" >"$SP/fd-fused4-spin-$AT.txt" 2>&1
      LG "BAT 1s spinprobe (atomic $AT) rc=$? :: $(tail -1 "$SP/fd-fused4-spin-$AT.txt" | cut -c1-120)"
      stop "$pid" "$PORT_BAT"; }
  done
  LG "MODES: 1s --flip-auto 1 must be REFUSED: $(timeout 10 taskset -c "$SRV8" "$RED_BIN" --port "$PORT_BAT" --save '' --thread-mode 1s --flip-auto 1 2>&1 | head -1)"
  pid=$(boot "$RED_BIN" "$PORT_BAT" split4 --ratio 2:2 --flip-auto 1) && { LG "MODES: 2s --flip-auto 1 boots: $(redis-cli -p "$PORT_BAT" info flipctl | tr -d '\r' | grep flipctl_state)"; stop "$pid" "$PORT_BAT"; }
  date >"$SP/fd-fused4-done.txt"
fi

# ---- S9 differ at the ratio the sort suite requires ---------------------------------------------
if [ ! -s "$SP/fd-differ4.txt" ]; then
  wait_gate
  taskset -c 52-57,180-185 tests/differ_gate.sh "$RED_BIN" 8225 8226 "$SRV8" 6:2 >"$SP/fd-differ4.txt" 2>&1
  LG "DIFFER rc=$? :: $(tail -1 "$SP/fd-differ4.txt")"
else LG "differ: on file :: $(tail -1 "$SP/fd-differ4.txt")"; fi
grep -iE "FAIL" "$SP/fd-differ4.txt" | head -6

LG "ALL DONE"; touch "$SP/fd-red.done"
