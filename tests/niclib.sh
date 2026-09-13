#!/bin/bash
# Shared helpers for the optional 25GbE two-namespace gate tier.
set -u
NIC_SRV_NS=serverns;  NIC_SRV_IF=eno12409np1; NIC_SRV_IP=10.200.0.2
NIC_CLI_NS=clientns;  NIC_CLI_IF=eno12399np0; NIC_CLI_IP=10.200.0.1
NIC_SRV_CORES=${NIC_SRV_CORES:-0-63}
NIC_LG_CORES=${NIC_LG_CORES:-64-127}
NIC_DROP="setpriv --reuid=$(id -u) --regid=$(id -g) --clear-groups"
NIC_NOFILE=${NIC_NOFILE:-262144}

nsrv(){ sudo -n ip netns exec $NIC_SRV_NS $NIC_DROP sh -c "ulimit -n $NIC_NOFILE 2>/dev/null; exec \"\$@\"" _ "$@"; }
ncli(){ sudo -n ip netns exec $NIC_CLI_NS $NIC_DROP sh -c "ulimit -n $NIC_NOFILE 2>/dev/null; exec \"\$@\"" _ "$@"; }
nsrv_root(){ sudo -n ip netns exec $NIC_SRV_NS "$@"; }
ncli_root(){ sudo -n ip netns exec $NIC_CLI_NS "$@"; }

nic_say(){ echo "$(date +%H:%M:%S) $*" | tee -a "${BL_R:-/dev/null}"; }

nic_tune(){
    local ns ifc got_u got_f bad=0
    for ns in "$NIC_SRV_NS:$NIC_SRV_IF" "$NIC_CLI_NS:$NIC_CLI_IF"; do
        ifc=${ns#*:}; ns=${ns%%:*}
        sudo -n ip netns exec "$ns" ethtool -C "$ifc" adaptive-rx off rx-usecs 1 rx-frames 1 >/dev/null 2>&1
        got_u=$(sudo -n ip netns exec "$ns" ethtool -c "$ifc" 2>/dev/null | awk -F: '/^rx-usecs:/{gsub(/[[:space:]]/,"",$2);print $2; exit}')
        got_f=$(sudo -n ip netns exec "$ns" ethtool -c "$ifc" 2>/dev/null | awk -F: '/^rx-frames:/{gsub(/[[:space:]]/,"",$2);print $2; exit}')
        [ "${got_u:-x}" = 1 ] && [ "${got_f:-x}" = 1 ] \
            || { nic_say "   TUNE-FAIL $ns/$ifc rx-usecs=$got_u rx-frames=$got_f"; bad=1; }
    done
    return $bad
}

nic_assert_link(){
    local sp
    sp=$(sudo -n ip netns exec $NIC_SRV_NS ethtool $NIC_SRV_IF 2>/dev/null | awk '/Speed:/{print $2}')
    [ "$sp" = "25000Mb/s" ] || { nic_say "   LINK-FAIL speed=$sp (want 25000Mb/s)"; return 1; }
    sudo -n ip netns exec $NIC_CLI_NS ping -c1 -W2 $NIC_SRV_IP >/dev/null 2>&1 \
        || { nic_say "   LINK-FAIL no ping $NIC_CLI_IP -> $NIC_SRV_IP"; return 1; }
}

nic_kill_srv(){
    local port=$1 p t0
    t0=$(date +%s)
    while :; do
        p=$(nsrv_root ss -tlnpH "sport = :$port" 2>/dev/null | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u)
        [ -z "$p" ] && return 0
        [ $(( $(date +%s) - t0 )) -ge 60 ] \
            && { nic_say "   nic_kill_srv GAVE UP pids=$p"; return 1; }
        for x in $p; do
            kill -9 "$x" 2>/dev/null || sudo -n ip netns exec $NIC_SRV_NS kill -9 "$x" 2>/dev/null
        done
        sleep 1
    done
}

nic_boot(){
    local tag=$1; shift
    local bin=$1 pids count answering exe t0
    pids=$(nsrv_root ss -tlnpH "sport = :$NIC_PORT" 2>/dev/null | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u)
    [ -z "$pids" ] \
        || { nic_say "   PORT-GUARD-FAIL $tag pids=$pids on port $NIC_PORT"; return 1; }

    nsrv taskset -c "$NIC_SRV_CORES" "$@" > "$BL_LOGDIR/srv_$tag.log" 2>&1 &
    t0=$(date +%s)
    while :; do
        if ncli "$NIC_CLI_BIN" -h $NIC_SRV_IP -p "$NIC_PORT" ping 2>/dev/null | grep -q PONG; then
            pids=$(nsrv_root ss -tlnpH "sport = :$NIC_PORT" 2>/dev/null | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u)
            count=$(printf '%s\n' "$pids" | sed '/^$/d' | wc -l)
            answering=$(ncli "$NIC_CLI_BIN" -h $NIC_SRV_IP -p "$NIC_PORT" info server 2>/dev/null |
                tr -d '\r' | sed -n 's/^process_id:\([0-9][0-9]*\)$/\1/p')
            [ "$count" = 1 ] \
                || { nic_say "   IDENTITY-FAIL $tag $count distinct pids on port $NIC_PORT"; return 1; }
            exe=$(nsrv_root readlink -f "/proc/${answering:-0}/exe" 2>/dev/null)
            [ "$exe" = "$(readlink -f "$bin")" ] \
                || { nic_say "   IDENTITY-FAIL $tag answering=${answering:-none} runs ${exe:-none}, not $bin"; return 1; }
            return 0
        fi
        [ $(( $(date +%s) - t0 )) -ge 90 ] \
            && { nic_say "   BOOT-FAIL $tag (90s, no PONG over the wire)"; return 1; }
        sleep 1
    done
}

nic_memtier(){
    ncli timeout "${NIC_TO:-200}" taskset -c "$NIC_LG_CORES" memtier_benchmark \
        -s $NIC_SRV_IP -p "$NIC_PORT" "$@"
}
