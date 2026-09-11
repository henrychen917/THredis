#!/bin/bash
# Preflight quietness check: tools/quietcheck.sh CORES PORT
#   CORES: taskset-style list ("0-15" or "0-63,128-191")
#   PORT:  the port about to be bound
# Verifies (1) no existing listener holds PORT, (2) the assigned cores are quiet (<15% busy
# over a 0.7s sample, measured from /proc/stat so IRQ/kernel co-tenants count too).
# Exit 0 = quiet. Exit 1 = port held. Exit 2 = cores busy. Evidence printed to stderr.
set -u
CORES=$1; PORT=$2
# Port occupancy is independent of whether this user can inspect the listener's PID.
# Asking ss for the intended port also avoids matching a different port's decimal suffix.
LISTENERS=$(ss -H -ltn "sport = :$PORT" 2>/dev/null) || {
  echo "quietcheck: cannot inspect port $PORT" >&2
  exit 1
}
if [ -n "$LISTENERS" ]; then
  echo "quietcheck: port $PORT already listening" >&2
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
BUSY=$(python3 - "$S1" "$S2" "$CORELIST" <<'PY'
import sys
a={l.split()[0]:(int(l.split()[1]),int(l.split()[2])) for l in sys.argv[1].splitlines()}
b={l.split()[0]:(int(l.split()[1]),int(l.split()[2])) for l in sys.argv[2].splitlines()}
expected={f'cpu{int(c)}' for c in sys.argv[3].split()}
# An unreadable/offline selected CPU is absent evidence, never proof of idleness.
if not expected or set(a) != expected or set(b) != expected:
    sys.exit('quietcheck: missing selected CPU counters')
bad=[]
for c in a:
    dt=b[c][0]-a[c][0]; di=b[c][1]-a[c][1]
    if dt <= 0 or not 0 <= di <= dt:
        sys.exit(f'quietcheck: invalid selected CPU counters for {c}')
    if (dt-di)/dt > 0.15: bad.append("%s=%.0f%%" % (c, 100*(dt-di)/dt))
print(" ".join(bad))
PY
) || exit 2
if [ -n "$BUSY" ]; then
  echo "quietcheck: assigned cores busy: $BUSY" >&2
  exit 2
fi
exit 0
