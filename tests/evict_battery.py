#!/usr/bin/env python3
# Eviction scenario battery. One SECTION per fresh server boot (the lane has no FLUSHALL, and
# cross-section residue poisons verdicts). Driver: evict_drive.sh.
#   python3 evict_battery.py <port> <section>
# Sections: off noev lru vlru vttl lfu config
import socket, sys

HOST, PORT, SECTION = "127.0.0.1", int(sys.argv[1]), sys.argv[2]
s = socket.create_connection((HOST, PORT), timeout=20)
f = s.makefile("rb")

def enc(a):
    o = b"*%d\r\n" % len(a)
    for x in a:
        x = x.encode() if isinstance(x, str) else x
        o += b"$%d\r\n" % len(x) + x + b"\r\n"
    return o

def rr():
    line = f.readline()
    if not line: raise EOFError
    t = line[:1]
    if t in b"+-:": return line.strip()
    if t == b"$":
        n = int(line[1:-2]); return None if n == -1 else f.read(n + 2)[:-2]
    n = int(line[1:-2]); return [rr() for _ in range(n)]

def cmd(*a):
    s.sendall(enc(list(a))); return rr()

def must(*a):
    r = cmd(*a)
    assert not (isinstance(r, bytes) and r.startswith(b"-")), (a, r)
    return r

def fill(prefix, n, sz=100, ttl=None, start=0, tolerate_oom=False):
    B = 500; ooms = 0
    for i in range(start, start + n, B):
        chunk = b""
        cnt = 0
        for j in range(i, min(i + B, start + n)):
            if ttl: chunk += enc(["SET", "%s:%d" % (prefix, j), "v" * sz, "EX", str(ttl)])
            else:   chunk += enc(["SET", "%s:%d" % (prefix, j), "v" * sz])
            cnt += 1
        s.sendall(chunk)
        for _ in range(cnt):
            r = rr()
            if isinstance(r, bytes) and r.startswith(b"-"):
                assert tolerate_oom, r
                ooms += 1
    return ooms

def info_num(field):
    txt = cmd("INFO")
    for ln in txt.split(b"\r\n"):
        if ln.startswith(field.encode() + b":"): return int(ln.split(b":")[1])
    return None

def dbsize(): return int(cmd("DBSIZE")[1:])
def alive(prefix, rng): return sum(1 for i in rng if cmd("EXISTS", "%s:%d" % (prefix, i)) == b":1")

P = 0; F = 0
def check(name, ok, detail=""):
    global P, F
    print("  %-52s %s %s" % (name, "ok" if ok else "FAIL", detail))
    P, F = P + (1 if ok else 0), F + (0 if ok else 1)

MM = "1048576"   # 1MB / 16 shards = 64KB/shard; ~124B per 100B-value key => ~8.4k-key ceiling

if SECTION == "off":
    fill("off", 3000)
    check("disabled: 3000 writes all land", dbsize() == 3000, dbsize())
    check("disabled: evicted_keys==0", info_num("evicted_keys") == 0)

elif SECTION == "noev":
    must("CONFIG", "SET", "maxmemory", MM); must("CONFIG", "SET", "maxmemory-policy", "noeviction")
    oom = None
    for i in range(40000):
        s.sendall(enc(["SET", "noev:%d" % i, "x" * 100]))
        r = rr()
        if r.startswith(b"-"): oom = r; break
    check("noeviction: hits OOM", oom is not None, (oom or b"")[:40])
    check("noeviction: exact redis OOM text",
          oom == b"-OOM command not allowed when used memory > 'maxmemory'.")
    check("noeviction: evicted_keys stays 0", info_num("evicted_keys") == 0)

elif SECTION == "lru":
    must("CONFIG", "SET", "maxmemory", MM); must("CONFIG", "SET", "maxmemory-policy", "allkeys-lru")
    fill("lru", 8000)
    for r in range(3):     # keep hot set warm while pressure continues
        for i in range(50):
            s.sendall(enc(["GET", "lru:%d" % i])); rr()
        fill("lrufill", 2000, start=r * 2000)
    ev = info_num("evicted_keys")
    check("allkeys-lru: writes keep landing past limit", True)
    check("allkeys-lru: eviction FIRED", ev and ev > 0, "evicted=%s" % ev)
    check("allkeys-lru: plateau near ceiling (<10k of 14k offered)", dbsize() < 10000, dbsize())

elif SECTION == "vlru":
    must("CONFIG", "SET", "maxmemory", MM); must("CONFIG", "SET", "maxmemory-policy", "volatile-lru")
    fill("persist", 5000)                       # < ceiling: all land
    ooms = fill("pfill", 20000, tolerate_oom=True)   # no volatiles -> must OOM, never evict
    check("volatile-lru, no volatiles: OOMs observed", ooms > 0, ooms)
    check("volatile-lru: zero evictions without volatiles", info_num("evicted_keys") == 0)
    check("volatile-lru: persistents untouched", alive("persist", range(0, 5000, 100)) == 50)

elif SECTION == "vttl":
    must("CONFIG", "SET", "maxmemory", MM); must("CONFIG", "SET", "maxmemory-policy", "volatile-ttl")
    fill("soon", 3000, ttl=50)
    fill("late", 3000, ttl=10000)
    fill("perm", 300)
    fill("ttlfill", 8000, ttl=5000, tolerate_oom=True)
    ev = info_num("evicted_keys")
    soon, late = alive("soon", range(0, 3000, 60)), alive("late", range(0, 3000, 60))
    check("volatile-ttl: eviction FIRED", ev and ev > 0, "evicted=%s" % ev)
    check("volatile-ttl: soon evicted more than late", soon < late, "soon=%d late=%d" % (soon, late))
    check("volatile-ttl: permanents survive", alive("perm", range(300)) == 300)

elif SECTION == "lfu":
    must("CONFIG", "SET", "maxmemory", MM); must("CONFIG", "SET", "maxmemory-policy", "allkeys-lfu")
    fill("lfuhot", 20)
    for r in range(6):
        for _ in range(5):
            for i in range(20):
                s.sendall(enc(["GET", "lfuhot:%d" % i])); rr()
        fill("lfucold", 3000, start=r * 3000)
    ev = info_num("evicted_keys")
    hot = alive("lfuhot", range(20))
    check("allkeys-lfu: eviction FIRED", ev and ev > 0, "evicted=%s" % ev)
    check("allkeys-lfu: hot 20 mostly survive", hot >= 15, hot)

elif SECTION == "growth":
    # wrinkle fix: collection growth must respect maxmemory (pre-exec DENYOOM gate).
    must("CONFIG", "SET", "maxmemory", MM); must("CONFIG", "SET", "maxmemory-policy", "noeviction")
    oom = None; i = 0
    while i < 60000 and oom is None:          # one hash, grown far past the whole-server budget
        s.sendall(enc(["HSET", "bigh", "f%d" % i, "v" * 100]))
        r = rr()
        if isinstance(r, bytes) and r.startswith(b"-"): oom = r
        i += 1
    check("growth: HSET on one hash hits OOM", oom is not None, (oom or b"")[:40])
    check("growth: exact redis OOM text",
          oom == b"-OOM command not allowed when used memory > 'maxmemory'.")
    check("growth: reads still served", cmd("HLEN", "bigh")[:1] == b":")
    check("growth: DEL allowed while over budget", cmd("DEL", "bigh") == b":1")
    ok_after = cmd("HSET", "bigh2", "f", "v")
    check("growth: writes recover after DEL", ok_after == b":1", ok_after)
    must("CONFIG", "SET", "maxmemory-policy", "allkeys-lru")
    fill("filler", 3000)
    grown = 0; stopped = False
    for j in range(40000):
        s.sendall(enc(["RPUSH", "bigl", "x" * 100]))
        r = rr()
        if isinstance(r, bytes) and r.startswith(b"-"): stopped = True; break
        grown += 1
    ev = info_num("evicted_keys")
    check("growth: allkeys-lru evicts others for list growth", ev and ev > 0, "evicted=%s" % ev)
    check("growth: gate closes once nothing evictable remains", stopped, "grew %d then %s" % (grown, "OOM" if stopped else "never stopped"))

elif SECTION == "config":
    must("CONFIG", "SET", "maxmemory", MM); must("CONFIG", "SET", "maxmemory-policy", "allkeys-lru")
    fill("cfg", 3000)
    must("CONFIG", "SET", "maxmemory", "0")     # live-disable
    n0 = dbsize()
    fill("cfg2", 6000)
    check("maxmemory=0 re-disables (all 6000 land)", dbsize() == n0 + 6000, dbsize() - n0)
    check("samples: rejects 0", cmd("CONFIG", "SET", "maxmemory-samples", "0").startswith(b"-"))
    check("samples: rejects 65", cmd("CONFIG", "SET", "maxmemory-samples", "65").startswith(b"-"))
    check("samples: accepts 64", cmd("CONFIG", "SET", "maxmemory-samples", "64") == b"+OK")
    check("policy: rejects junk", cmd("CONFIG", "SET", "maxmemory-policy", "zzz").startswith(b"-"))
    g = cmd("CONFIG", "GET", "maxmemory-policy")
    check("CONFIG GET policy roundtrip", g[1] == b"allkeys-lru", g)
    multi = cmd("CONFIG", "SET", "maxmemory", "4194304", "maxmemory-policy", "volatile-ttl")
    g2 = cmd("CONFIG", "GET", "*")
    check("multi-pair CONFIG SET coherent", multi == b"+OK" and b"4194304" in g2 and b"volatile-ttl" in g2)

print("SECTION %s: %d ok, %d FAIL" % (SECTION, P, F))
sys.exit(1 if F else 0)
