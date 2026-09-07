#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# red2.sh -- REDESIGN chain v2 (t-flipdamp, 2026-09-06 pm), folding the coordinator's owner-box
# correction: BASE-auto (t9final) is the bar to beat, cells are 120 s with a 1 Hz trace so
# time-to-first-move / moves-after-stabilization / steady-state are three separate numbers, the
# wrong-split boot must MOVE within bounded time on every run (a hold there is a FAIL), and the 40 s
# multi-key cell is kept only as the thrash-count row. Resumable + gate-pausing. Re-run to resume.
#   S1 build + unit + digests           S2 directed cost-gate x2 + hold x2
#   S3 TIME-TO-FIRST-MOVE 120 s, 1 Hz trace, arms off/base-auto/guard/red at boots 2:2, 3:1, 7:1(8thr)
#   S4 40 s thrash-count matrix (off/base-auto/guard/red)   S5 perf always-on cost
#   S6 gate row + batteries + differ    S7 report
source /home/user/Projects/wt-flipdamp/scratch/lib.sh
cd "$WT"
RED_BIN=$FIX_BIN
GUARD_BIN=$SP/bin/tomokv-flipguard
BASEAUTO_BIN=$SP/bin/tomokv-t9final
LG(){ echo "$(date +%T) $*"; }
wait_gate(){ local n=0; while ! gate_ok; do [ $((n%10)) = 0 ] && LG "paused: $(intruders | tr '\n' ';')$([ -f "$SP/quiet.done" ] || echo 'quiet.done MISSING')"; n=$((n+1)); sleep 30; done; }
SRV8=52,53,54,55,180,181,182,183; LG8=56,57,184,185
boot8(){ local SRV_CPUS=$SRV8; boot "$@"; }
CSV=$SP/fd-matrix5.csv

# ---- S1 build + unit ---------------------------------------------------------------------------
if [ ! -s "$SP/fd-r2-build.txt" ]; then
  wait_gate
  [ -s "$GUARD_BIN" ] && [ -s "$BASEAUTO_BIN" ] || { LG "shared binaries missing -- stop"; exit 1; }
  taskset -c 52-57,180-185 make -j12 >"$SP/fd-r2-build.txt" 2>&1; rc=$?
  LG "BUILD rc=$rc :: $(grep -E 'error|warning' "$SP/fd-r2-build.txt" | head -3 | tr '\n' ' ' | cut -c1-200)"
  [ $rc = 0 ] || { LG "BUILD FAILED"; rm -f "$SP/fd-r2-build.txt"; exit 1; }
  LG "ARMS red=$(sha256sum "$RED_BIN" | cut -c1-16) guard=$(sha256sum "$GUARD_BIN" | cut -c1-16) baseauto=$(sha256sum "$BASEAUTO_BIN" | cut -c1-16)"
fi
if [ ! -s "$SP/fd-r2-unit.txt" ]; then
  wait_gate; taskset -c 52-57,180-185 make unit >"$SP/fd-r2-unit.txt" 2>&1; rc=$?
  LG "UNIT rc=$rc :: $(tail -3 "$SP/fd-r2-unit.txt" | tr '\n' ' ' | cut -c1-200)"
  [ $rc = 0 ] || { LG "UNIT FAILED"; rm -f "$SP/fd-r2-unit.txt"; exit 1; }
fi

# ---- S2 directed cost-gate + hold --------------------------------------------------------------
for r in 1 2; do
  out=$SP/fd-r2-costgate-$r.txt
  [ -s "$out" ] && { LG "COSTGATE $r on file :: $(grep -E '^ok:|AssertionError' "$out" | tail -1 | cut -c1-160)"; continue; }
  wait_gate
  pid=$(boot "$RED_BIN" "$PORT_HOLD" "r2cg-$r" --ratio 3:1 --shards 64 --atomic 1 --flip-auto 1) || continue
  taskset -c "$LG_CPUS" timeout 600 python3 tests/flip_cost_gate.py 127.0.0.1 "$PORT_HOLD" >"$out" 2>&1; rc=$?
  echo "RC=$rc" >>"$out"; redis-cli -p "$PORT_HOLD" debug flipctl >>"$out" 2>&1
  LG "COSTGATE $r rc=$rc :: $(grep -E '^[1-4]\. |^ok:|AssertionError' "$out" | tr '\n' ' ' | cut -c1-400)"
  stop "$pid" "$PORT_HOLD"
done
for tag in red-a red-b; do
  out=$SP/fd-r2-hold-$tag.txt
  [ -s "$out" ] || { wait_gate; ./scratch/hold.sh "$RED_BIN" >"$out" 2>&1; }
  LG "HOLD $tag :: $(grep -E '^ok:|AssertionError' "$out" | tail -1 | cut -c1-200)"
done

# ---- S3 TIME-TO-FIRST-MOVE, 120 s cells, 1 Hz trace --------------------------------------------
# tm TAG BIN FA BOOT_IO BOOT_EX SRVCPUS LGCPUS LGTHREADS
tm(){
  local tag=$1 bin=$2 fa=$3 bio=$4 bex=$5 srv=${6:-$SRV_CPUS} lg=${7:-$LG_CPUS} lgt=${8:-$LG_THREADS}
  local out=$SP/fd-r2-tm-$tag.txt tr=$SP/fd-r2-tmtrace-$tag.txt secs=120 pid mt
  [ -s "$out" ] && { LG "TM $tag on file :: $(cat "$out")"; return 0; }
  wait_gate
  pid=$(SRV_CPUS=$srv boot "$bin" "$PORT_SIG" "r2tm-$tag" --ratio "$bio:$bex" --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  LG_CPUS=$lg preload "$PORT_SIG"
  ( t0=$(date +%s); while :; do
      tc=$(redis-cli -p "$PORT_SIG" info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p')
      sp=$(redis-cli -p "$PORT_SIG" info server 2>/dev/null | tr -d '\r' | grep -E '^(io|ex)_threads:' | sed 's/.*://' | tr '\n' ':' | sed 's/:$//')
      echo "$(( $(date +%s) - t0 )) ${tc:-0} ${sp:-0:0}"; sleep 1; done ) >"$tr" 2>&1 &
  local sampler=$!
  LG_CPUS=$lg LG_THREADS=$lgt ./scratch/mk.sh "$PORT_SIG" "$secs" >"$SP/fd-r2-tmmt-$tag.txt" 2>&1
  kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
  local info; info=$(redis-cli -p "$PORT_SIG" info all 2>/dev/null | tr -d '\r')
  redis-cli -p "$PORT_SIG" debug flipctl >"$SP/fd-r2-tmdbg-$tag.txt" 2>&1
  mt=$(tr '\r' '\n' <"$SP/fd-r2-tmmt-$tag.txt")
  local parsed; parsed=$(python3 ./scratch/ttfm.py "$tr" "$bio" "$bex" | head -1)
  echo "$tag boot=$bio:$bex fa=$fa memtier_rate=$(echo "$mt" | awk '/^Totals/{print $2}') p99=$(echo "$mt" | awk '/^Totals/{print $8}') flips=$(infog "$info" flip_completed) xfer=$(infog "$info" flip_clients_transferred) decision=$(infog "$info" flipctl_model_last_decision) misses=$(infog "$info" flipctl_model_misses) kappa=$(infog "$info" flipctl_model_kappa) live=$(infog "$info" io_threads):$(infog "$info" ex_threads) | $parsed" >"$out"
  LG "TM $(cat "$out")"
  stop "$pid" "$PORT_SIG"
}
# 4 threads (2:2 optimal on this rig): matched-split control + two wrong-split boots
for spec in "off-22:$RED_BIN:0:2:2" "base-22:$BASEAUTO_BIN:1:2:2" "guard-22:$GUARD_BIN:1:2:2" "red-22:$RED_BIN:1:2:2" \
            "off-31:$RED_BIN:0:3:1" "base-31:$BASEAUTO_BIN:1:3:1" "guard-31:$GUARD_BIN:1:3:1" "red-31:$RED_BIN:1:3:1"; do
  IFS=: read -r tag bin fa bio bex <<<"$spec"; tm "$tag" "$bin" "$fa" "$bio" "$bex"
done
# 8 threads, wrong-split 7:1 (more executors should win) -- the closest analogue to the owner's 28:4
for spec in "off8-71:$RED_BIN:0:7:1" "base8-71:$BASEAUTO_BIN:1:7:1" "guard8-71:$GUARD_BIN:1:7:1" "red8-71:$RED_BIN:1:7:1"; do
  IFS=: read -r tag bin fa bio bex <<<"$spec"; tm "$tag" "$bin" "$fa" "$bio" "$bex" "$SRV8" "$LG8" 4
done
LG "TIME-TO-FIRST-MOVE COMPLETE"

# ---- S4 40 s thrash-count matrix ---------------------------------------------------------------
for wl in mk sk1:1 sk9:1 get; do
  have=$(awk -F, -v w="$wl" 'NR>1 && $3==w {n++} END{print n+0}' "$CSV" 2>/dev/null); have=${have:-0}
  full=$((have / 5)); need=$((3 - full))
  [ "$need" -le 0 ] && { LG "matrix $wl complete ($have rows)"; continue; }
  LG "matrix $wl: $have rows, running $need round(s)"
  ROUND_OFFSET=$full ./scratch/abr.sh "$CSV" 40 "$need" "$wl" \
      pol0a=$RED_BIN:0 pol0b=$RED_BIN:0 red1=$RED_BIN:1 guard1=$GUARD_BIN:1 base1=$BASEAUTO_BIN:1
done
LG "THRASH MATRIX COMPLETE"

# ---- S5 perf always-on cost --------------------------------------------------------------------
RL=${RL:-1200}
perfcell(){ local tag=$1 bin=$2 fa=$3 pid c0 c1 out secs=55 win=35
  out=$SP/fd-r2-perf-$tag.txt
  [ -s "$out" ] && { LG "PERF $tag on file :: $(cat "$out")"; return 0; }
  wait_gate
  pid=$(boot "$bin" "$PORT_AB" "r2perf-$tag" --ratio 2:2 --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  preload "$PORT_AB"
  ( taskset -c "$LG_CPUS" memtier_benchmark -s 127.0.0.1 -p "$PORT_AB" --protocol=redis \
      -t "$LG_THREADS" -c "$LG_CONNS" --pipeline="$LG_PIPE" --rate-limiting="$RL" \
      --command="MGET __key__ __key__ __key__ __key__ __key__ __key__ __key__ __key__" --command-ratio=1 --command-key-pattern=R \
      --command="MSET __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__ __key__ __data__" --command-ratio=1 --command-key-pattern=R \
      -d 32 --key-minimum=1 --key-maximum=$KEYMAX --test-time="$secs" --distinct-client-seed --hide-histogram ) >"$SP/fd-r2-perfmt-$tag.txt" 2>&1 &
  local load=$!; sleep 12
  c0=$(redis-cli -p "$PORT_AB" info stats | tr -d '\r' | sed -n 's/^total_commands_processed://p')
  perf stat -e instructions,cycles,task-clock -x, -p "$pid" -o "$SP/fd-r2-perfstat-$tag.txt" -- sleep "$win" 2>/dev/null
  c1=$(redis-cli -p "$PORT_AB" info stats | tr -d '\r' | sed -n 's/^total_commands_processed://p')
  wait "$load"; local mt; mt=$(tr '\r' '\n' <"$SP/fd-r2-perfmt-$tag.txt")
  python3 - "$SP/fd-r2-perfstat-$tag.txt" "$c0" "$c1" "$tag" "$fa" "$(echo "$mt" | awk '/^Totals/{print $2}')" "$win" >"$out" <<'PYIN'
import sys
path, c0, c1, tag, fa, rate, win = sys.argv[1:8]
vals = {}
for line in open(path):
    f = line.strip().split(",")
    if len(f) > 2 and f[2]:
        try: vals[f[2]] = float(f[0])
        except ValueError: pass
ops = int(c1) - int(c0); ins, cyc, tc = vals.get("instructions", 0), vals.get("cycles", 0), vals.get("task-clock", 0)
print("%s fa=%s rate=%s window_cmds=%d instr/op=%.1f cycles/op=%.1f IPC=%.3f" % (
    tag, fa, rate, ops, ins/ops if ops else 0, cyc/ops if ops else 0, ins/cyc if cyc else 0))
PYIN
  LG "PERF $(cat "$out")"; stop "$pid" "$PORT_AB"
}
for rep in 1 2; do
  perfcell "base0-$rep" "$BASE_BIN" 0
  perfcell "red0-$rep"  "$RED_BIN"  0
  perfcell "red1-$rep"  "$RED_BIN"  1
done

# ---- S6 gate row + batteries + differ ----------------------------------------------------------
for r in 1 2; do
  out=$SP/fd-r2-ctl-$r.txt
  [ -s "$out" ] && { LG "CTL $r on file :: $(grep -E '^ok:|AssertionError|anchored off-rail' "$out" | head -1 | cut -c1-180)"; continue; }
  wait_gate
  pid=$(boot8 "$RED_BIN" "$PORT_BAT" "r2ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1) || continue
  taskset -c "$LG8" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" --stable-seconds 30 >"$out" 2>&1; rc=$?
  echo "RC=$rc" >>"$out"; redis-cli -p "$PORT_BAT" debug flipctl >>"$out" 2>&1
  LG "CTL $r rc=$rc :: $(grep -E 'anchored off-rail|^ok:|AssertionError' "$out" | head -1 | cut -c1-200)"
  stop "$pid" "$PORT_BAT"
done
if [ ! -s "$SP/fd-r2-bat-flipttl.txt" ]; then
  wait_gate
  pid=$(boot8 "$RED_BIN" "$PORT_BAT" r2bat --enable-debug-command yes) && {
    for t in flip flip_under_load flip_ttl; do
      case $t in flip_under_load) a="20";; *) a="";; esac
      taskset -c "$LG8" python3 tests/$t.py 127.0.0.1 "$PORT_BAT" $a >"$SP/fd-r2-bat-${t//_/}.txt" 2>&1
      LG "BAT 2s $t rc=$? :: $(tail -1 "$SP/fd-r2-bat-${t//_/}.txt" | cut -c1-120)"
    done
    taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" >"$SP/fd-r2-bat-spin.txt" 2>&1; LG "BAT spin rc=$? :: $(tail -1 "$SP/fd-r2-bat-spin.txt" | cut -c1-100)"
    taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" --idle-only >"$SP/fd-r2-bat-idle.txt" 2>&1; LG "BAT idle rc=$? :: $(tail -1 "$SP/fd-r2-bat-idle.txt" | cut -c1-100)"
    stop "$pid" "$PORT_BAT"; }
fi
if [ ! -s "$SP/fd-r2-fused-done.txt" ]; then
  wait_gate
  for AT in 0 1; do
    pid=$(boot8 "$RED_BIN" "$PORT_BAT" "r2fused-$AT" --thread-mode 1s --atomic "$AT" --enable-debug-command yes) && {
      for t in s6 multi_exec edgeproto atomfix; do
        taskset -c "$LG8" python3 tests/$t.py 127.0.0.1 "$PORT_BAT" >"$SP/fd-r2-fused-$t-$AT.txt" 2>&1
        LG "BAT 1s $t (atomic $AT) rc=$? :: $(tail -1 "$SP/fd-r2-fused-$t-$AT.txt" | cut -c1-100)"
      done
      taskset -c "$LG8" python3 tests/spinprobe.py "$PORT_BAT" "$pid" >"$SP/fd-r2-fused-spin-$AT.txt" 2>&1; LG "BAT 1s spin (atomic $AT) rc=$?"
      stop "$pid" "$PORT_BAT"; }
  done
  LG "MODES 1s --flip-auto 1 refused: $(timeout 10 taskset -c "$SRV8" "$RED_BIN" --port "$PORT_BAT" --save '' --thread-mode 1s --flip-auto 1 2>&1 | head -1)"
  date >"$SP/fd-r2-fused-done.txt"
fi
if [ ! -s "$SP/fd-r2-differ.txt" ]; then
  wait_gate
  taskset -c 52-57,180-185 tests/differ_gate.sh "$RED_BIN" 8225 8226 "$SRV8" 6:2 >"$SP/fd-r2-differ.txt" 2>&1
  LG "DIFFER rc=$? :: $(tail -1 "$SP/fd-r2-differ.txt")"
fi

# ---- S7 report ---------------------------------------------------------------------------------
python3 scratch/report2.py "$SP/fd-report2.html" 2>&1 | tail -1
LG "ALL DONE"; touch "$SP/fd-r2.done"
