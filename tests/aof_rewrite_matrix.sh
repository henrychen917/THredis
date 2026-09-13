#!/bin/bash
# Complete rewrite/recovery matrix. Every running-server signal targets the unique listening PID.
set -eu
cd "$(dirname "$0")/.."

PORT=${GATE_PORT:-7955}
CORES=${GATE_CORES:-224-231}
NCORES=$(taskset -c "$CORES" nproc)
if [ "$NCORES" -ge 8 ]; then RATIO=4:4
else RATIO=$(((NCORES+1)/2)):$((NCORES-(NCORES+1)/2)); fi
CLI=${REDIS_CLI:-redis-cli}
PERSIST_IO=${PERSIST_IO:-uring}
ACTIVE_PID=

cleanup() {
  local stopped=0
  if [ -n "$ACTIVE_PID" ] && kill -0 "$ACTIVE_PID" 2>/dev/null; then
    kill -TERM "$ACTIVE_PID" 2>/dev/null || true
    wait "$ACTIVE_PID" 2>/dev/null || true
    wait_for_release || true
    stopped=1
  fi
  [ "$stopped" = 0 ] || sleep 5
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
  local directory=$1 atomic=$2 debug=$3 log=$4
  local boot_pid socket_pid
  local debug_args=()
  [ "$debug" = yes ] && debug_args=(--enable-debug-command yes)
  taskset -c "$CORES" ./build/tomokv --port "$PORT" --bind 127.0.0.1 \
    --shards 16 --ratio "$RATIO" --protected-mode no --atomic "$atomic" \
    --persist-io "$PERSIST_IO" --appendonly yes --appendfsync everysec \
    --dir "$directory" "${debug_args[@]}" \
    >"$log" 2>&1 &
  boot_pid=$!
  for _ in $(seq 1 100); do
    if "$CLI" -h 127.0.0.1 -p "$PORT" ping >/dev/null 2>&1; then
      socket_pid=$(listener_pid)
      [ "$(printf '%s\n' "$socket_pid" | sed '/^$/d' | wc -l)" -eq 1 ] || return 1
      ACTIVE_PID=$socket_pid
      return 0
    fi
    if ! kill -0 "$boot_pid" 2>/dev/null; then wait "$boot_pid" || true; return 1; fi
    sleep 0.1
  done
  return 1
}

stop_clean() {
  kill -TERM "$ACTIVE_PID"
  wait "$ACTIVE_PID"
  wait_for_release
  ACTIVE_PID=
}

stop_now() {
  kill -KILL "$ACTIVE_PID"
  wait "$ACTIVE_PID" 2>/dev/null || true
  wait_for_release
  ACTIVE_PID=
}

normal_atomic1=
for atomic in 0 1; do
  directory=$(mktemp -d "/tmp/gate-aof-rewrite-${atomic}.XXXXXX")
  state="$directory/state.json"
  boot_server "$directory" "$atomic" yes "$directory/server-1.log"
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" populate "$state" 512 >/dev/null
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" rewrite "$state" "$directory" >/dev/null
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" manifest "$directory" >/dev/null
  stop_clean
  boot_server "$directory" "$atomic" yes "$directory/server-2.log"
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" verify "$state" >/dev/null
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" manifest "$directory" >/dev/null
  [ "$("$CLI" -h 127.0.0.1 -p "$PORT" DEBUG LOADAOF)" = OK ]
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" verify "$state" >/dev/null
  if [ "$atomic" = 1 ]; then
    before=$("$CLI" -h 127.0.0.1 -p "$PORT" info persistence | tr -d '\r' |
             sed -n 's/^aof_groups_committed://p')
    args=(MSET)
    for index in $(seq 0 15); do args+=("gcmt:rewrite:$index" "value-$index"); done
    "$CLI" -h 127.0.0.1 -p "$PORT" "${args[@]}" >/dev/null
    after=$before
    for _ in $(seq 1 300); do
      after=$("$CLI" -h 127.0.0.1 -p "$PORT" info persistence | tr -d '\r' |
              sed -n 's/^aof_groups_committed://p')
      [ "$after" -gt "$before" ] && break
      sleep 0.01
    done
    [ "$after" -gt "$before" ]
    sleep 2
    normal_atomic1=$directory
  fi
  stop_clean
done

before_manifest_dir=
for stage in before-mark before-manifest after-manifest; do
  directory=$(mktemp -d "/tmp/gate-aof-rewrite-${stage}.XXXXXX")
  state="$directory/state.json"
  boot_server "$directory" 1 yes "$directory/server-1.log"
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" populate "$state" 256 >/dev/null
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" pause "$stage" >/dev/null
  marker="$directory/appendonlydir/debug-aof-rewrite-stage"
  observed=0
  for _ in $(seq 1 500); do
    if [ -f "$marker" ] && grep -q "$stage" "$marker"; then observed=1; break; fi
    sleep 0.02
  done
  [ "$observed" = 1 ]
  stop_now
  boot_server "$directory" 1 yes "$directory/server-2.log"
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" verify "$state" >/dev/null
  if [ "$stage" = after-manifest ]; then
    python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" manifest "$directory" >/dev/null
  fi
  [ "$stage" = before-manifest ] && before_manifest_dir=$directory
  stop_clean
done

for kind in manifest base record-length group-vector; do
  directory=$(mktemp -d "/tmp/gate-aof-rewrite-corrupt-${kind}.XXXXXX")
  cp -a "$normal_atomic1"/. "$directory"/
  python3 tests/aof_rewrite.py 127.0.0.1 "$PORT" corrupt "$directory" "$kind" >/dev/null
  if taskset -c "$CORES" ./build/tomokv --port "$PORT" --bind 127.0.0.1 \
      --shards 16 --ratio "$RATIO" --protected-mode no --appendonly yes \
      --persist-io "$PERSIST_IO" --appendfsync everysec \
      --dir "$directory" >"$directory/refusal.log" 2>&1; then
    exit 1
  fi
  grep -q "AOF load plan failed" "$directory/refusal.log"
done

interior=$(mktemp -d /tmp/gate-aof-rewrite-corrupt-interior.XXXXXX)
cp -a "$before_manifest_dir"/. "$interior"/
manifest="$interior/appendonlydir/appendonly.aof.manifest"
first_incr=$(sed -n 's/^file \([^ ]*\).*type i.*/\1/p' "$manifest" | head -1)
first_path="$interior/appendonlydir/$first_incr"
first_size=$(stat -c %s "$first_path")
truncate -s $((first_size-7)) "$first_path"
if taskset -c "$CORES" ./build/tomokv --port "$PORT" --bind 127.0.0.1 \
    --shards 16 --ratio "$RATIO" --protected-mode no --appendonly yes \
    --persist-io "$PERSIST_IO" --appendfsync everysec \
    --dir "$interior" >"$interior/refusal.log" 2>&1; then
  exit 1
fi
grep -q "truncated AOF tail" "$interior/refusal.log"

echo "AOF REWRITE MATRIX PASS: atomic=0/1 stages=3 corruptions=5"
