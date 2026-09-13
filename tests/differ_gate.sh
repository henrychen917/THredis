#!/bin/bash
# Full-tier differential matrix against the pinned vanilla Redis 7.4 oracle.
# Usage: tests/differ_gate.sh TARGET_BIN TARGET_PORT ORACLE_PORT TARGET_CORES TARGET_RATIO
set -u
cd "$(dirname "$0")/.."

TARGET_BIN=${1:-./build/tomokv}
TARGET_PORT=${2:-${GATE_PORT:-7899}}
ORACLE_PORT=${3:-${GATE_DIFFER_ORACLE_PORT:-$((TARGET_PORT+1))}}
TARGET_CORES=${4:-${GATE_CORES:-0-7}}
TARGET_RATIO=${5:-${GATE_DIFFER_RATIO:-6:2}}
# TARGET GEOMETRY. `split` is the canonical production shape this matrix has always used: two
# thread roles at TARGET_RATIO with the read-local lane disarmed. `armed-fused` boots the same
# binary the other way -- one fused role, --read-local 1 -- because the whole read-local parse and
# resolve arm is UNREACHABLE from the split boot, so a green canonical matrix says nothing about
# it. The armed run adds a non-vacuity row for exactly that reason (see below) and skips the one
# suite that requires the split geometry to mean anything.
TARGET_GEOMETRY=${GATE_DIFFER_GEOMETRY:-split}
case "$TARGET_GEOMETRY" in
  split)       TARGET_SHAPE=(--ratio "$TARGET_RATIO");;
  armed-fused) TARGET_SHAPE=(--thread-mode fused --read-local 1);;
  *) echo "unknown GATE_DIFFER_GEOMETRY: $TARGET_GEOMETRY (want split|armed-fused)" >&2; exit 2;;
esac
ORACLE_CORES=${GATE_DIFFER_ORACLE_CORES:-$TARGET_CORES}
REDIS_ROOT=${REDIS74_ROOT:-/tmp/claude-1000/redis74}
ORACLE_BIN=${GATE_DIFFER_ORACLE_BIN:-$REDIS_ROOT/src/redis-server}
REDIS_CLI=${GATE_DIFFER_REDIS_CLI:-$REDIS_ROOT/src/redis-cli}
OUT=${GATE_DIFFER_OUT:-$(mktemp -d /tmp/gate-differ.XXXXXX)}
SEEDS=(7 19)
TARGET_PID=0
ORACLE_PID=0
BOOT_PID=0
PASS=0
FAIL=0

say(){ printf '  %-58s %s\n' "$1" "$2"; }

listeners(){
  ss -H -ltn "sport = :$1" 2>/dev/null
}

listener_pids(){
  ss -H -ltnp "sport = :$1" 2>/dev/null |
      sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u
}

port_free(){
  [ -z "$(listeners "$1")" ]
}

port_accepts(){
  (exec 3<>/dev/tcp/127.0.0.1/"$1"; exec 3<&-; exec 3>&-) 2>/dev/null
}

guard_port(){
  local port=$1 owner
  if port_free "$port"; then return 0; fi
  owner=$(listener_pids "$port" | paste -sd, -)
  say "port $port pre-boot guard" "REFUSE (already listening${owner:+; pid=$owner})"
  return 1
}

boot_owned(){ # label port cores logfile command...
  local label=$1 port=$2 cores=$3 logfile=$4 seen= failed_pid
  shift 4
  BOOT_PID=0
  guard_port "$port" || return 1
  taskset -c "$cores" "$@" >"$logfile" 2>&1 &
  BOOT_PID=$!
  for _ in $(seq 1 100); do
    if ! kill -0 "$BOOT_PID" 2>/dev/null; then
      wait "$BOOT_PID" 2>/dev/null
      say "$label boot" "FAIL (see $logfile)"
      BOOT_PID=0
      return 1
    fi
    if port_accepts "$port"; then
      seen=$(listener_pids "$port" | tr '\n' ' ')
      case " $seen " in
        *" $BOOT_PID "*) say "$label boot" "ok (pid=$BOOT_PID port=$port)"; return 0;;
      esac
    fi
    sleep 0.1
  done
  failed_pid=$BOOT_PID
  quiet_stop "$failed_pid" "$port"
  BOOT_PID=0
  say "$label boot" \
      "FAIL (listener pid ${seen:-unresolved} is not owned pid $failed_pid; see $logfile)"
  return 1
}

stop_owned(){ # label pid port
  local label=$1 pid=$2 port=$3 forced=0
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 100); do
      port_free "$port" && break
      sleep 0.1
    done
    if ! port_free "$port"; then
      forced=1
      kill -KILL "$pid" 2>/dev/null
    fi
  fi
  wait "$pid" 2>/dev/null
  if ! port_free "$port"; then
    say "$label stop" "FAIL (port $port still listening)"
    return 1
  fi
  if [ "$forced" -ne 0 ]; then
    say "$label stop" "FAIL (TERM timeout; exact pid $pid terminated)"
    return 1
  fi
  say "$label stop" "ok (port $port free)"
  return 0
}

quiet_stop(){ # exact owned pid only; used by the exit trap
  local pid=$1 port=$2
  if [ "$pid" -gt 0 ]; then
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null
      for _ in $(seq 1 100); do port_free "$port" && break; sleep 0.1; done
      port_free "$port" || kill -KILL "$pid" 2>/dev/null
    fi
    wait "$pid" 2>/dev/null
  fi
}

cleanup(){
  quiet_stop "$TARGET_PID" "$TARGET_PORT"
  quiet_stop "$ORACLE_PID" "$ORACLE_PORT"
}
trap cleanup EXIT

if ! [[ "$TARGET_PORT" =~ ^[0-9]+$ && "$ORACLE_PORT" =~ ^[0-9]+$ ]]; then
  echo "invalid differ gate ports" >&2
  exit 2
fi
if [ "$TARGET_PORT" = "$ORACLE_PORT" ]; then
  echo "target and oracle ports must differ" >&2
  exit 2
fi
if [ ! -x "$TARGET_BIN" ]; then
  echo "target binary is not executable: $TARGET_BIN" >&2
  exit 2
fi
if [ ! -x "$ORACLE_BIN" ]; then
  echo "pinned oracle is not executable: $ORACLE_BIN" >&2
  exit 2
fi
if [ ! -x "$REDIS_CLI" ]; then
  echo "pinned redis-cli is not executable: $REDIS_CLI" >&2
  exit 2
fi

DISCOVERED_SUITES=$(python3 tests/differ.py --list-generators) || {
  echo "failed to discover differ suites" >&2
  exit 2
}
readarray -t SUITES <<<"$DISCOVERED_SUITES"
if [ "${#SUITES[@]}" -eq 0 ]; then
  echo "differ suite discovery returned no suites" >&2
  exit 2
fi
printf 'DIFFER suites (%d): %s\n' "${#SUITES[@]}" "${SUITES[*]}"
printf 'DIFFER geometry: %s (%s)\n' "$TARGET_GEOMETRY" "${TARGET_SHAPE[*]}"
printf 'DIFFER matrix: atomic={0,1} seeds={%s,%s} legs=%d logs=%s\n' \
    "${SEEDS[0]}" "${SEEDS[1]}" "$((2 * ${#SEEDS[@]} * ${#SUITES[@]}))" "$OUT"

mkdir -p "$OUT/oracle"
ORACLE_LOG="$OUT/oracle.log"
boot_owned "vanilla Redis oracle" "$ORACLE_PORT" "$ORACLE_CORES" "$ORACLE_LOG" \
    env LC_ALL=C "$ORACLE_BIN" --port "$ORACLE_PORT" --bind 127.0.0.1 \
    --dir "$OUT/oracle" --dbfilename dump.rdb --appendonly no --save '' \
    --enable-debug-command yes || exit 1
ORACLE_PID=$BOOT_PID

ORACLE_INFO=$(
  "$REDIS_CLI" -h 127.0.0.1 -p "$ORACLE_PORT" --raw INFO server 2>/dev/null | tr -d '\r'
)
if ! grep -q '^redis_version:' <<<"$ORACLE_INFO" ||
   grep -Eq '^(tomokv_version|dragonfly_version):' <<<"$ORACLE_INFO"; then
  ID_LINES=$(grep -E '^(redis_version|tomokv_version|dragonfly_version):' <<<"$ORACLE_INFO" |
      paste -sd, -)
  say "oracle identity" "FAIL (${ID_LINES:-no recognized version fields})"
  stop_owned "oracle" "$ORACLE_PID" "$ORACLE_PORT" || true
  ORACLE_PID=0
  exit 1
fi
REDIS_VERSION=$(sed -n 's/^redis_version://p' <<<"$ORACLE_INFO" | head -1)
say "oracle identity" "ok (vanilla redis_version=$REDIS_VERSION)"

START_SECONDS=$SECONDS
for ATOMIC in 0 1; do
  TARGET_LOG="$OUT/target-atomic-$ATOMIC.log"
  # The oracle is persistence-silent above; give the target the same explicit save value so CONFIG
  # remains part of the differential surface instead of diverging by harness construction.
  if ! boot_owned "target atomic=$ATOMIC" "$TARGET_PORT" "$TARGET_CORES" "$TARGET_LOG" \
      "$TARGET_BIN" --port "$TARGET_PORT" --bind 127.0.0.1 --shards 16 \
      "${TARGET_SHAPE[@]}" --atomic "$ATOMIC" --save '' \
      --enable-debug-command yes; then
    FAIL=$((FAIL+1))
    break
  fi
  TARGET_PID=$BOOT_PID

  for SEED in "${SEEDS[@]}"; do
    for SUITE in "${SUITES[@]}"; do
      # The cross-owner SORT suite asserts the 6:2 thread split out of INFO and refuses any other
      # geometry, so under the fused boot it can only self-abort. It is fully covered by the split
      # run above; skipping it here is a harness constraint, stated rather than hidden, and it is
      # deliberately NOT counted as a pass.
      if [ "$TARGET_GEOMETRY" = armed-fused ] && [ "$SUITE" = sort ]; then
        say "differ sort (atomic=$ATOMIC seed=$SEED)" "SKIP (suite requires the split 6:2 geometry)"
        continue
      fi
      LEG="differ $SUITE (atomic=$ATOMIC seed=$SEED)"
      LEG_LOG="$OUT/$SUITE-a$ATOMIC-s$SEED.txt"
      if taskset -c "$TARGET_CORES" timeout 900 python3 tests/differ.py \
          127.0.0.1 "$TARGET_PORT" 127.0.0.1 "$ORACLE_PORT" "$SUITE" "$SEED" \
          >"$LEG_LOG" 2>&1; then
        say "$LEG" "ok ($(tail -n 1 "$LEG_LOG"))"
        PASS=$((PASS+1))
      else
        say "$LEG" "FAIL (see $LEG_LOG; $(tail -n 1 "$LEG_LOG"))"
        FAIL=$((FAIL+1))
      fi
    done
  done

  # The multi/atomic=1/seed=19 leg once caught a 1-in-many reader-vs-cleanup race (an EXISTS that
  # returned 2 of 3 committed keys) that no distilled stresser has reproduced since. One roll per
  # gate is a weak sentinel for a rare race, so this leg alone is repeated: the stream is
  # deterministic, so each repetition re-rolls the exact failing op's timing against the state the
  # earlier legs accumulated. Repeats are extra rolls of the SAME dice, not new coverage --
  # they intentionally do not appear in any expected-row ledger outside this script.
  if [ "$ATOMIC" -eq 1 ]; then
    for REP in $(seq "${GATE_DIFFER_MULTI_REPEATS:-4}"); do
      LEG="differ multi (atomic=1 seed=19 rep $REP)"
      LEG_LOG="$OUT/multi-a1-s19-rep$REP.txt"
      if taskset -c "$TARGET_CORES" timeout 900 python3 tests/differ.py \
          127.0.0.1 "$TARGET_PORT" 127.0.0.1 "$ORACLE_PORT" multi 19 \
          >"$LEG_LOG" 2>&1; then
        say "$LEG" "ok ($(tail -n 1 "$LEG_LOG"))"
        PASS=$((PASS+1))
      else
        say "$LEG" "FAIL (see $LEG_LOG; $(tail -n 1 "$LEG_LOG"))"
        FAIL=$((FAIL+1))
      fi
    done
  fi

  # NON-VACUITY, armed-fused only. This geometry exists to drive the matrix THROUGH the armed
  # read-local lane; a run in which that lane never served a single read proved nothing about it
  # and must not be reported as a pass. Read the counter out of the live target before it is
  # stopped and fail the leg when it is zero or missing.
  if [ "$TARGET_GEOMETRY" = armed-fused ]; then
    RL_INFO=$("$REDIS_CLI" -h 127.0.0.1 -p "$TARGET_PORT" --raw INFO all 2>/dev/null | tr -d '\r')
    RL_HITS=$(sed -n 's/^read_local_hits://p' <<<"$RL_INFO" | head -1)
    RL_FB=$(sed -n 's/^read_local_fallbacks://p' <<<"$RL_INFO" | head -1)
    if [ "${RL_HITS:-0}" -gt 0 ] 2>/dev/null; then
      say "read-local lane fired (atomic=$ATOMIC)" "ok (hits=$RL_HITS fallbacks=${RL_FB:-?})"
      PASS=$((PASS+1))
    else
      say "read-local lane fired (atomic=$ATOMIC)" "FAIL (hits=${RL_HITS:-unset}) -- vacuous run"
      FAIL=$((FAIL+1))
    fi
  fi

  stop_owned "target atomic=$ATOMIC" "$TARGET_PID" "$TARGET_PORT" || FAIL=$((FAIL+1))
  TARGET_PID=0
done

stop_owned "oracle" "$ORACLE_PID" "$ORACLE_PORT" || FAIL=$((FAIL+1))
ORACLE_PID=0
ELAPSED=$((SECONDS-START_SECONDS))
printf 'DIFFER GATE: pass=%d fail=%d runtime=%dm%02ds\n' \
    "$PASS" "$FAIL" "$((ELAPSED/60))" "$((ELAPSED%60))"
exit $((FAIL > 0))
