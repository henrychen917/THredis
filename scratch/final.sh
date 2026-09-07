#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# final.sh -- everything the report needs, in priority order, each step gate-checked:
#   1 matrix2: pol0a pol0b pol1 base1 x mk sk1:1 sk9:1 get, 40s cells, 3 ABBA rounds
#   2 directed hold test: policy x2 (expect pass), base x1 (expect fail)
#   3 flip batteries (gate rows) on 8 server threads: flip.py, flip_under_load.py, flip_ttl.py, flipctl.py
#   4 both thread modes boot with --flip-auto 1
#   5 differ against the Redis 7.4 oracle
source /home/user/Projects/wt-flipdamp/scratch/lib.sh
cd "$WT"
step() { echo "=== $* $(date +%T)"; }
step MATRIX2
rm -f "$SP/fd-matrix2.csv"
for wl in ${WLS:-mk sk1:1 sk9:1 get}; do
  ./scratch/ab.sh "$SP/fd-matrix2.csv" 40 3 "$wl" pol0a=$FIX_BIN:0 pol0b=$FIX_BIN:0 pol1=$FIX_BIN:1 base1=$BASE_BIN:1 || { echo "MATRIX ABORT wl=$wl rc=$?"; exit 1; }
done
echo "MATRIX2 DONE"
step HOLD
for i in 1 2; do
  ./scratch/hold.sh "$FIX_BIN" >"$SP/fd-hold-pol-$i.txt" 2>&1; rc=$?
  echo "HOLD pol $i rc=$rc :: $(grep -E '^ok:|^anchored|AssertionError|GATE' "$SP/fd-hold-pol-$i.txt" | tr '\n' ' ' | cut -c1-300)"
done
./scratch/hold.sh "$BASE_BIN" >"$SP/fd-hold-base.txt" 2>&1; rc=$?
echo "HOLD base rc=$rc :: $(grep -E '^ok:|AssertionError|GATE' "$SP/fd-hold-base.txt" | head -1 | cut -c1-200)"
step BATTERIES
SRV_CPUS=52,53,54,55,180,181,182,183; LG_CPUS=56,57,184,185
require_gate || exit 3
PID=$(boot "$FIX_BIN" "$PORT_BAT" bat --enable-debug-command yes) || exit 2
taskset -c "$LG_CPUS" python3 tests/flip.py 127.0.0.1 "$PORT_BAT" >"$SP/fd-bat-flip.txt" 2>&1; echo "flip.py rc=$? :: $(tail -1 "$SP/fd-bat-flip.txt" | cut -c1-120)"
taskset -c "$LG_CPUS" python3 tests/flip_under_load.py 127.0.0.1 "$PORT_BAT" 20 >"$SP/fd-bat-flipload.txt" 2>&1; echo "flip_under_load.py rc=$? :: $(tail -1 "$SP/fd-bat-flipload.txt" | cut -c1-120)"
taskset -c "$LG_CPUS" python3 tests/flip_ttl.py 127.0.0.1 "$PORT_BAT" >"$SP/fd-bat-flipttl.txt" 2>&1; echo "flip_ttl.py rc=$? :: $(tail -1 "$SP/fd-bat-flipttl.txt" | cut -c1-120)"
stop "$PID" "$PORT_BAT"
require_gate || exit 3
PID=$(boot "$FIX_BIN" "$PORT_BAT" batctl --ratio 6:2 --atomic 0 --flip-auto 1) || exit 2
taskset -c "$LG_CPUS" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" --stable-seconds 30 >"$SP/fd-bat-flipctl.txt" 2>&1; echo "flipctl.py rc=$? :: $(grep -vE '^\s*$' "$SP/fd-bat-flipctl.txt" | tail -2 | tr '\n' ' ' | cut -c1-300)"
redis-cli -p "$PORT_BAT" debug flipctl >"$SP/fd-bat-flipctl-dbg.txt" 2>&1
stop "$PID" "$PORT_BAT"
step MODES
PID=$(boot "$FIX_BIN" "$PORT_BAT" fused --thread-mode 1s --flip-auto 1) && echo "fused --flip-auto 1 boots: $(redis-cli -p "$PORT_BAT" info flipctl | tr -d '\r' | grep flipctl_state)" && stop "$PID" "$PORT_BAT"
PID=$(boot "$FIX_BIN" "$PORT_BAT" split2 --ratio 2:2 --flip-auto 1) && echo "split --flip-auto 1 boots: $(redis-cli -p "$PORT_BAT" info flipctl | tr -d '\r' | grep flipctl_state)" && stop "$PID" "$PORT_BAT"
step DIFFER
require_gate || exit 3
taskset -c 52-57,180-185 tests/differ_gate.sh "$FIX_BIN" 8225 8226 52,53,180,181 2:2 >"$SP/fd-differ.txt" 2>&1; echo "DIFFER rc=$? :: $(grep -cE 'ok|PASS' "$SP/fd-differ.txt") ok-lines, $(grep -ciE 'fail|mismatch' "$SP/fd-differ.txt") fail-lines :: $(tail -2 "$SP/fd-differ.txt" | tr '\n' ' ' | cut -c1-200)"
echo "FINAL DONE $(date +%T)"; touch "$SP/fd-final.done"
