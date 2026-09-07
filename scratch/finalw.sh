#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# finalw.sh -- RESUMABLE continuation of final.sh: runs only what is missing, pauses (never exits)
# while the box marker is held, then builds the report. Safe to run after final.sh ended any way.
source /home/user/Projects/wt-flipdamp/scratch/lib.sh
cd "$WT"
wait_gate() { while ! gate_ok; do echo "paused $(date +%T): box marker held"; sleep 30; done; }
CSV=$SP/fd-matrix2.csv
echo "=== RESUME $(date +%T)"
for wl in mk sk1:1 sk9:1 get; do
  # grep -c prints 0 AND exits 1 on no match, so `|| echo 0` used to append a SECOND 0 and the
  # arithmetic below died ("0\n0"): every workload after mk was silently skipped. Count the
  # rows from the CSV shape instead, which also cannot be fooled by a colon in the label.
  have=$(awk -F, -v w="$wl" 'NR>1 && $3==w {n++} END{print n+0}' "$CSV" 2>/dev/null)
  have=${have:-0}
  full=$((have / 4)); [ $((have % 4)) -ne 0 ] && echo "note: $wl has a partial round ($have rows); completing by full rounds"
  need=$((3 - full)); [ "$need" -le 0 ] && { echo "$wl complete ($have rows)"; continue; }
  echo "$wl: $have rows, running $need more round(s)"
  ROUND_OFFSET=$((full + 1)) ./scratch/abw.sh "$CSV" 40 "$need" "$wl" pol0a=$FIX_BIN:0 pol0b=$FIX_BIN:0 pol1=$FIX_BIN:1 base1=$BASE_BIN:1
done
echo "MATRIX2 COMPLETE $(date +%T)"
verdict() { grep -qE '^ok:|AssertionError|Traceback' "$1" 2>/dev/null; }
for spec in "pol-1:$FIX_BIN" "pol-2:$FIX_BIN" "base:$BASE_BIN"; do
  tag=${spec%%:*}; bin=${spec#*:}; out=$SP/fd-hold-$tag.txt; tries=0
  until verdict "$out" || [ $tries -ge 3 ]; do wait_gate; ./scratch/hold.sh "$bin" >"$out" 2>&1; tries=$((tries+1)); done
  echo "HOLD $tag :: $(grep -E '^ok:|^anchored|AssertionError' "$out" | tr '\n' ' ' | cut -c1-300)"
done
SRV_CPUS=52,53,54,55,180,181,182,183; LG_CPUS=56,57,184,185
if [ ! -s "$SP/fd-bat-flipttl.txt" ]; then
  wait_gate
  PID=$(boot "$FIX_BIN" "$PORT_BAT" bat --enable-debug-command yes) && {
    taskset -c "$LG_CPUS" python3 tests/flip.py 127.0.0.1 "$PORT_BAT" >"$SP/fd-bat-flip.txt" 2>&1; echo "flip.py rc=$? :: $(tail -1 "$SP/fd-bat-flip.txt" | cut -c1-120)"
    taskset -c "$LG_CPUS" python3 tests/flip_under_load.py 127.0.0.1 "$PORT_BAT" 20 >"$SP/fd-bat-flipload.txt" 2>&1; echo "flip_under_load.py rc=$? :: $(tail -1 "$SP/fd-bat-flipload.txt" | cut -c1-120)"
    taskset -c "$LG_CPUS" python3 tests/flip_ttl.py 127.0.0.1 "$PORT_BAT" >"$SP/fd-bat-flipttl.txt" 2>&1; echo "flip_ttl.py rc=$? :: $(tail -1 "$SP/fd-bat-flipttl.txt" | cut -c1-120)"
    stop "$PID" "$PORT_BAT"; }
else echo "batteries flip/flip_under_load/flip_ttl: on file"; fi
if [ ! -s "$SP/fd-bat-flipctl.txt" ]; then
  wait_gate
  PID=$(boot "$FIX_BIN" "$PORT_BAT" batctl --ratio 6:2 --atomic 0 --flip-auto 1) && {
    taskset -c "$LG_CPUS" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_BAT" --stable-seconds 30 >"$SP/fd-bat-flipctl.txt" 2>&1; echo "flipctl.py rc=$? :: $(grep -vE '^\s*$' "$SP/fd-bat-flipctl.txt" | tail -2 | tr '\n' ' ' | cut -c1-300)"
    redis-cli -p "$PORT_BAT" debug flipctl >"$SP/fd-bat-flipctl-dbg.txt" 2>&1; stop "$PID" "$PORT_BAT"; }
else echo "flipctl.py: on file :: $(grep -vE '^\s*$' "$SP/fd-bat-flipctl.txt" | tail -1 | cut -c1-200)"; fi
wait_gate
PID=$(boot "$FIX_BIN" "$PORT_BAT" fused --thread-mode 1s --flip-auto 1) && echo "fused --flip-auto 1 boots: $(redis-cli -p "$PORT_BAT" info flipctl | tr -d '\r' | grep flipctl_state)" && stop "$PID" "$PORT_BAT"
PID=$(boot "$FIX_BIN" "$PORT_BAT" split2 --ratio 2:2 --flip-auto 1) && echo "split --flip-auto 1 boots: $(redis-cli -p "$PORT_BAT" info flipctl | tr -d '\r' | grep flipctl_state)" && stop "$PID" "$PORT_BAT"
if [ ! -s "$SP/fd-differ.txt" ]; then
  wait_gate
  taskset -c 52-57,180-185 tests/differ_gate.sh "$FIX_BIN" 8225 8226 52,53,180,181 2:2 >"$SP/fd-differ.txt" 2>&1; echo "DIFFER rc=$? :: $(grep -cE 'ok|PASS' "$SP/fd-differ.txt") ok-lines, $(grep -ciE 'fail|mismatch' "$SP/fd-differ.txt") fail-lines :: $(tail -2 "$SP/fd-differ.txt" | tr '\n' ' ' | cut -c1-200)"
else echo "differ: on file :: $(tail -1 "$SP/fd-differ.txt" | cut -c1-200)"; fi
python3 scratch/report.py "$SP/fd-report.html" 2>&1 | tail -1
echo "ALL DONE $(date +%T)"; touch "$SP/fd-final2.done"
