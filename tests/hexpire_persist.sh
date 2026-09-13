#!/bin/bash
# Out-of-process recovery driver for hash-field TTLs.
#   tests/hexpire_persist.sh <binary> <port> [taskset-cpus]
# Seeds deadlines, terminates the server, brings it back from a snapshot and then from an AOF, and
# checks the deadlines survived both. Deadlines are absolute, so "survived" means byte-identical.
set -u
BIN="${1:-./build/tomokv}"
PORT="${2:-7250}"
CPUS="${3:-}"
CLI="${CLI:-/tmp/claude-1000/redis74/src/redis-cli}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d /tmp/hexpire-persist.XXXXXX)"
PID=""
FAILED=0

run() { if [ -n "$CPUS" ]; then taskset -c "$CPUS" "$@"; else "$@"; fi; }

boot() {
  # SO_REUSEPORT means a leaked server silently splits connections with the new one; refuse to
  # start unless the port is genuinely free (epyc-server-leak-incident).
  if ss -ltn "sport = :$PORT" 2>/dev/null | grep -q ":$PORT"; then
    echo "GUARD: port $PORT already has a listener"; exit 1
  fi
  run "$BIN" --port "$PORT" --bind 127.0.0.1 --shards 4 --dir "$WORK" "$@" \
      >>"$WORK/server.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 60); do
    if "$CLI" -p "$PORT" ping 2>/dev/null | grep -q PONG; then
      # Take the pid from the listening socket, not from $!: a wrapper in between would leave the
      # real server running after the kill, and SO_REUSEPORT would then split the next boot.
      PID=$(ss -ltnp "sport = :$PORT" 2>/dev/null | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2)
      return 0
    fi
    sleep 0.2
  done
  echo "BOOT FAILED"; tail -20 "$WORK/server.log"; exit 1
}
stop() {
  [ -n "$PID" ] && kill "$PID" 2>/dev/null
  PID=""
  for _ in $(seq 1 40); do
    ss -ltn "sport = :$PORT" 2>/dev/null | grep -q ":$PORT" || return 0
    sleep 0.2
  done
  echo "GUARD: port $PORT did not free after kill"; exit 1
}
step() { if "$@"; then echo "  ok   $*"; else echo "  FAIL $*"; FAILED=1; fi; }

echo "== snapshot recovery =="
boot --dbfilename hexpire.tomo
DEADLINE=$(python3 "$ROOT/tests/hexpire.py" 127.0.0.1 "$PORT" persistbuild | sed -n 's/^PERSIST_DEADLINE=//p')
[ -n "$DEADLINE" ] || { echo "seed failed"; exit 1; }
"$CLI" -p "$PORT" SAVE >/dev/null || { echo "SAVE failed"; exit 1; }
stop
boot --load "$WORK/hexpire.tomo"
step python3 "$ROOT/tests/hexpire.py" 127.0.0.1 "$PORT" "persistcheck:$DEADLINE"
stop

echo "== AOF recovery =="
rm -f "$WORK"/*.tomo
boot --appendonly yes --appendfilename hexpire.aof
DEADLINE=$(python3 "$ROOT/tests/hexpire.py" 127.0.0.1 "$PORT" persistbuild | sed -n 's/^PERSIST_DEADLINE=//p')
sleep 1
stop
boot --appendonly yes --appendfilename hexpire.aof
step python3 "$ROOT/tests/hexpire.py" 127.0.0.1 "$PORT" "persistcheck:$DEADLINE"
stop

if [ "$FAILED" = 0 ]; then echo "hexpire persist: PASS ($WORK)"; else echo "hexpire persist: FAIL ($WORK)"; fi
exit "$FAILED"
