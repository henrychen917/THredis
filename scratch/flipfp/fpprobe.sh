#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# fpprobe.sh -- VACUITY-PROOF liveness + detection test for the sampled fingerprint.
#
# The accuracy cells left a question the perf numbers cannot answer: a detector that never
# observes anything is both cheap and silent, and "no false triggers" is exactly what a DEAD
# fingerprint reports. So this cell asserts the gate OPENS.
#
#   Leg A (liveness): stationary single-key load, learned band. After the controller anchors,
#     DEBUG FLIPCTL signature_jitter/band must be NONZERO -- observe() only ever runs on a
#     published window, so jitter>0 proves the sampled writer published and the detector read it.
#   Leg B (detection): with the anchor held, swap the workload to 8-key MGET/MSET -- a maximal
#     signature shift (class family, keys_per_multikey, value_bytes/command, pass depth). The
#     fingerprint trigger counter must move off zero.
#
# Both legs run for each arm.  fpprobe.sh <binary> <tag>
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
BIN=$1; TAG=$2; W=${W:-}
PORT=$PORT_ON
OUT=$FP/fp-$TAG.txt
SRV_CPUS=$SRV4; LG_CPUS=$LG4
extra=""; [ -n "$W" ] && extra="--flip-work-window $W"
pid=$(SRV_CPUS=$SRV4 boot "$BIN" "$PORT" "fp-$TAG" --ratio 2:2 --shards 64 --atomic 0 \
        --flip-auto 1 $extra) || exit 1
LG_CPUS=$LG4 preload "$PORT" 4

# ---- Leg A: stationary load until anchored, then read the detector's learned state -------------
LG_CPUS=$LG4 sk_load "$PORT" 150 4 1:1 >"$FP/fp-$TAG-A.mt" 2>&1 &
load=$!
anchored=no
for i in $(seq 130); do
  st=$(infog "$(flipinfo $PORT)" flipctl_state)
  [ "$st" = anchored ] && { anchored=yes; break; }
  sleep 1
done
dbgA=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r')
dbgg(){ echo "$1" | sed -n "s/^$2=//p" | head -1; }
jit=$(dbgg "$dbgA" signature_jitter); band=$(dbgg "$dbgA" signature_band)
dist=$(dbgg "$dbgA" signature_distance)
fpA=$(infog "$(flipinfo $PORT)" flipctl_fingerprint_triggers)
wait $load 2>/dev/null

# ---- Leg B: maximal signature shift on the held anchor ------------------------------------------
LG_CPUS=$LG4 mk_load "$PORT" 90 4 >"$FP/fp-$TAG-B.mt" 2>&1 &
load=$!
fpB=$fpA
for i in $(seq 90); do
  fpB=$(infog "$(flipinfo $PORT)" flipctl_fingerprint_triggers)
  [ "${fpB:-0}" -gt "${fpA:-0}" ] && break
  sleep 1
done
dbgB=$($CLI -p $PORT debug flipctl 2>&1 | tr -d '\r')
wait $load 2>/dev/null
info=$(flipinfo $PORT)

{ echo "$TAG bin=$(basename "$BIN") W=${W:-default} anchored=$anchored"
  echo "  LEG A (liveness): jitter=$jit band=$band distance=$dist fp_triggers=$fpA rate=$(mt_rate "$FP/fp-$TAG-A.mt")"
  echo "  LEG B (detection): fp_triggers=$fpB (was $fpA) shift_distance=$(dbgg "$dbgB" last_shift_distance) shift_band=$(dbgg "$dbgB" last_shift_band) bandB=$(dbgg "$dbgB" signature_band) rate=$(mt_rate "$FP/fp-$TAG-B.mt")"
  echo "  final: state=$(infog "$info" flipctl_state) triggers=$(infog "$info" flipctl_triggers) fp=$(infog "$info" flipctl_fingerprint_triggers)"
  echo "--- debug A ---"; echo "$dbgA"
  echo "--- debug B ---"; echo "$dbgB"; } >"$OUT"
head -4 "$OUT"
stop "$pid" "$PORT"
