#!/bin/bash
# Snapshot, direct-AOF and rewritten-AOF recovery for a whole-key deadline, an elapsed-unreaped
# key and a hash-field deadline. Every boot re-reads the state that the previous one persisted, so
# a deadline that was written RELATIVE instead of absolute resurrects a key here.
# Usage: tests/edgetime_persist.sh [BINARY] [PORT] [CPUS]
set -u

BIN="${1:-./build/tomokv}"
PORT="${2:-7412}"
CPUS="${3:-32-37}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${CLI:-/tmp/claude-1000/redis74/src/redis-cli}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/edgetime-persist.XXXXXX")"
PID=""
FAILED=0

listener_pid() {
  ss -lntpH | awk -v port=":$PORT" '$4 ~ port "$" {
    if (match($0,/pid=[0-9]+/)) { print substr($0,RSTART+4,RLENGTH-4); exit }
  }'
}

boot() {
  if ss -lntH | grep -q ":$PORT "; then
    echo "GUARD: port $PORT already has a listener"
    exit 1
  fi
  taskset -c "$CPUS" "$BIN" --port "$PORT" --bind 127.0.0.1 --shards 16 --ratio 2:4 \
    --atomic 1 --enable-debug-command yes --protected-mode no "$@" \
    >>"$WORK/server.log" 2>&1 &
  for _ in $(seq 1 100); do
    PID="$(listener_pid)"
    if [ -n "$PID" ] && "$CLI" -p "$PORT" PING 2>/dev/null | grep -q PONG; then
      return 0
    fi
    sleep 0.1
  done
  echo "BOOT FAILED"
  tail -40 "$WORK/server.log"
  exit 1
}

stop() {
  if [ -n "$PID" ]; then
    current="$(listener_pid)"
    if [ "$current" != "$PID" ]; then
      echo "GUARD: listener pid changed from $PID to $current"
      exit 1
    fi
    kill "$PID"
  fi
  PID=""
  for _ in $(seq 1 100); do
    ss -lntH | grep -q ":$PORT " || return 0
    sleep 0.1
  done
  echo "GUARD: port $PORT did not release"
  exit 1
}

check_state() {
  if ! python3 "$ROOT/tests/edgetime.py" 127.0.0.1 "$PORT" "persistcheck:$1"; then
    FAILED=1
  fi
}

seed() {
  output="$(python3 "$ROOT/tests/edgetime.py" 127.0.0.1 "$PORT" persistbuild)" || exit 1
  echo "$output"
  DEADLINE="$(echo "$output" | sed -n 's/^PERSIST_DEADLINE=//p')"
  [ -n "$DEADLINE" ] || { echo "seed did not report a deadline"; exit 1; }
}

trap 'if [ -n "$PID" ]; then stop; fi' EXIT

echo "== snapshot recovery =="
SNAP="$WORK/snapshot"
mkdir -p "$SNAP"
boot --dir "$SNAP" --dbfilename edgetime.tomo
seed
"$CLI" -p "$PORT" SAVE >/dev/null || exit 1
stop
boot --dir "$SNAP" --load "$SNAP/edgetime.tomo"
check_state "$DEADLINE"
stop

echo "== direct AOF recovery =="
DIRECT="$WORK/aof-direct"
mkdir -p "$DIRECT"
boot --dir "$DIRECT" --appendonly yes --appendfilename edgetime.aof --appendfsync always
seed
stop
boot --dir "$DIRECT" --appendonly yes --appendfilename edgetime.aof --appendfsync always
check_state "$DEADLINE"
stop

echo "== rewritten AOF recovery =="
REWRITE="$WORK/aof-rewrite"
mkdir -p "$REWRITE"
boot --dir "$REWRITE" --appendonly yes --appendfilename edgetime.aof --appendfsync always
seed
"$CLI" -p "$PORT" BGREWRITEAOF >/dev/null || exit 1
rewrite_ok=0
for _ in $(seq 1 300); do
  info="$("$CLI" -p "$PORT" INFO PERSISTENCE)"
  requests="$(echo "$info" | sed -n 's/^aof_rewrite_requests:\([0-9][0-9]*\).*/\1/p')"
  completions="$(echo "$info" | sed -n 's/^aof_rewrite_completions:\([0-9][0-9]*\).*/\1/p')"
  in_progress="$(echo "$info" | sed -n 's/^aof_rewrite_in_progress:\([01]\).*/\1/p')"
  if [ "${requests:-0}" -ge 1 ] && [ "${completions:-0}" -ge 1 ] && \
     [ "${in_progress:-1}" -eq 0 ]; then
    rewrite_ok=1
    echo "rewrite fired: requests=$requests completions=$completions"
    break
  fi
  sleep 0.1
done
if [ "$rewrite_ok" -ne 1 ]; then
  echo "FAIL: AOF rewrite detector did not fire"
  FAILED=1
fi
stop
boot --dir "$REWRITE" --appendonly yes --appendfilename edgetime.aof --appendfsync always
check_state "$DEADLINE"
stop

if [ "$FAILED" -eq 0 ]; then
  echo "edgetime persistence: PASS ($WORK)"
else
  echo "edgetime persistence: FAIL ($WORK)"
fi
exit "$FAILED"
