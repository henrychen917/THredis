#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# rv3.sh -- build + verify the fingerprint-band floor: STABLE HOLD (tests/flipctl.py, whose stable
# phase is the row that was failing) x3 with the derived band, the directed
# battery, and the two wrong-split boots + the stationary cell (must still move / must not move).
source /home/user/Projects/wt-flipdamp/scratch/lib.sh; cd "$WT"
LG(){ echo "$(date +%T) $*"; }
wait_gate(){ local n=0; while ! gate_ok; do [ $((n%10)) = 0 ] && LG "paused"; n=$((n+1)); sleep 30; done; }
SRV8=52,53,54,55,180,181,182,183; LG8=56,57,184,185
boot8(){ local SRV_CPUS=$SRV8; boot "$@"; }
wait_gate
taskset -c 52-57,180-185 make -j12 >"$SP/fd-r6-build.txt" 2>&1; rc=$?
LG "BUILD rc=$rc diag=$(grep -c -E 'error|warning' "$SP/fd-r6-build.txt") sha=$(sha256sum "$FIX_BIN" | cut -c1-16)"
[ $rc = 0 ] || exit 1
taskset -c 52-57,180-185 make unit >"$SP/fd-r6-unit.txt" 2>&1; LG "UNIT rc=$?"
# STABLE HOLD x3 -- the failing row. Report band, distance and ratio whatever the verdict.
for r in 1 2 3; do
  out=$SP/fd-r6-ctl-$r.txt; wait_gate
  pid=$(boot8 "$FIX_BIN" "$PORT_BAT" "r6ctl-$r" --ratio 6:2 --atomic 0 --flip-auto 1) || continue
  taskset -c "$LG8" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" --stable-seconds 30 >"$out" 2>&1; rc=$?
  echo "RC=$rc" >>"$out"; redis-cli -p "$PORT_BAT" debug flipctl >>"$out" 2>&1
  LG "CTL $r rc=$rc :: $(grep -E 'stable hold|anchored off-rail|^ok:|AssertionError' "$out" | head -2 | tr '\n' ' ' | cut -c1-200) :: $(grep -E '^signature_(band|distance|noise_bound)|^last_trigger' "$out" | tr '\n' ' ')"
  stop "$pid" "$PORT_BAT"
done
out=$SP/fd-r6-costgate.txt; wait_gate
pid=$(boot "$FIX_BIN" "$PORT_HOLD" r6cg --ratio 3:1 --shards 64 --atomic 1 --flip-auto 1) && {
  taskset -c "$LG_CPUS" timeout 600 python3 tests/flip_cost_gate.py 127.0.0.1 "$PORT_HOLD" >"$out" 2>&1
  LG "COSTGATE rc=$? :: $(grep -E '^ok:|AssertionError' "$out" | tail -1 | cut -c1-140)"; stop "$pid" "$PORT_HOLD"; }
for tag in red-a red-b; do
  out=$SP/fd-r6-hold-$tag.txt; wait_gate; ./scratch/hold.sh "$FIX_BIN" >"$out" 2>&1
  LG "HOLD $tag :: $(grep -E '^ok:|AssertionError' "$out" | tail -1 | cut -c1-160)"
done
tm(){ local tag=$1 bio=$2 bex=$3 srv=${4:-$SRV_CPUS} lg=${5:-$LG_CPUS} lgt=${6:-$LG_THREADS}
  local out=$SP/fd-r6-tm-$tag.txt tr=$SP/fd-r6-tmtrace-$tag.txt secs=120 pid mt; wait_gate
  pid=$(SRV_CPUS=$srv boot "$FIX_BIN" "$PORT_SIG" "r6tm-$tag" --ratio "$bio:$bex" --shards 64 --atomic 1 --flip-auto 1) || return 1
  LG_CPUS=$lg preload "$PORT_SIG"
  ( t0=$(date +%s); while :; do tc=$(redis-cli -p "$PORT_SIG" info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p'); sp=$(redis-cli -p "$PORT_SIG" info server 2>/dev/null | tr -d '\r' | grep -E '^(io|ex)_threads:' | sed 's/.*://' | tr '\n' ':' | sed 's/:$//'); echo "$(( $(date +%s) - t0 )) ${tc:-0} ${sp:-0:0}"; sleep 1; done ) >"$tr" 2>&1 &
  local sampler=$!
  LG_CPUS=$lg LG_THREADS=$lgt ./scratch/mk.sh "$PORT_SIG" "$secs" >"$SP/fd-r6-tmmt-$tag.txt" 2>&1
  kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
  local info; info=$(redis-cli -p "$PORT_SIG" info all 2>/dev/null | tr -d '\r')
  redis-cli -p "$PORT_SIG" debug flipctl >"$SP/fd-r6-tmdbg-$tag.txt" 2>&1
  mt=$(tr '\r' '\n' <"$SP/fd-r6-tmmt-$tag.txt")
  echo "$tag boot=$bio:$bex rate=$(echo "$mt" | awk '/^Totals/{print $2}') flips=$(infog "$info" flip_completed) xfer=$(infog "$info" flip_clients_transferred) trig=$(infog "$info" flipctl_triggers) fp_trig=$(infog "$info" flipctl_fingerprint_triggers) decision=$(infog "$info" flipctl_model_last_decision) refine=$(infog "$info" flipctl_refine_decision)/$(infog "$info" flipctl_refine_steps) sig_band=$(infog "$info" flipctl_signature_band) live=$(infog "$info" io_threads):$(infog "$info" ex_threads) | $(python3 ./scratch/ttfm.py "$tr" "$bio" "$bex" | head -1)" >"$out"
  LG "TM $(cat "$out")"; stop "$pid" "$PORT_SIG"
}
tm red-22 2 2
tm red-31 3 1
tm red8-71 7 1 "$SRV8" "$LG8" 4
LG "RV3 DONE"; touch $SP/fd-r6.done
