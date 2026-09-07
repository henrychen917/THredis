#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# One boot per battery, port 8075, cores 32-47,160-175. Kills by PID and guards the port before
# every boot: a leaked co-binding server would split traffic through SO_REUSEPORT and quietly turn
# a real defect into a pass.
set -u
BIN="${1:-./build/tomokv}"
MODE="${2:-1s}"
OUT="${3:-/tmp/rlbatch-batteries-$MODE.txt}"
PORT=8075
CORES=32-47,160-175
CLI=/tmp/claude-1000/redis74/src/redis-cli
PASS=0; FAIL=0
: > "$OUT"

owners(){ ss -H -ltnp "sport = :$PORT" 2>/dev/null | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u | paste -sd, -; }
boot(){ # boot <args...>
  local o; o=$(owners)
  if [ -n "$o" ] || (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    echo "GUARD FAIL: port $PORT busy (pid=$o)" | tee -a "$OUT"; exit 1
  fi
  SRVLOG=$(mktemp /tmp/rlbatch-bat-srv.XXXXXX)
  taskset -c "$CORES" "$BIN" --port $PORT --bind 127.0.0.1 --shards 16 "$@" > "$SRVLOG" 2>&1 &
  SRV=$!
  for _ in $(seq 150); do
    kill -0 "$SRV" 2>/dev/null || { wait "$SRV" 2>/dev/null; echo "BOOT DIED ($*) see $SRVLOG" | tee -a "$OUT"; return 1; }
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0; sleep 0.2
  done
  echo "BOOT TIMEOUT ($*) see $SRVLOG" | tee -a "$OUT"; return 1
}
halt(){ kill -TERM "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null
        for _ in $(seq 60); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null || break; sleep 0.1; done; sleep 0.5; }
run(){ # run <test> [-- test-args...] [pre-command...]
  local t=$1; shift
  local args=()
  if [ "${1:-}" = "--" ]; then shift; while [ $# -gt 0 ] && [ "$1" != "::" ]; do args+=("$1"); shift; done
     [ "${1:-}" = "::" ] && shift; fi
  [ $# -gt 0 ] && $CLI -p $PORT "$@" >/dev/null 2>&1
  if timeout 900 python3 "tests/$t.py" 127.0.0.1 $PORT "${args[@]+"${args[@]}"}" > "/tmp/rlbatch-$MODE-$t.txt" 2>&1; then
    echo "PASS  $MODE $t" | tee -a "$OUT"; PASS=$((PASS+1))
  else
    echo "FAIL  $MODE $t  (see /tmp/rlbatch-$MODE-$t.txt)" | tee -a "$OUT"; FAIL=$((FAIL+1))
  fi
}

if [ "$MODE" = 1s ]; then
  ARM=(--thread-mode fused --read-local 1 --enable-debug-command yes)
  for t in s6 ryow atomic_hazards multi_exec blocking blockmulti xscript expwide; do
    boot "${ARM[@]}" || exit 1; run "$t"; halt
  done
  boot "${ARM[@]}" || exit 1; run session_monotonic CONFIG SET atomic 1; halt
  boot --thread-mode fused --atomic 1 --read-local 1 \
       --enable-debug-command yes || exit 1
  run bplus; halt
else
  ARM=(--thread-mode 2s --ratio 6:10 --enable-debug-command yes)
  for t in s6 ryow atomic_hazards multi_exec blocking blockmulti xscript expwide flip; do
    boot "${ARM[@]}" || exit 1; run "$t"; halt
  done
  boot "${ARM[@]}" || exit 1; run flip_under_load -- 20; halt
  boot "${ARM[@]}" || exit 1; run session_monotonic CONFIG SET atomic 1; halt
fi
echo "== $MODE: $PASS pass, $FAIL fail" | tee -a "$OUT"
