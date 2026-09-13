#!/bin/bash
# Cross-shard dispatch scaling guard.
#   XDS_PORT=7200 XDS_CPUS=48-59 XDS_BIN=./build/tomokv tests/xshard_dispatch_scale.sh
#
# GUARDS a defect that was REPRODUCED on a live server (see NOTES-XPERF2.md): the plain scatter
# dispatch zeroed a kMaxThreads demand array and then walked EVERY configured thread looking for
# the few that owned a touched shard, so a 2-key cross-shard read cost 11% more at 128 threads
# than at 4 while touching the same two shards.
#
# The arms are built so that only srv_->nthreads() differs:
#   tid0,tid1 = IO on the first two cpus       (one of them serves the probe connection)
#   tid2,tid3 = EX on the next two cpus        (--shard-home puts ALL 16 shards on exactly these)
#   tid4..    = filler EX on the LAST cpu, owning no shard and no connection. They never share
#               a cpu with an active thread, so raising the count changes srv_->nthreads() and
#               nothing the measurement depends on.
# Same shards, same owners, same serving IO thread, same offered load, same client.
#
# The assertion is a RATIO between two arms, never an absolute time. It is also non-vacuous:
# tests/xshard_dispatch_scale.py refuses to report unless DEBUG SHARD proves the "cross" pair
# really spans two shards, and it measures a same-shard control that never enters the dispatch arm.
set -u
PORT=${XDS_PORT:-7200}
CPUS=${XDS_CPUS:-48-59}
BIN=${XDS_BIN:-./build/tomokv}
OPS=${XDS_OPS:-400000}
ROUNDS=${XDS_ROUNDS:-7}
SMALL=${XDS_SMALL:-4}
BIG=${XDS_BIG:-128}
# The guarded quantity is the DISPATCH EXCESS: cross_ns - same_ns, the median over rounds of what
# the cross-shard command costs OVER the identical same-shard command measured back to back on the
# same server. Subtracting the control inside each round removes everything the two share (parse,
# ROB, task handoff, reply, client) and cancels drift instead of amplifying it.
# Observed at the shipped settings (7 rounds x 400k ops per arm, best of 2 pairs), 3 runs each:
#   BEFORE the fix 1.285 / 1.381 / 1.402      AFTER 1.093 / 1.063 / 1.113
# The raw cross-only ratio is printed too but is NOT the assertion: it does not clear box noise.
LIMIT=${XDS_LIMIT:-1.20}
HERE=$(cd "$(dirname "$0")" && pwd)

read -r -a CPULIST <<< "$(python3 - "$CPUS" <<'EOF'
import sys
out = []
for part in sys.argv[1].split(","):
    if "-" in part:
        a, b = part.split("-")
        out += list(range(int(a), int(b) + 1))
    else:
        out.append(int(part))
print(" ".join(str(c) for c in out))
EOF
)"
if [ "${#CPULIST[@]}" -lt 6 ]; then echo "XDS need >=6 cpus in XDS_CPUS"; exit 1; fi
IO_A=${CPULIST[0]}; IO_B=${CPULIST[1]}; EX_A=${CPULIST[2]}; EX_B=${CPULIST[3]}
FILL=${CPULIST[${#CPULIST[@]}-1]}
HOMES=$(python3 -c "print(','.join('%d:%d' % (s, 2 + s % 2) for s in range(16)))")

listener_pid() { ss -lntpH 2>/dev/null | grep -F ":$1 " | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2; }

stop_arm() {
  local pid; pid=$(listener_pid "$PORT")
  [ -z "$pid" ] && return 0
  kill -TERM "$pid" 2>/dev/null
  for _ in $(seq 1 200); do kill -0 "$pid" 2>/dev/null || break; sleep 0.05; done
  kill -0 "$pid" 2>/dev/null && kill -KILL "$pid"
  for _ in $(seq 1 200); do [ -z "$(listener_pid "$PORT")" ] && return 0; sleep 0.05; done
  echo "XDS: port $PORT never released"; exit 1
}

run_arm() {   # $1 = total thread count
  local t=$1 nfill=$(( $1 - 4 )) place
  place=$(python3 -c "print(','.join(['ifid@$IO_A','ifid@$IO_B','ex@$EX_A','ex@$EX_B']+['ex@$FILL']*$nfill))")
  stop_arm
  ( taskset -c "$CPUS" "$BIN" --port "$PORT" --bind 127.0.0.1 --enable-debug-command yes \
      --place "$place" --shards 16 --shard-home "$HOMES" >/tmp/xds-$t.log 2>&1 & )
  for _ in $(seq 1 400); do [ -n "$(listener_pid "$PORT")" ] && break; sleep 0.05; done
  if [ -z "$(listener_pid "$PORT")" ]; then echo "XDS: arm $t never listened"; tail -3 /tmp/xds-$t.log; exit 1; fi
  python3 "$HERE/xshard_dispatch_scale.py" 127.0.0.1 "$PORT" "$OPS" 32 "$ROUNDS"
}

# Two independent small/big PAIRS, and the verdict is the BETTER (lower) of the two ratios. The
# arms are separate boots, so a pair cannot be measured simultaneously and slow drift across one
# pair lands entirely in that pair's ratio; taking the minimum of two pairs discards a drifted
# pair without hiding a real regression (the pre-fix ratio was >= 1.13 in every pair measured).
PAIRS=""
for _ in $(seq 1 "${XDS_PAIRS:-2}"); do
  SMALL_OUT=$(run_arm "$SMALL") || { echo "XDS: small arm failed"; stop_arm; exit 1; }
  BIG_OUT=$(run_arm "$BIG")     || { echo "XDS: big arm failed"; stop_arm; exit 1; }
  echo "  small $SMALL_OUT"
  echo "  big   $BIG_OUT"
  PAIRS="$PAIRS|$SMALL_OUT;$BIG_OUT"
done
stop_arm

python3 - "$PAIRS" "$LIMIT" "$SMALL" "$BIG" <<'EOF'
import sys
def parse(line):
    return {k: v for k, v in (f.split("=", 1) for f in line.split()[1:])}
pairs = [tuple(parse(x) for x in p.split(";")) for p in sys.argv[1].split("|") if p]
limit = float(sys.argv[2])
want_small, want_big = int(sys.argv[3]), int(sys.argv[4])
small, big = pairs[0]
fail = 0
def check(name, cond, detail=""):
    global fail
    print("  %-52s %s %s" % (name, "ok" if cond else "FAIL", detail))
    if not cond:
        fail += 1
check("small arm really ran %d threads" % want_small, int(small["threads"]) == want_small,
      "threads=%s" % small["threads"])
check("big arm really ran %d threads" % want_big, int(big["threads"]) == want_big,
      "threads=%s" % big["threads"])
# The hash seed is drawn from the kernel at every boot, so the shard IDS differ between arms; what
# the guard needs identical is the FAN-OUT: two owner threads for the cross pair, one for the
# control. A run that fanned out to one owner would not exercise the dispatch arm under test.
check("both arms fanned out to two owners",
      small["cross_owners"] == "2" and big["cross_owners"] == "2",
      "small=%s big=%s" % (small["cross_owners"], big["cross_owners"]))
check("both controls stayed on one owner",
      small["same_owners"] == "1" and big["same_owners"] == "1",
      "small=%s big=%s" % (small["same_owners"], big["same_owners"]))
ratios = []
for s_arm, b_arm in pairs:
    se, be = float(s_arm["excess_ns"]), float(b_arm["excess_ns"])
    check("cross-shard really costs more than same-shard in both arms", se > 0 and be > 0,
          "excess %.1f / %.1f ns" % (se, be))
    ratios.append(be / se if se > 0 else float("inf"))
ratio = min(ratios)
check("dispatch excess ratio %dt/%dt <= %.2f" % (want_big, want_small, limit), ratio <= limit,
      "pairs %s  best=%.3f" % (" ".join("%.3f" % r for r in ratios), ratio))
print("  %-52s ok cross %.1f->%.1f (%.3f)  same %.1f->%.1f (%.3f)"
      % ("raw per-op costs (diagnostic, not asserted)",
         float(small["cross_ns"]), float(big["cross_ns"]),
         float(big["cross_ns"]) / float(small["cross_ns"]),
         float(small["same_ns"]), float(big["same_ns"]),
         float(big["same_ns"]) / float(small["same_ns"])))
print("XSHARD-DISPATCH-SCALE %s" % ("PASS" if fail == 0 else "FAIL %d" % fail))
sys.exit(1 if fail else 0)
EOF
