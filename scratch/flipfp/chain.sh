#!/bin/bash
# Boot grammar updated for derived controls; historical PRE/POST results need fresh validation.
# chain.sh -- t-flipfp proof chain. Resumable (every cell skips when its output file exists),
# gate-pausing (quiet.done age + intruders before every cell). Re-run to resume.
#   S0 digests            S1 slope cells PRE / POST / PRE-null (flip-auto 0) on 1T, 2T, 2s
#   S1b retired: the fixed-band controller cost experiment has no equivalent automatic-band arm.
#   S2 accuracy: anchored band + false triggers, homogeneous MK and heterogeneous SET|GET, PRE vs POST
#   S3 wrong-split boots 3:1 (4 thr) and 5:1 (6 thr): PRE-auto, POST-auto, flipguard, POST off
#   S4 flipctl.py gate row (POST x2, PRE x1)   S5 batteries 1s + 2s   S6 differ   S7 gate quick
#   S8 tables
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
cd "$WT" || exit 1
ONLY=${ONLY:-}
want(){ [ -z "$ONLY" ] || [[ ",$ONLY," == *",$1,"* ]]; }

# ---- S0 digests --------------------------------------------------------------------------------
if [ ! -s "$FP/digests.txt" ]; then
  for b in "$PRE_BIN" "$POST_BIN"; do [ -s "$b" ] || { LG "missing $b"; exit 1; }; done
  { for b in "$PRE_BIN" "$POST_BIN"; do echo "$(sha256sum "$b" | cut -c1-16) $(stat -c %s "$b") $(basename "$b")"; done
    echo "pre=$(git -C "$WT_BASE" rev-parse --short HEAD) post=$(git -C "$WT" rev-parse --short HEAD)"; } > "$FP/digests.txt"
  if [ "$(sha256sum "$PRE_BIN" | cut -c1-16)" = "$(sha256sum "$POST_BIN" | cut -c1-16)" ]; then LG "PRE==POST DIGEST -- arms identical"; exit 1; fi
  LG "DIGESTS $(tr '\n' ';' < "$FP/digests.txt")"
fi

# ---- S1 slope cells ----------------------------------------------------------------------------
slope(){ # slope TAG BIN GEOM [EXTRA] [SETTLE_S]
  local tag=$1 bin=$2 geom=$3 extra=${4:-} settle=${5:-0}
  local out=$FP/slope-$tag.csv
  if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
  [ -s "$out" ] && { LG "SLOPE $tag on file ($(wc -l < "$out") rows)"; return 0; }
  wait_gate
  local srv cli mode
  case $geom in
    1t) mode=1s; srv=176;     cli="49 50 51";;
    2t) mode=1s; srv=176-177; cli="50 51 178 179";;
    2s) mode=2s; srv=176-177; cli="50 51 178 179";;
    *) LG "bad geom $geom"; return 1;;
  esac
  LG "SLOPE $tag geom=$geom bin=$(basename "$bin") extra='$extra' settle=$settle"
  rm -f "$out.part"
  MODE=$mode RATIO=1:1 RL=1 ATOMIC=${ATOMIC:-1} SHARDS=64 SRVCORES=$srv CLICORES="$cli" CONNS=8 PORT=$PORT_SLOPE \
    N1=300000 N2=900000 PIPE=32 KEYLEN=16 VLEN=32 RINGS=4096 SHAPES="set_over get_hit" PMU_GROUPS="be beu rs" \
    FOREIGNMAX=5 SIBMAX=2 PASS_BUDGET_S=240 QUIET_WAIT_S=120 EXTRA="$extra" SETTLE_S=$settle \
    "$FP/cm/multi.sh" "$bin" "$tag" "$out.part" 2 >>"$FP/slope-$tag.log" 2>&1; local rc=$?
  [ $rc = 0 ] && mv "$out.part" "$out"
  LG "SLOPE $tag rc=$rc rows=$(wc -l < "$out" 2>/dev/null) :: $(grep -E 'settle:|end:' "$FP/slope-$tag.log" | tail -2 | tr '\n' ' ' | cut -c1-300)"
}
if want S1; then
  for geom in 1t 2t 2s; do
    slope "pre-$geom"     "$PRE_BIN"  "$geom"
    slope "post-$geom"    "$POST_BIN" "$geom"
    slope "prenull-$geom" "$PRE_BIN"  "$geom"
  done
fi
# S1b deliberately emits no measurements; its historical tables remain readable in S8.

# ---- S2 accuracy: anchored band + false triggers on a stationary mix ---------------------------
acc(){ # acc TAG BIN KIND(mk|hetero)
  local tag=$1 bin=$2 kind=$3
  local out=$FP/acc-$tag.txt
  if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
  [ -s "$out" ] && { LG "ACC $tag on file :: $(head -1 "$out")"; return 0; }
  wait_gate
  local pid; pid=$(SRV_CPUS=$SRV4 boot "$bin" "$PORT_ON" "acc-$tag" --ratio 2:2 --shards 64 --atomic 0 --flip-auto 1) || return 1
  LG_CPUS=$LG4 preload "$PORT_ON" 4
  local t0; t0=$(date +%s)
  ( while :; do echo "$(( $(date +%s) - t0 )) $(flipinfo $PORT_ON | grep -E '^flipctl_(state|triggers|fingerprint_triggers|signature_band|last_trigger)' | sed 's/flipctl_//' | tr '\n' ' ')"; sleep 2; done ) >"$FP/acc-$tag.trace" 2>&1 &
  local sampler=$!
  case $kind in
    mk) LG_CPUS=$LG4 mk_load "$PORT_ON" 150 4 >"$FP/acc-$tag.mt" 2>&1;;
    hetero) LG_CPUS=50,178 sk_load "$PORT_ON" 150 2 1:0 >"$FP/acc-$tag.mt" 2>&1 & local a=$!
            LG_CPUS=51,179 sk_load "$PORT_ON" 150 2 0:1 >"$FP/acc-$tag.mt2" 2>&1; wait $a;;
  esac
  kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
  local info all; info=$(flipinfo $PORT_ON); all=$($CLI -p $PORT_ON info all | tr -d '\r')
  # triggers AFTER the boot maneuver on a stationary load = false triggers
  echo "$tag kind=$kind state=$(infog "$info" flipctl_state) anchor=$(infog "$info" flipctl_anchor_io):$(infog "$info" flipctl_anchor_ex) band=$(infog "$info" flipctl_signature_band) rate_band=$(infog "$info" flipctl_rate_band) triggers=$(infog "$info" flipctl_triggers) boot=$(infog "$info" flipctl_boot_triggers) fp=$(infog "$info" flipctl_fingerprint_triggers) surge=$(infog "$info" flipctl_rate_surge_triggers) collapse=$(infog "$info" flipctl_rate_collapse_triggers) last=$(infog "$info" flipctl_last_trigger) flips=$(infog "$all" flip_completed) xfer=$(infog "$all" flip_clients_transferred) rate=$(mt_rate "$FP/acc-$tag.mt")$([ -s "$FP/acc-$tag.mt2" ] && echo "+$(mt_rate "$FP/acc-$tag.mt2")")" >"$out.part"
  $CLI -p $PORT_ON debug flipctl >>"$out.part" 2>&1
  mv "$out.part" "$out"; LG "ACC $(head -1 "$out")"
  stop "$pid" "$PORT_ON"
}
if want S2; then
  for kind in mk hetero; do
    acc "pre-$kind"  "$PRE_BIN"  "$kind"
    acc "post-$kind" "$POST_BIN" "$kind"
  done
fi

# ---- S3 wrong-split boots: time-to-first-move (the fingerprint must still let it move) ---------
tm(){ # tm TAG BIN FA BIO BEX SRV LG LGT
  local tag=$1 bin=$2 fa=$3 bio=$4 bex=$5 srv=$6 lg=$7 lgt=$8
  local out=$FP/tm-$tag.txt tr=$FP/tm-$tag.trace
  if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
  [ -s "$out" ] && { LG "TM $tag on file :: $(cat "$out")"; return 0; }
  wait_gate
  local pid; pid=$(SRV_CPUS=$srv boot "$bin" "$PORT_TM" "tm-$tag" --ratio "$bio:$bex" --shards 64 --atomic 1 --flip-auto "$fa") || return 1
  LG_CPUS=$lg preload "$PORT_TM" "$lgt"
  ( t0=$(date +%s); while :; do
      tc=$($CLI -p $PORT_TM info stats 2>/dev/null | tr -d '\r' | sed -n 's/^total_commands_processed://p')
      sp=$($CLI -p $PORT_TM info server 2>/dev/null | tr -d '\r' | grep -E '^(io|ex)_threads:' | sed 's/.*://' | tr '\n' ':' | sed 's/:$//')
      echo "$(( $(date +%s) - t0 )) ${tc:-0} ${sp:-0:0}"; sleep 1; done ) >"$tr" 2>&1 &
  local sampler=$!
  LG_CPUS=$lg mk_load "$PORT_TM" 120 "$lgt" >"$FP/tm-$tag.mt" 2>&1
  kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
  local info; info=$($CLI -p $PORT_TM info all 2>/dev/null | tr -d '\r')
  $CLI -p $PORT_TM debug flipctl >"$FP/tm-$tag.dbg" 2>&1
  echo "$tag boot=$bio:$bex fa=$fa bin=$(basename "$bin") rate=$(mt_rate "$FP/tm-$tag.mt") flips=$(infog "$info" flip_completed) xfer=$(infog "$info" flip_clients_transferred) live=$(infog "$info" io_threads):$(infog "$info" ex_threads) triggers=$(infog "$info" flipctl_triggers) fp=$(infog "$info" flipctl_fingerprint_triggers) last=$(infog "$info" flipctl_last_trigger) | $(python3 "$FP/ttfm.py" "$tr" "$bio" "$bex" | head -1)" >"$out.part"
  mv "$out.part" "$out"; LG "TM $(cat "$out")"
  stop "$pid" "$PORT_TM"
}
if want S3; then
  tm pre-31   "$PRE_BIN"   1 3 1 "$SRV4" "$LG4" 4
  tm post-31  "$POST_BIN"  1 3 1 "$SRV4" "$LG4" 4
  tm off-31   "$POST_BIN"  0 3 1 "$SRV4" "$LG4" 4
  tm pre6-51   "$PRE_BIN"   1 5 1 "$SRV6" "$LG6" 2
  tm post6-51  "$POST_BIN"  1 5 1 "$SRV6" "$LG6" 2
fi

# ---- S4 gate row -------------------------------------------------------------------------------
ctl(){ # ctl TAG BIN
  local tag=$1 bin=$2
  local out=$FP/ctl-$tag.txt
  if [ -z "$tag" ] || [ "${out#*$tag}" = "$out" ]; then LG "HARNESS BUG: tag='$tag' out='$out'"; return 1; fi
  [ -s "$out" ] && { LG "CTL $tag on file :: $(grep -E '^ok:|AssertionError|anchored off-rail|Error' "$out" | head -1 | cut -c1-200)"; return 0; }
  wait_gate
  local pid; pid=$(SRV_CPUS=$MY_MASK boot "$bin" "$PORT_CTL" "ctl-$tag" --ratio 6:2 --atomic 0 --flip-auto 1) || return 1
  taskset -c "$MY_MASK" timeout 300 python3 tests/flipctl.py --host 127.0.0.1 --port "$PORT_CTL" --stable-seconds 30 >"$out.part" 2>&1; local rc=$?
  echo "RC=$rc" >>"$out.part"; flipinfo "$PORT_CTL" >>"$out.part"; $CLI -p "$PORT_CTL" debug flipctl >>"$out.part" 2>&1
  mv "$out.part" "$out"; LG "CTL $tag rc=$rc :: $(grep -E '^ok:|AssertionError|anchored off-rail|Error' "$out" | head -1 | cut -c1-200)"
  stop "$pid" "$PORT_CTL"
}
if want S4; then
  ctl post-1 "$POST_BIN"; ctl post-2 "$POST_BIN"; ctl post-3 "$POST_BIN"
  ctl pre-1 "$PRE_BIN"; ctl pre-2 "$PRE_BIN"; ctl pre-3 "$PRE_BIN"
fi

# ---- S5 batteries both modes -------------------------------------------------------------------
if want S5; then
  for mode in 1s 2s; do
    [ -s "$FP/bat-$mode.txt" ] && { LG "BAT $mode on file :: $(tail -1 "$FP/bat-$mode.txt")"; continue; }
    wait_gate
    bash "$FP/bat.sh" "$POST_BIN" "$mode" "$FP/bat-$mode.txt.part" >"$FP/bat-$mode.log" 2>&1; rc=$?
    mv "$FP/bat-$mode.txt.part" "$FP/bat-$mode.txt"
    LG "BAT $mode rc=$rc :: $(grep -c '^PASS' "$FP/bat-$mode.txt") pass / $(grep -c '^FAIL' "$FP/bat-$mode.txt") fail"
  done
fi

# ---- S6 differ ---------------------------------------------------------------------------------
if want S6 && [ ! -s "$FP/differ.txt" ]; then
  wait_gate
  taskset -c "$MY_MASK" tests/differ_gate.sh "$POST_BIN" "$PORT_DIFF" "$PORT_ORACLE" "$MY_MASK" 6:2 >"$FP/differ.txt.part" 2>&1; rc=$?
  echo "RC=$rc" >>"$FP/differ.txt.part"; mv "$FP/differ.txt.part" "$FP/differ.txt"
  LG "DIFFER rc=$rc :: $(tail -2 "$FP/differ.txt" | tr '\n' ' ' | cut -c1-200)"
fi

# ---- S7 gate quick -----------------------------------------------------------------------------
if want S7 && [ ! -s "$FP/gate-quick.txt" ]; then
  wait_gate
  GATE_CORES=$MY_MASK GATE_PORT=$PORT_GATE GATE_LEDGER=$FP/gate-ledger-quick.txt \
    taskset -c "$MY_MASK" bash tests/gate.sh quick >"$FP/gate-quick.txt.part" 2>&1; rc=$?
  echo "RC=$rc" >>"$FP/gate-quick.txt.part"; mv "$FP/gate-quick.txt.part" "$FP/gate-quick.txt"
  LG "GATE quick rc=$rc :: $(grep -E '^GATE' "$FP/gate-quick.txt" | tail -1)"
fi

# ---- S8 tables ---------------------------------------------------------------------------------
if want S8; then
  for geom in 1t 2t 2s; do
    [ -s "$FP/slope-pre-$geom.csv" ] && [ -s "$FP/slope-post-$geom.csv" ] && \
      python3 "$FP/cm/ab.py" --pre="$FP/slope-pre-$geom.csv" --post="$FP/slope-post-$geom.csv" --shapes=set_over,get_hit --md >"$FP/table-$geom.md" 2>&1
    [ -s "$FP/slope-pre-$geom.csv" ] && [ -s "$FP/slope-prenull-$geom.csv" ] && \
      python3 "$FP/cm/ab.py" --pre="$FP/slope-pre-$geom.csv" --post="$FP/slope-prenull-$geom.csv" --shapes=set_over,get_hit --md >"$FP/table-null-$geom.md" 2>&1
  done
  [ -s "$FP/slope-pre1-2s.csv" ] && [ -s "$FP/slope-post1-2s.csv" ] && \
    python3 "$FP/cm/ab.py" --pre="$FP/slope-pre1-2s.csv" --post="$FP/slope-post1-2s.csv" --shapes=set_over,get_hit --md >"$FP/table-on-2s.md" 2>&1
fi
LG "CHAIN DONE"; date >"$FP/chain.done"
