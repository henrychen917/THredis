#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# rv.sh -- re-verification of the long-window-noise fix on the FINAL binary: gate row x2, directed
# cost-gate x1, and the three 120 s cells that exposed the defects (red-22 stationary: no spurious
# re-maneuver; red-31 / red8-71: one bounded move). Tags r3-*. Gate-pausing.
source /home/user/Projects/wt-flipdamp/scratch/lib.sh
cd "$WT"
LG(){ echo "$(date +%T) $*"; }
wait_gate(){ local n=0; while ! gate_ok; do [ $((n%10)) = 0 ] && LG "paused"; n=$((n+1)); sleep 30; done; }
SRV8=52,53,54,55,180,181,182,183; LG8=56,57,184,185
boot8(){ local SRV_CPUS=$SRV8; boot "$@"; }
LG "BIN $(sha256sum "$FIX_BIN" | cut -c1-16)"
for r in 1 2; do
  out=$SP/fd-r3-ctl-$r.txt; wait_gate
  pid=$(boot8 "$FIX_BIN" "$PORT_BAT" "r3ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1) || continue
  taskset -c "$LG8" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" --stable-seconds 30 >"$out" 2>&1; rc=$?
  echo "RC=$rc" >>"$out"; redis-cli -p "$PORT_BAT" debug flipctl >>"$out" 2>&1
  LG "CTL $r rc=$rc :: $(grep -E 'anchored off-rail|^ok:|AssertionError' "$out" | head -2 | tr '\n' ' ' | cut -c1-260)"
  stop "$pid" "$PORT_BAT"
done
out=$SP/fd-r3-costgate.txt; wait_gate
pid=$(boot "$FIX_BIN" "$PORT_HOLD" r3cg --ratio 3:1 --shards 64 --atomic 1 --flip-auto 1) && {
  taskset -c "$LG_CPUS" timeout 600 python3 tests/flip_cost_gate.py 127.0.0.1 "$PORT_HOLD" >"$out" 2>&1; rc=$?
  echo "RC=$rc" >>"$out"; redis-cli -p "$PORT_HOLD" debug flipctl >>"$out" 2>&1
  LG "COSTGATE rc=$rc :: $(grep -E '^[1-4]\. |^ok:|AssertionError' "$out" | tr '\n' ' ' | cut -c1-400)"
  stop "$pid" "$PORT_HOLD"; }
tm(){ local tag=$1 bin=$2 fa=$3 bio=$4 bex=$5 srv=${6:-$SRV_CPUS} lg=${7:-$LG_CPUS} lgt=${8:-$LG_THREADS}
  local out=$SP/fd-r3-tm-$tag.txt tr=$SP/fd-r3-tmtrace-$tag.txt secs=120 pid mt; wait_gate
  pid=$(SRV_CPUS=$srv boot "$bin" "$PORT_SIG" "r3tm-$tag" --ratio "$bio:$bex" --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  LG_CPUS=$lg preload "$PORT_SIG"
  ( t0=$(date +%s); while :; do tc=$(redis-cli -p "$PORT_SIG" info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p'); sp=$(redis-cli -p "$PORT_SIG" info server 2>/dev/null | tr -d '\r' | grep -E '^(io|ex)_threads:' | sed 's/.*://' | tr '\n' ':' | sed 's/:$//'); st=$(redis-cli -p "$PORT_SIG" info flipctl 2>/dev/null | tr -d '\r' | grep -E '^flipctl_(state|triggers|model_last_decision):' | sed 's/.*://' | tr '\n' '/' ); echo "$(( $(date +%s) - t0 )) ${tc:-0} ${sp:-0:0} ${st}"; sleep 1; done ) >"$tr" 2>&1 &
  local sampler=$!
  LG_CPUS=$lg LG_THREADS=$lgt ./scratch/mk.sh "$PORT_SIG" "$secs" >"$SP/fd-r3-tmmt-$tag.txt" 2>&1
  kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
  local info; info=$(redis-cli -p "$PORT_SIG" info all 2>/dev/null | tr -d '\r')
  redis-cli -p "$PORT_SIG" debug flipctl >"$SP/fd-r3-tmdbg-$tag.txt" 2>&1
  mt=$(tr '\r' '\n' <"$SP/fd-r3-tmmt-$tag.txt")
  echo "$tag boot=$bio:$bex fa=$fa memtier_rate=$(echo "$mt" | awk '/^Totals/{print $2}') p99=$(echo "$mt" | awk '/^Totals/{print $8}') flips=$(infog "$info" flip_completed) xfer=$(infog "$info" flip_clients_transferred) trig=$(infog "$info" flipctl_triggers) holds=$(infog "$info" flipctl_model_holds) decision=$(infog "$info" flipctl_model_last_decision) misses=$(infog "$info" flipctl_model_misses) kappa=$(infog "$info" flipctl_model_kappa) refine=$(infog "$info" flipctl_refine_decision)/$(infog "$info" flipctl_refine_steps) rate_band=$(infog "$info" flipctl_rate_band) live=$(infog "$info" io_threads):$(infog "$info" ex_threads) | $(python3 ./scratch/ttfm.py "$tr" "$bio" "$bex" | head -1)" >"$out"
  LG "TM $(cat "$out")"
  stop "$pid" "$PORT_SIG"
}
tm red-22 "$FIX_BIN" 1 2 2
tm red-31 "$FIX_BIN" 1 3 1
tm red8-71 "$FIX_BIN" 1 7 1 "$SRV8" "$LG8" 4
LG "RV DONE"; touch $SP/fd-r3.done
