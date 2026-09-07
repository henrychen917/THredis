# Boot/guard helpers for the rlbatch lane. Ports 8075-8078, cores 32-47 / 160-175 ONLY.
PORT=${PORT:-8075}
SRVCORE=${SRVCORE:-32}
CLICORE=${CLICORE:-33}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

port_owners(){ ss -H -ltnp "sport = :$1" 2>/dev/null | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u | paste -sd, -; }

guard_port(){ # a leaked co-binding server silently splits traffic via SO_REUSEPORT: refuse to boot
  local owners; owners=$(port_owners "$1")
  if [ -n "$owners" ] || (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null; then
    echo "GUARD: port $1 already in use (pid=$owners)" >&2; return 1
  fi
  return 0
}

boot_srv(){ # boot_srv <binary> <logfile> [extra args...] -> SRV pid
  local bin="$1" log="$2"; shift 2
  guard_port "$PORT" || return 1
  # EXTRA_ARGS lets a caller add boot flags (e.g. --lb 0) without a second copy
  # of this function; word splitting is intentional.
  # shellcheck disable=SC2086
  taskset -c "$SRVCORE" "$bin" --port "$PORT" --bind 127.0.0.1 --shards 16 \
      --thread-mode fused --read-local "${RL:-1}" ${EXTRA_ARGS:-} "$@" > "$log" 2>&1 &
  SRV=$!
  for _ in $(seq 150); do
    kill -0 "$SRV" 2>/dev/null || { wait "$SRV" 2>/dev/null; echo "BOOT DIED, see $log" >&2; return 1; }
    (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0
    sleep 0.2
  done
  echo "BOOT TIMEOUT, see $log" >&2; return 1
}

stop_srv(){ # kill by PID only; never pkill a pattern that can match this shell
  [ -n "${SRV:-}" ] && kill -TERM "$SRV" 2>/dev/null
  wait "$SRV" 2>/dev/null
  for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null || break; sleep 0.1; done
  SRV=
}
