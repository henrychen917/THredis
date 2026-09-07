#!/bin/bash
# report.sh -- assemble the lane's PRE/POST tables and verdict rows from the chain outputs.
source /tmp/claude-1000/-home-user-Projects/ee6eb242-5302-49cf-b767-1a2d8d8f0f61/scratchpad/flipfp/fl.sh
cd "$FP" || exit 1
ROWS='cycles/op|instr/op|IPC|USER instr/op (:u)|USER store-queue-full|store-queue-full cyc|load-queue-full cyc|retire-queue-full cyc|USER dmnd fills same-CCX|dmnd fills L2|driver ops/s'
pick(){ # pick TABLE.md SHAPE -> the rows of interest for that shape
  awk -v shape="$2" -v rows="$ROWS" 'BEGIN{n=split(rows,r,"|")} /^#### /{on = index($0, shape " ")==6} on && /^\| / {name=$0; sub(/^\| /,"",name); sub(/ \|.*/,"",name); for(i=1;i<=n;i++) if (name==r[i]) print}' "$1"
}
echo "# t-flipfp results  ($(date +%F' '%T))"
echo; echo "digests: $(tr '\n' ';' < digests.txt)"
for geom in 1t 2t 2s; do
  for kind in "" null-; do
    t=table-$kind$geom.md; [ -s "$t" ] || continue
    for shape in set_over get_hit; do
      echo; echo "### ${kind:-PRE-vs-POST }$geom $shape (flip-auto 0)"; echo
      echo "| metric | PRE | POST | delta | delta % |"; echo "|---|---:|---:|---:|---:|"
      pick "$t" "$shape"
    done
  done
done
if [ -s table-on-2s.md ]; then
  for shape in set_over get_hit; do
    echo; echo "### Historical retired fixed-band experiment: flip-auto 1 (anchored, band 0) 2s $shape: PRE vs POST"; echo
    echo "| metric | PRE | POST | delta | delta % |"; echo "|---|---:|---:|---:|---:|"
    pick table-on-2s.md "$shape"
  done
  echo; echo "controller state during the flip-auto 1 slope cells:"; grep -h "settle:\|end:" slope-pre1-2s.log slope-post1-2s.log 2>/dev/null | sed 's/^/    /'
fi
echo; echo "### accuracy (2:2, auto band, stationary load: triggers after boot = false triggers)"; echo
for f in acc-pre-mk.txt acc-post-mk.txt acc-pre-hetero.txt acc-post-hetero.txt; do [ -s "$f" ] && echo "- $(head -1 "$f")"; done
echo; echo "### wrong-split boots (MK8 1:1, atomic 1, 120 s)"; echo
for f in tm-pre-31.txt tm-post-31.txt tm-off-31.txt tm-pre6-51.txt tm-post6-51.txt; do [ -s "$f" ] && { echo "- $(head -1 "$f")"; echo "  $(sed -n 2p "$f")"; }; done
echo; echo "### tests/flipctl.py (6:2, --stable-seconds 30; band and sampling from each recorded boot)"; echo
pass=0; runs=0
for f in ctl-post-1.txt ctl-post-2.txt ctl-post-3.txt ctl-pre-1.txt ctl-pre-2.txt ctl-pre-3.txt; do
  [ -s "$f" ] || continue
  rc=$(sed -n 's/^RC=//p' "$f" | head -1)
  case $f in ctl-post-*) runs=$((runs+1)); [ "$rc" = 0 ] && pass=$((pass+1));; esac
  echo "- $f: RC=$rc :: $(grep -E '^ok:|AssertionError|anchored off-rail|Error' "$f" | head -1 | cut -c1-190)"
done
echo; echo "POST tests/flipctl.py pass count: $pass / $runs"
echo; echo "### batteries / differ / gate"; echo
for m in 1s 2s; do [ -s bat-$m.txt ] && echo "- batteries $m: $(tail -1 bat-$m.txt)"; done
[ -s differ.txt ] && echo "- differ: $(grep -E 'PASS|FAIL|RC=' differ.txt | tail -3 | tr '\n' ' ' | cut -c1-200)"
[ -s gate-quick.txt ] && echo "- gate quick: $(grep -E '^GATE' gate-quick.txt | tail -1) RC=$(tail -1 gate-quick.txt)"
