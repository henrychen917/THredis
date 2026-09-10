#!/bin/bash
# Live manual/automatic rewrite triggers, observability, limiter, and restart matrix.
set -eu
cd "$(dirname "$0")/.."

PORT=${GATE_PORT:-7955}
CORES=${GATE_CORES:-224-231}
NCORES=$(taskset -c "$CORES" nproc)
if [ "$NCORES" -ge 8 ]; then RATIO=4:4
else RATIO=$(((NCORES+1)/2)):$((NCORES-(NCORES+1)/2)); fi
CLI=${REDIS_CLI:-redis-cli}
BIN=${GATE_CANDIDATE_BINARY:-${TOMO_BIN:-./build/tomokv}}
NET_IO=${NET_IO:-uring}
ACTIVE_PID=
[ -z "${GATE_LOAD_CORES:-}" ] || taskset -pc "$GATE_LOAD_CORES" "$$" >/dev/null

cleanup() {
  if [ -n "$ACTIVE_PID" ] && kill -0 "$ACTIVE_PID" 2>/dev/null; then
    kill -TERM "$ACTIVE_PID" 2>/dev/null || true
    wait "$ACTIVE_PID" 2>/dev/null || true
    wait_for_release || true
  fi
}
trap cleanup EXIT

listener_pid() {
  ss -lntpH 2>/dev/null | sed -n "/:$PORT /s/.*pid=\\([0-9][0-9]*\\).*/\\1/p" | sort -u
}

wait_for_release() {
  for _ in $(seq 1 200); do
    [ -z "$(listener_pid)" ] && return 0
    sleep 0.05
  done
  return 1
}

boot_server() {
  local directory=$1 atomic=$2 log=$3
  local boot_pid socket_pid
  [ -z "$(listener_pid)" ] || { echo "AOF rewrite triggers: port $PORT already listening" >&2; return 1; }
  taskset -c "$CORES" "$BIN" --port "$PORT" --bind 127.0.0.1 \
    --shards 16 --ratio "$RATIO" --protected-mode no --atomic "$atomic" \
    --net-io "$NET_IO" --appendonly yes --appendfsync everysec \
    --auto-aof-rewrite-percentage 0 \
    --enable-debug-command yes --dir "$directory" >"$log" 2>&1 &
  boot_pid=$!
  ACTIVE_PID=$boot_pid
  for _ in $(seq 1 100); do
    if ! kill -0 "$boot_pid" 2>/dev/null; then
      wait "$boot_pid" 2>/dev/null || true
      ACTIVE_PID=
      return 1
    fi
    if "$CLI" -h 127.0.0.1 -p "$PORT" ping >/dev/null 2>&1; then
      socket_pid=$(listener_pid)
      [ "$socket_pid" = "$boot_pid" ] || return 1
      return 0
    fi
    sleep 0.1
  done
  return 1
}

stop_server() {
  kill -TERM "$ACTIVE_PID"
  wait "$ACTIVE_PID"
  wait_for_release
  ACTIVE_PID=
}

for atomic in 0 1; do
  directory=$(mktemp -d "${TMPDIR:-/tmp}/gate-aof-trigger-${atomic}.XXXXXX")
  state="$directory/state.json"
  boot_server "$directory" "$atomic" "$directory/server-1.log"
  python3 tests/aof_rewrite_triggers.py 127.0.0.1 "$PORT" run \
    "$state" "$directory" "$atomic" >"$directory/trigger.log" 2>&1
  stop_server
  boot_server "$directory" "$atomic" "$directory/server-2.log"
  python3 tests/aof_rewrite_triggers.py 127.0.0.1 "$PORT" verify \
    "$state" "$directory" "$atomic" >>"$directory/trigger.log" 2>&1
  stop_server
  if grep -q "AOF rewrite error" "$directory/server-2.log"; then exit 1; fi
done

echo "AOF REWRITE TRIGGER MATRIX PASS: atomic=0/1 live-config info auto backoff restart"
