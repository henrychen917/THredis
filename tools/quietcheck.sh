#!/bin/bash
# Preflight quietness check: tools/quietcheck.sh CORES PORT
#   CORES: taskset-style list ("0-15" or "0-63,128-191")
#   PORT:  the port about to be bound
# Verifies (1) no existing listener holds PORT, (2) the assigned cores are quiet (<15% busy
# over a 0.7s sample, measured from /proc/stat so IRQ/kernel co-tenants count too).
# Exit 0 = quiet. Exit 1 = port held. Exit 2 = cores busy. Offenders printed to stderr.
set -u
CORES=$1; PORT=$2
HOLDERS=$(ss -tlnp 2>/dev/null | grep ":$PORT " | grep -oE 'pid=[0-9]+' | sort -u)
if [ -n "$HOLDERS" ]; then
  echo "quietcheck: port $PORT already held by ${HOLDERS//pid=/}" >&2
  exit 1
fi
expand_cores() {
  local out="" part
  for part in ${1//,/ }; do
    case "$part" in
      *-*) seq "${part%-*}" "${part#*-}";;
      *) echo "$part";;
    esac
  done
}
CORELIST=$(expand_cores "$CORES")
snap() { awk -v want="$1" 'BEGIN{split(want,w," "); for(i in w) sel["cpu"w[i]]=1}
  $1 in sel {idle=$5+$6; total=0; for(f=2;f<=9;f++) total+=$f; print $1, total, idle}' /proc/stat; }
S1=$(snap "$CORELIST"); sleep 0.7; S2=$(snap "$CORELIST")
BUSY=$(python3 - "$S1" "$S2" <<'PY'
import sys
a={l.split()[0]:(int(l.split()[1]),int(l.split()[2])) for l in sys.argv[1].splitlines()}
b={l.split()[0]:(int(l.split()[1]),int(l.split()[2])) for l in sys.argv[2].splitlines()}
bad=[]
for c in a:
    dt=b[c][0]-a[c][0]; di=b[c][1]-a[c][1]
    if dt>0 and (dt-di)/dt > 0.15: bad.append("%s=%.0f%%" % (c, 100*(dt-di)/dt))
print(" ".join(bad))
PY
)
if [ -n "$BUSY" ]; then
  echo "quietcheck: assigned cores busy: $BUSY" >&2
  ps -eo pid,psr,pcpu,comm --sort=-pcpu | head -6 >&2
  exit 2
fi
exit 0
