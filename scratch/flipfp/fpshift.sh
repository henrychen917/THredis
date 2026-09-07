#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# fpshift.sh -- RATE-NEUTRAL workload shift: does the fingerprint still detect a mix change?
#
# fpprobe's leg B swapped single-key 1:1 for 8-key MGET/MSET, which collapses the command rate ~9x;
# the rate detector wins that race in both arms, so it cannot attribute detection to the fingerprint.
# Here the command RATE is pinned with --rate-limiting on both sides of the shift and only the CLASS
# changes: phase A is GET-only (class Read, value_bytes/command 0), phase B is SET-only (class Write,
# value_bytes/command 32). Same connections, same pipeline, same per-connection rate. A fingerprint
# trigger in phase B is then attributable to the signature, not to the rate.
#
#   fpshift.sh <binary> <tag>       env: RL (per-connection ops/s, default 4000)
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
BIN=$1; TAG=$2; RL=${RL:-4000}
PORT=$PORT_ON
OUT=$FP/fps-$TAG.txt
dbgg(){ echo "$1" | sed -n "s/^$2=//p" | head -1; }
lim_load(){ # lim_load PORT SECS RATIO OUTFILE
  taskset -c "$LG4" memtier_benchmark -s 127.0.0.1 -p "$1" --protocol=redis -t 4 -c 32 --pipeline=32 \
    --ratio="$3" --key-pattern=R:R -d 32 --key-minimum=1 --key-maximum=$KEYMAX --test-time="$2" \
    --rate-limiting=$RL --distinct-client-seed --hide-histogram >"$4" 2>&1
}
pid=$(SRV_CPUS=$SRV4 boot "$BIN" "$PORT" "fps-$TAG" --ratio 2:2 --shards 64 --atomic 0 \
        --flip-auto 1) || exit 1
LG_CPUS=$LG4 preload "$PORT" 4

# ---- phase A: GET only, rate pinned, until anchored -------------------------------------------
lim_load "$PORT" 150 0:1 "$FP/fps-$TAG-A.mt" &
load=$!; anchored=no
for i in $(seq 140); do
  [ "$(infog "$(flipinfo $PORT)" flipctl_state)" = anchored ] && { anchored=yes; break; }; sleep 1
done
dbgA=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r'); infoA=$(flipinfo $PORT)
fpA=$(infog "$infoA" flipctl_fingerprint_triggers); trA=$(infog "$infoA" flipctl_triggers)
wait $load 2>/dev/null

# ---- phase B: SET only at the SAME pinned rate -------------------------------------------------
lim_load "$PORT" 100 1:0 "$FP/fps-$TAG-B.mt" &
load=$!; fpB=$fpA
for i in $(seq 100); do
  fpB=$(infog "$(flipinfo $PORT)" flipctl_fingerprint_triggers)
  [ "${fpB:-0}" -gt "${fpA:-0}" ] && break; sleep 1
done
dbgB=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r')
wait $load 2>/dev/null
infoB=$(flipinfo $PORT)
rA=$(mt_rate "$FP/fps-$TAG-A.mt"); rB=$(mt_rate "$FP/fps-$TAG-B.mt")
{ echo "$TAG bin=$(basename "$BIN") anchored=$anchored rateA=$rA rateB=$rB rate_delta=$(awk -v a="$rA" -v b="$rB" 'BEGIN{if(a>0)printf "%+.1f%%",(b-a)*100/a}')"
  echo "  A(GET-only, anchored): band=$(dbgg "$dbgA" signature_band) jitter=$(dbgg "$dbgA" signature_jitter) triggers=$trA fp=$fpA"
  echo "  B(SET-only, same rate): fp=$fpB (was $fpA) triggers=$(infog "$infoB" flipctl_triggers) surge=$(infog "$infoB" flipctl_rate_surge_triggers) collapse=$(infog "$infoB" flipctl_rate_collapse_triggers) last=$(infog "$infoB" flipctl_last_trigger)"
  echo "     shift_distance=$(dbgg "$dbgB" last_shift_distance) shift_band=$(dbgg "$dbgB" last_shift_band)"
  echo "--- debug A ---"; echo "$dbgA"; echo "--- debug B ---"; echo "$dbgB"; } >"$OUT"
head -4 "$OUT"
stop "$pid" "$PORT"
