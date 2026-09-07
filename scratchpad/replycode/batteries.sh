#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# One boot per battery, port 8079, cores 48-55. 1s runs ARMED (read-local + atomic filter).
set -u
cd /home/user/Projects/wt-replycode
S=/tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad
PORT=8420
CORES=58-63,186-191
BIN=${BIN:-./build/tomokv}
DIR=$(mktemp -d $S/bat.XXXXXX)
PASS=0; FAIL=0; FAILED=""

run(){ # MODE TEST
  local MODE=$1 T=$2 PID LOG=$S/bat-$1-$2.log OUT=$S/bat-$1-$2.txt
  if ss -H -ltn "sport = :$PORT" 2>/dev/null | grep -q .; then
     echo "  GUARD-REFUSE $MODE/$T (port busy)"; FAIL=$((FAIL+1)); FAILED="$FAILED $MODE/$T:guard"; return; fi
  if [ "$MODE" = 1s ]; then
    taskset -c $CORES $BIN --port $PORT --bind 127.0.0.1 --thread-mode 1s \
      --read-local 1 --atomic 1 --enable-debug-command yes \
      --dir "$DIR" >"$LOG" 2>&1 &
  else
    taskset -c $CORES $BIN --port $PORT --bind 127.0.0.1 --thread-mode 2s --ratio 6:2 \
      --atomic 1 --enable-debug-command yes --dir "$DIR" >"$LOG" 2>&1 &
  fi
  PID=$!
  local up=0
  for _ in $(seq 150); do
    kill -0 $PID 2>/dev/null || break
    if ss -H -ltnp "sport = :$PORT" 2>/dev/null | grep -q "pid=$PID,"; then up=1; break; fi
    sleep 0.2
  done
  if [ $up -eq 0 ]; then echo "  BOOTFAIL $MODE/$T (see $LOG)"; FAIL=$((FAIL+1)); FAILED="$FAILED $MODE/$T:boot"; kill -9 $PID 2>/dev/null; return; fi
  if timeout 900 python3 tests/$T.py 127.0.0.1 $PORT >"$OUT" 2>&1; then
     echo "  ok   $MODE $T"; PASS=$((PASS+1))
  else
     echo "  FAIL $MODE $T (see $OUT)"; FAIL=$((FAIL+1)); FAILED="$FAILED $MODE/$T"
  fi
  kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null
  for _ in $(seq 100); do ss -H -ltn "sport = :$PORT" 2>/dev/null | grep -q . || break; sleep 0.1; done
}

echo "=== 1s (armed) ==="
for t in s6 ryow bplus atomic_hazards multi_exec edgeproto edgeenc resp3 dumprestore pubsub aclreply; do run 1s $t; done
echo "=== 2s ==="
for t in s6 ryow atomic_hazards multi_exec edgeproto edgeenc resp3 dumprestore pubsub flip aclreply; do run 2s $t; done
echo "=== TOTAL pass=$PASS fail=$FAIL ==="
[ -n "$FAILED" ] && echo "FAILED:$FAILED"
exit 0
