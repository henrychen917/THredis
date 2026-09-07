#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# One boot per battery on port 8300, cores 58-61,186-189 (the half of this lane's allocation no other
# lane's process can reach -- a
# correctness battery is not a timing measurement, so the server may use every core it owns).
# Kills by PID and guards the port before every boot: a leaked co-binding server would split
# traffic through SO_REUSEPORT and quietly turn a real defect into a pass.
set -u
BIN="${1:-./build/tomokv}"
MODE="${2:-1s}"
OUT="${3:-/tmp/ringsize-batteries-$MODE.txt}"
PORT=8300
CORES=58-63,186-191
CLI=/tmp/claude-1000/redis74/src/redis-cli
# THE 2s RATIO IS DERIVED FROM THE MASK, NOT WRITTEN DOWN. --ratio io:ex asks for io+ex threads and
# the server refuses to start more threads than it has allowed cpus ("--ratio: 16 threads but only
# 8 allowed cpus", which is exactly how the 2s battery died at 19:55 once this lane was confined to
# eight logical cpus). Keeping the base lane's 6:10 SHAPE and scaling it to whatever mask this lane
# actually holds means the battery follows the pin instead of contradicting it.
ncpus(){ python3 -c "
import sys
n=0
for part in sys.argv[1].split(','):
    a,_,b = part.partition('-')
    n += int(b)-int(a)+1 if b else 1
print(n)" "$1"; }
NCPU=$(ncpus "$CORES")
RIO=$(( NCPU * 6 / 16 )); [ "$RIO" -lt 1 ] && RIO=1
REX=$(( NCPU - RIO ));    [ "$REX" -lt 1 ] && REX=1
RATIO=${RATIO:-$RIO:$REX}
PASS=0; FAIL=0
: > "$OUT"

owners(){ ss -H -ltnp "sport = :$PORT" 2>/dev/null | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u | paste -sd, -; }
boot(){ # boot <args...>
  local o; o=$(owners)
  if [ -n "$o" ] || (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    echo "GUARD FAIL: port $PORT busy (pid=$o)" | tee -a "$OUT"; exit 1
  fi
  SRVLOG=$(mktemp /tmp/ringsize-bat-srv.XXXXXX)
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
  if timeout 900 python3 "tests/$t.py" 127.0.0.1 $PORT "${args[@]+"${args[@]}"}" > "/tmp/ringsize-$MODE-$t.txt" 2>&1; then
    echo "PASS  $MODE $t" | tee -a "$OUT"; PASS=$((PASS+1))
  else
    echo "FAIL  $MODE $t  (see /tmp/ringsize-$MODE-$t.txt)" | tee -a "$OUT"; FAIL=$((FAIL+1))
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
  ARM=(--thread-mode 2s --ratio "$RATIO" --enable-debug-command yes)
  for t in s6 ryow atomic_hazards multi_exec blocking blockmulti xscript expwide flip; do
    boot "${ARM[@]}" || exit 1; run "$t"; halt
  done
  boot "${ARM[@]}" || exit 1; run flip_under_load -- 20; halt
  boot "${ARM[@]}" || exit 1; run session_monotonic CONFIG SET atomic 1; halt
fi
echo "== $MODE: $PASS pass, $FAIL fail" | tee -a "$OUT"
