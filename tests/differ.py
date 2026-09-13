#!/usr/bin/env python3
# DIFFERENTIAL battery: run one deterministic command stream against the TARGET (tomokv-cpp) and
# the ORACLE (vanilla Redis 7.4.2 -- byte-exact redis semantics) and diff every reply.
#   python3 tests/differ.py <target_host> <target_port> <oracle_host> <oracle_port> <suite> [seed] [-3]
#   python3 tests/differ.py --list-generators
# Exit 0 iff zero diffs. Suites include the ordinary byte-comparison generators plus property
# suites (e.g. s6fix, scan) for commands whose successful replies are intentionally
# nondeterministic and must be compared as sets rather than byte streams.
import socket, sys, random, re, time, hashlib, select

LIST_GENERATORS = sys.argv[1:] == ["--list-generators"]
if LIST_GENERATORS:
    TH = OH = ""
    TP = OP = 0
    SUITE = ""
    EXTRA = []
else:
    TH, TP, OH, OP = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
    SUITE = sys.argv[5]
    EXTRA = sys.argv[6:]
RESP3 = "-3" in EXTRA
SEED = int(next((arg for arg in EXTRA if arg != "-3"), "7"))

def conn_mode(h, p, resp3):
    s = socket.create_connection((h, p), timeout=30)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    f = s.makefile("rb")
    if resp3:
        s.sendall(enc(["HELLO", "3"]))
        hello = read_reply(f)
        if not hello.startswith(b"%7\r\n"):
            raise RuntimeError("HELLO 3 failed for %s:%d: %r" % (h, p, hello[:96]))
    return s, f
def conn(h, p):
    return conn_mode(h, p, RESP3)
def enc(args):
    o = b"*%d\r\n" % len(args)
    for a in args:
        if isinstance(a, str): a = a.encode()
        o += b"$%d\r\n" % len(a) + a + b"\r\n"
    return o
def read_reply(f):
    line = f.readline()
    if not line: raise EOFError
    t = line[:1]
    if t in b"+-:,_#(": return line
    if t in (b"$", b"=", b"!"):
        n = int(line[1:-2])
        if n == -1: return line
        return line + f.read(n + 2)
    if t in (b"*", b"~", b">"):
        n = int(line[1:-2])
        if n == -1: return line
        return line + b"".join(read_reply(f) for _ in range(n))
    if t in (b"%", b"|"):
        n = int(line[1:-2])
        return line + b"".join(read_reply(f) for _ in range(n * 2))
    raise ValueError(repr(line[:24]))

def target_stats():
    """Target-only mechanism counters; DEBUG/INFO is never sent to the Redis oracle."""
    sock, file = conn(TH, TP)
    try:
        sock.sendall(enc(["INFO", "STATS"]))
        parsed = parse_reply(read_reply(file))
        if not isinstance(parsed, bytes):
            raise RuntimeError("target INFO STATS did not return a bulk string")
        out = {}
        for line in parsed.split(b"\r\n"):
            if b":" not in line: continue
            name, value = line.split(b":", 1)
            try: out[name.decode()] = int(value)
            except ValueError: pass
        return out
    finally:
        sock.close()

def parse_reply(r):
    # Decode serialized RESP2/RESP3 into a Python structure for unordered normalization.
    def one(pos):
        marker = r[pos:pos + 1]
        end = r.index(b"\r\n", pos)
        line = r[pos + 1:end]
        next_pos = end + 2
        if marker in b"+-:, #(".replace(b" ", b""):
            return marker + line, next_pos
        if marker == b"_": return None, next_pos
        if marker in (b"$", b"=", b"!"):
            length = int(line)
            if length == -1: return None, next_pos
            value = r[next_pos:next_pos + length]
            return value, next_pos + length + 2
        if marker in (b"*", b"~", b">"):
            count = int(line)
            if count == -1: return None, next_pos
            values = []
            for _ in range(count):
                value, next_pos = one(next_pos)
                values.append(value)
            return values, next_pos
        if marker in (b"%", b"|"):
            values = []
            for _ in range(int(line) * 2):
                value, next_pos = one(next_pos)
                values.append(value)
            return values, next_pos
        raise ValueError("unsupported RESP marker %r" % marker)
    try:
        value, consumed = one(0)
        return value if consumed == len(r) else None
    except (ValueError, IndexError):
        return None

SORTED_SET_REPLIES = {"SMEMBERS", "SPOPSHAPE", "HKEYS", "HVALS",
                      "KEYS", "SINTER", "SUNION", "SDIFF"}

def sort_nested(value):
    # FUNCTION LIST enumerates libraries and their functions in hash order on Redis and in name
    # order here; both are unordered by contract. Canonicalize any list whose elements are all
    # lists (the library list, the function list) and leave flat key/value rows alone.
    if not isinstance(value, list):
        return value
    out = [sort_nested(item) for item in value]
    if out and all(isinstance(item, list) for item in out):
        out.sort(key=repr)
    return out
def normalize_introspection(cmdname, argv, r):
    """MEMORY USAGE, ROLE and COMMAND INFO are compared for MEANING, not bytes.

    MEMORY USAGE: the number is our accounting, not redis's, so only hit-vs-miss is comparable.
    COMMAND INFO: the flag names, ACL category list and key-spec map are redis-internal; the name,
    arity and legacy key range are the parts the router and every client actually consume.
    """
    if cmdname == "MEMORY" and len(argv) > 1 and argv[1].upper() == "USAGE":
        return b"USAGE:nil" if r.startswith((b"$-1", b"_")) else b"USAGE:present"
    if cmdname == "ROLE":
        parsed = parse_reply(r)
        if isinstance(parsed, list) and len(parsed) >= 3:
            # element 1 is the master replication offset: redis advances it against a replication
            # backlog it keeps and we do not, so it is not a comparable quantity. The role name and
            # the replica list are.
            return b"ROLE:%s/%s" % (parsed[0], repr(parsed[2]).encode())
        return r
    if cmdname == "COMMAND" and len(argv) > 1 and argv[1].upper() == "INFO":
        rows = parse_reply(r)
        if not isinstance(rows, list):
            return r
        out = []
        for row in rows:
            if row is None:
                out.append(b"<nil>")
            elif isinstance(row, list) and len(row) >= 6:
                out.append(b"%s/%s/%s/%s/%s" % (row[0], row[1], row[3], row[4], row[5]))
            else:
                out.append(b"<shape?>")
        return b"CMDINFO:" + b";".join(out)
    return r

def normalize(cmdname, r):
    if cmdname == "FUNCTION" and r[:1] in (b"*", b"%"):
        parsed = parse_reply(r)
        if parsed is not None:
            return b"FNSORTED:" + repr(sort_nested(parsed)).encode()
    if cmdname == "HGETALL" and r[:1] in (b"*", b"%") and not r.startswith(b"*-1"):
        it = parse_reply(r)
        if it is not None and len(it) % 2 == 0:
            pairs = sorted((it[i], it[i+1]) for i in range(0, len(it), 2))
            return b"HSORTED:" + b";".join(b"%s=%s" % p for p in pairs)
    if cmdname in SORTED_SET_REPLIES:
        it = parse_reply(r)
        if isinstance(it, list):
            return b"SORTED:" + b",".join(sorted(x if x is not None else b"<nil>" for x in it))
    # HTTL/HPTTL are remaining-time answers computed from each server's own clock, so a
    # far-future deadline lands a few ms apart. Bucket to 10 s; the ABSOLUTE forms
    # (HEXPIRETIME/HPEXPIRETIME) stay byte-exact and are what actually pins the deadline.
    if cmdname in ("HTTL", "HPTTL") and r[:1] == b"*":
        it = parse_reply(r)
        if isinstance(it, list):
            out = []
            for x in it:
                v = int(x[1:]) if isinstance(x, bytes) and x[:1] == b":" else None
                if v is None: out.append(b"?")
                elif v < 0: out.append(b"%d" % v)
                else:
                    ms = v * 1000 if cmdname == "HTTL" else v
                    out.append(b"~%d" % (ms // 10000))
            return b"HTTLB:" + b",".join(out)
    # TTL/PTTL race by wall time between servers: bucket to second granularity.
    if cmdname in ("TTL", "PTTL", "EXPIRETIME", "PEXPIRETIME") and r[:1] == b":":
        try:
            v = int(r[1:-2])
            if v > 0:
                if cmdname in ("PTTL",): v = (v + 999) // 1000
                if cmdname in ("PEXPIRETIME",): v //= 1000
                return b":~%d\r\n" % v
        except ValueError: pass
    return r

def gen_string(rng):
    keys = ["s%d" % i for i in range(24)]
    ops = []
    def K(): return rng.choice(keys)
    vals = ["", "x", "hello world", "12345", "-7", "+1", "007", "-0",
            "9223372036854775807", "-9223372036854775808", "9223372036854775808",
            "3.0e3", "10.5", "a" * 300, "\x00\x01bin\xff"]
    for _ in range(4000):
        c = rng.randrange(20)
        if   c == 0: ops.append(["SET", K(), rng.choice(vals)])
        elif c == 1: ops.append(["GET", K()])
        elif c == 2: ops.append(["APPEND", K(), rng.choice(vals)])
        elif c == 3: ops.append(["STRLEN", K()])
        elif c == 4: ops.append(["GETRANGE", K(), str(rng.randrange(-20, 20)), str(rng.randrange(-20, 20))])
        elif c == 5: ops.append(["SETRANGE", K(), str(rng.randrange(0, 24)), rng.choice(vals[:8])])
        elif c == 6: ops.append(["GETSET", K(), rng.choice(vals)])
        elif c == 7: ops.append(["SETNX", K(), rng.choice(vals)])
        elif c == 8: ops.append(["INCR", K()])
        elif c == 9: ops.append(["DECR", K()])
        elif c == 10: ops.append(["INCRBY", K(), rng.choice(["1", "-3", "100", "9223372036854775807", "notanum"])])
        elif c == 11: ops.append(["DECRBY", K(), rng.choice(["2", "-5", "9223372036854775807", "-9223372036854775808"])])
        elif c == 12: ops.append(["INCRBYFLOAT", K(), rng.choice(["0.1", "-3.5e2", "1e100", "nan", "abc", "5.0e3"])])
        elif c == 13: ops.append(["DEL", K()])
        elif c == 14: ops.append(["EXISTS", K()])
        elif c == 15:
            form = rng.randrange(6)
            if form == 0: ops.append(["SET", K(), rng.choice(vals), "NX"])
            elif form == 1: ops.append(["SET", K(), rng.choice(vals), "XX"])
            elif form == 2: ops.append(["SET", K(), rng.choice(vals), "XX", "GET"])
            elif form == 3: ops.append(["SET", K(), rng.choice(vals), "NX", "GET"])
            elif form == 4: ops.append(["SET", K(), rng.choice(vals), "KEEPTTL"])
            else:          ops.append(["SET", K(), rng.choice(vals), "EX", "1000", "GET"])
        elif c == 16: ops.append(["EXPIRE", K(), rng.choice(["1000", "0", "-5"])])
        elif c == 17: ops.append(["TTL", K()])
        elif c == 18: ops.append(["PERSIST", K()])
        elif c == 19: ops.append(["GETDEL", K()])
    # directed adversarial tail (from NOTES-STRING.md)
    ops += [
        ["SET", "adv", "42"], ["APPEND", "adv", ""], ["OBJECTENCODINGSKIP"],
        ["APPEND", "adv", "9"], ["GET", "adv"],
        ["SET", "adv2", "10"], ["SETRANGE", "adv2", "5", "zz"], ["GET", "adv2"],
        ["SETRANGE", "advnew", "3", "abc"], ["GET", "advnew"], ["STRLEN", "advnew"],
        ["SET", "adv3", "5"], ["INCRBYFLOAT", "adv3", "0.1"], ["GET", "adv3"],
        ["INCRBYFLOAT", "adv3", "-5.1"], ["GET", "adv3"],
        ["SET", "ttlk", "v", "EX", "1000"], ["TTL", "ttlk"], ["APPEND", "ttlk", "w"], ["TTL", "ttlk"],
        ["GETSET", "ttlk", "z"], ["TTL", "ttlk"],
        ["SET", "c", "v", "NX", "XX"],
        ["SET", "c", "v", "EX", "10", "PX", "10000"],
        ["SET", "c", "v", "EX", "10", "KEEPTTL"],
        ["SET", "c", "v", "EX", "0"], ["SET", "c", "v", "EX", "-1"],
        ["SETEX", "se", "0", "v"], ["PSETEX", "se", "-1", "v"],
        ["GETRANGE", "missing", "0", "-1"], ["SETRANGE", "adv2", "0", ""],
        ["DECRBY", "of", "-9223372036854775808"],
        ["SET", "of2", "9223372036854775807"], ["INCR", "of2"],
    ]
    return [o for o in ops if o[0] != "OBJECTENCODINGSKIP"]

rng = random.Random(SEED)
def gen_set(rng):
    keys = ["set%d" % i for i in range(12)]
    ints = [str(rng.randrange(-100, 100)) for _ in range(40)] + ["9223372036854775807", "-9223372036854775808", "0", "-0", "007"]
    strs = ["a", "bb", "hello", "x" * 70, "binm"] + ["member-%d" % i for i in range(8)]
    ops = []
    def K(): return rng.choice(keys)
    def members(pool, k):
        return [rng.choice(pool) for _ in range(k)]
    for _ in range(3500):
        c = rng.randrange(14)
        pool = ints if rng.randrange(3) else (ints + strs)
        if   c in (0,1,2): ops.append(["SADD", K()] + members(pool, rng.randrange(1, 6)))
        elif c == 3: ops.append(["SREM", K()] + members(pool, rng.randrange(1, 4)))
        elif c == 4: ops.append(["SISMEMBER", K(), rng.choice(pool)])
        elif c == 5: ops.append(["SMISMEMBER", K()] + members(pool, rng.randrange(1, 4)))
        elif c == 6: ops.append(["SCARD", K()])
        elif c == 7: ops.append(["SMEMBERS", K()])
        elif c == 8: ops.append(["DEL", K()])
        elif c == 9: ops.append(["EXISTS", K()])
        elif c == 10: ops.append(["TYPE", K()])
        elif c == 11: ops.append(["SADD", K()] + members(strs, rng.randrange(1, 4)))
        elif c == 12: ops.append(["GET", K()])                    # WRONGTYPE path
        elif c == 13: ops.append(["SADD", "grow"] + ["g%d" % rng.randrange(400) for _ in range(5)])
    # directed: conversions + WRONGTYPE + edge counts (deterministic only)
    ops += [
        ["DEL", "iv"], ["SADD", "iv"] + [str(i) for i in range(200)], ["SCARD", "iv"],   # int compact -> table
        ["SADD", "iv", "notint"], ["SCARD", "iv"], ["SMEMBERS", "iv"],
        ["DEL", "big"], ["SADD", "big", "y" * 100], ["SCARD", "big"],                    # value > 64 forces table
        ["SET", "str", "v"], ["SADD", "str", "m"], ["SCARD", "str"],
        ["SADD", "one", "solo"], ["SPOP", "one"], ["EXISTS", "one"],                      # SPOP deterministic on 1-elem
        ["SADD", "one2", "solo2"], ["SRANDMEMBER", "one2"], ["SPOP", "one2", "1"],
        ["SPOP", "missingset"], ["SPOP", "missingset", "3"], ["SRANDMEMBER", "missingset"],
        ["SRANDMEMBER", "missingset", "5"], ["SRANDMEMBER", "missingset", "-5"],
        ["SMISMEMBER", "missingset", "a", "b"],
    ]
    return ops

def gen_list(rng):
    keys = ["l%d" % i for i in range(10)]
    vals = ["a", "bb", "ccc", "x" * 80, "v-%d" % 0] + ["item%d" % i for i in range(12)]
    ops = []
    def K(): return rng.choice(keys)
    def V(): return rng.choice(vals)
    for _ in range(3500):
        c = rng.randrange(16)
        if   c in (0,1): ops.append(["LPUSH", K()] + [V() for _ in range(rng.randrange(1,4))])
        elif c in (2,3): ops.append(["RPUSH", K()] + [V() for _ in range(rng.randrange(1,4))])
        elif c == 4: ops.append(["LPUSHX", K(), V()])
        elif c == 5: ops.append(["RPUSHX", K(), V()])
        elif c == 6: ops.append(["LPOP", K()] if rng.randrange(2) else ["LPOP", K(), str(rng.randrange(0,4))])
        elif c == 7: ops.append(["RPOP", K()] if rng.randrange(2) else ["RPOP", K(), str(rng.randrange(0,4))])
        elif c == 8: ops.append(["LLEN", K()])
        elif c == 9: ops.append(["LRANGE", K(), str(rng.randrange(-8,8)), str(rng.randrange(-8,8))])
        elif c == 10: ops.append(["LINDEX", K(), str(rng.randrange(-10,10))])
        elif c == 11: ops.append(["LSET", K(), str(rng.randrange(-6,6)), V()])
        elif c == 12: ops.append(["LINSERT", K(), rng.choice(["BEFORE","AFTER"]), V(), V()])
        elif c == 13: ops.append(["LREM", K(), str(rng.randrange(-3,4)), V()])
        elif c == 14: ops.append(["LTRIM", K(), str(rng.randrange(-6,6)), str(rng.randrange(-6,6))])
        elif c == 15: ops.append(["LPOS", K(), V()] + (["RANK", str(rng.choice([-2,-1,1,2])), "COUNT", str(rng.randrange(0,3))] if rng.randrange(2) else []))
    ops += [
        ["DEL","dl"], ["RPUSH","dl","a","b","c"], ["LTRIM","dl","1","0"], ["EXISTS","dl"],
        ["RPUSH","dl2","a"], ["LINSERT","dl2","BEFORE","missing","x"], ["LLEN","dl2"],
        ["LPOS","dl2","a","RANK","-1"], ["LPOS","missinglist","a"],
        ["RPUSH","big"] + ["e%d" % i for i in range(300)], ["LLEN","big"], ["LRANGE","big","250","260"],
        ["LPOP","big","5"], ["RPOP","big","5"], ["LREM","big","0","e100"], ["LLEN","big"],
        ["SET","strk","v"], ["LPUSH","strk","x"], ["LLEN","strk"],
        ["LSET","missinglist","0","v"], ["LPOP","missinglist","2"],
    ]
    return ops

def gen_stream(rng):
    """Explicit-ID primary arm. Approximate trim and wall-clock IDs are property-tested elsewhere."""
    keys = ["xs%d" % i for i in range(12)]
    next_id = {key: 0 for key in keys}
    known = {key: [] for key in keys}
    ops = []

    def add(key):
        next_id[key] += 1
        ident = "%d-%d" % (next_id[key] // 7 + 1, next_id[key] % 7)
        known[key].append(ident)
        fields = ["f", "v%d" % next_id[key]]
        if rng.randrange(3) == 0:
            fields += ["g", "x%d" % rng.randrange(100)]
        ops.append(["XADD", key, ident] + fields)

    for key in keys:
        add(key)
    for _ in range(4000):
        key = rng.choice(keys)
        choice = rng.randrange(12)
        if choice < 4:
            add(key)
        elif choice == 4:
            ops.append(["XLEN", key])
        elif choice == 5:
            start = rng.choice(["-", "0-0", "(" + (rng.choice(known[key]) if known[key] else "0-0"),
                                str(rng.randrange(0, 20))])
            end = rng.choice(["+", "%d-%d" % (rng.randrange(1, 30), rng.randrange(0, 8))])
            op = ["XRANGE", key, start, end]
            if rng.randrange(2): op += ["COUNT", str(rng.randrange(0, 6))]
            ops.append(op)
        elif choice == 6:
            end = rng.choice(["+", "%d-%d" % (rng.randrange(1, 30), rng.randrange(0, 8))])
            start = rng.choice(["-", "(" + (rng.choice(known[key]) if known[key] else "0-0")])
            op = ["XREVRANGE", key, end, start]
            if rng.randrange(2): op += ["COUNT", str(rng.randrange(0, 6))]
            ops.append(op)
        elif choice == 7:
            ids = [rng.choice(known[key]) if known[key] and rng.randrange(3) else
                   "%d-%d" % (rng.randrange(1, 30), rng.randrange(0, 8))
                   for _ in range(rng.randrange(1, 4))]
            ops.append(["XDEL", key] + ids)
        elif choice == 8:
            ops.append(["XTRIM", key, "MAXLEN", "=", str(rng.randrange(0, 12))])
        elif choice == 9:
            ops.append(["XTRIM", key, "MINID", "=",
                        "%d-%d" % (rng.randrange(1, 30), rng.randrange(0, 8))])
        elif choice == 10:
            cursor = rng.choice(known[key]) if known[key] and rng.randrange(2) else "0-0"
            ops.append(["XREAD", "COUNT", str(rng.randrange(1, 5)), "STREAMS", key, cursor])
        else:
            second = rng.choice(keys)
            c1 = rng.choice(known[key]) if known[key] and rng.randrange(2) else "0-0"
            c2 = rng.choice(known[second]) if known[second] and rng.randrange(2) else "0-0"
            ops.append(["XREAD", "COUNT", "2", "STREAMS", key, second, c1, c2])
    ops += [
        ["XADD", "edge", "0-0", "f", "v"], ["EXISTS", "edge"],
        ["XADD", "edge", "1-0", "f", "v"], ["XADD", "edge", "1-0", "f", "dup"],
        ["XADD", "edge", "1-*", "f", "auto-seq"], ["XADD", "edge", "1-*", "f", "auto-seq2"],
        ["XRANGE", "edge", "1", "1"], ["XRANGE", "edge", "(1-0", "+"],
        ["XRANGE", "edge", "-", "+", "COUNT", "0"],
        ["XDEL", "edge", "1-0", "bad-id"], ["XLEN", "edge"],
        ["XTRIM", "edge", "MAXLEN", "=", "0"], ["XLEN", "edge"],
        ["XADD", "nomake", "NOMKSTREAM", "*", "f", "v"], ["EXISTS", "nomake"],
        ["XADD", "edge", "MAXLEN", "=", "1", "LIMIT", "1", "2-0", "f", "v"],
        ["SET", "stream-wrong", "v"], ["XLEN", "stream-wrong"],
        ["XREAD", "STREAMS", "edge", "stream-wrong", "0-0", "0-0"],
    ]
    return ops

def gen_streamgrp(rng):
    """Consumer-group mix with explicit stream IDs and no wall-clock-valued replies."""
    keys = ["xg%d" % i for i in range(8)]
    consumers = ["a", "b", "c", "d"]
    next_id = {key: 5 for key in keys}
    known = {key: ["%d-0" % i for i in range(1, 6)] for key in keys}
    ops = []
    for key in keys:
        for ident in known[key]:
            ops.append(["XADD", key, ident, "f", "v" + ident])
        ops.append(["XGROUP", "CREATE", key, "g", "0-0", "ENTRIESREAD", "0"])

    for _ in range(4000):
        key = rng.choice(keys)
        consumer = rng.choice(consumers)
        choice = rng.randrange(16)
        if choice in (0, 1, 2):
            next_id[key] += 1
            ident = "%d-0" % next_id[key]
            known[key].append(ident)
            ops.append(["XADD", key, ident, "f", "v" + ident])
        elif choice == 3:
            ops.append(["XREADGROUP", "GROUP", "g", consumer, "COUNT",
                        str(rng.randrange(1, 5)), "STREAMS", key, ">"])
        elif choice == 4:
            cursor = rng.choice(["0-0"] + known[key])
            ops.append(["XREADGROUP", "GROUP", "g", consumer, "COUNT", "3",
                        "STREAMS", key, cursor])
        elif choice == 5:
            ids = [rng.choice(known[key]) for _ in range(rng.randrange(1, 4))]
            ops.append(["XACK", key, "g"] + ids)
        elif choice == 6:
            ops.append(["XPENDING", key, "g"])
        elif choice == 7:
            ids = [rng.choice(known[key]) for _ in range(rng.randrange(1, 4))]
            ops.append(["XCLAIM", key, "g", consumer, "0"] + ids + ["JUSTID"])
        elif choice == 8:
            ident = rng.choice(known[key])
            ops.append(["XCLAIM", key, "g", consumer, "0", ident, "FORCE", "JUSTID"])
        elif choice == 9:
            start = rng.choice(["0-0"] + known[key])
            ops.append(["XAUTOCLAIM", key, "g", consumer, "0", start,
                        "COUNT", str(rng.randrange(1, 4)), "JUSTID"])
        elif choice == 10:
            ids = [rng.choice(known[key]) for _ in range(rng.randrange(1, 3))]
            ops.append(["XDEL", key] + ids)
        elif choice == 11:
            ops.append(["XTRIM", key, "MAXLEN", "=", str(rng.randrange(2, 12))])
        elif choice == 12:
            ident = rng.choice(known[key])
            read = int(ident.split("-", 1)[0])
            ops.append(["XGROUP", "SETID", key, "g", ident,
                        "ENTRIESREAD", str(read)])
        elif choice == 13:
            ops.append(["XGROUP", "CREATECONSUMER", key, "g", consumer])
        elif choice == 14:
            ops.append(["XGROUP", "DELCONSUMER", key, "g", consumer])
        else:
            ops.append(["XPENDING", key, "g"])
    ops += [
        ["XGROUP", "CREATE", keys[0], "g", "0-0"],
        ["XACK", keys[0], "missing", "1-0"],
        ["XPENDING", keys[0], "missing"],
        ["XSETID", keys[0], "%d-0" % next_id[keys[0]]],
    ]
    return ops

def gen_zset(rng):
    keys = ["z%d" % i for i in range(8)]
    mems = ["m%d" % i for i in range(20)] + ["alpha", "beta", "x" * 70]
    scores = ["1", "2.5", "-3", "0", "1e3", "-1.5e-2", "inf", "-inf", "3.0000000000000004", "9007199254740993"]
    ops = []
    def K(): return rng.choice(keys)
    def M(): return rng.choice(mems)
    def S(): return rng.choice(scores)
    for _ in range(3500):
        c = rng.randrange(20)
        if   c in (0,1,2): ops.append(["ZADD", K()] + sum([[S(), M()] for _ in range(rng.randrange(1,4))], []))
        elif c == 3: ops.append(["ZADD", K(), rng.choice(["NX","XX","GT","LT"]), S(), M()])
        elif c == 4: ops.append(["ZADD", K(), "XX", "CH", S(), M()])
        elif c == 5: ops.append(["ZADD", K(), "INCR", S(), M()])
        elif c == 6: ops.append(["ZSCORE", K(), M()])
        elif c == 7: ops.append(["ZMSCORE", K(), M(), M()])
        elif c == 8: ops.append(["ZINCRBY", K(), S(), M()])
        elif c == 9: ops.append(["ZCARD", K()])
        elif c == 10: ops.append(["ZCOUNT", K(), rng.choice(["-inf","0","(1"]), rng.choice(["+inf","10","(5"])])
        elif c == 11: ops.append(["ZRANGE", K(), str(rng.randrange(-5,5)), str(rng.randrange(-5,5))] + (["WITHSCORES"] if rng.randrange(2) else []))
        elif c == 12: ops.append(["ZRANGE", K(), rng.choice(["-inf","(0","2"]), rng.choice(["+inf","8","(9"]), "BYSCORE"] + (["WITHSCORES"] if rng.randrange(2) else []))
        elif c == 13: ops.append(["ZRANGEBYSCORE", K(), "-inf", "+inf", "LIMIT", str(rng.randrange(0,3)), str(rng.randrange(-1,4))])
        elif c == 14: ops.append(["ZRANK", K(), M()] + (["WITHSCORE"] if rng.randrange(2) else []))
        elif c == 15: ops.append(["ZREVRANK", K(), M()])
        elif c == 16: ops.append(["ZREM", K(), M(), M()])
        elif c == 17: ops.append(["ZPOPMIN", K()] if rng.randrange(2) else ["ZPOPMAX", K(), str(rng.randrange(0,3))])
        elif c == 18: ops.append(["ZREMRANGEBYRANK", K(), str(rng.randrange(-4,3)), str(rng.randrange(-3,4))])
        elif c == 19: ops.append(["ZREMRANGEBYSCORE", K(), rng.choice(["-inf","(0"]), rng.choice(["+inf","5"])])
    ops += [
        ["DEL","lex"], ["ZADD","lex","0","a","0","b","0","c","0","d"],
        ["ZRANGEBYLEX","lex","-","+"], ["ZRANGEBYLEX","lex","[b","(d"], ["ZRANGEBYLEX","lex","(a","[c"],
        ["ZREVRANGEBYLEX","lex","+","-"], ["ZLEXCOUNT","lex","-","+"], ["ZLEXCOUNT","lex","[b","+"],
        ["ZRANGE","lex","(a","[c","BYLEX"], ["ZRANGE","lex","+","-","BYLEX","REV"],
        ["ZREMRANGEBYLEX","lex","[b","[c"], ["ZRANGE","lex","-","+","BYLEX"],
        ["ZADD","cf","NX","GT","1","m"], ["ZADD","cf","GT","LT","1","m"],
        ["ZADD","nan1","INCR","inf","m"], ["ZADD","nan1","INCR","-inf","m"],
        ["ZINCRBY","nan2","inf","m"], ["ZINCRBY","nan2","-inf","m"],
        ["ZADD","big"] + sum([["%d" % i, "bm%d" % i] for i in range(200)], []),
        ["ZCARD","big"], ["ZRANGE","big","95","105","WITHSCORES"], ["ZRANK","big","bm150"],
        ["ZPOPMIN","big","3"], ["ZPOPMAX","big","3"], ["ZCOUNT","big","(10","(20"],
        ["SET","strz","v"], ["ZADD","strz","1","m"], ["ZSCORE","strz","m"],
        ["ZPOPMIN","missing"], ["ZPOPMIN","missing","5"], ["ZRANK","missing","x","WITHSCORE"],
    ]
    return ops

def gen_zsetops(rng):
    zkeys = ["zo:z%d" % i for i in range(18)]
    skeys = ["zo:s%d" % i for i in range(8)]
    destinations = ["zo:d%d" % i for i in range(10)]
    members = ["m%d" % i for i in range(20)] + ["shared", "tie"]
    ops = []
    for _ in range(4200):
        c = rng.randrange(13)
        if c <= 2:
            key = rng.choice(zkeys)
            pairs = []
            for _ in range(rng.randrange(1, 4)):
                pairs += [str(rng.randrange(-30, 31)), rng.choice(members)]
            ops.append(["ZADD", key] + pairs)
        elif c == 3:
            ops.append(["SADD", rng.choice(skeys)] +
                       [rng.choice(members) for _ in range(rng.randrange(1, 5))])
        elif c == 4:
            ops.append(["DEL", rng.choice(zkeys + skeys)])
        elif c in (5, 6):
            command = "ZUNION" if c == 5 else "ZINTER"
            count = rng.randrange(1, 6)
            keys = [rng.choice(zkeys + skeys) for _ in range(count)]
            tail = []
            if rng.randrange(2):
                tail += ["WEIGHTS"] + [str(rng.randrange(-3, 4)) for _ in range(count)]
            if rng.randrange(2): tail += ["AGGREGATE", rng.choice(["SUM", "MIN", "MAX"])]
            if rng.randrange(2): tail += ["WITHSCORES"]
            ops.append([command, str(count)] + keys + tail)
        elif c == 7:
            count = rng.randrange(1, 6)
            ops.append(["ZDIFF", str(count)] +
                       [rng.choice(zkeys + skeys) for _ in range(count)] +
                       (["WITHSCORES"] if rng.randrange(2) else []))
        elif c == 8:
            count = rng.randrange(1, 6)
            tail = [] if rng.randrange(2) else ["LIMIT", str(rng.randrange(0, 8))]
            ops.append(["ZINTERCARD", str(count)] +
                       [rng.choice(zkeys + skeys) for _ in range(count)] + tail)
        elif c in (9, 10):
            command = "ZUNIONSTORE" if c == 9 else "ZINTERSTORE"
            count = rng.randrange(1, 6)
            keys = [rng.choice(zkeys + skeys) for _ in range(count)]
            tail = []
            if rng.randrange(2):
                tail += ["WEIGHTS"] + [str(rng.randrange(-3, 4)) for _ in range(count)]
            if rng.randrange(2): tail += ["AGGREGATE", rng.choice(["SUM", "MIN", "MAX"])]
            ops.append([command, rng.choice(destinations), str(count)] + keys + tail)
        elif c == 11:
            count = rng.randrange(1, 6)
            ops.append(["ZDIFFSTORE", rng.choice(destinations), str(count)] +
                       [rng.choice(zkeys + skeys) for _ in range(count)])
        else:
            ops.append(["ZRANGE", rng.choice(destinations), "0", "-1", "WITHSCORES"])
    return ops

def gen_geo(rng):
    keys = ["geo:g%d" % i for i in range(10)]
    destinations = ["geo:d%d" % i for i in range(10)]
    points = [
        ("13.361389", "38.115556", "palermo"),
        ("15.087269", "37.502669", "catania"),
        ("-73.985700", "40.748400", "nyc"),
        ("139.691700", "35.689500", "tokyo"),
        ("2.294500", "48.858400", "paris"),
        ("151.209300", "-33.868800", "sydney"),
    ]
    units = ["m", "km", "ft", "mi"]
    ops = []
    for _ in range(4200):
        c = rng.randrange(19)
        key = rng.choice(keys)
        lon, lat, member = rng.choice(points)
        if c <= 3:
            option = rng.choice([[], ["NX"], ["XX"], ["CH"], ["NX", "CH"], ["XX", "CH"]])
            ops.append(["GEOADD", key] + option + [lon, lat, member])
        elif c == 4:
            ops.append(["GEOPOS", key, member, rng.choice(points)[2], "missing"])
        elif c == 5:
            ops.append(["GEOHASH", key, member, rng.choice(points)[2]])
        elif c == 6:
            ops.append(["GEODIST", key, member, rng.choice(points)[2], rng.choice(units)])
        elif c in (7, 8):
            center = rng.choice(points)
            source = ["FROMMEMBER", center[2]] if c == 7 else ["FROMLONLAT", center[0], center[1]]
            shape = ["BYRADIUS", str(rng.choice([1, 30, 200, 20000])), rng.choice(units)]
            tail = rng.choice([["ASC"], ["DESC"], ["ASC", "WITHDIST"],
                               ["ASC", "WITHHASH"], ["ASC", "WITHCOORD"],
                               ["ASC", "WITHDIST", "WITHHASH", "WITHCOORD"],
                               ["ASC", "COUNT", "2"], ["ASC", "COUNT", "1"]])
            ops.append(["GEOSEARCH", key] + source + shape + tail)
        elif c == 9:
            center = rng.choice(points)
            ops.append(["GEOSEARCH", key, "FROMLONLAT", center[0], center[1],
                        "BYBOX", str(rng.choice([10, 500, 40000])),
                        str(rng.choice([10, 500, 40000])), "km",
                        rng.choice(["ASC", "DESC"])])
        elif c in (10, 11):
            center = rng.choice(points)
            tail = ["STOREDIST"] if c == 11 else []
            ops.append(["GEOSEARCHSTORE", rng.choice(destinations), key,
                        "FROMMEMBER", center[2], "BYRADIUS",
                        str(rng.choice([30, 200, 20000])), "km"] + tail)
        elif c == 12:
            center = rng.choice(points)
            ops.append(["GEORADIUS", key, center[0], center[1],
                        str(rng.choice([30, 200, 20000])), "km",
                        rng.choice(["ASC", "DESC"]), "COUNT", "3"])
        elif c == 13:
            center = rng.choice(points)
            ops.append(["GEORADIUS", key, center[0], center[1],
                        str(rng.choice([30, 200, 20000])), "km", "STORE",
                        rng.choice(destinations)])
        elif c == 14:
            center = rng.choice(points)
            ops.append(["GEORADIUSBYMEMBER", key, center[2],
                        str(rng.choice([30, 200, 20000])), "km",
                        rng.choice(["ASC", "DESC"])])
        elif c == 15:
            center = rng.choice(points)
            ops.append(["GEORADIUSBYMEMBER", key, center[2],
                        str(rng.choice([30, 200, 20000])), "km",
                        rng.choice(["STORE", "STOREDIST"]), rng.choice(destinations)])
        elif c == 16:
            center = rng.choice(points)
            ops.append(["GEORADIUS_RO", key, center[0], center[1],
                        str(rng.choice([30, 200, 20000])), "km",
                        rng.choice(["ASC", "DESC"])])
        elif c == 17:
            center = rng.choice(points)
            ops.append(["GEORADIUSBYMEMBER_RO", key, center[2],
                        str(rng.choice([30, 200, 20000])), "km",
                        rng.choice(["ASC", "DESC"])])
        else:
            ops.append(["ZCARD", rng.choice(destinations)])
    return ops


def gen_doubles(rng):
    """Every double-taking argument, written in every spelling redis's float parsers accept.

    Two things are byte-compared here that used to be excluded from every suite.  The FORMATTER arm
    drives values where Grisu2's digits are not the shortest round-trip digits, which is where the
    old std::to_chars renderer diverged.  The GRAMMAR arm drives the spellings the old parsers
    refused -- hexadecimal, an empty or space-padded bound, an overflowing bound -- and the
    spellings they must still refuse.  Both sides are deterministic, so the stream can reach zero.

    Blocking pops always run against a key made ready in the SAME op batch, so a valid timeout pops
    instead of parking a connection the differ cannot un-park.  The searched geo key keeps its two
    fixed members, whose distances are distinct: coordinate spellings are added to a separate key
    and read back by score, because equal-distance matches have no defined order on either server.
    """
    # Values chosen so the reply exercises the formatter, not the arithmetic: each is applied to a
    # fresh member, so the stored score is the parsed argument and nothing else.
    formatter = ["1e126", "1000000000000000.2", "7.18288826277728e18", "9474039721387.188",
                 "-1.1919455290949598e-37", "1e23", "9223372036854775808",
                 "4611686018427387904", "-4611686018427387904", "1e15", "1e18", "1e-5",
                 "1e-6", "1e-7", "5e-324", "1.7976931348623157e308", "3.141592653589793",
                 "0.5", "-0", "1e-320", "2e125", "1.5e-10"]
    # Spellings a VALUE accepts (hex included) and spellings it refuses.
    values_ok = ["0x10", "0X1A", "0x1p-2", "+5", "05", ".5", "5.", "5e-324", "infinity",
                 "INF", "-inf", "-3.5", "1000", "0"]
    values_bad = ["", " 5", "\t5", "5 ", "5\x005", "1e309", "-1e309", "1e-400", "nan",
                  "-nan", "abc", "5abc", "++5", "1e", ".", "0b101", "1_000"]
    # Spellings a BOUND accepts on top of those, and the ones it still refuses.
    bounds_ok = ["", "(", " 5", "\t\n 5", "0x10", "(0x10", "1e309", "-1e309", "1e-400",
                 "5\x005", "-inf", "+inf", "(5", "0", "16", "-5"]
    bounds_bad = ["nan", "5 ", "abc", "5abc", "++5", ".", "1e", " (5"]
    timeouts_ok = ["0x10", "0x1p-2", "+5", "+0", "1e-400", "5e-324", "-0.0004", "0.0005", "0.25"]
    timeouts_bad = ["1e309", "inf", "infinity", "9223372036854775.808", "-inf", "-1e309",
                    "-0.5", " 5", "", "nan", "abc", "9223372036854775"]
    sort_words = ["", " 5", "0x10", "1", "2", "-3", "5e-324", "1e400", "inf", "nan", "abc",
                  "5\x005", "+5", "05"]
    zkeys = ["db:z%d" % i for i in range(6)]

    ops = [["DEL"] + zkeys +
           ["db:r", "db:f", "db:h", "db:g", "db:gadd", "db:s", "db:t", "db:tz", "db:zi", "db:rw"]]
    ops += [["ZADD", "db:r", "-5", "a", "0", "b", "5", "c", "16", "d"],
            ["GEOADD", "db:g", "13.361389", "38.115556", "palermo"],
            ["GEOADD", "db:g", "15.087269", "37.502669", "catania"]]

    member = [0]

    def fresh():
        member[0] += 1
        return "m%d" % member[0]

    # A store's DESTINATION is never read back inside this stream, and never reused. Reading it
    # here found a cross-shard anomaly that has nothing to do with float text: on a long pipeline
    # a ZRANGESTORE can answer :1 while a ZRANGE of the destination two ops later still answers an
    # empty array, and the effect turns up against an EARLIER read instead. It reproduces on some
    # boots and not others (routing is seeded per boot), so it belongs to the scatter engine, not
    # to this lane -- NOTES-DOUBLES2.md carries the reproducer. The parse evidence this suite needs
    # is the store's own reply (a count, or the exact error), which is compared as usual.
    destination = [0]

    def dest():
        destination[0] += 1
        return "db:d%d" % destination[0]

    while len(ops) < 5000:
        choice = rng.randrange(22)
        key = rng.choice(zkeys)
        if choice <= 2:                                   # formatter, through three reply shapes
            value = rng.choice(formatter)
            name = fresh()
            ops += [["ZADD", key, "INCR", value, name],
                    ["ZSCORE", key, name],
                    ["ZMSCORE", key, name, "absent"]]
        elif choice == 3:
            ops.append(["ZADD", key, rng.choice(values_ok + values_bad), fresh()])
        elif choice == 4:
            ops.append(["ZADD", key, "GT", "CH", rng.choice(values_ok + values_bad), fresh()])
        elif choice == 5:
            ops += [["ZADD", "db:zi", "1", "m"],
                    ["ZINCRBY", "db:zi", rng.choice(values_ok + values_bad), "m"],
                    ["ZSCORE", "db:zi", "m"], ["DEL", "db:zi"]]
        elif choice in (6, 7):
            lo = rng.choice(bounds_ok + bounds_bad)
            hi = rng.choice(bounds_ok + bounds_bad)
            ops.append([rng.choice(["ZCOUNT", "ZRANGEBYSCORE"]), "db:r", lo, hi])
        elif choice == 8:
            ops.append(["ZREVRANGEBYSCORE", "db:r", rng.choice(bounds_ok + bounds_bad), "-inf"])
        elif choice == 9:
            ops.append(["ZRANGE", "db:r", rng.choice(bounds_ok + bounds_bad), "+inf", "BYSCORE",
                        "WITHSCORES"])
        elif choice == 10:
            ops.append(["ZRANGESTORE", dest(), "db:r", rng.choice(bounds_ok + bounds_bad),
                        "+inf", "BYSCORE"])
        elif choice == 11:
            ops += [["ZADD", "db:rw", "1", "a", "2", "b"],
                    ["ZREMRANGEBYSCORE", "db:rw", rng.choice(bounds_ok + bounds_bad), "(-inf"],
                    ["ZRANGE", "db:rw", "0", "-1", "WITHSCORES"], ["DEL", "db:rw"]]
        elif choice == 12:
            weight = rng.choice(values_ok + values_bad)
            ops.append([rng.choice(["ZUNION", "ZINTER"]), "1", "db:r", "WEIGHTS", weight,
                        "WITHSCORES"])
        elif choice == 13:
            ops.append([rng.choice(["ZUNIONSTORE", "ZINTERSTORE"]), dest(), "2", "db:r", key,
                        "WEIGHTS", rng.choice(values_ok + values_bad),
                        rng.choice(values_ok + values_bad),
                        "AGGREGATE", rng.choice(["SUM", "MIN", "MAX"])])
        elif choice == 14:
            ops += [["SET", "db:f", rng.choice(["1", "0x10", " 1", "2.5"])],
                    ["INCRBYFLOAT", "db:f", rng.choice(values_ok + values_bad)],
                    ["GET", "db:f"]]
        elif choice == 15:
            ops += [["DEL", "db:h"], ["HSET", "db:h", "f", rng.choice(["1", "0x10", " 1"])],
                    ["HINCRBYFLOAT", "db:h", "f", rng.choice(values_ok + values_bad)],
                    ["HGET", "db:h", "f"]]
        elif choice == 16:
            # Coordinate spellings go to their OWN key, read back by score rather than by
            # distance. Adding them to the searched key instead put many members at identical
            # coordinates, and the order of equal-distance matches is not defined by either
            # server -- a tie, not a defect, but it makes the reply uncomparable.
            name = fresh()
            ops += [["GEOADD", "db:gadd", rng.choice(values_ok + values_bad), "10", name],
                    ["ZSCORE", "db:gadd", name], ["ZCARD", "db:gadd"], ["DEL", "db:gadd"]]
        elif choice == 17:
            radius = rng.choice(values_ok + values_bad)
            ops.append(rng.choice([
                ["GEOSEARCH", "db:g", "FROMLONLAT", "13", "38", "BYRADIUS", radius, "km", "ASC"],
                ["GEOSEARCH", "db:g", "FROMLONLAT", "13", "38", "BYBOX", radius, radius, "km",
                 "ASC"],
                ["GEORADIUS_RO", "db:g", "13", "38", radius, "km", "ASC"],
                ["GEORADIUSBYMEMBER_RO", "db:g", "palermo", radius, "km", "ASC"],
            ]))
        elif choice == 18:
            ops.append(["GEOSEARCHSTORE", dest(), "db:g", "FROMLONLAT", "13", "38", "BYRADIUS",
                        rng.choice(values_ok + values_bad), "km"])
        elif choice == 19:
            # Numeric SORT is covered by the directed battery instead, and SORT ... ALPHA is not
            # covered at all. Both are excluded for reasons that were MEASURED, not assumed:
            #
            #   * sortCommand reads errno without clearing it, so any command earlier in the SAME
            #     pipeline batch that made strtod raise ERANGE poisons the next numeric SORT --
            #     including a SUCCESSFUL one, e.g. `ZADD z 5e-324 m` then `SORT list`, which
            #     answers "One or more scores can't be converted into double" on redis and sorts
            #     here. That is a leak, not a rule, so it is not reproduced; a suite carrying it
            #     could never reach zero.
            #   * SORT ... ALPHA orders with strcoll under the process locale (redis calls
            #     setlocale(LC_COLLATE,"")), so under en_US.UTF-8 it puts "1" before "-3". This
            #     tree orders by bytes. That is a collation difference, not a float difference,
            #     and it predates this lane.
            #
            # The slot still fires: it drives the same list through a lexical path both servers
            # agree on, so the arm is not silently dead.
            ops += [["DEL", "db:s"],
                    ["RPUSH", "db:s", rng.choice(sort_words), rng.choice(sort_words), "1"],
                    ["LRANGE", "db:s", "0", "-1"], ["DEL", "db:s"]]
        elif choice == 20:                                # blocking timeouts, key made ready first
            timeout = rng.choice(timeouts_ok + timeouts_bad)
            ops += [["DEL", "db:t"], ["RPUSH", "db:t", "v"],
                    [rng.choice(["BLPOP", "BRPOP"]), "db:t", timeout],
                    ["LLEN", "db:t"], ["DEL", "db:t"]]
        else:
            timeout = rng.choice(timeouts_ok + timeouts_bad)
            ops += [["DEL", "db:tz"], ["ZADD", "db:tz", "1.25", "m"],
                    [rng.choice(["BZPOPMIN", "BZPOPMAX"]), "db:tz", timeout],
                    ["ZCARD", "db:tz"], ["DEL", "db:tz"]]
    return ops[:5000]


def gen_hash(rng):
    keys = ["h%d" % i for i in range(12)]
    fields = ["f%d" % i for i in range(20)] + ["", "bin\x00fld", "F" * 80]
    vals = ["", "x", "hello world", "12345", "-7", "9223372036854775807", "1e100",
            "v" * 120, "\x00\x01\xff"]
    nums = ["1", "-3", "100", "9223372036854775807", "-9223372036854775808", "notanum"]
    floats = ["0.1", "-3.5e2", "1e100", "nan", "abc", "5.0e3", "10.5"]
    ops = []
    def K(): return rng.choice(keys)
    def F(): return rng.choice(fields)
    for _ in range(3500):
        c = rng.randrange(16)
        if   c in (0, 1, 2):
            pairs = []
            for _ in range(rng.randrange(1, 4)): pairs += [F(), rng.choice(vals)]
            ops.append(["HSET", K()] + pairs)
        elif c == 3: ops.append(["HGET", K(), F()])
        elif c == 4: ops.append(["HDEL", K()] + [F() for _ in range(rng.randrange(1, 3))])
        elif c == 5: ops.append(["HMGET", K()] + [F() for _ in range(rng.randrange(1, 4))])
        elif c == 6: ops.append(["HSETNX", K(), F(), rng.choice(vals)])
        elif c == 7: ops.append(["HLEN", K()])
        elif c == 8: ops.append(["HEXISTS", K(), F()])
        elif c == 9: ops.append(["HSTRLEN", K(), F()])
        elif c == 10: ops.append(["HINCRBY", K(), rng.choice(fields[:6]), rng.choice(nums)])
        elif c == 11: ops.append(["HINCRBYFLOAT", K(), rng.choice(fields[:6]), rng.choice(floats)])
        elif c == 12: ops.append([rng.choice(["HGETALL", "HKEYS", "HVALS"]), K()])
        elif c == 13: ops.append(["DEL", K()])
        elif c == 14: ops.append([rng.choice(["TYPE", "EXISTS", "HLEN"]), K()])
        elif c == 15: ops.append(["HSET", "hgrow"] + ["g%d" % rng.randrange(300), rng.choice(vals)])
    # directed: promotion, big values, binary, WRONGTYPE, deterministic HRANDFIELD forms
    ops += [
        ["DEL", "hp"], ["HSET", "hp"] + sum((["p%d" % i, "v%d" % i] for i in range(200)), []),
        ["HLEN", "hp"], ["HGET", "hp", "p150"], ["HGETALL", "hp"],
        ["DEL", "hbig"], ["HSET", "hbig", "f", "Y" * 100], ["HSTRLEN", "hbig", "f"],
        ["HSET", "hnul", "a\x00b", "c\x00d"], ["HGET", "hnul", "a\x00b"], ["HGETALL", "hnul"],
        ["HSET", "hemp", "", ""], ["HGET", "hemp", ""], ["HSTRLEN", "hemp", ""], ["HLEN", "hemp"],
        ["SET", "hstr", "v"], ["HSET", "hstr", "f", "v"], ["HGET", "hstr", "f"],
        ["HSET", "h0", "wt", "x"], ["GET", "h0"], ["APPEND", "h0", "y"],
        ["HRANDFIELD", "missingh"], ["HRANDFIELD", "missingh", "3"],
        ["HRANDFIELD", "missingh", "-3"], ["HRANDFIELD", "missingh", "3", "WITHVALUES"],
        ["DEL", "hone"], ["HSET", "hone", "solo", "sv"],
        ["HRANDFIELD", "hone"], ["HRANDFIELD", "hone", "5"], ["HRANDFIELD", "hone", "-4"],
        ["HRANDFIELD", "hone", "5", "WITHVALUES"], ["HRANDFIELD", "hone", "0"],
        ["HSET", "hdelall", "a", "1", "b", "2"], ["HDEL", "hdelall", "a", "b"],
        ["EXISTS", "hdelall"], ["TYPE", "hdelall"],
        ["HSETNX", "hnx", "f", "first"], ["HSETNX", "hnx", "f", "second"], ["HGET", "hnx", "f"],
        ["HINCRBY", "hof", "f", "9223372036854775807"], ["HINCRBY", "hof", "f", "1"],
        ["HSET", "hif", "f", "5"], ["HINCRBYFLOAT", "hif", "f", "0.1"], ["HGET", "hif", "f"],
        ["HMGET", "missingh2", "a", "b", "c"],
    ]
    return ops

def gen_hexpire(rng):
    # Hash-field TTLs.  Every deadline is ABSOLUTE and either far in the future or definitively in
    # the past, so the whole stream is deterministic on both servers: no sleep, no clock race, and
    # the "already past" deadlines exercise the immediate-delete return code (2) reproducibly.
    keys = ["hx%d" % i for i in range(10)]
    fields = ["f%d" % i for i in range(14)] + ["", "bin\x00fld", "L" * 70]
    vals = ["", "v", "hello world", "12345", "-7", "w" * 130, "\x00\x01\xff"]
    base = 1900000000000                      # ~2030, comfortably inside the 46-bit ceiling
    future_ms = [base, base + 1, base + 7777, base + 3600000, base + 86400000]
    past_ms = [1, 2, 1000, 1500000000000 - 1000000000]
    conds = [[], ["NX"], ["XX"], ["GT"], ["LT"]]
    setters = ["HEXPIREAT", "HPEXPIREAT"]
    readers = ["HTTL", "HPTTL", "HEXPIRETIME", "HPEXPIRETIME"]
    ops = []
    def K(): return rng.choice(keys)
    def F(): return rng.choice(fields)
    def flist(n): return [F() for _ in range(n)]

    # seed every key so the interesting branches are reachable from op 0
    for k in keys:
        pairs = []
        for i in range(6):
            pairs += ["f%d" % i, "v%d" % i]
        ops.append(["HSET", k] + pairs)

    for _ in range(4200):
        c = rng.randrange(22)
        if c in (0, 1, 2, 3, 4):
            cmd = rng.choice(setters)
            at = rng.choice(future_ms if rng.randrange(5) else past_ms)
            if cmd == "HEXPIREAT":
                at //= 1000
            n = rng.randrange(1, 4)
            fs = flist(n)
            ops.append([cmd, K(), str(at)] + rng.choice(conds) + ["FIELDS", str(n)] + fs)
        elif c in (5, 6, 7):
            n = rng.randrange(1, 4)
            ops.append([rng.choice(readers), K(), "FIELDS", str(n)] + flist(n))
        elif c == 8:
            n = rng.randrange(1, 3)
            ops.append(["HPERSIST", K(), "FIELDS", str(n)] + flist(n))
        elif c in (9, 10):
            pairs = []
            for _ in range(rng.randrange(1, 3)):
                pairs += [F(), rng.choice(vals)]
            ops.append(["HSET", K()] + pairs)
        elif c == 11:
            ops.append(["HDEL", K()] + flist(rng.randrange(1, 3)))
        elif c == 12:
            ops.append(["HGET", K(), F()])
        elif c == 13:
            ops.append(["HMGET", K()] + flist(rng.randrange(1, 4)))
        elif c == 14:
            ops.append(["HGETALL", K()])
        elif c == 15:
            ops.append([rng.choice(["HLEN", "EXISTS", "TYPE"]), K()])
        elif c == 16:
            ops.append(["HEXISTS", K(), F()])
        elif c == 17:
            ops.append(["HSTRLEN", K(), F()])
        elif c == 18:
            ops.append(["HSETNX", K(), F(), rng.choice(vals)])
        elif c == 19:
            ops.append(["HINCRBY", K(), rng.choice(fields[:4]), rng.choice(["1", "-2"])])
        elif c == 20:
            ops.append(["DEL", K()])
        else:
            ops.append(["HSET", K(), F(), rng.choice(vals)])

    # directed tail: the error surface, the ceiling, and the representation transitions
    far = str(base + 500)
    ops += [
        ["DEL", "hxe"], ["SET", "hxstr", "v"],
        ["HEXPIRE", "hxstr", "100", "FIELDS", "1", "a"],
        ["HTTL", "hxstr", "FIELDS", "1", "a"],
        ["HPERSIST", "hxstr", "FIELDS", "1", "a"],
        ["HEXPIREAT", "hxmissing", "99999999", "FIELDS", "2", "a", "b"],
        ["HTTL", "hxmissing", "FIELDS", "3", "a", "b", "c"],
        ["HPEXPIREAT", "hxe", "abc", "FIELDS", "1", "a"],
        ["HPEXPIREAT", "hxe", "-1", "FIELDS", "1", "a"],
        ["HPEXPIREAT", "hxe", "70368744177664", "FIELDS", "1", "a"],
        ["HEXPIREAT", "hxe", "70368744178", "FIELDS", "1", "a"],
        ["HEXPIRE", "hxe", "1000000000000", "FIELDS", "1", "a"],
        ["HPEXPIRE", "hxe", "70368744177664", "FIELDS", "1", "a"],
        ["HEXPIREAT", "hxe", "100", "FIELDS", "2", "a"],
        ["HEXPIREAT", "hxe", "100", "FIELDS", "1", "a", "b"],
        ["HEXPIREAT", "hxe", "100", "FIELDS", "0", "a"],
        ["HEXPIREAT", "hxe", "100", "FIELDS", "-3", "a"],
        ["HEXPIREAT", "hxe", "100", "FIELDS", "zz", "a"],
        ["HTTL", "hxe", "FIELDS", "zz", "a"], ["HTTL", "hxe", "FIELDS", "0", "a"],
        ["HEXPIREAT", "hxe", "100", "NX", "XX", "FIELDS", "1", "a"],
        ["HEXPIREAT", "hxe", "100", "GT", "LT", "FIELDS", "1", "a"],
        ["HEXPIREAT", "hxe", "100", "NOPE", "1", "a"],
        ["HPERSIST", "hxe", "NOPE", "1", "a"],
        # one-field hash: the first deadline changes its internal representation
        ["DEL", "hxsmall"], ["HSET", "hxsmall", "solo", "sv"],
        ["HPEXPIREAT", "hxsmall", far, "FIELDS", "1", "solo"],
        ["HGET", "hxsmall", "solo"], ["HPEXPIRETIME", "hxsmall", "FIELDS", "1", "solo"],
        ["HPERSIST", "hxsmall", "FIELDS", "1", "solo"],
        ["HPEXPIRETIME", "hxsmall", "FIELDS", "1", "solo"],
        # last field expires immediately -> the key must disappear
        ["DEL", "hxlast"], ["HSET", "hxlast", "a", "1", "b", "2"],
        ["HEXPIREAT", "hxlast", "1", "FIELDS", "1", "a"],
        ["HLEN", "hxlast"], ["EXISTS", "hxlast"],
        ["HEXPIREAT", "hxlast", "1", "FIELDS", "1", "b"],
        ["HLEN", "hxlast"], ["EXISTS", "hxlast"], ["TYPE", "hxlast"],
        # value writes clear the deadline, counter writes keep it
        ["DEL", "hxw"], ["HSET", "hxw", "a", "1", "n", "5"],
        ["HPEXPIREAT", "hxw", far, "FIELDS", "2", "a", "n"],
        ["HSET", "hxw", "a", "2"], ["HPEXPIRETIME", "hxw", "FIELDS", "2", "a", "n"],
        ["HINCRBY", "hxw", "n", "1"], ["HPEXPIRETIME", "hxw", "FIELDS", "1", "n"],
        ["HSETNX", "hxw", "n", "9"], ["HPEXPIRETIME", "hxw", "FIELDS", "1", "n"],
        ["HDEL", "hxw", "n"], ["HSET", "hxw", "n", "5"],
        ["HPEXPIRETIME", "hxw", "FIELDS", "1", "n"],
        # deadlines travel with the value
        ["DEL", "hxc"], ["DEL", "hxc2"], ["HSET", "hxc", "a", "1", "b", "2"],
        ["HPEXPIREAT", "hxc", far, "FIELDS", "1", "a"],
        ["COPY", "hxc", "hxc2"], ["HPEXPIRETIME", "hxc2", "FIELDS", "2", "a", "b"],
        ["HPERSIST", "hxc2", "FIELDS", "1", "a"],
        ["HPEXPIRETIME", "hxc", "FIELDS", "1", "a"],
        ["RENAME", "hxc", "hxc3"], ["HPEXPIRETIME", "hxc3", "FIELDS", "2", "a", "b"],
        # a promoted hash carrying deadlines on a subset
        ["DEL", "hxbig"],
        ["HSET", "hxbig"] + sum(([("g%d" % i), ("gv%d" % i)] for i in range(200)), []),
        ["HPEXPIREAT", "hxbig", far, "FIELDS", "3", "g0", "g100", "g199"],
        ["HPEXPIRETIME", "hxbig", "FIELDS", "4", "g0", "g100", "g199", "g5"],
        ["HLEN", "hxbig"], ["HGET", "hxbig", "g100"],
        ["HEXPIREAT", "hxbig", "1", "FIELDS", "2", "g0", "g5"],
        ["HLEN", "hxbig"], ["HGET", "hxbig", "g0"],
        # binary and empty field names
        ["DEL", "hxbin"], ["HSET", "hxbin", "a\x00b", "1", "", "2"],
        ["HPEXPIREAT", "hxbin", far, "FIELDS", "2", "a\x00b", ""],
        ["HPEXPIRETIME", "hxbin", "FIELDS", "2", "a\x00b", ""],
        ["HGETALL", "hxbin"],
        # case-insensitive tokens
        ["hpexpireat", "hxbin", far, "nx", "fields", "1", "a\x00b"],
        ["httl", "hxbin", "fields", "1", "a\x00b"],
    ]
    return ops

def gen_edgetime(rng):
    """Expiry where it meets cross-shard commands, introspection and hash-field deadlines.

    Two rules make expiry byte-comparable across two servers that execute the same pipeline a few
    milliseconds apart:

    * ACTIVE EXPIRY IS OFF for the whole suite (`DEBUG SET-ACTIVE-EXPIRE 0`, which both servers
      spell the same way). Every removal is then driven by a command in this stream, so both sides
      reap the same keys at the same point in the stream instead of racing their background cycles.
    * DEADLINES ARE ABSOLUTE. `future_ms` is a month out and `past_ms` is 1, so "elapsed" and
      "live" are properties of the deadline rather than of when a server got round to the op.

    Deliberately not in the stream: DBSIZE and INFO (batch-published on TomoKV by design,
    NOTES-COMPAT.md), SCAN (COUNT is a slot-work hint here, so the two servers reap different
    entries per call and the states drift apart), OBJECT ENCODING (comparable only with the
    encoding-knob alignment the servertail suite performs), and RANDOMKEY (its shard-choice
    divergence is a separate shelved finding, written up in NOTES-EDGETIME.md).
    """
    stamp = int(time.time() * 1000)
    future_ms = stamp + 30 * 24 * 60 * 60 * 1000
    future_s = future_ms // 1000
    past_ms = 1

    strings = ["et:s:%02d:%s" %
               (i, "".join(rng.choice("abcdef0123456789") for _ in range(34)))
               for i in range(36)]
    sets = ["et:set:%02d:%s" %
            (i, "".join(rng.choice("abcdef0123456789") for _ in range(30)))
            for i in range(12)]
    hashes = ["et:h:%02d:%s" %
              (i, "".join(rng.choice("abcdef0123456789") for _ in range(30)))
              for i in range(10)]
    values = ["", "v", "hello", "42", "value-" + "x" * 90]
    members = ["m%d" % i for i in range(12)]
    fields = ["f%d" % i for i in range(8)]
    # Redis folds repeated flags and accepts XX with GT or LT; only NX+{XX,GT,LT} and GT+LT are
    # refused, and an unknown token outranks both refusals.
    conditions = [[], ["NX"], ["XX"], ["GT"], ["LT"],
                  ["NX", "NX"], ["XX", "XX"], ["GT", "GT"], ["LT", "LT"],
                  ["XX", "GT"], ["XX", "LT"], ["xx", "gt"], ["Nx"]]
    ops = [["DEBUG", "SET-ACTIVE-EXPIRE", "0"]]

    def sk(): return rng.choice(strings)
    def setk(): return rng.choice(sets)
    def hk(): return rng.choice(hashes)
    def sample(pool, low, high): return rng.sample(pool, rng.randrange(low, high))
    def pairs(n):
        out = []
        for key in rng.sample(strings, n):
            out += [key, rng.choice(values)]
        return out

    for key in strings:
        ops.append(["SET", key, rng.choice(values)])
    for key in sets:
        ops.append(["SADD", key] + rng.sample(members, 4))
    for key in hashes:
        ops.append(["HSET", key, "f0", "v0", "f1", "v1", "f2", "v2"])

    for _ in range(4200):
        choice = rng.randrange(36)
        if choice == 0:
            ops.append(["SET", sk(), rng.choice(values)])
        elif choice == 1:
            ops.append(["SET", sk(), rng.choice(values), "KEEPTTL"])
        elif choice == 2:
            ops.append(["PEXPIREAT", sk(), str(future_ms)] + rng.choice(conditions))
        elif choice == 3:
            ops.append(["PEXPIREAT", sk(), str(past_ms)] + rng.choice(conditions))
        elif choice == 4:
            ops.append(["EXPIREAT", sk(), str(future_s)] + rng.choice(conditions))
        elif choice == 5:
            ops.append(["PERSIST", sk()])
        elif choice == 6:
            ops.append([rng.choice(["TTL", "PTTL", "EXPIRETIME", "PEXPIRETIME"]), sk()])
        elif choice == 7:
            ops.append(["GETEX", sk(), "PXAT", str(future_ms)])
        elif choice == 8:
            ops.append(["GETEX", sk(), "PXAT", str(past_ms)])
        elif choice == 9:
            ops.append(["GETEX", sk(), "PERSIST"])
        elif choice == 10:
            ops.append(["GET", sk()])
        elif choice == 11:
            ops.append(["MGET"] + sample(strings, 2, 8))
        elif choice == 12:
            ops.append(["MSET"] + pairs(rng.randrange(2, 7)))
        elif choice == 13:
            ops.append(["DEL"] + sample(strings, 2, 7))
        elif choice == 14:
            source, destination = rng.sample(strings, 2)
            ops.append(["RENAME", source, destination])
        elif choice == 15:
            ops.append(["EXISTS"] + sample(strings, 2, 8))
        elif choice == 16:
            ops.append(["TYPE", sk()])
        elif choice == 17:
            # Normalized to hit-vs-miss by normalize_introspection(): an expired-but-unreaped key
            # is still resident, so redis (and now TomoKV) still report its footprint.
            ops.append(["MEMORY", "USAGE", sk()])
        elif choice == 18:
            ops.append(["KEYS", "et:s:*"])
        elif choice == 19:
            # DUMP is deliberately excluded: its payload encoding is an intentional divergence
            # (see the wiredump suite below), so it can only be compared for presence.
            ops.append(["STRLEN", sk()] if rng.randrange(2) else ["TOUCH"] + sample(strings, 2, 6))
        elif choice == 20:
            ops.append(["SADD", setk(), rng.choice(members), rng.choice(members)])
        elif choice == 21:
            ops.append(["PEXPIREAT", setk(), str(rng.choice([future_ms, past_ms]))])
        elif choice == 22:
            destination = rng.choice(strings + sets)
            ops.append(["SINTERSTORE", destination] + sample(sets, 2, 5))
        elif choice == 23:
            ops.append(["SMEMBERS", setk()])
        elif choice == 24:
            ops.append(["HSET", hk(), rng.choice(fields), rng.choice(values)])
        elif choice == 25:
            ops.append(["HPEXPIREAT", hk(), str(future_ms), "FIELDS", "1", rng.choice(fields)])
        elif choice == 26:
            ops.append(["HPEXPIREAT", hk(), str(past_ms), "FIELDS", "1", rng.choice(fields)])
        elif choice == 27:
            ops.append(["PEXPIREAT", hk(), str(rng.choice([future_ms, past_ms]))])
        elif choice == 28:
            ops.append(["HGETALL", hk()])
        elif choice == 29:
            ops.append(["HPEXPIRETIME", hk(), "FIELDS", "2",
                        rng.choice(fields), rng.choice(fields)])
        elif choice == 30:
            ops.append(["HPERSIST", hk(), "FIELDS", "1", rng.choice(fields)])
        elif choice == 31:
            ops.append(["HLEN", hk()])
        elif choice == 32:
            ops.append(["PERSIST", setk()])
        elif choice == 33:
            ops.append(["COPY", sk(), sk()] + rng.choice([[], ["REPLACE"]]))
        elif choice == 34:
            ops.append(["MEMORY", "USAGE", hk()])
        else:
            ops.append(["PERSIST", hk()])

    # Reset before each conditional arm so its result never depends on the arm before it.
    edge = "et:edge"
    for options, ttl_setup in (
            (["NX", "NX"], False), (["XX", "XX"], True),
            (["GT", "GT"], True), (["LT", "LT"], True),
            (["XX", "GT"], True), (["XX", "LT"], True), (["NX", "NX", "NX"], True)):
        ops.append(["SET", edge, "v"])
        if ttl_setup:
            ops.append(["PEXPIREAT", edge, str(future_ms)])
        ops.append(["PEXPIREAT", edge, str(future_ms + 1000)] + options)
        ops.append(["PEXPIRETIME", edge])
    for options in (["NX", "XX"], ["NX", "GT"], ["NX", "LT"], ["GT", "LT"],
                    ["NX", "NX", "XX"], ["GT", "GT", "LT"], ["XX", "GT", "LT"],
                    ["BOGUS"], ["NX", "BOGUS"], ["NX", "XX", "BOGUS"], [""], ["bOgUs"]):
        ops.append(["SET", edge, "v"])
        ops.append(["PEXPIREAT", edge, str(future_ms)] + options)
    # A non-numeric deadline is "not an integer"; an unrepresentable one is "invalid expire time".
    for command, deadline in (("EXPIRE", "abc"), ("PEXPIRE", "abc"), ("EXPIREAT", "abc"),
                              ("PEXPIREAT", "abc"), ("EXPIRE", "9223372036854775807"),
                              ("EXPIRE", "-9223372036854775808")):
        ops.append(["SET", edge, "v"])
        ops.append([command, edge, deadline])
        ops.append([command, edge, deadline, "NX"])
        ops.append(["PEXPIRETIME", edge])
    for options in (["EX", "abc"], ["EXAT", "abc"], ["PX", "abc"], ["PXAT", "abc"],
                    ["EX", "0"], ["EX", "-1"], ["EXAT", "0"]):
        ops.append(["SET", edge, "v"] + options)
        ops.append(["GETEX", edge] + options)
        ops.append(["SETEX", edge, options[-1], "v"])
        ops.append(["PSETEX", edge, options[-1], "v"])
    # GETEX shares SET's extended-argument grammar: one form may repeat, two may not.
    for options in (["EX", "600", "EX", "1200"], ["PERSIST", "PERSIST"], ["EX", "600", "PERSIST"],
                    ["PERSIST", "EX", "600"], ["EX", "600", "PX", "600000"], ["FOO"], ["EX"],
                    ["PXAT", str(future_ms), "PXAT", str(future_ms + 7000)]):
        ops.append(["SET", edge, "v"])
        ops.append(["GETEX", edge] + options)
        ops.append(["TTL", edge])
        ops.append(["PEXPIRETIME", edge])
    ops += [
        ["SET", edge, "v"], ["PEXPIRE", edge, "0"], ["EXISTS", edge],
        ["SET", edge, "v"], ["PEXPIRE", edge, "-9223372036854775808"], ["EXISTS", edge],
        ["SET", edge, "v"], ["PEXPIREAT", edge, "9223372036854775807"], ["PEXPIRETIME", edge],
        ["PERSIST", edge], ["PERSIST", edge],
        ["SET", edge, "old", "PXAT", str(future_ms)],
        ["SET", edge, "new", "KEEPTTL"], ["PEXPIRETIME", edge],
        ["GETEX", edge, "PXAT", "1"], ["GET", edge], ["MEMORY", "USAGE", edge],
        # Whole-key expiry dominates field deadlines; PERSIST leaves the field deadline intact.
        ["HSET", "et:hwhole", "a", "1", "b", "2"],
        ["HPEXPIREAT", "et:hwhole", str(future_ms), "FIELDS", "1", "a"],
        ["PEXPIREAT", "et:hwhole", str(future_ms + 5000)],
        ["PERSIST", "et:hwhole"],
        ["HPEXPIRETIME", "et:hwhole", "FIELDS", "2", "a", "b"],
        ["PTTL", "et:hwhole"],
        ["PEXPIREAT", "et:hwhole", "1"], ["EXISTS", "et:hwhole"], ["TYPE", "et:hwhole"],
        # Last surviving field goes: the hash goes with it.
        ["HSET", "et:hlast", "only", "v"],
        ["HPEXPIREAT", "et:hlast", "1", "FIELDS", "1", "only"],
        ["EXISTS", "et:hlast"], ["TYPE", "et:hlast"], ["HLEN", "et:hlast"],
        ["DEBUG", "SET-ACTIVE-EXPIRE", "1"],
    ]
    return ops


def gen_xshard(rng):
    # Long, unrelated names spread across the full router instead of clustering in a small prefix.
    keys = ["xs:%02d:%s" % (i, "".join(rng.choice("abcdef0123456789") for _ in range(38)))
            for i in range(48)]
    setkeys = keys[:16]
    listkeys = keys[16:28]
    strkeys = keys[28:]
    members = ["m%d" % i for i in range(18)] + ["long-member-" + "x" * 60]
    values = ["", "v", "hello", "42", "value-" + "y" * 90]
    ops = []

    def ks(n, pool=keys): return [rng.choice(pool) for _ in range(n)]
    def pairs(n):
        out = []
        for key in ks(n): out += [key, rng.choice(values)]
        return out

    # Establish all three value families, then continually collide and delete them below.
    for key in strkeys: ops.append(["SET", key, rng.choice(values)])
    for key in setkeys: ops.append(["SADD", key] + rng.sample(members, rng.randrange(1, 6)))
    for key in listkeys: ops.append(["RPUSH", key] + rng.sample(members, rng.randrange(1, 6)))

    for _ in range(4200):
        c = rng.randrange(28)
        if c == 0: ops.append(["MGET"] + ks(rng.randrange(2, 8)))
        elif c == 1: ops.append(["MSET"] + pairs(rng.randrange(2, 7)))
        elif c == 2: ops.append(["DEL"] + ks(rng.randrange(2, 7)))
        elif c == 3: ops.append(["UNLINK"] + ks(rng.randrange(2, 7)))
        elif c == 4: ops.append(["EXISTS"] + ks(rng.randrange(2, 8)))
        elif c == 5: ops.append(["TOUCH"] + ks(rng.randrange(2, 8)))
        elif c == 6: ops.append(["MSETNX"] + pairs(rng.randrange(2, 6)))
        elif c == 7: ops.append(["RENAME", rng.choice(keys), rng.choice(keys)])
        elif c == 8: ops.append(["RENAMENX", rng.choice(keys), rng.choice(keys)])
        elif c == 9:
            ops.append(["COPY", rng.choice(keys), rng.choice(keys)] +
                       (["REPLACE"] if rng.randrange(2) else []))
        elif c == 10: ops.append(["SMOVE", rng.choice(setkeys), rng.choice(setkeys), rng.choice(members)])
        elif c == 11:
            ops.append(["LMOVE", rng.choice(listkeys), rng.choice(listkeys),
                        rng.choice(["LEFT", "RIGHT"]), rng.choice(["LEFT", "RIGHT"])])
        elif c == 12: ops.append(["RPOPLPUSH", rng.choice(listkeys), rng.choice(listkeys)])
        elif c == 13: ops.append(["SINTER"] + ks(rng.randrange(2, 6), setkeys))
        elif c == 14: ops.append(["SUNION"] + ks(rng.randrange(2, 6), setkeys))
        elif c == 15: ops.append(["SDIFF"] + ks(rng.randrange(2, 6), setkeys))
        elif c == 16: ops.append(["SINTERSTORE", rng.choice(keys)] + ks(rng.randrange(2, 5), setkeys))
        elif c == 17: ops.append(["SUNIONSTORE", rng.choice(keys)] + ks(rng.randrange(2, 5), setkeys))
        elif c == 18: ops.append(["SDIFFSTORE", rng.choice(keys)] + ks(rng.randrange(2, 5), setkeys))
        elif c == 19:
            src = ks(rng.randrange(1, 5), setkeys)
            ops.append(["SINTERCARD", str(len(src))] + src +
                       (["LIMIT", str(rng.randrange(0, 5))] if rng.randrange(2) else []))
        elif c == 20: ops.append(["SET", rng.choice(keys), rng.choice(values)])
        elif c == 21: ops.append(["SADD", rng.choice(keys), rng.choice(members), rng.choice(members)])
        elif c == 22: ops.append(["RPUSH", rng.choice(keys), rng.choice(members)])
        elif c == 23: ops.append(["KEYS", rng.choice(["xs:*", "xs:0?:*", "*member*", "no-match-*"])])
        elif c == 24: ops.append(["GET", rng.choice(keys)])
        elif c == 25: ops.append(["SMEMBERS", rng.choice(setkeys)])
        elif c == 26: ops.append(["LRANGE", rng.choice(listkeys), "0", "-1"])
        else: ops.append(["TYPE", rng.choice(keys)])

    # Directed edge semantics. These names are also long enough to avoid accidentally sharing one
    # shard in small deployments; the implementation still handles same-owner coalescing.
    a, b, c, d = keys[0], keys[17], keys[31], keys[45]
    ops += [
        ["DEL", a, b, c, d],
        ["RENAME", a, a],                                      # missing self -> no such key
        ["SET", a, "self"], ["RENAME", a, a], ["GET", a],
        ["RENAMENX", a, a],
        ["SET", b, "old"], ["COPY", a, b], ["GET", b],
        ["COPY", a, b, "REPLACE"], ["GET", b],
        ["SET", c, "hit"], ["DEL", d],
        ["MSETNX", b, "would-change", c, "blocked", d, "would-create"],
        ["MGET", b, c, d],
        ["DEL", a, b, c, d], ["SADD", a, "x", "y", "z"], ["SADD", b, "y", "z", "q"],
        ["SET", c, "wrong-destination"],
        ["SINTERSTORE", c, a, b], ["SMEMBERS", c],
        ["SUNIONSTORE", c, a, b], ["SMEMBERS", c],
        ["SDIFFSTORE", c, a, b], ["SMEMBERS", c],
        ["KEYS", "xs:*"], ["KEYS", "xs:0?:*"], ["KEYS", "does-not-match-*"],
    ]
    return ops

def gen_storeorder(rng):
    """Cross-shard STORE destinations, READ BACK and DELETED on the same pipelined connection.

    Every other cross-shard suite deliberately keeps store destinations unread (a store's own reply
    is its evidence), which leaves one thing unmeasured: whether the destination the store just
    acknowledged is visible to the NEXT command the same connection sent.  That gap is what this
    suite closes.  Each cycle is `<mutate a source> ; <STORE dst ...> ; <read dst> ; DEL dst`, the
    destinations are drawn from a small reused pool so a late install shows up as a shifted
    timeline rather than as a one-off, and the whole family is covered -- including RENAME/RENAMENX
    and MSETNX, which are the members that do NOT take the two-hop connection barrier.

    Everything diffed here is byte-deterministic on both servers:
      * scores are integers, so nothing depends on float rendering,
      * set destinations are read with SCARD/SMISMEMBER rather than SMEMBERS, whose order is
        implementation-defined on both sides,
      * SORT is numeric over integer lists (never ALPHA, whose order is locale-collated on redis),
      * PFMERGE destinations are probed with EXISTS, not PFCOUNT.
    The suite runs with a deep pipeline chunk (see BATCH below): a short chunk makes the client
    wait for replies, and a destination install can only be late while it does not.
    """
    zs = ["so:z%d" % i for i in range(4)]
    ss = ["so:s%d" % i for i in range(4)]
    ls = ["so:l%d" % i for i in range(3)]
    bs = ["so:b%d" % i for i in range(3)]
    hs = ["so:h%d" % i for i in range(2)]
    geo = "so:g0"
    dests = ["so:d%d" % i for i in range(8)]
    members = ["m%d" % i for i in range(12)]

    ops = [["DEL"] + zs + ss + ls + bs + hs + dests + [geo]]
    for i, k in enumerate(zs):
        ops.append(["ZADD", k, "1", "a", "2", "b", "3", "c", str(10 + i), "d"])
    for i, k in enumerate(ss):
        ops.append(["SADD", k, "a", "b", "c", "e%d" % i])
    for k in ls:
        ops.append(["RPUSH", k, "3", "1", "2", "10"])
    for i, k in enumerate(bs):
        ops.append(["SET", k, "abcdefgh"[: 3 + i]])
    for k in hs:
        ops.append(["PFADD", k, "a", "b", "c"])
    ops.append(["GEOADD", geo, "13.361389", "38.115556", "palermo",
                "15.087269", "37.502669", "catania"])

    tick = [0]

    def bump():
        """Move a source so consecutive stores into one destination differ."""
        tick[0] += 1
        n = tick[0]
        which = rng.randrange(5)
        if which == 0: return [["ZADD", rng.choice(zs), str(n % 40), "n%d" % (n % 9)]]
        if which == 1: return [["SADD", rng.choice(ss), "n%d" % (n % 9)]]
        if which == 2: return [["RPUSH", rng.choice(ls), str(n % 40)],
                               ["LTRIM", rng.choice(ls), "0", "9"]]
        if which == 3: return [["SET", rng.choice(bs), "v" * (1 + n % 7)]]
        return [["PFADD", rng.choice(hs), "n%d" % (n % 40)]]

    def store_cycle(dst):
        c = rng.randrange(18)
        if c == 0:
            return [["ZRANGESTORE", dst, rng.choice(zs), "0", "-1"],
                    ["ZRANGE", dst, "0", "-1", "WITHSCORES"]]
        if c == 1:
            return [["ZRANGESTORE", dst, rng.choice(zs), "(1", "+inf", "BYSCORE"],
                    ["ZRANGE", dst, "0", "-1", "WITHSCORES"]]
        if c == 2:
            return [["ZUNIONSTORE", dst, "2", rng.choice(zs), rng.choice(zs),
                     "AGGREGATE", rng.choice(["SUM", "MIN", "MAX"])],
                    ["ZRANGE", dst, "0", "-1", "WITHSCORES"]]
        if c == 3:
            return [["ZINTERSTORE", dst, "2", rng.choice(zs), rng.choice(zs)],
                    ["ZRANGE", dst, "0", "-1", "WITHSCORES"]]
        if c == 4:
            return [["ZDIFFSTORE", dst, "2", rng.choice(zs), rng.choice(zs)],
                    ["ZRANGE", dst, "0", "-1", "WITHSCORES"]]
        if c == 5:
            return [["SUNIONSTORE", dst, rng.choice(ss), rng.choice(ss)],
                    ["SCARD", dst], ["SMISMEMBER", dst] + members[:4]]
        if c == 6:
            return [["SINTERSTORE", dst, rng.choice(ss), rng.choice(ss)],
                    ["SCARD", dst], ["SMISMEMBER", dst] + members[:4]]
        if c == 7:
            return [["SDIFFSTORE", dst, rng.choice(ss), rng.choice(ss)],
                    ["SCARD", dst], ["SMISMEMBER", dst] + members[:4]]
        if c == 8:
            return [["SORT", rng.choice(ls), "STORE", dst], ["LRANGE", dst, "0", "-1"]]
        if c == 9:
            return [["GEOSEARCHSTORE", dst, geo, "FROMLONLAT", "13", "38",
                     "BYRADIUS", rng.choice(["50", "500", "5000"]), "km", "ASC"],
                    ["ZCARD", dst], ["ZRANGE", dst, "0", "-1"]]
        if c == 10:
            return [["COPY", rng.choice(zs + bs), dst, "REPLACE"], ["TYPE", dst], ["EXISTS", dst]]
        if c == 11:
            return [["BITOP", rng.choice(["AND", "OR", "XOR"]), dst,
                     rng.choice(bs), rng.choice(bs)], ["GET", dst]]
        if c == 12:
            return [["PFMERGE", dst, rng.choice(hs), rng.choice(hs)], ["EXISTS", dst]]
        if c == 13:
            src = rng.choice(ss)
            return [["SMOVE", src, dst, rng.choice(members[:6] + ["a", "b", "c"])],
                    ["SCARD", dst], ["SCARD", src]]
        if c == 14:
            src = rng.choice(ls)
            return [["LMOVE", src, dst, rng.choice(["LEFT", "RIGHT"]),
                     rng.choice(["LEFT", "RIGHT"])], ["LRANGE", dst, "0", "-1"]]
        if c == 15:
            src = rng.choice(ls)
            return [["RPOPLPUSH", src, dst], ["LRANGE", dst, "0", "-1"]]
        if c == 16:
            # RENAME/RENAMENX are the members that drop the two-hop barrier at --atomic 1.
            src = rng.choice(bs)
            return [["SET", src, "r%d" % tick[0]],
                    [rng.choice(["RENAME", "RENAMENX"]), src, dst],
                    ["GET", dst], ["EXISTS", src]]
        if c == 17:
            return [["MSETNX", dst, "m%d" % tick[0], rng.choice(bs) + ":x", "y"],
                    ["GET", dst]]
        raise AssertionError(c)

    while len(ops) < 4600:
        dst = rng.choice(dests)
        ops += bump()
        ops += store_cycle(dst)
        if rng.randrange(3) == 0:
            ops.append(["EXISTS", dst])
        # The DEL is the second half of the shape: a destination install that arrives after it
        # survives into the NEXT cycle's read, which is exactly how the anomaly presents.
        ops.append(["DEL", dst])
        ops.append(["EXISTS", dst])
    return ops[:4600]

def gen_scan(rng):
    """SCAN family. The diffed stream is the OPTION SURFACE plus the mutations that shape the
    tables; the completeness property itself cannot be byte-compared (cursor values and emission
    order are implementation-defined) and is checked in the `scan` property block below, which
    walks each cursor to completion on both servers and compares the resulting SETS.

    `SCAN 0 TYPE <unknown>` is deliberately absent: TomoKV rejects an unknown type name and Redis
    accepts it and returns an empty batch. That divergence is real but it belongs to the compat
    surface, not to cursor completeness, and folding it in here would only hide this suite's
    verdict behind an unrelated failure.
    """
    ops = []
    for i in range(400):
        ops.append(["SET", "s:str:%03d" % i, "v%d" % i])
    for i in range(60):
        ops.append(["HSET", "s:hash", "f%03d" % i, "v"])
        ops.append(["SADD", "s:set", "m%03d" % i])
        ops.append(["ZADD", "s:zset", str(i), "m%03d" % i])
        ops.append(["LPUSH", "s:list", "e%03d" % i])
    # Enough members to force every collection past its compact encoding into a real table, which
    # is the only encoding a cursor exists in.
    for i in range(400):
        ops.append(["HSET", "s:hbig", "f%04d" % i, "v" * (i % 70)])
        ops.append(["SADD", "s:sbig", "m%04d" % i])
        ops.append(["ZADD", "s:zbig", str(i * 1.5), "m%04d" % i])
    # Random churn so the two servers see identical tables with identical tombstone histories.
    for _ in range(2200):
        c = rng.randrange(10)
        n = rng.randrange(400)
        if   c < 3: ops.append(["SET", "s:str:%03d" % n, "r%d" % rng.randrange(1000)])
        elif c == 3: ops.append(["DEL", "s:str:%03d" % n])
        elif c == 4: ops.append(["HSET", "s:hbig", "f%04d" % n, "r"])
        elif c == 5: ops.append(["HDEL", "s:hbig", "f%04d" % n])
        elif c == 6: ops.append(["SADD", "s:sbig", "m%04d" % n])
        elif c == 7: ops.append(["SREM", "s:sbig", "m%04d" % n])
        elif c == 8: ops.append(["ZADD", "s:zbig", str(n), "m%04d" % n])
        else: ops.append(["ZREM", "s:zbig", "m%04d" % n])
    # Option surface: every reply here IS byte-comparable.
    ops += [
        ["SCAN", "abc"], ["SCAN", "-1"], ["SCAN", "0", "COUNT", "0"], ["SCAN", "0", "COUNT", "-1"],
        ["SCAN", "0", "COUNT", "abc"], ["SCAN", "0", "BADOPT"], ["SCAN", "0", "MATCH"],
        ["SCAN", "0", "COUNT"], ["SCAN", "0", "TYPE"],
        ["HSCAN", "s:hbig", "abc"], ["HSCAN", "s:hbig", "0", "COUNT", "0"],
        ["HSCAN", "s:hbig", "0", "BADOPT"], ["HSCAN", "s:missing", "0"],
        ["HSCAN", "s:missing", "0", "NOVALUES"], ["HSCAN", "s:hbig", "0", "NOVALUES", "BADOPT"],
        ["SSCAN", "s:sbig", "abc"], ["SSCAN", "s:sbig", "0", "COUNT", "0"],
        ["SSCAN", "s:missing", "0"], ["SSCAN", "s:sbig", "0", "MATCH"],
        ["ZSCAN", "s:zbig", "abc"], ["ZSCAN", "s:zbig", "0", "COUNT", "0"],
        ["ZSCAN", "s:missing", "0"], ["ZSCAN", "s:zbig", "0", "BADOPT"],
        # WRONGTYPE is a reply shape, and it must not depend on which encoding the key holds.
        ["HSCAN", "s:sbig", "0"], ["SSCAN", "s:zbig", "0"], ["ZSCAN", "s:hbig", "0"],
        ["SSCAN", "s:list", "0"], ["HSCAN", "s:str:001", "0"],
        # A small collection answers in one call with cursor 0 on both servers.
        ["DEL", "s:tiny"], ["SADD", "s:tiny", "a", "b", "c"], ["SSCAN", "s:tiny", "0"],
        ["SSCAN", "s:tiny", "0", "COUNT", "1"], ["SSCAN", "s:tiny", "0", "MATCH", "a*"],
        ["DEL", "s:htiny"], ["HSET", "s:htiny", "a", "1", "b", "2"], ["HSCAN", "s:htiny", "0"],
        ["HSCAN", "s:htiny", "0", "NOVALUES"],
        ["DEL", "s:ztiny"], ["ZADD", "s:ztiny", "1", "a", "2", "b"], ["ZSCAN", "s:ztiny", "0"],
        ["ZSCAN", "s:ztiny", "0", "MATCH", "b"],
    ]
    return ops


def gen_multi(rng):
    # MULTI/EXEC against vanilla, over a key set spread across the whole router.  The bodies mix
    # MULTI-OWNER reads (MGET/EXISTS/TOUCH over many keys) with blind writes, because that is the
    # pair the in-EXEC read-cut lane (t-execiso) changed: a read inside EXEC now resolves at the
    # transaction's cut instead of at each fragment's own moment, and the transaction's own writes
    # must stay visible through the origin-conn overlay.  DISCARD, EXECABORT and empty-transaction
    # shapes are generated too.  Everything here has a deterministically ORDERED reply -- an EXEC
    # reply is compared byte-for-byte and normalize() cannot reach inside it to sort a set reply.
    #
    # RUNS AT BOTH --atomic 0 AND --atomic 1.  It used to be atomic-0 only, and it used to skip
    # three whole families, because of one defect in the MVCC resolver: a transaction's own
    # still-private candidate carries epoch 0 and the winner comparison ranked by epoch, so that
    # candidate lost to any older COMMITTED version of the same key.  Lane t-execfix fixed that
    # (NOTES-EXECFIX.md), so the three families it forced out are generated again and are what the
    # suite now covers most densely:
    #   lists       RPUSH/LRANGE/LLEN/LPOP -- the read-modify-write whose lost element was silent
    #   string RMW  INCRBY/APPEND -- cloned from a stale base
    #   a key touched TWICE inside one transaction, and read back after it
    # Cross-shard LCS inside MULTI is generated too: it answered an internal error before that lane.
    #
    # STILL EXCLUDED, and this one is unrelated to the above: a key DUPLICATED inside a single
    # command (DEL k k, MSET k a k b, MGET k k).  That is its own pre-existing divergence family in
    # this tree, present bare as well as in MULTI; tests/differ.py's `xshard` suite is where it
    # belongs.  ks() keeps using rng.sample to enforce it.
    listkeys = ["mx:l%02d:%s" % (i, "".join(rng.choice("abcdef0123456789") for _ in range(36)))
                for i in range(6)]
    strkeys = ["mx:%02d:%s" % (i, "".join(rng.choice("abcdef0123456789") for _ in range(38)))
               for i in range(18)]
    values = ["", "v", "hello", "42", "-7", "value-" + "y" * 90]
    ops = []

    def ks(n, pool=None):
        pool = pool or strkeys
        return rng.sample(pool, min(n, len(pool)))

    def pairs(n):
        out = []
        for key in ks(n): out += [key, rng.choice(values)]
        return out

    for key in strkeys: ops.append(["SET", key, rng.choice(values)])
    for key in listkeys: ops.append(["RPUSH", key, "seed"])

    def make():
        c = rng.randrange(21)
        if c == 0: return ["MGET"] + ks(rng.randrange(2, 8))
        if c == 1: return ["MSET"] + pairs(rng.randrange(2, 6))
        if c == 2: return ["EXISTS"] + ks(rng.randrange(2, 8))
        if c == 3: return ["TOUCH"] + ks(rng.randrange(2, 8))
        if c == 4: return ["DEL"] + ks(rng.randrange(2, 6))
        if c == 5: return ["SET", rng.choice(strkeys), rng.choice(values)]
        if c == 6: return ["GET", rng.choice(strkeys)]
        if c == 7: return ["GETDEL", rng.choice(strkeys)]
        if c == 8: return ["SETNX", rng.choice(strkeys), rng.choice(values)]
        if c == 9: return ["STRLEN", rng.choice(strkeys)]
        if c == 10: return ["MSETNX"] + pairs(rng.randrange(2, 5))
        if c == 11: return ["GETSET", rng.choice(strkeys), rng.choice(values)]
        if c == 12: return ["TYPE", rng.choice(strkeys)]
        if c == 13: return ["GETRANGE", rng.choice(strkeys), "0", str(rng.randrange(0, 5))]
        if c == 14: return ["INCRBY", rng.choice(strkeys), str(rng.randrange(-5, 6))]
        if c == 15: return ["APPEND", rng.choice(strkeys), rng.choice(values)]
        if c == 16: return ["RPUSH", rng.choice(listkeys), rng.choice(values)]
        if c == 17: return ["LRANGE", rng.choice(listkeys), "0", "-1"]
        if c == 18: return ["LLEN", rng.choice(listkeys)]
        if c == 19: return ["LPOP", rng.choice(listkeys)]
        two = ks(2)
        return ["LCS", two[0], two[1]] + rng.choice([[], ["LEN"]])

    def body(n):
        # A KEY MAY BE TOUCHED TWICE INSIDE ONE TRANSACTION.  That overlap is the shape the
        # resolver defect used to break, so it is now deliberately generated rather than avoided.
        return [make() for _ in range(n)]

    for _ in range(700):
        mode = rng.randrange(10)
        if mode == 0:
            # DISCARD must drop the queue and leave the keyspace untouched.
            ops.append(["MULTI"]); ops += body(rng.randrange(1, 5)); ops.append(["DISCARD"])
        elif mode == 1:
            # A queue-time rejection must turn EXEC into EXECABORT on both servers.
            ops.append(["MULTI"]); ops += body(rng.randrange(0, 3))
            ops.append(["GET"])                       # wrong arity: rejected while queueing
            ops += body(rng.randrange(0, 3)); ops.append(["EXEC"])
        elif mode == 2:
            ops.append(["MULTI"]); ops.append(["EXEC"])            # empty transaction
        else:
            ops.append(["MULTI"]); ops += body(rng.randrange(1, 7)); ops.append(["EXEC"])
        # Bare traffic between transactions: read-after-write ACROSS transactions, which is the
        # other half of the resolver fix (a plain read after an EXEC write on the same key).
        ops += body(1)
    return ops


def gen_xmove(rng):
    """Element-mover-heavy stream: cross-key, same-key, empty, and WRONGTYPE transitions."""
    listkeys = ["xm:list:%02d:%s" % (i, "l" * (31 + i % 5)) for i in range(14)]
    setkeys = ["xm:set:%02d:%s" % (i, "s" * (33 + i % 7)) for i in range(14)]
    wrongkeys = ["xm:wrong:%02d:%s" % (i, "w" * 37) for i in range(6)]
    values = ["", "a", "bb", "value-7", "nul\x00value", "x" * 90]
    members = ["m%d" % i for i in range(24)] + ["", "nul\x00member", "y" * 85]
    ops = []

    for key in listkeys:
        ops.append(["RPUSH", key] + [rng.choice(values) for _ in range(rng.randrange(1, 7))])
    for key in setkeys:
        ops.append(["SADD", key] + rng.sample(members, rng.randrange(1, 7)))
    for key in wrongkeys:
        ops.append(["SET", key, rng.choice(values)])

    for _ in range(4200):
        choice = rng.randrange(24)
        if choice < 6:
            source = rng.choice(listkeys)
            destination = source if rng.randrange(8) == 0 else rng.choice(listkeys)
            ops.append(["LMOVE", source, destination, rng.choice(["LEFT", "RIGHT"]),
                        rng.choice(["LEFT", "RIGHT"])])
        elif choice < 9:
            source = rng.choice(listkeys)
            destination = source if rng.randrange(8) == 0 else rng.choice(listkeys)
            ops.append(["RPOPLPUSH", source, destination])
        elif choice == 9:
            ops.append([rng.choice(["LPUSH", "RPUSH"]), rng.choice(listkeys),
                        rng.choice(values)])
        elif choice == 10:
            ops.append([rng.choice(["LPOP", "RPOP"]), rng.choice(listkeys)])
        elif choice == 11:
            ops.append(["LRANGE", rng.choice(listkeys), "0", "-1"])
        elif choice < 17:
            source = rng.choice(setkeys)
            destination = source if rng.randrange(8) == 0 else rng.choice(setkeys)
            ops.append(["SMOVE", source, destination, rng.choice(members)])
        elif choice == 17:
            ops.append(["SADD", rng.choice(setkeys), rng.choice(members)])
        elif choice == 18:
            ops.append(["SREM", rng.choice(setkeys), rng.choice(members)])
        elif choice == 19:
            ops.append(["SMEMBERS", rng.choice(setkeys)])
        elif choice == 20:
            ops.append(["DEL", rng.choice(listkeys + setkeys)])
        elif choice == 21:
            ops.append(["LMOVE", rng.choice(listkeys), rng.choice(wrongkeys),
                        rng.choice(["LEFT", "RIGHT"]), rng.choice(["LEFT", "RIGHT"])])
        elif choice == 22:
            ops.append(["SMOVE", rng.choice(setkeys), rng.choice(wrongkeys),
                        rng.choice(members)])
        else:
            ops.append([rng.choice(["LLEN", "SCARD", "TYPE"]),
                        rng.choice(listkeys + setkeys)])

    same_list, same_set = listkeys[0], setkeys[0]
    source_list, destination_list = listkeys[1], listkeys[-1]
    source_set, destination_set = setkeys[1], setkeys[-1]
    ops += [
        ["DEL", same_list], ["RPUSH", same_list, "a", "b", "c"],
        ["LMOVE", same_list, same_list, "LEFT", "RIGHT"],
        ["RPOPLPUSH", same_list, same_list], ["LRANGE", same_list, "0", "-1"],
        ["DEL", same_set], ["SADD", same_set, "member"],
        ["SMOVE", same_set, same_set, "member"],
        ["SMOVE", same_set, same_set, "missing"], ["SMEMBERS", same_set],
        ["DEL", source_list, destination_list],
        ["LMOVE", source_list, destination_list, "LEFT", "RIGHT"],
        ["RPUSH", source_list, "only"],
        ["RPOPLPUSH", source_list, destination_list],
        ["EXISTS", source_list], ["LRANGE", destination_list, "0", "-1"],
        ["DEL", source_set, destination_set],
        ["SMOVE", source_set, destination_set, "member"],
        ["SADD", source_set, "member"], ["SMOVE", source_set, destination_set, "member"],
        ["EXISTS", source_set], ["SMEMBERS", destination_set],
        ["SET", wrongkeys[0], "wrong"],
        ["RPUSH", source_list, "kept"],
        ["LMOVE", source_list, wrongkeys[0], "LEFT", "RIGHT"],
        ["LRANGE", source_list, "0", "-1"],
        ["SADD", source_set, "kept"],
        ["SMOVE", source_set, wrongkeys[0], "kept"], ["SMEMBERS", source_set],
    ]
    return ops


def gen_edgeenc(rng):
    """ENCODING and SIZE boundaries (lane t-edgeenc).

    Value lengths are drawn from a boundary-heavy LADDER rather than uniformly, because the
    interesting lengths in this tree are not spread out -- they are the compact/expanded promotion
    limits (aligned to the oracle's listpack limits by the suite preamble), the 192-byte KvObj
    embed line, the varint width step at 128, and the allocator size classes the embedded capacity
    is cut from. KEY LENGTH is a second ladder: a small collection lives in its key's own
    allocation and its in-place capacity is good_size() slack there, so two keys of different
    lengths holding identical contents leave the tail form at different points.

    What is deliberately NOT generated, each with the reason:
      * OBJECT ENCODING on STRINGS and LISTS. The embstr/raw line is 192 here and 44 in redis, an
        APPENDed short string stays embstr here, and the list knob is an aggregate byte budget
        here against an entry count there -- three documented deviations (NOTES-SERVERTAIL.md 5,
        src/store/typeval.h). OBJECT ENCODING on hash/set/zset IS generated: those align exactly.
      * SORT ... ALPHA. Redis sorts it with strcoll() under the collation locale, so an oracle
        started under en_US.UTF-8 orders "-5" after "42" while this tree uses byte order. That is
        an oracle-environment difference, not a target defect: LC_ALL=C on the oracle makes the
        two agree. Nothing here should depend on which locale the oracle happened to inherit.
      * XINFO STREAM. radix-tree-keys/radix-tree-nodes are counters of a structure this tree does
        not have and answers with a fixed stub.
      * A key touched twice inside one MULTI, and any list op inside MULTI. Both re-open the
        pre-existing epoch-0 MVCC resolver defect written up in NOTES-EXECISO.md section 10 (a)
        and (c); NOTES-EDGEENC.md carries the size-independent reproducer for it. Generating them
        here would make this suite permanently red for a defect it does not exist to cover.
    """
    # 192 = kEmbedThreshold; 64 = the aligned per-value compact limit; 128 = the varint step.
    LADDER = [0, 1, 2, 7, 8, 15, 16, 31, 32, 39, 40, 47, 48, 55, 56, 63, 64, 65, 79, 80, 95, 96,
              97, 111, 112, 126, 127, 128, 129, 159, 160, 175, 176, 190, 191, 192, 193, 194, 200,
              255, 256, 257, 300]
    # key names of many lengths: the tail capacity is slack in the key's own block
    KLENS = [1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144]

    def size():
        return rng.choice(LADDER)

    def val(tag=None):
        n = size()
        ch = tag or rng.choice("abcdefghij")
        return (ch * n)[:n]

    def keyset(prefix, count):
        out = []
        for i in range(count):
            klen = KLENS[i % len(KLENS)]
            body = ("%s%d" % (prefix, i)) + "K" * klen
            out.append(body[:max(len(prefix) + 1, klen)])
        return out

    skeys = keyset("s", 10)
    hkeys = keyset("h", 8)
    setkeys = keyset("t", 8)
    zkeys = keyset("z", 8)
    lkeys = keyset("l", 8)
    ops = []

    def S(): return rng.choice(skeys)
    def H(): return rng.choice(hkeys)
    def T(): return rng.choice(setkeys)
    def Z(): return rng.choice(zkeys)
    def L(): return rng.choice(lkeys)

    for _ in range(3200):
        c = rng.randrange(40)
        if c == 0: ops.append(["SET", S(), val()])
        elif c == 1: ops.append(["GET", S()])
        elif c == 2: ops.append(["APPEND", S(), val()])
        elif c == 3: ops.append(["SETRANGE", S(), str(rng.choice([0, 1, 63, 64, 96, 191, 192, 193])), val()])
        elif c == 4: ops.append(["GETRANGE", S(), str(rng.randrange(-8, 200)), str(rng.randrange(-8, 260))])
        elif c == 5: ops.append(["STRLEN", S()])
        elif c == 6: ops.append(["GETDEL", S()])
        elif c == 7: ops.append(["GETSET", S(), val()])
        elif c == 8: ops.append(["COPY", S(), S(), "REPLACE"])
        elif c == 9: ops.append(["HSET", H(), val("f") or "e", val("v")])
        elif c == 10: ops.append(["HGET", H(), val("f") or "e"])
        elif c == 11: ops.append(["HDEL", H(), val("f") or "e"])
        elif c == 12: ops.append(["HGETALL", H()])
        elif c == 13: ops.append(["HLEN", H()])
        elif c == 14: ops.append(["HSTRLEN", H(), val("f") or "e"])
        elif c == 15: ops.append(["OBJECT", "ENCODING", H()])
        elif c == 16: ops.append(["SADD", T(), val("m")])
        elif c == 17: ops.append(["SREM", T(), val("m")])
        elif c == 18: ops.append(["SMEMBERS", T()])
        elif c == 19: ops.append(["SCARD", T()])
        elif c == 20: ops.append(["SISMEMBER", T(), val("m")])
        elif c == 21: ops.append(["OBJECT", "ENCODING", T()])
        elif c == 22: ops.append(["SADD", T(), str(rng.choice([0, 1, -1, 32767, 32768, -32768,
                                                               2147483647, 2147483648,
                                                               9223372036854775807,
                                                               -9223372036854775808]))])
        elif c == 23: ops.append(["ZADD", Z(), str(rng.randrange(-5, 6)), val("m")])
        elif c == 24: ops.append(["ZREM", Z(), val("m")])
        elif c == 25: ops.append(["ZRANGE", Z(), "0", "-1", "WITHSCORES"])
        elif c == 26: ops.append(["ZCARD", Z()])
        elif c == 27: ops.append(["ZSCORE", Z(), val("m")])
        elif c == 28: ops.append(["ZREMRANGEBYRANK", Z(), str(rng.randrange(-3, 3)), str(rng.randrange(-3, 3))])
        elif c == 29: ops.append(["OBJECT", "ENCODING", Z()])
        elif c == 30: ops.append(["RPUSH", L(), val("e")])
        elif c == 31: ops.append(["LPUSH", L(), val("e")])
        elif c == 32: ops.append(["LRANGE", L(), "0", "-1"])
        elif c == 33: ops.append(["LSET", L(), str(rng.randrange(-3, 3)), val("e")])
        elif c == 34: ops.append(["LINSERT", L(), rng.choice(["BEFORE", "AFTER"]), val("e"), val("e")])
        elif c == 35: ops.append(["LREM", L(), str(rng.randrange(-2, 3)), val("e")])
        elif c == 36: ops.append(["LPOP", L()])
        elif c == 37: ops.append(["LLEN", L()])
        elif c == 38: ops.append(["DEL", rng.choice(skeys + hkeys + setkeys + zkeys + lkeys)])
        else: ops.append(["MGET"] + rng.sample(skeys, 4))

    # ---- directed tail: the exact thresholds, in both directions -----------------------------
    E = 192
    for n in (E - 1, E, E + 1):
        k = "edge:str:%d" % n
        body = ("q" * n)[:n]
        ops += [["SET", k, body], ["GET", k], ["STRLEN", k],
                ["APPEND", k, "Z"], ["GET", k], ["STRLEN", k],
                ["SETRANGE", k, str(n), "TT"], ["GET", k], ["STRLEN", k],
                ["SET", k, body, "EX", "1000"], ["APPEND", k, "Y"], ["GET", k],
                ["PERSIST", k], ["GET", k],
                ["COPY", k, k + "c"], ["GET", k + "c"],
                ["RENAME", k + "c", k + "r"], ["GET", k + "r"],
                ["DEL", k, k + "r"]]

    # promotion and the one-way rule, at the aligned limits (128 entries / 64 bytes)
    for limit_kind, entries in (("entries", 129), ("value", 3)):
        for kind in ("hash", "set", "zset"):
            k = "edge:%s:%s" % (kind, limit_kind)
            ops.append(["DEL", k])
            for i in range(entries):
                payload = ("p" * (65 if limit_kind == "value" and i == entries - 1 else 4))
                if kind == "hash":
                    ops.append(["HSET", k, "f%04d" % i, payload])
                elif kind == "set":
                    ops.append(["SADD", k, "m%04d%s" % (i, payload)])
                else:
                    ops.append(["ZADD", k, str(i), "m%04d%s" % (i, payload)])
                if i in (0, entries - 2, entries - 1):
                    ops.append(["OBJECT", "ENCODING", k])
            # shrink back below the threshold: promotion is one-way on BOTH servers
            for i in range(entries - 1):
                payload = "p" * 4
                if kind == "hash":
                    ops.append(["HDEL", k, "f%04d" % i])
                elif kind == "set":
                    ops.append(["SREM", k, "m%04d%s" % (i, payload)])
                else:
                    ops.append(["ZREM", k, "m%04d%s" % (i, payload)])
            ops += [["OBJECT", "ENCODING", k], ["DEL", k]]

    # the tail-embed line swept over key length, all four types, contents read back every step
    for klen in KLENS:
        k = ("E" * klen)[:klen]
        ops.append(["DEL", k])
        for i in range(10):
            ops += [["RPUSH", k, ("%02d" % i) + "e" * 30], ["LRANGE", k, "0", "-1"]]
        ops += [["LSET", k, "0", "L" + "l" * 120], ["LRANGE", k, "0", "-1"],
                ["LINSERT", k, "AFTER", "L" + "l" * 120, "MID"], ["LRANGE", k, "0", "-1"],
                ["LREM", k, "0", "MID"], ["LRANGE", k, "0", "-1"], ["DEL", k]]
        for i in range(8):
            ops += [["HSET", k, "f%d" % i, "h" * 30], ["HGETALL", k]]
        ops += [["HSET", k, "f0", "H" * 150], ["HGETALL", k], ["OBJECT", "ENCODING", k],
                ["DEL", k]]
        for i in range(8):
            ops += [["ZADD", k, str(i), "m%d%s" % (i, "z" * 28)], ["ZRANGE", k, "0", "-1"]]
        ops += [["ZADD", k, "99", "m0" + "z" * 28], ["ZRANGE", k, "0", "-1", "WITHSCORES"],
                ["OBJECT", "ENCODING", k], ["DEL", k]]
        for i in range(8):
            ops += [["SADD", k, "m%d%s" % (i, "s" * 28)], ["SMEMBERS", k]]
        ops += [["SADD", k, "S" * 200], ["SMEMBERS", k], ["OBJECT", "ENCODING", k], ["DEL", k]]

    # the reply-path cutovers: zero-copy borrow (16384) and the multi-key gather slot (1024)
    for n in (1020, 1023, 1024, 1025, 1030, 16380, 16383, 16384, 16385, 16390):
        k = "edge:zc:%d" % n
        body = ("Q" * n)[:n]
        ops += [["SET", k, body], ["GET", k], ["STRLEN", k],
                ["GETRANGE", k, "0", "7"], ["GETRANGE", k, str(n - 8), "-1"],
                ["PING"], ["ECHO", "after-big"], ["GET", k],
                ["APPEND", k, "ZZ"], ["STRLEN", k], ["GETRANGE", k, str(n - 2), "-1"],
                ["DEL", k]]
    gathered = ["edge:g%d" % i for i in range(6)]
    for n in (1023, 1024, 1025, 2048):
        pairs = []
        for i, k in enumerate(gathered):
            pairs += [k, (chr(97 + i) * n)[:n]]
        ops += [["MSET"] + pairs, ["MGET"] + gathered,
                ["MGET", gathered[0], "edge:absent", gathered[1]],
                ["EXISTS"] + gathered, ["DEL"] + gathered]

    # integer encoding and the INT64 edges, including the collection-member spellings
    INTS = ["0", "1", "-1", "+1", "-0", "00", "007", "1.0", "1e3", "2147483647", "2147483648",
            "-2147483648", "9223372036854775807", "-9223372036854775808",
            "9223372036854775808", "-9223372036854775809", "18446744073709551615", "0x10", ""]
    for i, text in enumerate(INTS):
        k = "edge:int:%d" % i
        ops += [["SET", k, text], ["GET", k], ["STRLEN", k],
                ["INCR", k], ["GET", k], ["DECRBY", k, "3"], ["GET", k],
                ["SET", k, text], ["APPEND", k, "9"], ["GET", k],
                ["SET", k, text], ["SETRANGE", k, "0", "5"], ["GET", k],
                ["SET", k, text], ["GETRANGE", k, "0", "0"], ["GETRANGE", k, "-1", "-1"],
                ["DEL", k],
                ["SADD", "edge:iset", text], ["SCARD", "edge:iset"],
                ["OBJECT", "ENCODING", "edge:iset"], ["SISMEMBER", "edge:iset", text]]
    ops += [["SMEMBERS", "edge:iset"], ["DEL", "edge:iset"]]

    # value-copying commands at each boundary size
    for n in (1, 63, 64, 65, 95, 96, 97, 191, 192, 193, 1024, 16384):
        s, d = "edge:cp:%d" % n, "edge:cp:%d:d" % n
        body = ("c" * n)[:n]
        ops += [["DEL", s, d], ["SET", s, body], ["COPY", s, d], ["GET", d],
                ["COPY", s, d], ["COPY", s, d, "REPLACE"], ["GET", d],
                ["RENAME", s, d], ["GET", d], ["EXISTS", s], ["DEL", d]]
        la, lb = "edge:lm:%d:a" % n, "edge:lm:%d:b" % n
        ops += [["DEL", la, lb], ["RPUSH", la, body, body + "2"],
                ["LMOVE", la, lb, "LEFT", "RIGHT"], ["LRANGE", la, "0", "-1"],
                ["LRANGE", lb, "0", "-1"], ["RPOPLPUSH", la, lb], ["LRANGE", lb, "0", "-1"],
                ["DEL", la, lb]]
        sa, sb = "edge:sm:%d:a" % n, "edge:sm:%d:b" % n
        ops += [["DEL", sa, sb], ["SADD", sa, body, body + "2"], ["SMOVE", sa, sb, body],
                ["SMEMBERS", sa], ["SMEMBERS", sb],
                ["SUNIONSTORE", sa + "u", sa, sb], ["SMEMBERS", sa + "u"],
                ["DEL", sa, sb, sa + "u"]]

    # cross-shard writes at boundary sizes (bare, not inside MULTI -- see the docstring)
    xkeys = ["edge:x%d" % i for i in range(5)]
    for n in (1, 64, 96, 192, 193, 1024, 16384):
        pairs = []
        for i, k in enumerate(xkeys):
            pairs += [k, (chr(97 + i) * n)[:n]]
        ops += [["DEL"] + xkeys, ["MSET"] + pairs, ["MGET"] + xkeys,
                ["MSETNX"] + pairs, ["MGET"] + xkeys, ["EXISTS"] + xkeys,
                ["UNLINK"] + xkeys[:2], ["MGET"] + xkeys,
                ["MSETNX"] + pairs, ["MGET"] + xkeys, ["DEL"] + xkeys]
    return ops


def gen_servertail(rng):
    """LCS-heavy, plus the introspection replies that are genuinely byte-comparable.

    Threshold alignment is done in the suite preamble below, NOT here: the two servers spell their
    encoding knobs differently, so the same intent needs two different CONFIG SET commands and they
    cannot travel in the diffed op stream.

    Deliberately EXCLUDED, with reasons:
      - OBJECT ENCODING on strings of 45..192 bytes. Our embstr/raw boundary is kEmbedThreshold
        (192); redis's is 44. The names mean the same thing at different sizes.
      - OBJECT REFCOUNT. Redis reports INT_MAX for its shared small integers; we have no shared
        object table and always report 1.
      - MEMORY USAGE values, and COMMAND INFO flag/acl/key-spec arrays. Normalized in normalize().
    """
    alphabet = "abcdef"
    ops = []

    def rand_string(lo, hi):
        return "".join(rng.choice(alphabet) for _ in range(rng.randrange(lo, hi)))

    # Long, unrelated key names so the pair lands on different shards most of the time; the
    # repeated-key form exercises the same-shard local fast path.
    pairs = []
    for i in range(24):
        pairs.append(("st:a:%02d:%s" % (i, "q" * 34), "st:b:%02d:%s" % (i, "w" * 34)))
    for key_a, key_b in pairs:
        ops.append(["SET", key_a, rand_string(0, 40)])
        ops.append(["SET", key_b, rand_string(0, 40)])

    for _ in range(3600):
        c = rng.randrange(10)
        key_a, key_b = rng.choice(pairs)
        if c == 0:
            ops.append(["SET", key_a, rand_string(0, 45)])
        elif c == 1:
            ops.append(["SET", key_b, rand_string(0, 45)])
        elif c == 2:
            ops.append(["LCS", key_a, key_b])
        elif c == 3:
            ops.append(["LCS", key_a, key_b, "LEN"])
        elif c == 4:
            ops.append(["LCS", key_a, key_b, "IDX"])
        elif c == 5:
            ops.append(["LCS", key_a, key_b, "IDX", "WITHMATCHLEN"])
        elif c == 6:
            ops.append(["LCS", key_a, key_b, "IDX", "MINMATCHLEN",
                        str(rng.randrange(-2, 6)), "WITHMATCHLEN"])
        elif c == 7:
            ops.append(["LCS", key_a, key_a])           # same-shard local path
        elif c == 8:
            ops.append(rng.choice([["LCS", key_a, key_b, "LEN", "IDX"],
                                   ["LCS", key_a, key_b, "BOGUS"],
                                   ["LCS", key_a, key_b, "MINMATCHLEN"],
                                   ["LCS", key_a, key_b, "MINMATCHLEN", "abc"],
                                   ["LCS", key_a, "st:absent:%d" % rng.randrange(9)]]))
        else:
            ops.append(["SUBSTR", key_a, str(rng.randrange(-8, 8)), str(rng.randrange(-8, 8))])

    # OBJECT ENCODING across type mutations, restricted to shapes both servers agree on.
    ops.append(["SET", "st:enc:int", "42"])
    ops.append(["SET", "st:enc:short", "a" * 20])
    ops.append(["SET", "st:enc:long", "a" * 300])
    for name in ("st:enc:int", "st:enc:short", "st:enc:long"):
        ops.append(["OBJECT", "ENCODING", name])
    # APPEND leaves redis in `raw` regardless of the resulting length, because redis over-allocates
    # an appended string for future growth. We have no such growth reservation, so a 3-byte result
    # stays embstr. The TYPE is still comparable; the encoding is not, for the same embstr/raw
    # reason listed in the docstring.
    ops.append(["APPEND", "st:enc:int", "x"])
    ops.append(["TYPE", "st:enc:int"])
    ops.append(["OBJECT", "ENCODING", "st:enc:absent"])
    ops.append(["OBJECT", "HELP"])

    for key, add, small, big in (("st:enc:hash", "HSET", 3, 700),
                                 ("st:enc:set", "SADD", 3, 300),
                                 ("st:enc:zset", "ZADD", 3, 300),
                                 ("st:enc:list", "RPUSH", 3, 300)):
        ops.append(["DEL", key])
        for i in range(small):
            if add == "HSET": ops.append([add, key, "f%d" % i, "v%d" % i])
            elif add == "ZADD": ops.append([add, key, str(i), "m%d" % i])
            else: ops.append([add, key, "m%d" % i])
        ops.append(["OBJECT", "ENCODING", key])
        for i in range(small, big):
            if add == "HSET": ops.append([add, key, "f%d" % i, "v%d" % i])
            elif add == "ZADD": ops.append([add, key, str(i), "m%d" % i])
            else: ops.append([add, key, "m%d" % i])
        ops.append(["OBJECT", "ENCODING", key])
        ops.append(["TYPE", key])

    ops.append(["DEL", "st:enc:intset"])
    for i in range(20):
        ops.append(["SADD", "st:enc:intset", str(i)])
    ops.append(["OBJECT", "ENCODING", "st:enc:intset"])
    ops.append(["SADD", "st:enc:intset", "notanint"])
    ops.append(["OBJECT", "ENCODING", "st:enc:intset"])

    ops.append(["XADD", "st:enc:stream", "1-1", "f", "v"])
    ops.append(["OBJECT", "ENCODING", "st:enc:stream"])

    # Presence, not value (normalized): MEMORY USAGE must agree on hit vs miss.
    for name in ("st:enc:short", "st:enc:absent", "st:enc:hash", "st:enc:stream"):
        ops.append(["MEMORY", "USAGE", name])

    # COMMAND INFO shapes for a fixed list (normalized to name/arity/key-range).
    # `object` and `memory` are deliberately absent: redis reports a 0/0/0 key range on the
    # container and hangs the real key spec off its subcommands, while our registry row carries the
    # truthful argv[2] range because ACL, MULTI and the router all consume it. Ours is the more
    # informative answer and it is not going to be made wrong to match a byte.
    for name in ("get", "set", "mset", "del", "lcs", "substr", "ping", "subscribe",
                 "sort_ro", "getrange", "zadd", "hset", "sinterstore", "xadd"):
        ops.append(["COMMAND", "INFO", name])
    ops.append(["COMMAND", "GETKEYS", "MSET", "a", "1", "b", "2"])
    ops.append(["COMMAND", "GETKEYS", "GET", "k"])
    ops.append(["COMMAND", "GETKEYS", "LCS", "k1", "k2"])
    ops.append(["COMMAND", "GETKEYS", "PING"])
    ops.append(["COMMAND", "GETKEYS", "NOSUCHCOMMAND", "k"])

    # Scope A parity replies that are byte-identical by construction.
    ops.append(["ROLE"])
    ops.append(["WAIT", "0", "0"])
    ops.append(["WAIT", "0", "-1"])
    ops.append(["FAILOVER", "ABORT"])
    ops.append(["PFSELFTEST"])
    ops.append(["DEL", "st:sort"])
    for value in ("3", "1", "2", "10"):
        ops.append(["RPUSH", "st:sort", value])
    ops.append(["SORT_RO", "st:sort"])
    ops.append(["SORT_RO", "st:sort", "DESC"])
    ops.append(["SORT_RO", "st:sort", "LIMIT", "1", "2"])
    ops.append(["SORT_RO", "st:sort", "ALPHA"])
    ops.append(["SORT_RO", "st:sort", "STORE", "st:sortdst"])
    ops.append(["SORT_RO", "st:absent:sort"])
    return ops


def gen_arity(rng):
    """Container-subcommand arity/name rows observed byte-different before lane t-arity.

    Successful control forms live in tests/arity.py. This differ generator intentionally contains
    only reproduced differences, then varies subcommand case and harmless filler arguments over a
    4200-op stream so every reply remains byte-comparable.
    """
    directed = [
        ["ACL", "CAT", "keyspace", "x"], ["ACL", "DELUSER"],
        ["ACL", "GENPASS", "8", "x"], ["ACL", "GETUSER"],
        ["ACL", "GETUSER", "default", "x"], ["ACL", "LIST", "x"],
        ["ACL", "LOAD", "x"], ["ACL", "SAVE", "x"],
        ["ACL", "LOG", "1", "x"], ["ACL", "SETUSER"],
        ["ACL", "USERS", "x"], ["ACL", "WHOAMI", "x"],
        ["ACL", "HELP", "x"], ["ACL", "BOGUS"],
        ["CONFIG", "GET"], ["CONFIG", "SET", "maxmemory"],
        ["CONFIG", "REWRITE", "x"], ["CONFIG", "RESETSTAT", "x"],
        ["CONFIG", "HELP", "x"], ["CONFIG", "BOGUS"],
        ["OBJECT", "ENCODING", "arity:key", "x"],
        ["OBJECT", "REFCOUNT", "arity:key", "x"],
        ["OBJECT", "IDLETIME", "arity:key", "x"],
        ["OBJECT", "FREQ", "arity:key", "x"], ["OBJECT", "HELP", "x"],
        ["MEMORY", "USAGE", "arity:key", "SAMPLES", "1", "x"],
        ["MEMORY", "STATS", "x"], ["MEMORY", "DOCTOR", "x"],
        ["MEMORY", "PURGE", "x"], ["MEMORY", "MALLOC-STATS", "x"],
        ["MEMORY", "HELP", "x"],
        ["LATENCY", "HISTORY"], ["LATENCY", "HISTORY", "command", "x"],
        ["LATENCY", "GRAPH"], ["LATENCY", "GRAPH", "command", "x"],
        ["LATENCY", "DOCTOR", "x"], ["LATENCY", "LATEST", "x"],
        ["LATENCY", "HELP", "x"],
        ["SLOWLOG", "GET", "1", "x"], ["SLOWLOG", "LEN", "x"],
        ["SLOWLOG", "RESET", "x"], ["SLOWLOG", "HELP", "x"],
    ]

    def mixed_case(text):
        return "".join(ch.upper() if rng.randrange(2) else ch.lower() for ch in text)

    ops = [list(argv) for argv in directed]
    while len(ops) < 4200:
        argv = list(rng.choice(directed))
        argv[1] = mixed_case(argv[1])
        ops.append(argv)
    return ops


def gen_bitmap(rng):
    # Long, unrelated key names make random BITOPs exercise both localfast and cross-shard paths.
    keys = ["bm:%02d:%s" % (i, "".join(rng.choice("abcdef0123456789") for _ in range(30)))
            for i in range(24)]
    ops = []
    values = [b"", b"\x00", b"\xff", b"\x80\x01", b"hello", b"12345",
              bytes(range(16)), b"\x00\xff\x55\xaa" * 8]
    def K(): return rng.choice(keys)
    def R(): return str(rng.randrange(-80, 96))

    for _ in range(4200):
        c = rng.randrange(16)
        if c in (0, 1, 2):
            ops.append(["SETBIT", K(), str(rng.randrange(0, 1024)), str(rng.randrange(2))])
        elif c in (3, 4):
            ops.append(["GETBIT", K(), str(rng.randrange(0, 1536))])
        elif c == 5:
            ops.append(["BITCOUNT", K()])
        elif c == 6:
            ops.append(["BITCOUNT", K(), R(), R()])
        elif c == 7:
            ops.append(["BITCOUNT", K(), R(), R(), rng.choice(["BYTE", "BIT"])])
        elif c == 8:
            ops.append(["BITPOS", K(), str(rng.randrange(2))])
        elif c == 9:
            ops.append(["BITPOS", K(), str(rng.randrange(2)), R()])
        elif c == 10:
            ops.append(["BITPOS", K(), str(rng.randrange(2)), R(), R()])
        elif c == 11:
            ops.append(["BITPOS", K(), str(rng.randrange(2)), R(), R(),
                        rng.choice(["BYTE", "BIT"])])
        elif c == 12:
            operation = rng.choice(["AND", "OR", "XOR"])
            ops.append(["BITOP", operation, K()] + [K() for _ in range(rng.randrange(1, 5))])
        elif c == 13:
            ops.append(["BITOP", "NOT", K(), K()])
        elif c == 14:
            ops.append([rng.choice(["SET", "APPEND"]), K(), rng.choice(values)])
        else:
            ops.append([rng.choice(["GET", "STRLEN", "DEL"]), K()])

    a, b, c, d, e = keys[:5]
    ops += [
        # MSB-first indexing, zero-filled growth, reads beyond the physical string.
        ["DEL", a], ["SETBIT", a, "0", "1"], ["SETBIT", a, "7", "1"],
        ["SETBIT", a, "15", "1"], ["SETBIT", a, "79", "1"],
        ["GETBIT", a, "0"], ["GETBIT", a, "1"], ["GETBIT", a, "79"],
        ["GETBIT", a, "80"], ["STRLEN", a], ["GET", a],

        # Negative clamping and BYTE versus BIT ranges, including partial edge bytes.
        ["SET", b, b"\xf0\x0f\x81"],
        ["BITCOUNT", b], ["BITCOUNT", b, "-2", "-1"],
        ["BITCOUNT", b, "-100", "100", "BYTE"],
        ["BITCOUNT", b, "3", "18", "BIT"],
        ["BITCOUNT", b, "-17", "-2", "BIT"],
        ["BITCOUNT", b, "-1", "-2", "BIT"],
        ["BITPOS", b, "1"], ["BITPOS", b, "0", "1"],
        ["BITPOS", b, "1", "4", "18", "BIT"],
        ["BITPOS", b, "0", "4", "18", "BIT"],
        ["BITPOS", b, "0", "0", "0", "BYTE"],
        ["BITPOS", b, "0", "99"],

        # Missing sources are empty, shorter inputs zero-extend, and an empty result deletes dest.
        ["SET", a, b"\xff\x0f\xaa"], ["SET", b, b"\x0f\xf0"], ["DEL", c, d, e],
        ["BITOP", "AND", c, a, b], ["GET", c], ["STRLEN", c],
        ["BITOP", "OR", d, a, b, e], ["GET", d],
        ["BITOP", "XOR", e, a, b], ["GET", e],
        ["BITOP", "NOT", c, b], ["GET", c],
        ["BITOP", "AND", d, "bitmap-missing-1", "bitmap-missing-2"],
        ["EXISTS", d], ["STRLEN", d],

        # Destination/source aliasing and ordinary string-command interoperability.
        ["SET", a, "hello"], ["APPEND", a, b"\x00\xff"], ["SETBIT", a, "9", "1"],
        ["GETRANGE", a, "0", "-1"], ["STRLEN", a], ["BITCOUNT", a],
        ["BITOP", "XOR", a, a, b], ["GET", a], ["GETBIT", a, "9"],
        ["SET", e, "12345"], ["SETBIT", e, "3", "0"], ["GET", e],
        ["APPEND", e, "tail"], ["BITPOS", e, "1"], ["GETRANGE", e, "0", "-1"],

        # Exact parser/type failures must not mutate the destination.
        ["BITOP", "NOT", c, a, b], ["BITOP", "NOPE", c, a],
        ["SETBIT", a, "-1", "1"], ["SETBIT", a, "1", "2"],
        ["SADD", "bitmap-wrongtype", "x"], ["SET", c, "keep"],
        ["BITOP", "OR", c, a, "bitmap-wrongtype"], ["GET", c],
    ]
    return ops

def gen_bitfield(rng):
    keys = ["bf:%02d:%s" % (i, "".join(rng.choice("abcdef0123456789") for _ in range(24)))
            for i in range(20)]
    types = ["u1", "u2", "u7", "u8", "u16", "u31", "u32", "u63",
             "i1", "i2", "i7", "i8", "i16", "i31", "i32", "i63", "i64"]
    values = ["-9223372036854775808", "-65537", "-257", "-129", "-1", "0", "1",
              "127", "255", "256", "65535", "9223372036854775807"]
    ops = []
    def K(): return rng.choice(keys)
    def T(): return rng.choice(types)
    def O(t):
        bits = int(t[1:])
        return ("#%d" % rng.randrange(0, 48)) if rng.randrange(3) == 0 \
            else str(rng.randrange(0, 48 * bits + 24))

    for _ in range(3200):
        choice = rng.randrange(18)
        key = K()
        type_name = T()
        offset = O(type_name)
        if choice < 4:
            ops.append(["BITFIELD", key, "GET", type_name, offset])
        elif choice < 8:
            ops.append(["BITFIELD", key, "SET", type_name, offset, rng.choice(values)])
        elif choice < 12:
            ops.append(["BITFIELD", key, "OVERFLOW", rng.choice(["WRAP", "SAT", "FAIL"]),
                        "INCRBY", type_name, offset, rng.choice(values)])
        elif choice == 12:
            second = T()
            ops.append(["BITFIELD", key,
                        "GET", type_name, offset,
                        "OVERFLOW", rng.choice(["WRAP", "SAT", "FAIL"]),
                        "INCRBY", second, O(second), rng.choice(values),
                        "GET", second, O(second)])
        elif choice == 13:
            ops.append(["BITFIELD_RO", key, "GET", type_name, offset])
        elif choice == 14:
            ops.append(["SETBIT", key, str(rng.randrange(0, 512)), str(rng.randrange(2))])
        elif choice == 15:
            ops.append(["GETBIT", key, str(rng.randrange(0, 768))])
        elif choice == 16:
            ops.append([rng.choice(["SET", "APPEND"]), key,
                        rng.choice([b"", b"12345", b"hello", b"\x00\xff\x80"])])
        else:
            ops.append([rng.choice(["GET", "STRLEN", "DEL"]), key])

    a, b, c, d = keys[:4]
    ops += [
        ["DEL", a, b, c, d],
        ["BITFIELD", a], ["BITFIELD", a, "OVERFLOW", "SAT"],
        ["BITFIELD", a, "GET", "u8", "0"], ["EXISTS", a],
        ["BITFIELD", a, "SET", "u8", "0", "255", "GET", "u8", "0",
         "INCRBY", "u8", "0", "1", "GET", "u8", "0"],
        ["BITFIELD", a, "OVERFLOW", "SAT", "INCRBY", "u8", "0", "-1",
         "OVERFLOW", "WRAP", "INCRBY", "u8", "0", "-999"],
        ["BITFIELD", b, "SET", "i8", "0", "127", "OVERFLOW", "WRAP",
         "INCRBY", "i8", "0", "1", "OVERFLOW", "SAT", "INCRBY", "i8", "0", "-1"],
        ["BITFIELD", b, "OVERFLOW", "FAIL", "SET", "i8", "0", "999",
         "GET", "i8", "0"],
        ["BITFIELD", c, "SET", "u4", "#2", "10", "GET", "u4", "#2"],
        ["STRLEN", c], ["GET", c],
        ["SET", d, "12345"], ["BITFIELD", d, "GET", "u8", "0"],
        ["SET", d, "v", "PX", "600000"], ["BITFIELD", d, "SET", "u8", "#4", "9"],
        ["PTTL", d],
        ["BITFIELD_RO", a, "GET", "u8", "0"],
        ["BITFIELD_RO", a, "GET", "u8", "0", "SET", "u8", "0", "1"],
        ["BITFIELD", a, "GET", "u64", "0"], ["BITFIELD", a, "GET", "i65", "0"],
        ["BITFIELD", a, "GET", "i8", "#1152921504606846976"],
        ["BITFIELD", a, "OVERFLOW", "NOPE"], ["BITFIELD", a, "GET", "u8"],
        ["SADD", "bf:wrongtype", "x"], ["BITFIELD", "bf:wrongtype"],
    ]
    return ops

def gen_hll(rng):
    keys = ["hll:%02d:%s" % (i, "".join(rng.choice("abcdef0123456789") for _ in range(30)))
            for i in range(18)]
    elements = ["e:%04d" % i for i in range(700)] + ["", "nul\x00byte", "\xff\x00binary"]
    ops = []

    # Random PFADD streams deliberately repeat both elements within one call and across calls.
    # GET/STRLEN make the HYLL bytes themselves part of the differential contract, not just counts.
    for _ in range(1800):
        key = rng.choice(keys)
        chosen = [rng.choice(elements) for _ in range(rng.randrange(1, 6))]
        if rng.randrange(4) == 0: chosen.append(chosen[0])
        ops.append(["PFADD", key] + chosen)
        action = rng.randrange(10)
        if action < 3: ops.append(["PFCOUNT", key])
        elif action == 3: ops.append(["PFCOUNT"] + [rng.choice(keys) for _ in range(rng.randrange(2, 6))])
        elif action == 4: ops.append(["STRLEN", key])
        elif action == 5: ops.append(["GET", key])

    sparse, dense = "hll:sparse", "hll:dense"
    ops += [
        ["DEL", sparse, dense],
        ["PFADD", sparse, "a", "b", "a", "c"], ["PFCOUNT", sparse],
        ["GET", sparse], ["STRLEN", sparse],
        ["PFADD", sparse, "a", "b", "c"], ["PFCOUNT", sparse], ["GET", sparse],
    ]

    # Cross the default 3000-byte sparse ceiling, then compare the fixed 12304-byte dense image.
    for base in range(0, 5000, 64):
        ops.append(["PFADD", dense] + ["dense:%05d" % i for i in range(base, min(base + 64, 5000))])
    ops += [["PFCOUNT", dense], ["STRLEN", dense], ["GET", dense]]

    # Overlapping sources include a pre-existing destination because Redis merges argv[1] too.
    dst, left, right = "hll:merge:dst", "hll:merge:left", "hll:merge:right"
    for base in range(0, 1000, 50):
        ops.append(["PFADD", left] + ["overlap:%04d" % i for i in range(base, base + 50)])
    for base in range(500, 1500, 50):
        ops.append(["PFADD", right] + ["overlap:%04d" % i for i in range(base, base + 50)])
    for base in range(1200, 1700, 50):
        ops.append(["PFADD", dst] + ["overlap:%04d" % i for i in range(base, base + 50)])
    ops += [
        ["PFCOUNT", left, right, dst], ["PFMERGE", dst, left, right],
        ["STRLEN", dst], ["GET", dst], ["PFCOUNT", dst],
        ["PFCOUNT", left, right, dst], ["STRLEN", dst], ["GET", dst],
        ["PFADD", "hll:small-left", "a", "b", "c"],
        ["PFADD", "hll:small-right", "b", "c", "d"],
        ["PFADD", "hll:small-dst", "d", "e"],
        ["PFMERGE", "hll:small-dst", "hll:small-left", "hll:small-right"],
        ["STRLEN", "hll:small-dst"], ["GET", "hll:small-dst"],
        ["PFCOUNT", "hll:small-dst"], ["GET", "hll:small-dst"],
        ["PFMERGE", "hll:empty-dst", "hll:missing-a", "hll:missing-b"],
        ["PFCOUNT", "hll:empty-dst"], ["GET", "hll:empty-dst"],
        ["PFMERGE", "hll:empty-dst"], ["GET", "hll:empty-dst"],
    ]

    # Distinguish a non-string WRONGTYPE, a string with a bad HLL header, and a valid header whose
    # sparse stream is corrupt. The invalid cache bit forces PFCOUNT to decode the corrupt stream.
    corrupt = b"HYLL\x01\x00\x00\x00" + b"\x00" * 7 + b"\x80" + b"\x00"
    ops += [
        ["SADD", "hll:set-type", "x"], ["PFCOUNT", "hll:set-type"],
        ["PFADD", "hll:set-type", "x"], ["PFMERGE", "hll:merge-type", "hll:set-type"],
        ["SET", "hll:bad", "not-a-hyperloglog"], ["PFCOUNT", "hll:bad"],
        ["PFADD", "hll:bad", "x"], ["PFMERGE", "hll:merge-bad", "hll:bad"],
        ["SET", "hll:int-bad", "42"], ["PFCOUNT", "hll:int-bad"],
        ["PFADD", "hll:int-bad", "x"], ["PFMERGE", "hll:merge-int-bad", "hll:int-bad"],
        ["SET", "hll:corrupt", corrupt], ["PFCOUNT", "hll:corrupt"],
        ["PFADD", "hll:corrupt", "x"], ["PFMERGE", "hll:merge-corrupt", "hll:corrupt"],
    ]
    return ops

def gen_cgaps(rng):
    # Container-gap commands deliberately use long unrelated names so their numkeys/STORE forms
    # exercise both scatter-v2 and same-owner coalescing across ordinary shard counts.
    keys = ["cg:%02d:%s" % (i, "".join(rng.choice("abcdef0123456789") for _ in range(34)))
            for i in range(48)]
    lists = keys[:14]
    zsets = keys[14:28]
    numeric = keys[28:38]
    alpha = keys[38:44]
    dests = keys[44:]
    nums = ["-10", "-2", "0", "1", "2", "10", "3.5", "1e2", "+4", "001"]
    words = ["a", "aa", "b", "z", "10", "2", "item-3", "item-20"]
    ops = []

    for key in lists:
        ops.append(["RPUSH", key] + [rng.choice(words) for _ in range(rng.randrange(1, 6))])
    for key in zsets:
        pairs = []
        for n in range(rng.randrange(1, 6)):
            pairs += [str(rng.randrange(-8, 9)), "zm-%d-%d" % (keys.index(key), n)]
        ops.append(["ZADD", key] + pairs)
    for key in numeric:
        ops.append([rng.choice(["RPUSH", "SADD"]), key] +
                   [rng.choice(nums) for _ in range(rng.randrange(1, 7))])
    for key in alpha:
        ops.append([rng.choice(["RPUSH", "SADD"]), key] +
                   [rng.choice(words) for _ in range(rng.randrange(1, 7))])

    for _ in range(3200):
        c = rng.randrange(18)
        if c == 0:
            ops.append(["LPOS", rng.choice(lists), rng.choice(words), "RANK",
                        str(rng.choice([-4, -2, -1, 1, 2, 4]))])
        elif c == 1:
            ops.append(["LPOS", rng.choice(lists), rng.choice(words), "RANK",
                        str(rng.choice([-3, -1, 1, 3])), "COUNT", str(rng.randrange(0, 5)),
                        "MAXLEN", str(rng.randrange(0, 8))])
        elif c == 2:
            chosen = [rng.choice(lists) for _ in range(rng.randrange(1, 6))]
            ops.append(["LMPOP", str(len(chosen))] + chosen +
                       [rng.choice(["LEFT", "RIGHT"]), "COUNT", str(rng.randrange(1, 7))])
        elif c == 3:
            chosen = [rng.choice(zsets) for _ in range(rng.randrange(1, 6))]
            ops.append(["ZMPOP", str(len(chosen))] + chosen +
                       [rng.choice(["MIN", "MAX"]), "COUNT", str(rng.randrange(1, 7))])
        elif c == 4:
            ops.append(["RPUSH", rng.choice(lists)] + [rng.choice(words) for _ in range(rng.randrange(1, 4))])
        elif c == 5:
            ops.append(["ZADD", rng.choice(zsets), str(rng.randrange(-20, 21)), rng.choice(words)])
        elif c == 6:
            ops.append(["ZRANGESTORE", rng.choice(dests), rng.choice(zsets),
                        str(rng.randrange(-6, 3)), str(rng.randrange(-2, 7))] +
                       (["REV"] if rng.randrange(2) else []))
        elif c == 7:
            ops.append(["ZRANGESTORE", rng.choice(dests), rng.choice(zsets),
                        rng.choice(["-inf", "(0", "2"]), rng.choice(["+inf", "10", "(8"]),
                        "BYSCORE"] + (["LIMIT", str(rng.randrange(-1, 4)),
                                       str(rng.randrange(-1, 5))] if rng.randrange(2) else []) +
                       (["REV"] if rng.randrange(2) else []))
        elif c == 8:
            ops.append(["SORT", rng.choice(numeric)] +
                       (["DESC"] if rng.randrange(2) else ["ASC"]) +
                       (["LIMIT", str(rng.randrange(-2, 5)), str(rng.randrange(-2, 6))]
                        if rng.randrange(2) else []))
        elif c == 9:
            ops.append(["SORT", rng.choice(alpha), "ALPHA", rng.choice(["ASC", "DESC"])])
        elif c == 10:
            ops.append(["SORT", rng.choice(numeric), "LIMIT", str(rng.randrange(-2, 5)),
                        str(rng.randrange(-2, 6)), "STORE", rng.choice(dests)])
        elif c == 11:
            ops.append(["SORT", rng.choice(alpha + zsets), "ALPHA", "STORE", rng.choice(dests),
                        rng.choice(["ASC", "DESC"])])
        elif c == 12: ops.append(["DEL", rng.choice(lists)])
        elif c == 13: ops.append(["DEL", rng.choice(zsets)])
        elif c == 14: ops.append(["LRANGE", rng.choice(dests), "0", "-1"])
        elif c == 15: ops.append(["ZRANGE", rng.choice(dests), "0", "-1", "WITHSCORES"])
        elif c == 16: ops.append(["TYPE", rng.choice(dests)])
        else: ops.append(["EXISTS", rng.choice(keys)])

    # Directed byte-exact edges requested by the container-gap mission.
    l0, l1, l2, lbig = lists[:4]
    z0, z1, z2, zbig = zsets[:4]
    d0, d1 = dests[:2]
    n0, a0 = numeric[0], alpha[0]
    ops += [
        ["DEL", l0, l1, l2], ["RPUSH", l0, "a", "x", "a", "y", "a"],
        ["LPOS", l0, "a", "RANK", "-1"],
        ["LPOS", l0, "a", "RANK", "-2", "COUNT", "0", "MAXLEN", "0"],
        ["LPOS", l0, "a", "RANK", "-2", "COUNT", "5", "MAXLEN", "2"],
        ["RPUSH", l1, "first", "second", "third"],
        ["LMPOP", "3", l2, l1, l0, "LEFT", "COUNT", "99"],
        ["LRANGE", l0, "0", "-1"], ["LRANGE", l1, "0", "-1"],
        ["DEL", l0, l1, l2], ["LMPOP", "3", l0, l1, l2, "RIGHT", "COUNT", "2"],
        ["LMPOP", "0", l0, "LEFT"], ["LMPOP", "1", l0, "LEFT", "COUNT", "0"],
        ["SET", l0, "wrong"], ["RPUSH", l1, "winner"],
        ["LMPOP", "2", l0, l1, "LEFT"], ["LMPOP", "2", l1, l0, "LEFT"],
        ["DEL", z0, z1, z2], ["ZADD", z1, "1", "one", "2", "two", "3", "three"],
        ["ZMPOP", "3", z0, z1, z2, "MAX", "COUNT", "99"],
        ["ZRANGE", z1, "0", "-1", "WITHSCORES"],
        ["DEL", z0, z1, z2], ["ZMPOP", "3", z0, z1, z2, "MIN", "COUNT", "2"],
        ["ZMPOP", "0", z0, "MIN"], ["ZMPOP", "1", z0, "MAX", "COUNT", "0"],
        ["SET", z0, "wrong"], ["ZADD", z1, "1", "winner"],
        ["ZMPOP", "2", z0, z1, "MIN"], ["ZMPOP", "2", z1, z0, "MIN"],
        ["DEL", lbig], ["RPUSH", lbig] +
            [("L%03d-" % i) + "x" * 72 for i in range(115)],
        ["LMPOP", "1", lbig, "RIGHT", "COUNT", "112"], ["LLEN", lbig],
        ["DEL", zbig], ["ZADD", zbig] +
            sum(([str(i), "Z%03d" % i] for i in range(200)), []),
        ["ZMPOP", "1", zbig, "MIN", "COUNT", "197"], ["ZCARD", zbig],

        ["DEL", z0, d0], ["ZADD", z0, "0", "a", "1", "b", "2", "c", "3", "d"],
        ["ZRANGESTORE", d0, z0, "1", "-2"], ["ZRANGE", d0, "0", "-1", "WITHSCORES"],
        ["ZRANGESTORE", d0, z0, "+inf", "-inf", "BYSCORE", "REV", "LIMIT", "0", "2"],
        ["ZRANGE", d0, "0", "-1", "WITHSCORES"],
        ["ZRANGESTORE", d0, z0, "9", "0"], ["EXISTS", d0],
        ["ZADD", z0, "0", "aa", "0", "ab", "0", "ba"],
        ["ZRANGESTORE", d0, z0, "+", "[aa", "BYLEX", "REV", "LIMIT", "0", "2"],
        ["ZRANGE", d0, "0", "-1", "WITHSCORES"],

        ["DEL", n0, a0, d0, d1], ["RPUSH", n0, "10", "2", "-1", "3.5"],
        ["SORT", n0], ["SORT", n0, "DESC", "LIMIT", "1", "2"],
        ["RPUSH", a0, "10", "2", "apple"], ["SORT", a0], ["SORT", a0, "ALPHA"],
        ["SET", d0, "wrong-type-destination"], ["SORT", a0, "ALPHA", "STORE", d0],
        ["TYPE", d0], ["LRANGE", d0, "0", "-1"],
        ["DEL", z2], ["ZADD", z2, "8", "10", "1", "2", "3", "-1"],
        ["SORT", z2], ["SORT", z2, "ALPHA", "DESC"],
        ["SET", d1, "preserve-on-error"], ["SORT", a0, "STORE", d1], ["GET", d1],
    ]
    return ops

def gen_script(rng):
    # Scripting surface: EVAL / EVALSHA / EVAL_RO / EVALSHA_RO / SCRIPT LOAD|EXISTS|FLUSH and the
    # FUNCTION library (FCALL / FCALL_RO / LIST / STATS).
    #
    # Every keyed activation declares 2--8 distinct keys.  This is deliberately wider than the
    # old single-key stream: the cross-owner lowering is the mechanism under test, and a suite
    # that never gives the router more than one owner can report a vacuous zero-diff result.
    # Every redis.call still names KEYS[i]. TomoKV enforces the declared range (Redis 7 only warns),
    # because routing happened before execution.
    # Everything else — reply conversion, the error text including the " script: <sha>, on
    # @user_script:<line>." tail, the read-only gate, the function metadata — is byte-compared.
    keys = ["sc:%d" % i for i in range(24)]
    values = ["1", "2", "-3", "abc", "", "10", "3.5", "hello world"]

    def K():
        return rng.choice(keys)

    def KS():
        return rng.sample(keys, rng.randrange(2, 9))

    def keyed(command, source, declared, *arguments):
        return [command, source, str(len(declared))] + declared + list(arguments)

    scripts = [
        "return 1",
        "return 'plain'",
        "return {1,2,3,'x'}",
        "return {ok='fine'}",
        "return {err='handmade'}",
        "return #KEYS",
        "return ARGV[1]",
        "return tostring(ARGV[1]) .. ':' .. tostring(#ARGV)",
        "return redis.call('GET', KEYS[1])",
        "return redis.call('SET', KEYS[1], ARGV[1])",
        "return redis.call('INCR', KEYS[1])",
        "return redis.call('APPEND', KEYS[1], ARGV[1])",
        "return redis.call('STRLEN', KEYS[1])",
        "return redis.call('EXISTS', KEYS[1])",
        "return redis.call('TYPE', KEYS[1])",
        "return redis.call('DEL', KEYS[1])",
        "return {redis.call('SET', KEYS[1], ARGV[1]), redis.call('GET', KEYS[1])}",
        "local v = redis.call('GET', KEYS[1]) if v then return v .. '!' else return false end",
        "local r = redis.pcall('INCR', KEYS[1]) if r.err then return r.err end return r",
        "return redis.call('LPUSH', KEYS[1], ARGV[1])",
        "return redis.call('LRANGE', KEYS[1], 0, -1)",
        "return redis.call('HSET', KEYS[1], 'f', ARGV[1])",
        "return redis.call('HGETALL', KEYS[1])",
        "return redis.call('ZADD', KEYS[1], 1, ARGV[1])",
        "return redis.call('ZSCORE', KEYS[1], ARGV[1])",
        "return redis.call('SADD', KEYS[1], ARGV[1])",
        "return redis.call('SCARD', KEYS[1])",
        ("local v = redis.call('GET', KEYS[1]); "
         "redis.call('SET', KEYS[2], v or ARGV[1]); "
         "return redis.call('GET', KEYS[2])"),
        ("redis.call('SET', KEYS[2], ARGV[1]); "
         "return {redis.call('GET', KEYS[1]), redis.call('GET', KEYS[2])}"),
        "local a = 0 for i = 1, 20 do a = a + i end return a",
    ]
    keyless = ["return 1", "return 'plain'", "return {1,2,3,'x'}", "return {ok='fine'}",
               "return {err='handmade'}", "return ARGV[1]", "return #KEYS",
               "local a = 0 for i = 1, 20 do a = a + i end return a"]
    # Deterministic error paths. Their replies carry the sha and the source line, so a divergence
    # in either the message or the tail shows up as a diff.
    broken = ["return (", "return redis.call('NOSUCHCOMMAND')", "error('kaput')",
              "error({err='raised table'})", "return zzz_undefined",
              "local x = nil\nreturn x.field"]
    ro_writes = ["return redis.call('SET', KEYS[1], 'ro')",
                 "return redis.call('INCR', KEYS[1])",
                 "return redis.call('DEL', KEYS[1])",
                 "return redis.call('LPUSH', KEYS[1], 'ro')"]

    library = ("#!lua name=difflib\n"
               "redis.register_function('dget', function(keys, args)\n"
               "  return {redis.call('GET', keys[1]), #keys, #args}\n"
               "end)\n"
               "redis.register_function{function_name='dset',\n"
               "  callback=function(keys, args) return redis.call('SET', keys[1], args[1]) end,\n"
               "  description='diff writer'}\n"
               "redis.register_function{function_name='dro',\n"
               "  callback=function(keys, args) return redis.call('GET', keys[1]) end,\n"
               "  flags={'no-writes'}, description='diff reader'}\n"
               "redis.register_function{function_name='dcount',\n"
               "  callback=function(keys, args) return #args end, flags={'no-writes'}}\n")
    library2 = ("#!lua name=difflib2\n"
                "redis.register_function('decho', function(keys, args) return args[1] end)\n")
    functions = ["dget", "dset", "dro", "dcount"]

    def sha(text):
        return hashlib.sha1(text.encode()).hexdigest()

    ops = [["SCRIPT", "FLUSH"], ["FUNCTION", "FLUSH"],
           ["FUNCTION", "LOAD", library], ["FUNCTION", "LOAD", library2],
           ["FUNCTION", "LOAD", library], ["FUNCTION", "LOAD", "REPLACE", library],
           ["FUNCTION", "STATS"]]
    for key in keys:
        ops.append(["SET", key, rng.choice(values)])

    loaded = []
    for _ in range(4200):
        pick = rng.randrange(100)
        if pick < 20:
            src = rng.choice(scripts)
            declared = KS()
            ops.append(keyed("EVAL", src, declared, rng.choice(values)))
        elif pick < 26:
            src = rng.choice(keyless)
            ops.append(["EVAL", src, "0", rng.choice(values)])
        elif pick < 34:
            src = rng.choice(scripts)
            loaded.append(src)
            ops.append(["SCRIPT", "LOAD", src])
        elif pick < 44:
            src = rng.choice(loaded) if loaded else scripts[0]
            declared = KS()
            ops.append(keyed("EVALSHA", sha(src), declared, rng.choice(values)))
        elif pick < 50:
            wanted = [sha(rng.choice(scripts)) for _ in range(rng.randrange(1, 4))]
            ops.append(["SCRIPT", "EXISTS"] + wanted)
        elif pick < 58:
            declared = KS()
            ops.append(keyed("EVAL_RO", rng.choice(scripts), declared, rng.choice(values)))
        elif pick < 62:
            declared = KS()
            ops.append(keyed("EVAL_RO", rng.choice(ro_writes), declared))
        elif pick < 66:
            src = rng.choice(loaded) if loaded else scripts[0]
            declared = KS()
            ops.append(keyed("EVALSHA_RO", sha(src), declared, rng.choice(values)))
        elif pick < 72:
            declared = KS()
            ops.append(keyed("EVAL", rng.choice(broken), declared))
        elif pick < 82:
            name = rng.choice(functions)
            declared = KS()
            ops.append(["FCALL", name, str(len(declared))] + declared + [rng.choice(values)])
        elif pick < 88:
            name = rng.choice(functions)
            declared = KS()
            ops.append(["FCALL_RO", name, str(len(declared))] + declared + [rng.choice(values)])
        elif pick < 90:
            declared = KS()
            ops.append(["FCALL", "not_a_function", str(len(declared))] + declared)
        elif pick < 92:
            ops.append(["FUNCTION", "LIST"])
        elif pick < 94:
            ops.append(["FUNCTION", "LIST", "LIBRARYNAME", rng.choice(["difflib", "difflib2",
                                                                       "absent"])])
        elif pick < 95:
            ops.append(["FUNCTION", "LIST", "WITHCODE"])
        elif pick < 96:
            ops.append(["FUNCTION", "STATS"])
        elif pick < 97:
            loaded = []
            ops.append(["SCRIPT", "FLUSH", rng.choice(["SYNC", "ASYNC"])])
        else:
            declared = KS()
            # SECOND is interpreted by the runner as a command on a distinct connection on each
            # server. The key is selected from the immediately following activation's declared
            # set, so this leg proves that script cuts compose with plain writes from another
            # client instead of merely exercising a second idle socket.
            key = rng.choice(declared)
            ops.append(["SECOND", "SET", key, rng.choice(values)])
            src = rng.choice(scripts)
            ops.append(keyed("EVAL", src, declared, rng.choice(values)))
    # PHASE 2 -- CROSS SCRIPTS ADJACENT TO OTHER MULTI-KEY GROUPS.
    #
    # Phase 1 now proves the staged engine directly with 2--8 declared keys. This tail adds a
    # distinct interaction: ordinary MSET/MGET/DEL groups over that same key space sit beside
    # cross-owner activations in one pipeline chunk, so script effects must compose with a live
    # non-script scatter group and remain visible to the following multi-key read.
    #
    # Every command here is ordinary Redis, byte-comparable against the oracle; the sharding is the
    # target's private business.
    write_fail = ["redis.call('INCR', KEYS[1]) error('boom')",
                  "redis.call('SET', KEYS[1], 'made') error('boom')",
                  "redis.call('APPEND', KEYS[1], 'XY') return nosuchglobal",
                  "redis.call('DEL', KEYS[1]) error('boom')",
                  "redis.call('INCR', KEYS[1]) return redis.call('LPUSH', KEYS[1], 'x')",
                  "redis.call('SET', KEYS[1], ARGV[1]) return redis.call('GET', KEYS[1])"]
    for _ in range(700):
        a, b, c = K(), K(), K()
        pick = rng.randrange(6)
        if pick == 0:
            ops.append(["MSET", a, rng.choice(values), b, rng.choice(values)])
        elif pick == 1:
            declared = [a, b] if a != b else KS()
            ops.append(keyed("EVAL", rng.choice(write_fail), declared, rng.choice(values)))
        elif pick == 2:
            ops.append(["MGET", a, b, c])
        elif pick == 3:
            ops.append(rng.choice([["DEL", a, b], ["UNLINK", a, c],
                                   ["EXISTS", a, b, c], ["TOUCH", a, b]]))
        elif pick == 4:
            declared = [a, b] if a != b else KS()
            ops.append(["FCALL", rng.choice(["dset", "dget"]), str(len(declared))] +
                       declared + [rng.choice(values)])
        else:
            ops.append(rng.choice([["GET", a], ["STRLEN", a], ["TYPE", a],
                                   ["SET", a, rng.choice(values)]]))

    ops.append(["FUNCTION", "DELETE", "difflib2"])
    ops.append(["FUNCTION", "LIST"])
    ops.append(["FUNCTION", "FLUSH"])
    ops.append(["FUNCTION", "LIST"])
    ops.append(["SCRIPT", "FLUSH"])
    return ops

# Blocking commands need several independently ordered clients and out-of-band writers, so they
# cannot use the ordinary one-request/one-reply pipeline below.  The randomized body remains a
# byte differ; the directed tail adds the wait/timeout/disconnect properties a linear stream cannot
# express. The directed tail covers transaction lowering and WAIT deadlines in the selected RESP
# mode; every result is now an ordinary byte comparison rather than a documented exception.
def run_blocking_differ(rng):
    ts, tf = conn_mode(TH, TP, RESP3)
    os_, of = conn_mode(OH, OP, RESP3)
    diffs = 0
    checks = 0
    logical_ops = 0
    multi_ready_cases = 0

    def mismatch(label, target, oracle):
        nonlocal diffs
        diffs += 1
        if diffs <= 16:
            print("  DIFF %s\n    target: %r\n    oracle: %r" %
                  (label, target, oracle), flush=True)

    def compare(label, target, oracle, unordered=False):
        nonlocal checks
        checks += 1
        if unordered:
            target = normalize("SMEMBERS", target)
            oracle = normalize("SMEMBERS", oracle)
        if target != oracle:
            mismatch(label, target[:256], oracle[:256])
        return target, oracle

    def both(argv, label=None):
        nonlocal logical_ops
        payload = enc(argv)
        ts.sendall(payload); os_.sendall(payload)
        logical_ops += 1
        return compare(label or " ".join(argv[:4]), read_reply(tf), read_reply(of))

    def property_check(label, target, oracle):
        nonlocal checks
        checks += 1
        if target != oracle:
            mismatch(label, repr(target).encode(), repr(oracle).encode())
        return target == oracle

    def info_blocked(sock, file):
        sock.sendall(enc(["INFO", "CLIENTS"]))
        body = parse_reply(read_reply(file))
        if not isinstance(body, bytes):
            raise RuntimeError("INFO CLIENTS returned an unexpected shape: %r" % (body,))
        match = re.search(br"(?:^|\r\n)blocked_clients:([0-9]+)(?:\r\n|$)", body)
        if not match:
            raise RuntimeError("INFO CLIENTS omitted blocked_clients")
        return int(match.group(1))

    def await_blocked(sock, file, wanted, timeout=3.0):
        deadline = time.monotonic() + timeout
        value = -1
        while time.monotonic() < deadline:
            value = info_blocked(sock, file)
            if value == wanted:
                return value
            time.sleep(0.01)
        return value

    for sock, file in ((ts, tf), (os_, of)):
        sock.sendall(enc(["FLUSHALL"]))
        if read_reply(file)[:1] != b"+":
            raise RuntimeError("FLUSHALL failed on blocking clean-slate")

    # >=4k deterministic command/reply comparisons.  Every blocking command is made ready before
    # dispatch, so this section explores parsing, key priority, edge selection and reply framing
    # without introducing timing as a false differential signal.
    timeouts = ["0", ".5", "0.25", "1e-3", "2"]
    directions = ["LEFT", "RIGHT"]
    for iteration in range(1600):
        stem = "cbd:%d:%02d" % (SEED, iteration % 37)
        a, b, dst = stem + ":a", stem + ":b", stem + ":dst"
        choice = rng.randrange(8)
        timeout = rng.choice(timeouts)
        both(["DEL", a, b, dst], "random %d DEL" % iteration)
        if choice == 0:
            both(["RPUSH", a, "a%d" % iteration])
            if rng.randrange(2):
                both(["RPUSH", b, "b%d" % iteration])
                multi_ready_cases += 1
            both(["BLPOP", a, b, timeout], "random %d BLPOP" % iteration)
        elif choice == 1:
            both(["LPUSH", a, "a%d" % iteration])
            if rng.randrange(2):
                both(["LPUSH", b, "b%d" % iteration])
                multi_ready_cases += 1
            both(["BRPOP", a, b, timeout], "random %d BRPOP" % iteration)
        elif choice == 2:
            values = ["m%d:%d" % (iteration, index) for index in range(1 + rng.randrange(3))]
            both(["RPUSH", a] + values)
            both(["BLMOVE", a, dst, rng.choice(directions), rng.choice(directions), timeout],
                 "random %d BLMOVE" % iteration)
        elif choice == 3:
            both(["RPUSH", a, "a0", "a1", "a2"])
            if rng.randrange(2):
                both(["RPUSH", b, "b0", "b1"])
                multi_ready_cases += 1
            both(["BLMPOP", timeout, "2", a, b, rng.choice(directions), "COUNT",
                  str(1 + rng.randrange(3))], "random %d BLMPOP" % iteration)
        elif choice == 4:
            both(["ZADD", a, "2", "a2", "1", "a1"])
            if rng.randrange(2):
                both(["ZADD", b, "3", "b3"])
                multi_ready_cases += 1
            both(["BZPOPMIN", a, b, timeout], "random %d BZPOPMIN" % iteration)
        elif choice == 5:
            both(["ZADD", a, "2", "a2", "1", "a1"])
            if rng.randrange(2):
                both(["ZADD", b, "3", "b3"])
                multi_ready_cases += 1
            both(["BZPOPMAX", a, b, timeout], "random %d BZPOPMAX" % iteration)
        elif choice == 6:
            both(["ZADD", a, "2", "a2", "1", "a1", "3", "a3"])
            if rng.randrange(2):
                both(["ZADD", b, "4", "b4"])
                multi_ready_cases += 1
            both(["BZMPOP", timeout, "2", a, b, rng.choice(["MIN", "MAX"]), "COUNT",
                  str(1 + rng.randrange(3))], "random %d BZMPOP" % iteration)
        else:
            both(["WAIT", "0", str(rng.randrange(4))], "random %d WAIT" % iteration)

    # Fractional expiration on every collection family.  Both requests are sent before either
    # reply is read, so the oracle and target wait concurrently; only their exact timeout frames
    # are compared.
    timeout_commands = [
        ["BLPOP", "cbd:timeout:l", ".01"],
        ["BRPOP", "cbd:timeout:l", "1e-2"],
        ["BLMOVE", "cbd:timeout:src", "cbd:timeout:dst", "LEFT", "RIGHT", ".01"],
        ["BRPOPLPUSH", "cbd:timeout:src", "cbd:timeout:dst", ".01"],
        ["BLMPOP", ".01", "1", "cbd:timeout:l", "LEFT", "COUNT", "2"],
        ["BZPOPMIN", "cbd:timeout:z", ".01"],
        ["BZPOPMAX", "cbd:timeout:z", "1e-2"],
        ["BZMPOP", ".01", "1", "cbd:timeout:z", "MIN", "COUNT", "2"],
    ]
    for command in timeout_commands:
        both(command, "fractional timeout " + command[0])

    # Timeout zero means forever for every blocking collection command.  Silence before the push
    # is the negative control; the subsequent exact reply proves the waiter actually armed and
    # then fired instead of merely being slow.
    forever_arms = [
        (["BLPOP", "cbd:forever:blpop", "0"],
         ["RPUSH", "cbd:forever:blpop", "v"]),
        (["BRPOP", "cbd:forever:brpop", "0"],
         ["LPUSH", "cbd:forever:brpop", "v"]),
        (["BLMOVE", "cbd:forever:blmove", "cbd:forever:dst", "RIGHT", "LEFT", "0"],
         ["RPUSH", "cbd:forever:blmove", "v"]),
        (["BRPOPLPUSH", "cbd:forever:brpoplpush", "cbd:forever:legacy-dst", "0"],
         ["RPUSH", "cbd:forever:brpoplpush", "v"]),
        (["BLMPOP", "0", "1", "cbd:forever:blmpop", "LEFT", "COUNT", "2"],
         ["RPUSH", "cbd:forever:blmpop", "v"]),
        (["BZPOPMIN", "cbd:forever:bzmin", "0"],
         ["ZADD", "cbd:forever:bzmin", "1", "v"]),
        (["BZPOPMAX", "cbd:forever:bzmax", "0"],
         ["ZADD", "cbd:forever:bzmax", "1", "v"]),
        (["BZMPOP", "0", "1", "cbd:forever:bzmpop", "MAX", "COUNT", "2"],
         ["ZADD", "cbd:forever:bzmpop", "1", "v"]),
    ]
    for command, wake in forever_arms:
        tbase, obase = info_blocked(ts, tf), info_blocked(os_, of)
        tw, twf = conn_mode(TH, TP, RESP3); ow, owf = conn_mode(OH, OP, RESP3)
        tw.sendall(enc(command)); ow.sendall(enc(command)); logical_ops += 1
        tarmed = await_blocked(ts, tf, tbase + 1)
        oarmed = await_blocked(os_, of, obase + 1)
        property_check("%s timeout-zero armed" % command[0], tarmed == tbase + 1,
                       oarmed == obase + 1)
        tready = bool(select.select([tw], [], [], 0.05)[0])
        oready = bool(select.select([ow], [], [], 0.05)[0])
        property_check("%s timeout-zero silence" % command[0], tready, oready)
        if tready or oready:
            mismatch("%s timeout-zero negative control" % command[0],
                     b"ready" if tready else b"silent", b"ready" if oready else b"silent")
        both(wake, "%s wake write" % command[0])
        compare("%s wake reply" % command[0], read_reply(twf), read_reply(owf))
        tw.close(); ow.close(); twf.close(); owf.close()

    # Redis serves clients blocked on one key in arrival order.  Register each client fully before
    # creating the next so accept-thread placement cannot turn registration order into a race.
    both(["DEL", "cbd:fifo"])
    tbase, obase = info_blocked(ts, tf), info_blocked(os_, of)
    waiters = []
    for index in range(6):
        tw, twf = conn_mode(TH, TP, RESP3); ow, owf = conn_mode(OH, OP, RESP3)
        command = ["BLPOP", "cbd:fifo", "0"]
        tw.sendall(enc(command)); ow.sendall(enc(command)); logical_ops += 1
        tarmed = await_blocked(ts, tf, tbase + index + 1)
        oarmed = await_blocked(os_, of, obase + index + 1)
        property_check("FIFO waiter %d armed" % index, tarmed - tbase, oarmed - obase)
        waiters.append((tw, twf, ow, owf))
    both(["RPUSH", "cbd:fifo"] + ["v%d" % i for i in range(len(waiters))], "FIFO wake")
    for index, (tw, twf, ow, owf) in enumerate(waiters):
        target = read_reply(twf); oracle = read_reply(owf)
        compare("FIFO waiter %d reply" % index, target, oracle)
        expected = [b"cbd:fifo", ("v%d" % index).encode()]
        property_check("FIFO waiter %d value" % index, parse_reply(target), expected)
        property_check("FIFO oracle waiter %d value" % index, parse_reply(oracle), expected)
        tw.close(); ow.close(); twf.close(); owf.close()

    # A disconnect must cancel its registry entry.  The value pushed afterward is the positive
    # control: a leaked waiter would consume it and make the immediate BLPOP return null.
    both(["DEL", "cbd:disconnect"])
    tbase, obase = info_blocked(ts, tf), info_blocked(os_, of)
    tw, twf = conn_mode(TH, TP, RESP3); ow, owf = conn_mode(OH, OP, RESP3)
    tw.sendall(enc(["BLPOP", "cbd:disconnect", "0"]))
    ow.sendall(enc(["BLPOP", "cbd:disconnect", "0"])); logical_ops += 1
    await_blocked(ts, tf, tbase + 1); await_blocked(os_, of, obase + 1)
    tw.close(); ow.close(); twf.close(); owf.close()
    tdrained = await_blocked(ts, tf, tbase)
    odrained = await_blocked(os_, of, obase)
    property_check("disconnect drains blocked client", tdrained - tbase, odrained - obase)
    both(["RPUSH", "cbd:disconnect", "survives"], "disconnect control push")
    target, oracle = both(["BLPOP", "cbd:disconnect", ".1"], "disconnect control pop")
    expected = [b"cbd:disconnect", b"survives"]
    property_check("disconnect target did not consume value", parse_reply(target), expected)
    property_check("disconnect oracle did not consume value", parse_reply(oracle), expected)

    # BLMOVE and WAIT already execute non-blockingly inside MULTI on both servers.
    for command in (["BLMOVE", "cbd:multi:src", "cbd:multi:dst", "LEFT", "RIGHT", "0"],
                    ["WAIT", "1", "0"]):
        both(["MULTI"], "MULTI before " + command[0])
        both(command, command[0] + " QUEUED")
        both(["EXEC"], command[0] + " EXEC")

    # Every blocking collection command becomes its nonblocking counterpart inside EXEC. Missing
    # data therefore produces a null element; ready data proves the lowering also performs the
    # mutation and preserves each command's legacy reply shape.
    multi_commands = [
        ["BLPOP", "cbd:multi:l", "0"],
        ["BRPOP", "cbd:multi:l", "0"],
        ["BLMPOP", "0", "1", "cbd:multi:l", "LEFT"],
        ["BZPOPMIN", "cbd:multi:z", "0"],
        ["BZPOPMAX", "cbd:multi:z", "0"],
        ["BZMPOP", "0", "1", "cbd:multi:z", "MIN"],
    ]
    for command in multi_commands:
        both(["DEL", "cbd:multi:l", "cbd:multi:z"], "MULTI missing cleanup")
        both(["MULTI"], "MULTI before missing " + command[0])
        both(command, "missing " + command[0] + " QUEUED")
        both(["EXEC"], "missing " + command[0] + " EXEC")

    ready_setup = [
        (["RPUSH", "cbd:multi:l", "v"], multi_commands[0]),
        (["RPUSH", "cbd:multi:l", "v"], multi_commands[1]),
        (["RPUSH", "cbd:multi:l", "v"], multi_commands[2]),
        (["ZADD", "cbd:multi:z", "1", "m"], multi_commands[3]),
        (["ZADD", "cbd:multi:z", "1", "m"], multi_commands[4]),
        (["ZADD", "cbd:multi:z", "1", "m"], multi_commands[5]),
    ]
    for setup, command in ready_setup:
        both(["DEL", "cbd:multi:l", "cbd:multi:z"], "MULTI ready cleanup")
        both(setup, "MULTI ready setup " + command[0])
        both(["MULTI"], "MULTI before ready " + command[0])
        both(command, "ready " + command[0] + " QUEUED")
        both(["EXEC"], "ready " + command[0] + " EXEC")

    # An unsatisfied standalone WAIT must leave both wires silent until the deadline. Timeout zero
    # means forever, so the harness closes both sockets after proving silence and never performs a
    # blocking read on that case.
    for timeout in ("200", "0"):
        tw, twf = conn_mode(TH, TP, RESP3); ow, owf = conn_mode(OH, OP, RESP3)
        command = ["WAIT", "1", timeout]
        started = time.monotonic()
        tw.sendall(enc(command)); ow.sendall(enc(command)); logical_ops += 1
        time.sleep(0.05)
        tready = bool(select.select([tw], [], [], 0)[0])
        oready = bool(select.select([ow], [], [], 0)[0])
        property_check("WAIT 1 %s readiness parity" % timeout, tready, oready)
        property_check("target WAIT 1 %s silence control" % timeout, tready, False)
        property_check("oracle WAIT 1 %s silence control" % timeout, oready, False)
        if timeout != "0":
            compare("WAIT finite final reply", read_reply(twf), read_reply(owf))
            property_check("WAIT finite deadline fired", time.monotonic() - started >= 0.15, True)
        tw.close(); ow.close(); twf.close(); owf.close()

    final_t, final_o = info_blocked(ts, tf), info_blocked(os_, of)
    property_check("blocking gauges drain", final_t, final_o)
    property_check("target blocking gauge zero control", final_t, 0)
    property_check("oracle blocking gauge zero control", final_o, 0)
    if multi_ready_cases == 0:
        mismatch("multiple-ready-key coverage did not fire", b"0", b">0")
    ts.close(); os_.close(); tf.close(); of.close()
    print("  coverage: multi_ready_cases=%d timeout_zero_arms=%d fifo_waiters=%d" %
          (multi_ready_cases, len(forever_arms), len(waiters)))
    print("DIFFER blocking: %d logical ops, %d checks, %d diffs -> %s" %
          (logical_ops, checks, diffs, "PASS" if diffs == 0 else "FAIL"))
    return 1 if diffs else 0


if SUITE == "blocking":
    sys.exit(run_blocking_differ(rng))


def gen_edgeproto(rng):
    """Protocol / argument-surface campaign: error TEXT, arity, option grammar, null shapes.

    Everything here answers DETERMINISTICALLY on both servers, which is what makes byte-comparing
    an error stream meaningful. That rules three families out on purpose:
      * random-member replies (SPOP/SRANDMEMBER/ZRANDMEMBER/HRANDFIELD with a count, SCAN listing
        order, RANDOMKEY) -- unordered by contract;
      * wall-clock replies (auto stream IDs, TTL seconds, TIME, OBJECT IDLETIME);
      * the divergences NOTES-EDGEPROTO.md records as deliberate or shelved (SCAN's lax cursor,
        COPY DB on a one-database server, redis's bare-strtod score ranges, SORT BY/GET, the
        grisu2-vs-shortest last digit). A suite that carried those could never reach zero.
    The tail is a random resample of the ERROR-only cases, which are idempotent, so a longer run
    is a longer run and not a different test.
    """
    setup = [
        ["SET", "ep:s", "hello world"], ["SET", "ep:s2", "hello there"], ["SET", "ep:n", "12345"],
        ["HSET", "ep:h", "f", "1", "g", "2"],
        ["RPUSH", "ep:l", "a", "b", "c", "a"],
        ["SADD", "ep:set", "a", "b", "c"], ["SADD", "ep:set2", "b", "c", "d"],
        ["ZADD", "ep:z", "1", "a", "2", "b", "3", "c"],
        ["XADD", "ep:x", "1-1", "f", "v"], ["XADD", "ep:x", "2-1", "f", "v"],
        ["GEOADD", "ep:g", "13.361389", "38.115556", "P"],
        ["GEOADD", "ep:g", "15.087269", "37.502669", "C"],
        ["PFADD", "ep:hll", "a", "b", "c"],
    ]
    errors = []

    # 1. Canonical decimal. Redis's string2ll takes ONE spelling of a number; every other one is
    #    an integer error, and which error depends on the argument's name.
    for bad in ("05", "+5", " 5", "5 ", "-0", "\t5", "1e3", "0x10", "3.", "+0", "", "abc",
                "9223372036854775808", "-9223372036854775809", "--1", ".5", "5\n"):
        errors += [
            ["LPOP", "ep:l", bad], ["RPOP", "ep:l", bad], ["LINDEX", "ep:l", bad],
            ["LRANGE", "ep:l", bad, "-1"], ["LRANGE", "ep:l", "0", bad],
            ["LSET", "ep:l", bad, "z"], ["LREM", "ep:l", bad, "a"],
            ["LTRIM", "ep:l", bad, "-1"], ["LINSERT", "ep:l", "BEFORE", "a", bad],
            ["LPOS", "ep:l", "a", "RANK", bad], ["LPOS", "ep:l", "a", "COUNT", bad],
            ["LPOS", "ep:l", "a", "MAXLEN", bad],
            ["SINTERCARD", bad, "ep:set"], ["SINTERCARD", "1", "ep:set", "LIMIT", bad],
            ["ZINTERCARD", "1", "ep:z", "LIMIT", bad], ["ZUNIONSTORE", "ep:d", bad, "ep:z"],
            ["ZREMRANGEBYRANK", "ep:z", bad, "-1"], ["ZRANGE", "ep:z", bad, "-1"],
            ["ZRANGESTORE", "ep:d", "ep:z", "(1", "+inf", "BYSCORE", "LIMIT", "0", bad],
            ["XRANGE", "ep:x", "-", "+", "COUNT", bad],
            ["XREVRANGE", "ep:x", "+", "-", "COUNT", bad],
            ["XTRIM", "ep:x", "MAXLEN", bad], ["XAUTOCLAIM", "ep:x", "g", "c", bad, "0"],
            ["SETRANGE", "ep:s", bad, "x"], ["GETRANGE", "ep:s", bad, "-1"],
            ["SETBIT", "ep:bit", bad, "1"], ["GETBIT", "ep:s", bad],
            ["BITCOUNT", "ep:s", bad, "-1"], ["BITPOS", "ep:s", bad],
            ["INCRBY", "ep:n", bad], ["DECRBY", "ep:n", bad], ["HINCRBY", "ep:h", "f", bad],
            ["SETEX", "ep:e", bad, "v"], ["PSETEX", "ep:e", bad, "v"],
            ["SET", "ep:e", "v", "EX", bad], ["SET", "ep:e", "v", "PX", bad],
            ["SET", "ep:e", "v", "EXAT", bad], ["GETEX", "ep:s", "EX", bad],
            ["EXPIRE", "ep:s", bad], ["PEXPIRE", "ep:s", bad],
            ["EXPIREAT", "ep:s", bad], ["PEXPIREAT", "ep:s", bad],
            ["WAIT", bad, "1"], ["WAIT", "0", bad], ["SLOWLOG", "GET", bad],
            ["LCS", "ep:s", "ep:s2", "MINMATCHLEN", bad],
            ["HRANDFIELD", "ep:h", bad], ["SRANDMEMBER", "ep:set", bad],
            ["ZRANDMEMBER", "ep:z", bad], ["SPOP", "ep:set", bad],
            ["ZPOPMIN", "ep:z", bad], ["ZPOPMAX", "ep:z", bad],
            ["RESTORE", "ep:r", bad, "payload"], ["COPY", "ep:s", "ep:cp", "DB", bad],
            ["SCAN", "0", "COUNT", bad], ["HSCAN", "ep:h", "0", "COUNT", bad],
            ["SSCAN", "ep:set", "0", "COUNT", bad], ["ZSCAN", "ep:z", "0", "COUNT", bad],
            ["BITFIELD", "ep:bf", "GET", "u8", bad], ["BITFIELD", "ep:bf", "SET", "u8", "0", bad],
        ]
    # The out-of-range ends of the same arguments, where redis names the bounds it enforces.
    errors += [
        ["SRANDMEMBER", "ep:set", "-9223372036854775808"],
        ["ZRANDMEMBER", "ep:z", "-9223372036854775808"],
        ["HRANDFIELD", "ep:h", "-9223372036854775808"],
        ["LPOS", "ep:l", "a", "RANK", "-9223372036854775808"],
        ["LPOP", "ep:l", "-1"], ["SPOP", "ep:set", "-1"], ["ZPOPMIN", "ep:z", "-1"],
        ["SINTERCARD", "0", "ep:set"], ["SINTERCARD", "-1", "ep:set"],
        ["SINTERCARD", "9", "ep:set"], ["SINTERCARD", "1", "ep:set", "LIMIT", "-1"],
        ["ZINTERCARD", "1", "ep:z", "LIMIT", "-1"],
        ["XTRIM", "ep:x", "MAXLEN", "-1"], ["XTRIM", "ep:x", "MAXLEN", "~", "1", "LIMIT", "-1"],
        ["XSETID", "ep:x", "5-1", "ENTRIESADDED", "-1"],
        ["SETEX", "ep:e", "0", "v"], ["SETEX", "ep:e", "-1", "v"], ["PSETEX", "ep:e", "0", "v"],
        ["SET", "ep:e", "v", "EX", "0"], ["GETEX", "ep:s", "EX", "0"],
        ["WAIT", "0", "9223372036854775807"],
        ["LCS", "ep:s", "ep:s2", "MINMATCHLEN", "9223372036854775808"],
        ["COPY", "ep:s", "ep:cp", "DB", "2147483648"],
    ]

    # 2. Arity, at the container and at the subcommand.
    errors += [
        ["GET"], ["GET", "a", "b"], ["SET", "k"], ["HSET", "ep:h", "f", "1", "g"],
        ["HMSET", "ep:h", "f", "1", "g"], ["HSETNX", "ep:h", "f", "1", "2"],
        ["MSET", "a", "1", "b"], ["MSETNX", "a", "1", "b"], ["ZADD", "ep:z", "1", "a", "2"],
        ["PFADD"], ["GEOPOS"], ["GEOHASH"], ["EXISTS"], ["DEL"], ["MGET"],
        ["XGROUP", "CREATE", "ep:x", "g"], ["XGROUP", "DESTROY", "ep:x", "g", "extra"],
        ["XGROUP", "CREATECONSUMER", "ep:x", "g"],
        ["XGROUP", "CREATECONSUMER", "ep:x", "g", "c", "extra"],
        ["XGROUP", "DELCONSUMER", "ep:x", "g"], ["XGROUP", "SETID", "ep:x", "g"],
        ["XINFO", "CONSUMERS", "ep:x"], ["XINFO", "CONSUMERS", "ep:x", "g", "extra"],
        ["XINFO", "GROUPS", "ep:x", "extra"], ["OBJECT", "ENCODING"],
        ["BITCOUNT", "ep:s", "0"], ["BITCOUNT", "ep:s", "0", "1", "BYTE", "extra"],
        ["SETRANGE", "ep:s", "0", "x", "extra"], ["TYPE", "ep:s", "extra"],
        ["NOSUCHCOMMAND"], ["NOSUCHCOMMAND", "a", "b"], ["nosuchcommand", "x"],
        ["", "a"],
    ]

    # 3. Option grammar: unknown words, repeats, exclusions, case, missing operands.
    errors += [
        ["SET", "ep:o", "v", "NX", "XX"], ["SET", "ep:o", "v", "EX", "1", "PX", "1"],
        ["SET", "ep:o", "v", "UNKNOWN"], ["SET", "ep:o", "v", "EX"],
        ["GETEX", "ep:s", "EX"], ["GETEX", "ep:s", "NOPE"],
        ["EXPIRE", "ep:s", "100", "NX", "XX"], ["EXPIRE", "ep:s", "100", "GT", "LT"],
        ["EXPIRE", "ep:s", "100", "NX", "GT"], ["EXPIRE", "ep:s", "100", "NOPE"],
        ["EXPIRE", "ep:s", "100", "XX", "GT"], ["EXPIREAT", "ep:s", "1", "XX", "LT"],
        ["BITCOUNT", "ep:s", "0", "1", "wat"], ["BITPOS", "ep:s", "2"],
        ["BITFIELD", "ep:bf", "OVERFLOW"], ["BITFIELD", "ep:bf", "GET", "i0", "0"],
        ["BITFIELD", "ep:bf", "GET", "i65", "0"], ["BITFIELD_RO", "ep:bf", "SET", "u8", "0", "1"],
        ["BITOP", "NOPE", "ep:d", "ep:s"], ["BITOP", "NOT", "ep:d", "ep:s", "ep:s2"],
        ["LPOS", "ep:l", "a", "RANK", "0"], ["LPOS", "ep:l", "a", "NOPE", "1"],
        ["LINSERT", "ep:l", "MIDDLE", "a", "b"], ["LMPOP", "1", "ep:l", "MIDDLE"],
        ["LMPOP", "2", "ep:l", "LEFT"], ["LMOVE", "ep:l", "ep:l2", "LEFT", "MIDDLE"],
        ["ZADD", "ep:z", "NX", "XX", "1", "m"], ["ZADD", "ep:z", "GT", "NX", "1", "m"],
        ["ZADD", "ep:z", "INCR", "1", "a", "2", "b"], ["ZADD", "ep:z", "NOPE", "1", "a"],
        ["ZRANGE", "ep:z", "0", "-1", "LIMIT", "1", "2"],
        ["ZRANGE", "ep:z", "0", "-1", "LIMIT", "0", "-1"],
        ["ZRANGE", "ep:z", "0", "-1", "LIMIT", "5", "-1"],
        ["ZRANGESTORE", "ep:zd", "ep:z", "0", "-1", "LIMIT", "0", "-1"],
        ["ZRANGESTORE", "ep:zd", "ep:z", "0", "-1", "LIMIT", "1", "2"],
        ["ZRANGE", "ep:z", "-", "+", "BYLEX", "WITHSCORES"],
        ["ZRANGE", "ep:z", "-inf", "+inf", "BYSCORE", "LIMIT", "0"],
        ["ZRANGE", "ep:z", "0", "-1", "NOPE"], ["ZRANGEBYLEX", "ep:z", "a", "+"],
        ["ZUNIONSTORE", "ep:zd", "1", "ep:z", "AGGREGATE", "BOGUS"],
        ["ZUNIONSTORE", "ep:zd", "1", "ep:z", "WEIGHTS"],
        ["ZDIFF", "2", "ep:z", "ep:z", "WEIGHTS", "1", "1"],
        ["ZMPOP", "1", "ep:z", "MIN", "MAX"], ["ZADD", "ep:z", "INCR", "nan", "a"],
        ["ZINCRBY", "ep:z", "nan", "a"], ["ZADD", "ep:z", "notanumber", "m"],
        ["HRANDFIELD", "ep:h", "1", "WITHVALUES", "extra"], ["HSCAN", "ep:h", "0", "MATCH"],
        ["HSCAN", "ep:h", "0", "NOPE", "x"], ["SSCAN", "ep:set", "0", "NOVALUES"],
        ["SCAN", "0", "TYPE"], ["SCAN", "0", "NOPE", "x"], ["ZSCAN", "ep:z", "0", "MATCH"],
        ["GEOADD", "ep:g", "NX", "XX", "0", "0", "m"], ["GEOADD", "ep:g", "NOPE", "0", "0", "m"],
        ["GEODIST", "ep:g", "P", "C", "furlong"],
        ["GEOSEARCH", "ep:g", "FROMMEMBER", "P", "FROMLONLAT", "0", "0", "BYRADIUS", "1", "m"],
        ["GEOSEARCH", "ep:g", "FROMLONLAT", "0", "0", "BYRADIUS", "abc", "m"],
        ["GEOSEARCH", "ep:g", "FROMLONLAT", "0", "0", "BYBOX", "abc", "1", "m"],
        ["GEOSEARCH", "ep:g", "FROMLONLAT", "0", "0", "BYBOX", "1", "abc", "m"],
        ["GEOSEARCH", "ep:g", "FROMLONLAT", "0", "0", "BYRADIUS", "-1", "m"],
        ["GEOADD", "ep:ga", "1e100", "0", "m"], ["GEOADD", "ep:ga", "0", "1e100", "m"],
        ["GEOADD", "ep:ga", "181", "0", "m"], ["GEOADD", "ep:ga", "0", "86", "m"],
        ["GEORADIUS", "ep:g", "0", "0", "abc", "m"],
        ["GEORADIUSBYMEMBER", "ep:g", "P", "abc", "m"],
        ["XADD", "ep:x", "1-1", "f"], ["XADD", "ep:x", "NOPE", "3-1", "f", "v"],
        ["XTRIM", "ep:x", "NOPE", "1"], ["XTRIM", "ep:x", "MINID", "notanid"],
        ["XTRIM", "ep:x", "BOGUS", "1"], ["XRANGE", "ep:x", "(", "+"],
        ["XRANGE", "ep:x", "1-1-1", "+"], ["XSETID", "ep:x", "1-1", "NOPE", "1"],
        ["XREAD", "STREAMS", "ep:x", "ep:x", "0-0"], ["XREAD", "STREAMS", "ep:x"],
        ["XREAD", "NOPE", "STREAMS", "ep:x", "0-0"],
        ["SORT", "ep:l", "ASC", "DESC"], ["SORT", "ep:l", "LIMIT", "0"],
        ["SORT_RO", "ep:l", "STORE", "ep:sd"], ["SORT", "ep:l", "NOPE"],
        ["LCS", "ep:s", "ep:s2", "LEN", "IDX"], ["LCS", "ep:s", "ep:s2", "NOPE"],
        ["COPY", "ep:s", "ep:s"], ["RESTORE", "ep:rr", "0", "bad", "NOPE"],
        ["PFCOUNT", "ep:s"], ["PFMERGE", "ep:hd", "ep:hll", "ep:s"], ["PFADD", "ep:s", "x"],
    ]

    # 4. Wrong type against every family, and the order in which the two complaints come out.
    errors += [
        ["LPOP", "ep:s", "abc"], ["LINDEX", "ep:s", "abc"], ["LRANGE", "ep:s", "abc", "abc"],
        ["SPOP", "ep:s", "abc"], ["ZRANGE", "ep:s", "abc", "abc"], ["HINCRBY", "ep:s", "f", "abc"],
        ["HINCRBYFLOAT", "ep:s", "f", "abc"], ["XRANGE", "ep:s", "bad", "bad"],
        ["XADD", "ep:s", "MAXLEN", "abc", "*", "f", "v"], ["SETRANGE", "ep:l", "abc", "x"],
        ["SETBIT", "ep:l", "abc", "1"], ["GETEX", "ep:l", "EX", "abc"], ["EXPIRE", "ep:l", "abc"],
        ["SETEX", "ep:l", "abc", "v"], ["INCRBY", "ep:l", "abc"], ["APPEND", "ep:l", "x"],
        ["LPUSH", "ep:s", "x"], ["SADD", "ep:s", "x"], ["ZADD", "ep:s", "1", "x"],
        ["XADD", "ep:s", "1-1", "f", "v"], ["GETSET", "ep:l", "x"], ["SMOVE", "ep:s", "ep:set", "a"],
        ["SMOVE", "ep:set", "ep:s", "a"], ["RENAME", "ep:nokey", "ep:s"],
        ["ZRANGESTORE", "ep:d", "ep:nokey", "abc", "abc"], ["GEOPOS", "ep:s", "m"],
        ["PFCOUNT", "ep:s", "ep:hll"], ["INCR", "ep:s"], ["INCRBYFLOAT", "ep:s", "1"],
    ]

    # 5. Null and empty shapes. RESP2 tells a null ARRAY from a null BULK; RESP3 spells both "_".
    shapes = [
        ["GET", "ep:miss"], ["GETDEL", "ep:miss"], ["GETEX", "ep:miss"], ["DUMP", "ep:miss"],
        ["HGET", "ep:h", "nope"], ["HMGET", "ep:miss", "a", "b"], ["HGETALL", "ep:miss"],
        ["LPOP", "ep:miss"], ["LPOP", "ep:miss", "2"], ["LINDEX", "ep:l", "99"],
        ["LPOS", "ep:l", "nope"], ["LPOS", "ep:l", "nope", "COUNT", "0"],
        ["RPOPLPUSH", "ep:miss", "ep:dst"], ["LMOVE", "ep:miss", "ep:dst", "LEFT", "RIGHT"],
        ["LMPOP", "1", "ep:miss", "LEFT"], ["BLPOP", "ep:miss", "0.01"],
        ["BRPOPLPUSH", "ep:miss", "ep:dst", "0.01"],
        ["BLMOVE", "ep:miss", "ep:dst", "LEFT", "RIGHT", "0.01"],
        ["BLMPOP", "0.01", "1", "ep:miss", "LEFT"], ["BZPOPMIN", "ep:miss", "0.01"],
        ["BZMPOP", "0.01", "1", "ep:miss", "MIN"],
        ["ZSCORE", "ep:z", "nope"], ["ZMSCORE", "ep:z", "a", "nope"], ["ZRANK", "ep:z", "nope"],
        ["ZRANK", "ep:z", "nope", "WITHSCORE"], ["ZRANK", "ep:z", "a", "WITHSCORE"],
        ["ZREVRANK", "ep:z", "nope", "WITHSCORE"], ["ZADD", "ep:z", "XX", "INCR", "1", "nope"],
        ["ZPOPMIN", "ep:miss"], ["ZPOPMIN", "ep:miss", "2"], ["ZMPOP", "1", "ep:miss", "MIN"],
        ["ZRANGE", "ep:miss", "0", "-1"], ["ZRANGE", "ep:miss", "0", "-1", "WITHSCORES"],
        ["SMEMBERS", "ep:miss"], ["SINTER", "ep:miss"], ["SINTERCARD", "1", "ep:miss"],
        ["XRANGE", "ep:miss", "-", "+"], ["XRANGE", "ep:x", "-", "+", "COUNT", "-1"],
        ["XREVRANGE", "ep:x", "+", "-", "COUNT", "-1"], ["XRANGE", "ep:x", "-", "+", "COUNT", "0"],
        ["XREAD", "COUNT", "1", "STREAMS", "ep:miss", "0"],
        ["GEOPOS", "ep:g", "nope"], ["GEODIST", "ep:g", "P", "nope"], ["GEOHASH", "ep:g", "nope"],
        ["GEOPOS", "ep:g"], ["GEOHASH", "ep:g"],
        ["MGET", "ep:miss", "ep:s"], ["LRANGE", "ep:miss", "0", "-1"], ["TYPE", "ep:miss"],
        ["GETRANGE", "ep:miss", "0", "-1"], ["SUBSTR", "ep:miss", "0", "-1"],
        ["ZRANGEBYSCORE", "ep:miss", "-inf", "+inf"], ["ZRANGEBYLEX", "ep:miss", "-", "+"],
        ["LCS", "ep:s", "ep:miss"], ["LCS", "ep:s", "ep:miss", "IDX"],
    ]

    # 6. Empty and binary-unsafe names and values through every write/read path.
    binary_key = b"ep:\x00key\r\n with space\xff"
    binary_value = b"\x00value\r\n\xfftail"
    binary = [
        ["SET", b"", b""], ["GET", b""], ["STRLEN", b""], ["APPEND", b"", b"x"], ["GET", b""],
        ["SET", binary_key, binary_value], ["GET", binary_key], ["STRLEN", binary_key],
        ["APPEND", binary_key, b"\x00\r\n"], ["GETRANGE", binary_key, "0", "-1"],
        ["SETRANGE", binary_key, "2", b"\x01\x02"], ["GET", binary_key], ["TYPE", binary_key],
        ["COPY", binary_key, b"ep:copy\x00"], ["GET", b"ep:copy\x00"],
        ["HSET", binary_key, b"\x00f\r\n", binary_value], ["HGET", binary_key, b"\x00f\r\n"],
        ["HSET", binary_key, b"", b""], ["HGET", binary_key, b""], ["HSTRLEN", binary_key, b""],
        ["RPUSH", b"ep:bl", binary_value, b""], ["LRANGE", b"ep:bl", "0", "-1"],
        ["LPOS", b"ep:bl", b""], ["LINSERT", b"ep:bl", "BEFORE", b"", b"\xfe"],
        ["SADD", b"ep:bs", binary_value, b""], ["SISMEMBER", b"ep:bs", binary_value],
        ["SCARD", b"ep:bs"], ["ZADD", b"ep:bz", "1", binary_value, "2", b""],
        ["ZSCORE", b"ep:bz", binary_value], ["ZRANK", b"ep:bz", b""],
        ["ZRANGEBYLEX", b"ep:bz", "-", "+"],
        ["XADD", b"ep:bx", "1-1", b"\x00f", binary_value], ["XRANGE", b"ep:bx", "-", "+"],
        ["SET", b"ep:" + b"k" * 8000, binary_value], ["GET", b"ep:" + b"k" * 8000],
        ["SET", b"ep:nul", b"a\x00b"], ["INCR", b"ep:nul"], ["STRLEN", b"ep:nul"],
        ["SET", b"ep:sp", b" 1"], ["INCR", b"ep:sp"], ["SET", b"ep:lz", b"01"], ["INCR", b"ep:lz"],
        ["SET", b"ep:pl", b"+1"], ["INCR", b"ep:pl"], ["SET", b"ep:nz", b"-0"], ["INCR", b"ep:nz"],
        ["ECHO", binary_value], ["ECHO", b""],
        ["SETRANGE", b"ep:huge", "536870911", "x"], ["SETBIT", b"ep:hb", "4294967296", "1"],
        ["GETRANGE", binary_key, "-9223372036854775808", "9223372036854775807"],
    ]

    # 7. Doubles whose text the two servers agree on: the integral fast path, the fixed/scientific
    #    boundary, and the signed zero. (The last-digit cases where redis's grisu2 and a shortest
    #    round-trip disagree are recorded in NOTES-EDGEPROTO.md, not tested here.)
    doubles = []
    for value in ("0", "-0", "1.5", "0.1", "3", "1e15", "1e16", "1e17", "1e18", "4e18",
                  "4611686018427387904", "5e18", "9.5e18", "1e19", "1e20", "1e22",
                  "9223372036854775808", "-9223372036854775809", "9007199254740993",
                  "1e-5", "1e-6", "1e-7", "1.2345e-8", "0.0001", "5e-324",
                  "1.7976931348623157e308", "123456789012345678", "0.015", "-0.015",
                  "inf", "-inf", "+inf"):
        doubles += [["DEL", "ep:dz"], ["ZADD", "ep:dz", "INCR", value, "m"],
                    ["ZSCORE", "ep:dz", "m"], ["ZRANGE", "ep:dz", "0", "-1", "WITHSCORES"],
                    ["ZINCRBY", "ep:dz", "0", "m"]]

    ops = list(setup) + errors + shapes + binary + doubles
    # The tail is drawn from the ERROR cases only: they leave no state behind, so resampling them
    # lengthens the run without making it a different (or an order-dependent) test.
    while len(ops) < 5200:
        ops.append(rng.choice(errors))
    return ops


def gen_sort(rng):
    """SORT with BY / GET dereference patterns.

    The full differential gate runs the target at --shards 16 --ratio 6:2. Before sending the
    generated stream, the runner below walks DEBUG SHARD candidates and fails unless concrete BY
    and GET keys from generated commands live on an executor other than their source's.

    TWO REPLY SHAPES ARE DELIBERATELY EXCLUDED because the reference itself does not define them:
      * `SORT <set> BY <globless>` WITHOUT STORE -- the reference dumps the set's own iteration
        order, which is an encoding artefact. Only the STORE form is generated, where the reference
        forces an alphabetic sort precisely to make it defined.
      * an alphabetic tie -- the reference leaves equal ALPHA weights wherever its sort algorithm
        put them and neither server promises stability, so every weight generated here is distinct.

    BOOT THE ORACLE UNDER ``LC_ALL=C``. Redis calls ``setlocale(LC_COLLATE, "")`` and then
    ``strcoll()`` for ALPHA comparisons; this server never calls setlocale, so its strcoll is byte
    order. Under en_US.UTF-8 the oracle orders ``1`` before ``-3`` where a byte comparison puts
    ``-3`` first, and every punctuation-bearing ALPHA row diffs for that reason alone. In the C
    locale strcoll IS the byte order and the two sides are comparing the same relation. (Whether
    this server SHOULD collate by locale is a separate compatibility question -- see NOTES-SORT.md.)
    """
    # Deliberately mixed case and punctuation: with the oracle in the C locale this exercises the
    # ALPHA comparison itself, which is where a locale difference would show up first.
    words = ["alpha", "Bravo", "char-lie", "delta", "Echo!", "fox_trot", "golf", "HOTEL"]
    ops = [["FLUSHALL"]]
    sources = ["so:l%d" % i for i in range(4)] + ["so:s%d" % i for i in range(3)] + \
              ["so:z%d" % i for i in range(3)]
    dests = ["so:dst%d" % i for i in range(6)]

    def rebuild(key):
        out = [["DEL", key]]
        members = rng.sample(range(100), rng.randint(1, 9))
        if key.startswith("so:l"):
            out.append(["RPUSH", key] + [str(m) for m in members])
        elif key.startswith("so:s"):
            out.append(["SADD", key] + [str(m) for m in members])
        else:
            out.append(["ZADD", key] + sum([[str(i * 7 + 1), str(m)]
                                            for i, m in enumerate(members)], []))
        # Weight keys: one per possible member, some absent, some a hash, some non-numeric.
        for m in members:
            roll = rng.randrange(10)
            if roll < 5:
                out.append(["SET", "so:w_%d" % m, str(rng.randint(-999, 999))])
            elif roll < 6:
                out.append(["DEL", "so:w_%d" % m])
            elif roll < 7:
                out.append(["SET", "so:w_%d" % m, rng.choice(words)])
            else:
                out.append(["DEL", "so:w_%d" % m])
                out.append(["HSET", "so:h_%d" % m, "f", str(rng.randint(-999, 999)),
                            "g", rng.choice(words)])
            if rng.random() < 0.7:
                out.append(["SET", "so:d_%d" % m, "v%d" % m])
            else:
                out.append(["DEL", "so:d_%d" % m])
        return out

    for key in sources:
        ops += rebuild(key)

    # Distinct alphabetic weights: one word per member id, so no ALPHA tie is ever produced.
    for m in range(100):
        ops.append(["SET", "so:a_%d" % m, "%s%03d" % (words[m % len(words)], m)])

    limits = [[], ["LIMIT", "0", "3"], ["LIMIT", "2", "2"], ["LIMIT", "-1", "-1"],
              ["LIMIT", "1", "100"]]
    gets = [[], ["GET", "#"], ["GET", "so:d_*"], ["GET", "#", "GET", "so:d_*"],
            ["GET", "so:h_*->g"], ["GET", "so:d_*", "GET", "so:h_*->f", "GET", "#"],
            ["GET", "so:absent_*"], ["GET", "literal"], ["GET", "##"]]

    while len(ops) < 4400:
        roll = rng.randrange(24)
        if roll == 0:
            ops += rebuild(rng.choice(sources))
            continue
        key = rng.choice(sources)
        is_set = key.startswith("so:s")
        cmd = ["SORT_RO" if rng.random() < 0.2 else "SORT", key]
        by = rng.randrange(8)
        alpha = False
        if by == 0:
            pass                                          # no BY: sort the elements themselves
        elif by == 1:
            cmd += ["BY", "so:w_*"]
        elif by == 2:
            cmd += ["BY", "so:h_*->f"]
        elif by == 3:
            cmd += ["BY", "so:absent_*"]
        elif by == 4:
            cmd += ["BY", "so:a_*"]
            alpha = True
        elif by == 5:
            cmd += ["BY", "so:h_*"]                       # a hash with no field: NULL weight
        elif by == 6:
            cmd += ["BY", rng.choice(["nosort", "constant", "?_x", "[ab]_x"])]
        else:
            cmd += ["BY", "so:w*_x"]                      # substituted key that never exists
        dontsort = by == 6
        if alpha or (rng.random() < 0.35 and by in (0, 4)):
            cmd.append("ALPHA")
            alpha = True
        if rng.random() < 0.4:
            cmd.append(rng.choice(["ASC", "DESC"]))
        cmd += rng.choice(limits)
        cmd += rng.choice(gets)
        store = rng.random() < 0.3 and cmd[0] == "SORT"
        # A set with ordering suppressed is only defined through STORE (see the docstring).
        if dontsort and is_set and not store:
            store = True
        if store:
            cmd += ["STORE", rng.choice(dests)]
        ops.append(cmd)
        if store:
            ops.append(["LRANGE", cmd[-1], "0", "-1"])
            ops.append(["TYPE", cmd[-1]])

    # Directed tail: every rule this lane implements, pinned in one place.
    ops += [
        ["DEL", "so:t"], ["RPUSH", "so:t", "3", "1", "2"],
        ["MSET", "so:tw_1", "10", "so:tw_2", "5", "so:tw_3", "1"],
        ["MSET", "so:td_1", "one", "so:td_2", "two", "so:td_3", "three"],
        ["HSET", "so:th_1", "f", "100", "g", "aaa"],
        ["HSET", "so:th_2", "f", "50", "g", "bbb"],
        ["HSET", "so:th_3", "f", "25", "g", "ccc"],
        ["MSET", "so:tn_1", "notanumber", "so:tn_2", "2", "so:tn_3", "3"],
        ["SET", "so:tp_2", "5"],
        ["MSET", "so:tx->f1", "5", "so:tx->f2", "1", "so:tx->f3", "3"],
        ["SET", "so:th_1->", "vv"],
        ["MSET", "so:tw\\1x", "100", "so:tw\\2x", "50", "so:tw\\3x", "25"],
        ["MSET", "so:tw3x", "1", "so:tw1x", "2", "so:tw2x", "3"],
        ["SORT", "so:t", "BY", "so:tw_*"],
        ["SORT", "so:t", "BY", "so:tw_*", "DESC"],
        ["SORT", "so:t", "BY", "so:tw_*", "GET", "#"],
        ["SORT", "so:t", "BY", "so:tw_*", "GET", "so:td_*"],
        ["SORT", "so:t", "BY", "so:tw_*", "GET", "#", "GET", "so:td_*", "GET", "so:th_*->g"],
        ["SORT", "so:t", "BY", "so:th_*->f"],
        ["SORT", "so:t", "BY", "so:th_*->"],
        ["SORT", "so:t", "BY", "so:tx->f*"],
        ["SORT", "so:t", "BY", "so:tw\\*x"],
        ["SORT", "so:t", "BY", "so:tw*x"],
        ["SORT", "so:t", "BY", "so:tn_*"],
        ["SORT", "so:t", "BY", "so:tn_*", "LIMIT", "0", "1"],
        ["SORT", "so:t", "BY", "so:tp_*"],
        ["SORT", "so:t", "BY", "so:th_*"],
        ["SORT", "so:t", "BY", "so:tmissing_*"],
        ["SORT", "so:t", "BY", "nosort"],
        ["SORT", "so:t", "BY", "nosort", "DESC"],
        ["SORT", "so:t", "BY", "nosort", "LIMIT", "1", "1"],
        ["SORT", "so:t", "BY", "nosort", "GET", "#", "GET", "so:td_*"],
        ["SORT", "so:t", "GET", "#"], ["SORT", "so:t", "GET", "so:tmissing_*"],
        ["SORT", "so:t", "GET", "literal"], ["SORT", "so:t", "GET", "##"],
        ["SORT_RO", "so:t", "BY", "so:tw_*", "GET", "so:td_*"],
        ["SORT_RO", "so:t", "BY", "so:tw_*", "STORE", "so:tno"],
        ["SORT", "so:t", "BY", "so:tw_*", "GET", "so:td_*", "STORE", "so:tdst"],
        ["LRANGE", "so:tdst", "0", "-1"], ["TYPE", "so:tdst"],
        ["SORT", "so:t", "GET", "so:tmissing_*", "STORE", "so:tdst2"],
        ["LRANGE", "so:tdst2", "0", "-1"],
        ["SET", "so:tkeep", "v"],
        ["SORT", "so:tabsent", "BY", "so:tw_*", "STORE", "so:tkeep"],
        ["EXISTS", "so:tkeep"],
        ["SORT", "so:tabsent", "BY", "so:tw_*", "GET", "so:td_*"],
        ["SORT", "so:tabsent", "BY", "nosort"],
        ["DEL", "so:tset"], ["SADD", "so:tset", "ddd", "aaa", "ccc", "bbb"],
        ["SORT", "so:tset", "BY", "nosort", "STORE", "so:tsd"],
        ["LRANGE", "so:tsd", "0", "-1"],
        ["SORT", "so:tset", "BY", "nosort", "ALPHA", "STORE", "so:tsd2"],
        ["LRANGE", "so:tsd2", "0", "-1"],
        ["SORT", "so:tset", "ALPHA"],
        ["DEL", "so:tz"], ["ZADD", "so:tz", "30", "aaa", "10", "bbb", "20", "ccc"],
        ["SORT", "so:tz", "BY", "nosort"],
        ["SORT", "so:tz", "BY", "nosort", "DESC"],
        ["SORT", "so:tz", "BY", "nosort", "STORE", "so:tzd"],
        ["LRANGE", "so:tzd", "0", "-1"],
        ["SORT", "so:tz", "ALPHA"],
        ["SORT", "so:tz"],
        ["SORT", "so:th_1", "BY", "nosort"],
        ["SORT", "so:th_1", "BY", "so:tw_*"],
        ["SORT", "so:th_1", "GET", "so:td_*"],
        ["SET", "so:tstr", "x"], ["SORT", "so:tstr", "BY", "so:tw_*", "GET", "#"],
        # Inside a transaction the dereference runs under the group's own read context.
        ["MULTI"],
        ["MSET", "so:tw_1", "40", "so:tw_2", "20", "so:tw_3", "60"],
        ["SORT", "so:t", "BY", "so:tw_*", "GET", "#", "GET", "so:td_*"],
        ["SORT", "so:t", "BY", "so:tw_*", "STORE", "so:tmdst"],
        ["EXEC"],
        ["LRANGE", "so:tmdst", "0", "-1"],
        ["SORT", "so:t", "BY", "so:tw_*"],
        # Read-your-own-writes: the weights are written by the command immediately before.
        ["MSET", "so:tw_1", "7", "so:tw_2", "9", "so:tw_3", "8"],
        ["SORT", "so:t", "BY", "so:tw_*", "GET", "#", "GET", "so:tw_*"],
        ["MSET", "so:tw_1", "9", "so:tw_2", "8", "so:tw_3", "7"],
        ["SORT", "so:t", "BY", "so:tw_*", "GET", "so:tw_*"],
        ["SORT", "so:t", "BY"], ["SORT", "so:t", "GET"],
        ["SORT", "so:t", "BADTOKEN", "BY", "so:tw_*"],
        ["SORT", "so:t", "BY", "so:tw_*", "GET", "#", "BY", "so:tn_*"],
        # ALPHA collation: punctuation and case, with and without STORE (the reference uses a byte
        # comparison for STORE and strcoll otherwise; in the C locale the two agree).
        ["DEL", "so:tc"], ["RPUSH", "so:tc", "a-b", "ab", "aB", "A", "_z", "z_"],
        ["SORT", "so:tc", "ALPHA"], ["SORT", "so:tc", "ALPHA", "DESC"],
        ["SORT", "so:tc", "ALPHA", "STORE", "so:tcd"], ["LRANGE", "so:tcd", "0", "-1"],
        ["MSET", "so:tca_1", "a-b", "so:tca_2", "aB", "so:tca_3", "_z"],
        ["SORT", "so:t", "BY", "so:tca_*", "ALPHA"],
        ["SORT", "so:t", "BY", "so:tca_*", "ALPHA", "DESC"],
        ["SORT", "so:t", "BY", "so:tca_*", "ALPHA", "STORE", "so:tcad"],
        ["LRANGE", "so:tcad", "0", "-1"],
        ["COMMAND", "GETKEYS", "SORT", "so:t", "BY", "so:tw_*", "GET", "so:td_*",
         "STORE", "so:tdst"],
    ]
    return ops


def gen_cmdgap(rng):
    """Small local additions found by the live COMMAND LIST inventory.

    The payload is a Redis 7.4 runtime-observed DUMP of the binary string
    ``cmdgap-value\\x00binary``. A valid payload makes RESTORE-ASKING coverage non-vacuous: the
    following GET byte-compares the installed value, while invalid payload/TTL/option rows pin the
    shared RESTORE validation surface. The three cluster-mode connection commands must produce
    Redis's standalone error, not merely become registered names.
    """
    payload = bytes.fromhex("0013636d646761702d76616c75650062696e6172790c003a0bba99e588262a")
    keys = ["cmdgap:restore:%02d" % index for index in range(12)]
    ops = []
    for _ in range(4200):
        key = rng.choice(keys)
        choice = rng.randrange(12)
        if choice == 0:
            ops.append(["ASKING"])
        elif choice == 1:
            ops.append(["READONLY"])
        elif choice == 2:
            ops.append(["READWRITE"])
        elif choice == 3:
            command = rng.choice(["ASKING", "READONLY", "READWRITE"])
            ops.append([command, "extra"])
        elif choice == 4:
            ops.append(["RESTORE-ASKING", key, "0", payload, "REPLACE"])
        elif choice == 5:
            ops.append(["GET", key])
        elif choice == 6:
            ops.append(["RESTORE-ASKING", key, "0", payload])
        elif choice == 7:
            ops.append(["DEL", key])
        elif choice == 8:
            ops.append(["RESTORE-ASKING", key, rng.choice(["-1", "abc"]), payload])
        elif choice == 9:
            ops.append(["RESTORE-ASKING", key, "0", b"bad-payload", "REPLACE"])
        elif choice == 10:
            ops.append(["RESTORE-ASKING", key, "0", payload, "BOGUS"])
        else:
            ops.append(["CMDGAP-NOSUCH", key])

    # Directed tail proves every new handler fired and leaves a successful restore immediately
    # followed by its value oracle. The unknown command is the detector's zero/control leg.
    ops += [
        ["ASKING"], ["READONLY"], ["READWRITE"],
        ["ASKING", "extra"], ["READONLY", "extra"], ["READWRITE", "extra"],
        ["DEL", "cmdgap:directed"],
        ["RESTORE-ASKING", "cmdgap:directed", "0", payload],
        ["GET", "cmdgap:directed"],
        ["RESTORE-ASKING", "cmdgap:directed", "0", payload],
        ["RESTORE-ASKING", "cmdgap:directed", "0", payload, "REPLACE"],
        ["GET", "cmdgap:directed"],
        ["CMDGAP-NOSUCH"],
    ]
    return ops


def gen_cmdgap2(rng):
    """Byte-differ the one self-contained command from the second live inventory lane.

    The ordinary HLL generator already pins Redis-compatible HYLL bytes. This stream exercises the
    debugging views of those bytes, including sparse opcode text, the 16,384-integer register
    array, and TODENSE's owner-local rewrite. Invalid and non-HLL strings distinguish header checks
    from corrupt sparse-stream checks; repeated GETs prove that TODENSE changed real stored bytes.
    """
    keys = ["cmdgap2:hll:%02d" % index for index in range(12)]
    elements = ["e:%04d" % index for index in range(256)] + ["", "nul\x00byte", "\xffbinary"]
    wrong = "cmdgap2:wrong"
    bad = "cmdgap2:bad"
    corrupt_key = "cmdgap2:corrupt"
    truncated_key = "cmdgap2:truncated-xzero"
    stable = "cmdgap2:stable"
    corrupt = b"HYLL\x01\x00\x00\x00" + b"\x00" * 7 + b"\x80" + b"\x00"
    truncated_xzero = b"HYLL\x01" + b"\x00" * 10 + b"\x80\x40"
    # Valid sparse stream: ZERO(1), XZERO(16383). It pins lowercase z vs uppercase Z decoding.
    sparse_zero = b"HYLL\x01" + b"\x00" * 10 + b"\x80" + b"\x00\x7f\xfe"

    ops = [
        ["SET", wrong, "ordinary-string"],
        ["SET", bad, "not-a-hyperloglog"],
        ["SET", corrupt_key, corrupt],
        ["SET", truncated_key, truncated_xzero],
        ["SET", stable, sparse_zero],
    ]
    for _ in range(4200):
        key = rng.choice(keys)
        choice = rng.randrange(30)
        if choice < 7:
            chosen = [rng.choice(elements) for _ in range(rng.randrange(1, 5))]
            ops.append(["PFADD", key] + chosen)
        elif choice < 11:
            ops.append(["PFDEBUG", "ENCODING", key])
        elif choice < 14:
            ops.append(["PFDEBUG", "DECODE", key])
        elif choice < 17:
            ops.append(["PFDEBUG", "TODENSE", key])
        elif choice == 17:
            ops.append(["PFDEBUG", "GETREG", key])
        elif choice < 20:
            ops.append(["GET", key])
        elif choice == 20:
            ops.append(["DEL", key])
        elif choice == 21:
            ops.append(["PFCOUNT", key])
        elif choice == 22:
            ops.append(["PFDEBUG", "ENCODING", "cmdgap2:missing"])
        elif choice == 23:
            ops.append(["PFDEBUG", "ENCODING", wrong])
        elif choice == 24:
            ops.append(["PFDEBUG", "ENCODING", bad])
        elif choice == 25:
            ops.append(["PFDEBUG", rng.choice(["ENCODING", "DECODE", "GETREG", "TODENSE"]),
                        rng.choice([corrupt_key, truncated_key])])
        elif choice == 26:
            ops.append(["PFDEBUG", "NOPE", stable])
        elif choice == 27:
            ops.append(["PFDEBUG", "ENCODING"])
        elif choice == 28:
            ops.append(["PFDEBUG", "ENCODING", stable, "extra"])
        else:
            ops.append(["PFDEBUG", rng.choice(["ENCODING", "DECODE", "GETREG"]), stable])

    # Directed tail proves every handler branch fired. The successful sparse-to-dense operation is
    # bracketed by ENCODING and GET, so returning :0 without storing the converted image cannot pass.
    direct = "cmdgap2:directed"
    direct_getreg = "cmdgap2:directed:getreg"
    ops += [
        ["DEL", direct], ["PFADD", direct, "a", "b", "c"],
        ["PFDEBUG", "ENCODING", direct], ["PFDEBUG", "DECODE", direct],
        ["GET", direct],
        ["PFDEBUG", "TODENSE", direct], ["PFDEBUG", "ENCODING", direct],
        ["GET", direct], ["PFDEBUG", "GETREG", direct],
        ["PFDEBUG", "DECODE", direct], ["PFDEBUG", "TODENSE", direct],
        ["DEL", direct_getreg], ["PFADD", direct_getreg, "a", "b", "c"],
        ["PFDEBUG", "ENCODING", direct_getreg],
        ["PFDEBUG", "GETREG", direct_getreg],
        ["PFDEBUG", "ENCODING", direct_getreg], ["GET", direct_getreg],
        ["PFDEBUG", "NOPE", direct],
        ["PFDEBUG", "ENCODING", "cmdgap2:missing"],
        ["PFDEBUG", "ENCODING", wrong], ["PFDEBUG", "ENCODING", bad],
        ["PFDEBUG", "ENCODING", corrupt_key], ["PFDEBUG", "DECODE", corrupt_key],
        ["PFDEBUG", "GETREG", corrupt_key], ["PFDEBUG", "TODENSE", corrupt_key],
        ["PFDEBUG", "ENCODING", truncated_key], ["PFDEBUG", "DECODE", truncated_key],
        ["PFDEBUG", "GETREG", truncated_key], ["PFDEBUG", "TODENSE", truncated_key],
        ["SET", "cmdgap2:decode-zero", sparse_zero],
        ["PFDEBUG", "DECODE", "cmdgap2:decode-zero"],
        ["PFDEBUG", "GETREG", "cmdgap2:decode-zero"],
        ["PFDEBUG", "ENCODING", "cmdgap2:decode-zero"],
        ["PFDEBUG", "ENCODING"], ["PFDEBUG", "ENCODING", stable, "extra"],
        ["CMDGAP2-NOSUCH"],
    ]
    return ops

# Sharded pub/sub is stateful and has spontaneous delivery frames, so it cannot use the ordinary
# one-request/one-reply pipeline below. Keep its whole differential driver in one mergeable block.
def run_spubsub_differ(rng):
    import select

    tp, tpf = conn(TH, TP)
    op, opf = conn(OH, OP)
    tl, tlf = conn(TH, TP)
    ol, olf = conn(OH, OP)
    diffs = 0
    checks = 0

    def compare(label, target, oracle, unordered=False):
        nonlocal diffs, checks
        checks += 1
        if unordered:
            target = normalize("SMEMBERS", target)
            oracle = normalize("SMEMBERS", oracle)
        if target != oracle:
            diffs += 1
            if diffs <= 12:
                print("  DIFF %s\n    target: %r\n    oracle: %r" %
                      (label, target[:160], oracle[:160]))

    channels = ["spubsub:differ:%02d" % i for i in range(12)]
    subscribed = set(channels[:5])
    initial = ["SSUBSCRIBE"] + sorted(subscribed)
    tl.sendall(enc(initial)); ol.sendall(enc(initial))
    for index in range(len(subscribed)):
        compare("initial SSUBSCRIBE %d" % index, read_reply(tlf), read_reply(olf))

    # Randomized subscription changes, local receiver counts, deliveries, and introspection.
    for sequence in range(600):
        choice = rng.randrange(20)
        channel = rng.choice(channels)
        if choice < 12:
            message = "payload:%d:%d" % (sequence, rng.randrange(1000000))
            command = ["SPUBLISH", channel, message]
            tp.sendall(enc(command)); op.sendall(enc(command))
            target = read_reply(tpf); oracle = read_reply(opf)
            compare("SPUBLISH %d" % sequence, target, oracle)
            if channel in subscribed:
                compare("smessage %d" % sequence, read_reply(tlf), read_reply(olf))
        elif choice < 15:
            command = ["SSUBSCRIBE", channel]
            tl.sendall(enc(command)); ol.sendall(enc(command))
            compare("SSUBSCRIBE %d" % sequence, read_reply(tlf), read_reply(olf))
            subscribed.add(channel)
        elif choice < 18:
            command = ["SUNSUBSCRIBE", channel]
            tl.sendall(enc(command)); ol.sendall(enc(command))
            compare("SUNSUBSCRIBE %d" % sequence, read_reply(tlf), read_reply(olf))
            subscribed.discard(channel)
        elif choice == 18:
            names = [rng.choice(channels) for _ in range(3)]
            command = ["PUBSUB", "SHARDNUMSUB"] + names
            tp.sendall(enc(command)); op.sendall(enc(command))
            compare("SHARDNUMSUB %d" % sequence, read_reply(tpf), read_reply(opf))
        else:
            command = ["PUBSUB", "SHARDCHANNELS", "spubsub:differ:*"]
            tp.sendall(enc(command)); op.sendall(enc(command))
            compare("SHARDCHANNELS %d" % sequence, read_reply(tpf), read_reply(opf), True)

    # Same bytes in regular/shard/pattern namespaces must not cross in either direction.
    cross = channels[0]
    if cross not in subscribed:
        tl.sendall(enc(["SSUBSCRIBE", cross])); ol.sendall(enc(["SSUBSCRIBE", cross]))
        compare("cross SSUBSCRIBE", read_reply(tlf), read_reply(olf))
        subscribed.add(cross)
    tr, trf = conn(TH, TP); ore, oref = conn(OH, OP)
    tr.sendall(enc(["SUBSCRIBE", cross])); ore.sendall(enc(["SUBSCRIBE", cross]))
    compare("cross SUBSCRIBE", read_reply(trf), read_reply(oref))
    tp.sendall(enc(["SPUBLISH", cross, "shard-only"]))
    op.sendall(enc(["SPUBLISH", cross, "shard-only"]))
    compare("cross SPUBLISH", read_reply(tpf), read_reply(opf))
    compare("cross smessage", read_reply(tlf), read_reply(olf))
    target_ready = bool(select.select([tr], [], [], 0.15)[0])
    oracle_ready = bool(select.select([ore], [], [], 0.15)[0])
    compare("SPUBLISH regular silence", b":" + str(target_ready).encode(),
            b":" + str(oracle_ready).encode())
    if target_ready or oracle_ready:
        diffs += 1

    tp.sendall(enc(["PUBLISH", cross, "regular-only"]))
    op.sendall(enc(["PUBLISH", cross, "regular-only"]))
    compare("cross PUBLISH", read_reply(tpf), read_reply(opf))
    compare("cross message", read_reply(trf), read_reply(oref))
    target_ready = bool(select.select([tl], [], [], 0.15)[0])
    oracle_ready = bool(select.select([ol], [], [], 0.15)[0])
    compare("PUBLISH shard silence", b":" + str(target_ready).encode(),
            b":" + str(oracle_ready).encode())
    if target_ready or oracle_ready:
        diffs += 1

    pattern = "spubsub:differ:*"
    tpat, tpatf = conn(TH, TP); opat, opatf = conn(OH, OP)
    tpat.sendall(enc(["PSUBSCRIBE", pattern])); opat.sendall(enc(["PSUBSCRIBE", pattern]))
    compare("cross PSUBSCRIBE", read_reply(tpatf), read_reply(opatf))
    tp.sendall(enc(["SPUBLISH", cross, "no-pattern"]))
    op.sendall(enc(["SPUBLISH", cross, "no-pattern"]))
    compare("pattern SPUBLISH", read_reply(tpf), read_reply(opf))
    compare("pattern smessage", read_reply(tlf), read_reply(olf))
    target_ready = bool(select.select([tpat], [], [], 0.15)[0])
    oracle_ready = bool(select.select([opat], [], [], 0.15)[0])
    compare("SPUBLISH pattern silence", b":" + str(target_ready).encode(),
            b":" + str(oracle_ready).encode())
    if target_ready or oracle_ready:
        diffs += 1

    # No-argument unsubscribe order is dictionary-dependent; compare the frame multiset.
    tl.sendall(enc(["SUNSUBSCRIBE"])); ol.sendall(enc(["SUNSUBSCRIBE"]))
    frame_count = len(subscribed) if subscribed else 1
    target_frames = [parse_reply(read_reply(tlf)) for _ in range(frame_count)]
    oracle_frames = [parse_reply(read_reply(olf)) for _ in range(frame_count)]
    def normalize_unsubscribe_all(frames):
        labels = sorted(frame[0] for frame in frames)
        channels_seen = sorted(frame[1] if frame[1] is not None else b"<nil>" for frame in frames)
        counts = sorted(frame[2] for frame in frames)
        return repr((labels, channels_seen, counts)).encode()
    compare("SUNSUBSCRIBE all", normalize_unsubscribe_all(target_frames),
            normalize_unsubscribe_all(oracle_frames))

    for sock in (tp, op, tl, ol, tr, ore, tpat, opat): sock.close()
    print("DIFFER spubsub: %d checks, %d diffs -> %s" %
          (checks, diffs, "PASS" if diffs == 0 else "FAIL"))
    return 1 if diffs else 0


def run_pubsub_differ(rng):
    # Full pub/sub differential.  Subscription acknowledgements and spontaneous deliveries share
    # one connection, so every expected frame comes from a Python-side membership model.  select()
    # is used only for negative controls; it never decides how many delivery frames to consume.
    diffs = 0
    checks = 0
    logical_ops = 0

    def mismatch(label, target, oracle):
        nonlocal diffs
        diffs += 1
        if diffs <= 16:
            print("  DIFF %s\n    target: %r\n    oracle: %r" %
                  (label, target[:320] if isinstance(target, bytes) else target,
                   oracle[:320] if isinstance(oracle, bytes) else oracle), flush=True)

    def compare(label, target, oracle, unordered=False):
        nonlocal checks
        checks += 1
        if unordered:
            target = normalize("SMEMBERS", target)
            oracle = normalize("SMEMBERS", oracle)
        if target != oracle:
            mismatch(label, target, oracle)
        return target, oracle

    def property_check(label, target, oracle):
        nonlocal checks
        checks += 1
        if target != oracle:
            mismatch(label, repr(target).encode(), repr(oracle).encode())

    def open_pair(resp3=False):
        t, tf = conn_mode(TH, TP, resp3)
        o, of = conn_mode(OH, OP, resp3)
        return t, tf, o, of

    def close_pair(pair):
        t, tf, o, of = pair
        t.close(); o.close(); tf.close(); of.close()

    def command_pair(pair, argv, label=None, unordered=False):
        nonlocal logical_ops
        t, tf, o, of = pair
        payload = enc(argv)
        t.sendall(payload); o.sendall(payload)
        logical_ops += 1
        return compare(label or " ".join(argv[:4]), read_reply(tf), read_reply(of), unordered)

    def raw_int(reply):
        try:
            return int(reply[1:-2]) if reply[:1] == b":" else None
        except ValueError:
            return None

    def parsed_count(reply):
        parsed = parse_reply(reply)
        if not isinstance(parsed, list) or len(parsed) != 3 or not isinstance(parsed[2], bytes):
            return None
        try:
            return int(parsed[2][1:]) if parsed[2][:1] == b":" else None
        except ValueError:
            return None

    def canonical_frames(frames):
        return repr(sorted((parse_reply(frame) for frame in frames), key=repr)).encode()

    def canonical_unsubscribe(frames):
        parsed = [parse_reply(frame) for frame in frames]
        labels = sorted(frame[0] for frame in parsed)
        names = sorted(frame[1] if frame[1] is not None else b"<nil>" for frame in parsed)
        counts = sorted(frame[2] for frame in parsed)
        return repr((labels, names, counts)).encode()

    # Unsubscribing from nothing has a real confirmation frame (including a null channel), under
    # both RESP2 array and RESP3 push framing.  The parsed shape assertion is the non-vacuous arm.
    for resp3 in (False, True):
        pair = open_pair(resp3)
        for verb in ("UNSUBSCRIBE", "PUNSUBSCRIBE", "SUNSUBSCRIBE"):
            target, oracle = command_pair(pair, [verb], "%s empty RESP%d" %
                                          (verb, 3 if resp3 else 2))
            expected = [verb.lower().encode(), None, b":0"]
            property_check("%s target empty shape" % verb, parse_reply(target), expected)
            property_check("%s oracle empty shape" % verb, parse_reply(oracle), expected)
        close_pair(pair)

    token = "psd:%d" % SEED
    admin = open_pair(False)

    # Confirmation counts combine regular channels + patterns, while shard counts remain a
    # separate namespace.  No-argument unsubscribe order is unspecified, so compare the frame
    # multiset and the complete descending count set rather than dictionary iteration order.
    mixed = open_pair(False)
    regular = [token + ":count:r0", token + ":count:r1"]
    patterns = [token + ":count:p0:*", token + ":count:p1:*"]
    shards = [token + ":count:s0", token + ":count:s1"]
    for verb, names, wanted in (("SUBSCRIBE", regular, [1, 2]),
                                ("PSUBSCRIBE", patterns, [3, 4]),
                                ("SSUBSCRIBE", shards, [1, 2])):
        t, tf, o, of = mixed
        payload = enc([verb] + names); t.sendall(payload); o.sendall(payload); logical_ops += 1
        for index, count in enumerate(wanted):
            target, oracle = compare("%s count %d" % (verb, index),
                                     read_reply(tf), read_reply(of))
            property_check("%s target count %d" % (verb, index), parsed_count(target), count)
            property_check("%s oracle count %d" % (verb, index), parsed_count(oracle), count)

    for verb, missing, count in (("UNSUBSCRIBE", token + ":count:missing", 4),
                                 ("PUNSUBSCRIBE", token + ":count:missing:*", 4),
                                 ("SUNSUBSCRIBE", token + ":count:missing-shard", 2)):
        target, oracle = command_pair(mixed, [verb, missing], verb + " missing")
        property_check(verb + " target missing count", parsed_count(target), count)
        property_check(verb + " oracle missing count", parsed_count(oracle), count)

    def unsubscribe_all(pair, verb, count, remaining=0):
        nonlocal logical_ops
        t, tf, o, of = pair
        payload = enc([verb]); t.sendall(payload); o.sendall(payload); logical_ops += 1
        target = [read_reply(tf) for _ in range(count)]
        oracle = [read_reply(of) for _ in range(count)]
        compare(verb + " all", canonical_unsubscribe(target), canonical_unsubscribe(oracle))
        tcounts = sorted(parsed_count(frame) for frame in target)
        ocounts = sorted(parsed_count(frame) for frame in oracle)
        wanted = list(range(remaining, remaining + count))
        property_check(verb + " target all counts", tcounts, wanted)
        property_check(verb + " oracle all counts", ocounts, wanted)

    unsubscribe_all(mixed, "UNSUBSCRIBE", len(regular), len(patterns))
    unsubscribe_all(mixed, "PUNSUBSCRIBE", len(patterns))
    unsubscribe_all(mixed, "SUNSUBSCRIBE", len(shards))
    close_pair(mixed)

    # One exact arm and two overlapping patterns must produce three frames.  Pattern iteration order
    # is not contractual, so compare a multiset here; the ordered arm below uses exact delivery.
    overlap = open_pair(False)
    channel = token + ":overlap:news:42"
    overlap_patterns = [token + ":overlap:news:*", token + ":overlap:*:42"]
    command_pair(overlap, ["SUBSCRIBE", channel], "overlap exact ack")
    t, tf, o, of = overlap
    payload = enc(["PSUBSCRIBE"] + overlap_patterns)
    t.sendall(payload); o.sendall(payload); logical_ops += 1
    for index in range(2):
        compare("overlap pattern ack %d" % index, read_reply(tf), read_reply(of))
    target_pub, oracle_pub = command_pair(admin, ["PUBLISH", channel, "payload"],
                                          "overlap PUBLISH")
    property_check("overlap target arm count", raw_int(target_pub), 3)
    property_check("overlap oracle arm count", raw_int(oracle_pub), 3)
    target_frames = [read_reply(tf) for _ in range(3)]
    oracle_frames = [read_reply(of) for _ in range(3)]
    compare("overlapping exact/pattern deliveries", canonical_frames(target_frames),
            canonical_frames(oracle_frames))
    target_kinds = sorted(parse_reply(frame)[0] for frame in target_frames)
    oracle_kinds = sorted(parse_reply(frame)[0] for frame in oracle_frames)
    wanted_kinds = [b"message", b"pmessage", b"pmessage"]
    property_check("overlap target fired all arms", target_kinds, wanted_kinds)
    property_check("overlap oracle fired all arms", oracle_kinds, wanted_kinds)
    command_pair(overlap, ["RESET"], "overlap RESET")
    close_pair(overlap)

    # RESP2 restricts ordinary commands in subscriber mode; RESP3 permits them.  The exact error,
    # PING framing and ordinary RESP3 replies are all byte-compared.
    for resp3 in (False, True):
        mode = open_pair(resp3)
        mode_channel = token + ":mode:%d" % (3 if resp3 else 2)
        command_pair(mode, ["SUBSCRIBE", mode_channel], "mode RESP%d subscribe" %
                     (3 if resp3 else 2))
        ping, _ = command_pair(mode, ["PING", "hello"], "mode RESP%d PING" %
                               (3 if resp3 else 2))
        set_reply, _ = command_pair(mode, ["SET", token + ":mode:key", "value"],
                                    "mode RESP%d SET" % (3 if resp3 else 2))
        pubsub_reply, _ = command_pair(mode, ["PUBSUB", "NUMSUB", mode_channel],
                                       "mode RESP%d PUBSUB" % (3 if resp3 else 2))
        for invalid in (["PUBSUB", "BOGUS"], ["PUBSUB", "NUMPAT", "extra"],
                        ["PUBSUB", "HELP", "extra"],
                        ["PUBSUB", "CHANNELS", "a", "b"]):
            command_pair(mode, invalid, "mode RESP%d %s" %
                         (3 if resp3 else 2, " ".join(invalid[:2])))
        publish_reply, _ = command_pair(mode, ["PUBLISH", token + ":mode:other", "x"],
                                        "mode RESP%d PUBLISH" % (3 if resp3 else 2))
        if resp3:
            property_check("RESP3 subscriber PING is ordinary bulk reply", ping[:1], b"$")
            property_check("RESP3 subscriber SET permitted", set_reply, b"+OK\r\n")
            property_check("RESP3 subscriber PUBSUB permitted", pubsub_reply[:1], b"*")
            property_check("RESP3 subscriber PUBLISH permitted", publish_reply[:1], b":")
        else:
            property_check("RESP2 subscriber PING uses array", ping[:1], b"*")
            property_check("RESP2 subscriber SET restricted", set_reply[:1], b"-")
            property_check("RESP2 subscriber PUBSUB restricted", pubsub_reply[:1], b"-")
            property_check("RESP2 subscriber PUBLISH restricted", publish_reply[:1], b"-")
        command_pair(mode, ["RESET"], "mode RESP%d RESET" % (3 if resp3 else 2))
        close_pair(mode)

    # Ordered stream to one exact subscriber.  Publishers are pipelined, but every subscriber frame
    # is compared and its payload is checked against the sequence number, so loss and reorder fire.
    ordered = open_pair(False)
    ordered_channel = token + ":ordered"
    command_pair(ordered, ["SUBSCRIBE", ordered_channel], "ordered subscribe")
    count = 128
    burst = b"".join(enc(["PUBLISH", ordered_channel, str(i)]) for i in range(count))
    admin[0].sendall(burst); admin[2].sendall(burst); logical_ops += count
    for index in range(count):
        compare("ordered publish reply %d" % index, read_reply(admin[1]), read_reply(admin[3]))
    for index in range(count):
        target, oracle = compare("ordered delivery %d" % index,
                                 read_reply(ordered[1]), read_reply(ordered[3]))
        expected = [b"message", ordered_channel.encode(), str(index).encode()]
        property_check("ordered target payload %d" % index, parse_reply(target), expected)
        property_check("ordered oracle payload %d" % index, parse_reply(oracle), expected)
    command_pair(ordered, ["RESET"], "ordered RESET")
    close_pair(ordered)

    # Randomized stateful stream.  Every regular channel matches at most one of these patterns, so
    # exact-before-pattern delivery order is byte-comparable while exact+pattern overlap still fires.
    channels = [token + ":d:%d:%d" % (group, index)
                for group in range(3) for index in range(4)]
    shard_channels = [token + ":s:%d" % index for index in range(6)]
    model_patterns = [token + ":d:%d:*" % group for group in range(3)]

    def pattern_matches(pattern, name):
        return pattern.endswith("*") and name.startswith(pattern[:-1])

    subscribers = []
    for index in range(6):
        pair = open_pair(index % 2 == 1)
        state = {"pair": pair, "exact": set(), "patterns": set(), "shard": set()}
        for name in rng.sample(channels, 2):
            command_pair(pair, ["SUBSCRIBE", name], "random initial SUBSCRIBE")
            state["exact"].add(name)
        if index % 3 != 2:
            pattern = model_patterns[index % len(model_patterns)]
            command_pair(pair, ["PSUBSCRIBE", pattern], "random initial PSUBSCRIBE")
            state["patterns"].add(pattern)
        shard = shard_channels[index % len(shard_channels)]
        command_pair(pair, ["SSUBSCRIBE", shard], "random initial SSUBSCRIBE")
        state["shard"].add(shard)
        subscribers.append(state)

    def expected_frames(state, name, shard):
        if shard:
            return 1 if name in state["shard"] else 0
        return ((1 if name in state["exact"] else 0) +
                sum(1 for pattern in state["patterns"] if pattern_matches(pattern, name)))

    def assert_ack_count(label, state, target, oracle, shard):
        wanted = len(state["shard"] if shard else state["exact"] | state["patterns"])
        property_check(label + " target model count", parsed_count(target), wanted)
        property_check(label + " oracle model count", parsed_count(oracle), wanted)

    for iteration in range(4000):
        choice = rng.randrange(100)
        if choice < 58:
            name = rng.choice(channels)
            message = "r%d:%d" % (iteration, rng.randrange(1000000))
            target, oracle = command_pair(admin, ["PUBLISH", name, message],
                                          "random PUBLISH %d" % iteration)
            wanted = sum(expected_frames(state, name, False) for state in subscribers)
            property_check("random PUBLISH target count %d" % iteration, raw_int(target), wanted)
            property_check("random PUBLISH oracle count %d" % iteration, raw_int(oracle), wanted)
            for sub_index, state in enumerate(subscribers):
                for frame_index in range(expected_frames(state, name, False)):
                    pair = state["pair"]
                    compare("random message %d sub%d frame%d" %
                            (iteration, sub_index, frame_index),
                            read_reply(pair[1]), read_reply(pair[3]))
        elif choice < 68:
            name = rng.choice(shard_channels)
            message = "s%d:%d" % (iteration, rng.randrange(1000000))
            target, oracle = command_pair(admin, ["SPUBLISH", name, message],
                                          "random SPUBLISH %d" % iteration)
            wanted = sum(expected_frames(state, name, True) for state in subscribers)
            property_check("random SPUBLISH target count %d" % iteration, raw_int(target), wanted)
            property_check("random SPUBLISH oracle count %d" % iteration, raw_int(oracle), wanted)
            for sub_index, state in enumerate(subscribers):
                if expected_frames(state, name, True):
                    pair = state["pair"]
                    compare("random smessage %d sub%d" % (iteration, sub_index),
                            read_reply(pair[1]), read_reply(pair[3]))
        elif choice < 76:
            state = rng.choice(subscribers); name = rng.choice(channels)
            subscribe = rng.randrange(2) == 0
            verb = "SUBSCRIBE" if subscribe else "UNSUBSCRIBE"
            target, oracle = command_pair(state["pair"], [verb, name],
                                          "random %s %d" % (verb, iteration))
            if subscribe: state["exact"].add(name)
            else: state["exact"].discard(name)
            assert_ack_count("random %s %d" % (verb, iteration), state, target, oracle, False)
        elif choice < 84:
            state = rng.choice(subscribers); pattern = rng.choice(model_patterns)
            subscribe = rng.randrange(2) == 0
            verb = "PSUBSCRIBE" if subscribe else "PUNSUBSCRIBE"
            target, oracle = command_pair(state["pair"], [verb, pattern],
                                          "random %s %d" % (verb, iteration))
            if subscribe: state["patterns"].add(pattern)
            else: state["patterns"].discard(pattern)
            assert_ack_count("random %s %d" % (verb, iteration), state, target, oracle, False)
        elif choice < 90:
            state = rng.choice(subscribers); name = rng.choice(shard_channels)
            subscribe = rng.randrange(2) == 0
            verb = "SSUBSCRIBE" if subscribe else "SUNSUBSCRIBE"
            target, oracle = command_pair(state["pair"], [verb, name],
                                          "random %s %d" % (verb, iteration))
            if subscribe: state["shard"].add(name)
            else: state["shard"].discard(name)
            assert_ack_count("random %s %d" % (verb, iteration), state, target, oracle, True)
        else:
            introspection = rng.randrange(5)
            if introspection == 0:
                command_pair(admin, ["PUBSUB", "CHANNELS", token + ":d:*"],
                             "random PUBSUB CHANNELS", True)
            elif introspection == 1:
                command_pair(admin, ["PUBSUB", "SHARDCHANNELS", token + ":s:*"],
                             "random PUBSUB SHARDCHANNELS", True)
            elif introspection == 2:
                command_pair(admin, ["PUBSUB", "NUMSUB"] + rng.sample(channels, 3),
                             "random PUBSUB NUMSUB")
            elif introspection == 3:
                command_pair(admin, ["PUBSUB", "SHARDNUMSUB"] +
                             rng.sample(shard_channels, 3), "random PUBSUB SHARDNUMSUB")
            else:
                target, oracle = command_pair(admin, ["PUBSUB", "NUMPAT"],
                                              "random PUBSUB NUMPAT")
                wanted = len(set().union(*(state["patterns"] for state in subscribers)))
                property_check("random NUMPAT target model", raw_int(target), wanted)
                property_check("random NUMPAT oracle model", raw_int(oracle), wanted)

    # Sentinel: the next exact delivery must be the sentinel, proving the model did not under-read
    # an earlier publish.  The explicit receiver count is the zero-vacuity control.
    sentinel = channels[0]
    target, oracle = command_pair(admin, ["PUBLISH", sentinel, "sentinel-final"],
                                  "random sentinel PUBLISH")
    wanted = sum(expected_frames(state, sentinel, False) for state in subscribers)
    property_check("sentinel target receiver count", raw_int(target), wanted)
    property_check("sentinel oracle receiver count", raw_int(oracle), wanted)
    if wanted == 0:
        mismatch("sentinel receiver control", b"0", b">0")
    for sub_index, state in enumerate(subscribers):
        for frame_index in range(expected_frames(state, sentinel, False)):
            pair = state["pair"]
            target_frame, oracle_frame = compare("sentinel sub%d frame%d" %
                                                 (sub_index, frame_index),
                                                 read_reply(pair[1]), read_reply(pair[3]))
            property_check("sentinel target payload", b"sentinel-final" in target_frame, True)
            property_check("sentinel oracle payload", b"sentinel-final" in oracle_frame, True)

    for state in subscribers:
        command_pair(state["pair"], ["RESET"], "random subscriber RESET")
        close_pair(state["pair"])
    close_pair(admin)
    print("DIFFER pubsub: %d logical ops, %d checks, %d diffs -> %s" %
          (logical_ops, checks, diffs, "PASS" if diffs == 0 else "FAIL"))
    return 1 if diffs else 0


if SUITE == "pubsub":
    sys.exit(run_pubsub_differ(rng))

def run_fanout_differ(rng):
    # FANOUT differential: every delivered frame is byte-compared against Redis, in order, for a
    # mixed population of RESP2/RESP3, exact/pattern/shard subscribers.  This is the suite that
    # covers the encode-once blob (one encoding serving '*' and '>' headers), the `pmessage` tail
    # reuse, the per-owner delivery batch, and the two ordering contracts the design promises:
    #   (1) a pipelined burst on ONE channel arrives in publish order (its single home orders it);
    #   (2) one-at-a-time publishes across MANY channels arrive in publish order (a home queues its
    #       deliveries before it releases the reply, so the next publish cannot overtake them).
    # Frame counts come from a Python-side model, not from select(): a lost, extra or reordered
    # frame therefore misaligns the byte comparison immediately instead of being timed out.
    diffs = 0
    checks = 0

    def compare(label, target, oracle):
        nonlocal diffs, checks
        checks += 1
        if target != oracle:
            diffs += 1
            if diffs <= 12:
                print("  DIFF %s\n    target: %r\n    oracle: %r" %
                      (label, target[:200], oracle[:200]))

    def open_pair(resp3):
        pair = []
        for host, port in ((TH, TP), (OH, OP)):
            s = socket.create_connection((host, port), timeout=20)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            f = s.makefile("rb")
            if resp3:
                s.sendall(enc(["HELLO", "3"]))
                hello = read_reply(f)
                if not hello.startswith(b"%7\r\n"):
                    raise RuntimeError("HELLO 3 failed on %s:%d" % (host, port))
            pair.append((s, f))
        return pair

    CHANNELS = ["fan:d:%02d" % i for i in range(8)]
    SHARDS = ["fan:s:%02d" % i for i in range(4)]
    # Disjoint by construction: no channel matches two patterns, because the relative order of two
    # pmessage frames for one publish is dictionary order on both servers and is not a contract.
    PREFIXES = ["fan:d:0", "fan:d:1"]
    PATTERNS = [p + "*" for p in PREFIXES]

    publisher = open_pair(False)
    subs = []
    NSUB = 9
    for index in range(NSUB):
        (ts, tf), (os_, of) = open_pair(index % 3 == 2)
        state = {"t": ts, "tf": tf, "o": os_, "of": of, "resp3": index % 3 == 2,
                 "exact": set(), "patterns": set(), "shard": set()}
        exact = rng.sample(CHANNELS, rng.randrange(1, 4))
        for channel in exact:
            for sock in (ts, os_): sock.sendall(enc(["SUBSCRIBE", channel]))
            compare("SUBSCRIBE ack %d" % index, read_reply(tf), read_reply(of))
            state["exact"].add(channel)
        if index % 2 == 0:
            pattern = rng.choice(PATTERNS)
            for sock in (ts, os_): sock.sendall(enc(["PSUBSCRIBE", pattern]))
            compare("PSUBSCRIBE ack %d" % index, read_reply(tf), read_reply(of))
            state["patterns"].add(pattern)
        if index % 3 != 1:
            shard = rng.choice(SHARDS)
            for sock in (ts, os_): sock.sendall(enc(["SSUBSCRIBE", shard]))
            compare("SSUBSCRIBE ack %d" % index, read_reply(tf), read_reply(of))
            state["shard"].add(shard)
        subs.append(state)

    def frames_for(state, channel, shard):
        # Redis walks the exact index first and the pattern list second, and so do we.
        n = 0
        if shard:
            return 1 if channel in state["shard"] else 0
        if channel in state["exact"]: n += 1
        for pattern in state["patterns"]:
            if channel.startswith(pattern[:-1]): n += 1
        return n

    def drain(published):
        # `published` is the publish order: [(channel, shard), ...].
        for index, state in enumerate(subs):
            expected = sum(frames_for(state, channel, shard) for channel, shard in published)
            for frame in range(expected):
                compare("sub%d frame %d" % (index, frame),
                        read_reply(state["tf"]), read_reply(state["of"]))

    ROUNDS = 220
    for round_index in range(ROUNDS):
        mode = rng.randrange(10)
        published = []
        if mode < 5:
            # (1) pipelined burst on ONE channel -- ordering owned by that channel's single home.
            shard = rng.randrange(4) == 0
            channel = rng.choice(SHARDS if shard else CHANNELS)
            verb = "SPUBLISH" if shard else "PUBLISH"
            burst = rng.randrange(3, 26)
            payload = [enc([verb, channel, "r%d:%d" % (round_index, i)]) for i in range(burst)]
            blob = b"".join(payload)
            publisher[0][0].sendall(blob); publisher[1][0].sendall(blob)
            for i in range(burst):
                compare("%s burst %d/%d" % (verb, round_index, i),
                        read_reply(publisher[0][1]), read_reply(publisher[1][1]))
                published.append((channel, shard))
        elif mode < 9:
            # (2) one at a time across MANY channels -- ordering owned by the delivery fence.
            for step in range(rng.randrange(2, 8)):
                shard = rng.randrange(4) == 0
                channel = rng.choice(SHARDS if shard else CHANNELS)
                verb = "SPUBLISH" if shard else "PUBLISH"
                command = enc([verb, channel, "s%d:%d" % (round_index, step)])
                publisher[0][0].sendall(command); publisher[1][0].sendall(command)
                compare("%s seq %d/%d" % (verb, round_index, step),
                        read_reply(publisher[0][1]), read_reply(publisher[1][1]))
                published.append((channel, shard))
        else:
            # Subscription churn plus the full introspection surface.
            state = subs[rng.randrange(NSUB)]
            channel = rng.choice(CHANNELS)
            if channel in state["exact"]:
                for sock in (state["t"], state["o"]): sock.sendall(enc(["UNSUBSCRIBE", channel]))
                compare("UNSUBSCRIBE churn %d" % round_index,
                        read_reply(state["tf"]), read_reply(state["of"]))
                state["exact"].discard(channel)
            else:
                for sock in (state["t"], state["o"]): sock.sendall(enc(["SUBSCRIBE", channel]))
                compare("SUBSCRIBE churn %d" % round_index,
                        read_reply(state["tf"]), read_reply(state["of"]))
                state["exact"].add(channel)
            for command, unordered in (
                    (["PUBSUB", "NUMSUB"] + rng.sample(CHANNELS, 3), False),
                    (["PUBSUB", "NUMPAT"], False),
                    (["PUBSUB", "SHARDNUMSUB"] + rng.sample(SHARDS, 2), False),
                    (["PUBSUB", "CHANNELS", "fan:d:*"], True),
                    (["PUBSUB", "SHARDCHANNELS", "fan:s:*"], True)):
                publisher[0][0].sendall(enc(command)); publisher[1][0].sendall(enc(command))
                target = read_reply(publisher[0][1]); oracle = read_reply(publisher[1][1])
                if unordered:
                    target = normalize("SMEMBERS", target)
                    oracle = normalize("SMEMBERS", oracle)
                compare(" ".join(command[:3]), target, oracle)
        drain(published)

    # Sentinel sweep: every subscriber's NEXT frame must be this publish. Any frame either server
    # leaked earlier and we did not account for would sit ahead of it and show up right here.
    sentinel = CHANNELS[0]
    command = enc(["PUBLISH", sentinel, "sentinel-final"])
    publisher[0][0].sendall(command); publisher[1][0].sendall(command)
    compare("sentinel PUBLISH", read_reply(publisher[0][1]), read_reply(publisher[1][1]))
    for index, state in enumerate(subs):
        for frame in range(frames_for(state, sentinel, False)):
            target = read_reply(state["tf"]); oracle = read_reply(state["of"])
            compare("sentinel sub%d frame %d" % (index, frame), target, oracle)
            if b"sentinel-final" not in target:
                diffs += 1
                print("  DIFF sentinel sub%d saw a stale frame: %r" % (index, target[:120]))

    for state in subs:
        state["t"].close(); state["o"].close()
    publisher[0][0].close(); publisher[1][0].close()
    print("DIFFER fanout: %d checks, %d diffs -> %s" %
          (checks, diffs, "PASS" if diffs == 0 else "FAIL"))
    return 1 if diffs else 0


if SUITE == "fanout":
    sys.exit(run_fanout_differ(rng))
if SUITE == "spubsub":
    sys.exit(run_spubsub_differ(rng))
# Notification differential is structurally different from command/reply suites: each side needs
# a driver plus a pattern subscriber, and streams are compared at every known owner-side fire
# point. The oracle for this suite must be vanilla Redis (see SPEC-WAVEA §0.2).
def run_notify_suite(rng):
    tds, tdf = conn(TH, TP); ods, odf = conn(OH, OP)
    tls, tlf = conn(TH, TP); ols, olf = conn(OH, OP)
    for sock, file in ((tds, tdf), (ods, odf)):
        sock.sendall(enc(["CONFIG", "SET", "notify-keyspace-events", ""]))
        if read_reply(file)[:1] != b"+": raise RuntimeError("notification disable failed")
        sock.sendall(enc(["FLUSHALL"])); read_reply(file)
        sock.sendall(enc(["CONFIG", "SET", "notify-keyspace-events", "EA"]))
        if read_reply(file)[:1] != b"+": raise RuntimeError("notification enable failed")
    for sock, file in ((tls, tlf), (ols, olf)):
        sock.sendall(enc(["PSUBSCRIBE", "__keyevent@0__:*"]))
        read_reply(file)

    stream = []
    for i in range(160):
        stem = "nd:%d" % i
        choice = rng.randrange(10)
        if choice == 0:
            stream += [(["SET", stem, str(rng.randrange(1000))], 1)]
        elif choice == 1:
            stream += [(["RPUSH", stem, "a", "b"], 1), (["LPOP", stem, "2"], 2)]
        elif choice == 2:
            stream += [(["SADD", stem, "a"], 1), (["SREM", stem, "a"], 2)]
        elif choice == 3:
            stream += [(["HSET", stem, "f", "v"], 1), (["HDEL", stem, "f"], 2)]
        elif choice == 4:
            stream += [(["ZADD", stem, "1", "m"], 1), (["ZREM", stem, "m"], 2)]
        elif choice == 5:
            stream += [(["MSET", stem + ":a", "1", stem + ":z", "2"], 2),
                       (["DEL", stem + ":a", stem + ":z"], 2)]
        elif choice == 6:
            stream += [(["SET", stem, "v", "EX", "1000"], 2),
                       (["PERSIST", stem], 1)]
        elif choice == 7:
            stream += [(["SET", stem + ":a", "v"], 1),
                       (["RENAME", stem + ":a", stem + ":z"], 2)]
        elif choice == 8:
            # Redis 7.4 only publishes the empty-result del when an old destination existed;
            # pre-seed it so this differential arm has one event on both sides. The directed
            # battery separately enforces this spec's stronger nonexistent-destination arm.
            stream += [(["SET", stem, "old"], 1),
                       (["SINTERSTORE", stem, stem + ":none-a", stem + ":none-z"], 1)]
        else:
            stream += [(["XADD", stem, "1-0", "f", "v"], 1),
                       (["XADD", stem, "2-0", "f", "w"], 1),
                       (["XDEL", stem, "1-0"], 1),
                       (["XTRIM", stem, "MAXLEN", "=", "0"], 1)]

    diffs = 0
    events = 0
    for i, (op, count) in enumerate(stream):
        payload = enc(op)
        tds.sendall(payload); ods.sendall(payload)
        target_reply = read_reply(tdf); oracle_reply = read_reply(odf)
        if target_reply != oracle_reply:
            diffs += 1
            if diffs <= 12:
                print("  REPLY DIFF op %d %r\n    target: %r\n    oracle: %r" %
                      (i, op[:5], target_reply[:96], oracle_reply[:96]))
        for _ in range(count):
            try:
                target = parse_reply(read_reply(tlf))
            except TimeoutError:
                print("  TARGET TIMEOUT op %d %r expected_events=%d" % (i, op[:5], count))
                return diffs + 1
            try:
                oracle = parse_reply(read_reply(olf))
            except TimeoutError:
                print("  ORACLE TIMEOUT op %d %r expected_events=%d" % (i, op[:5], count))
                return diffs + 1
            target_event = target[2:4] if target and len(target) == 4 else target
            oracle_event = oracle[2:4] if oracle and len(oracle) == 4 else oracle
            events += 1
            if target_event != oracle_event:
                diffs += 1
                if diffs <= 12:
                    print("  EVENT DIFF op %d %r\n    target: %r\n    oracle: %r" %
                          (i, op[:5], target_event, oracle_event))
    for sock in (tds, ods):
        sock.sendall(enc(["CONFIG", "SET", "notify-keyspace-events", ""]))
    read_reply(tdf); read_reply(odf)
    for sock in (tds, ods, tls, ols): sock.close()
    print("DIFFER notify: %d ops, %d events, %d diffs -> %s" %
          (len(stream), events, diffs, "PASS" if diffs == 0 else "FAIL"))
    return diffs

if SUITE == "notify":
    sys.exit(1 if run_notify_suite(rng) else 0)

# DUMP payloads have multiple equally valid encodings (TomoKV intentionally emits the simple
# canonical RDB types), so byte-comparing the bulk strings would reject interoperability. This
# suite cross-RESTOREs each side's payload into the other side and then byte-compares full reads.
def run_wiredump_suite(rng):
    ts, tf = conn(TH, TP); os_, of = conn(OH, OP)

    def command(sock, file, args):
        sock.sendall(enc(args))
        return read_reply(file)

    def payload(reply):
        value = parse_reply(reply)
        return value if isinstance(value, bytes) else None

    def full_read(sock, file, kind, key, ttl_fields=()):
        if kind == "string": op = ["GET", key]
        elif kind == "list": op = ["LRANGE", key, "0", "-1"]
        elif kind == "hash": op = ["HGETALL", key]
        elif kind == "set": op = ["SMEMBERS", key]
        else: op = ["ZRANGE", key, "0", "-1", "WITHSCORES"]
        value = normalize(op[0], command(sock, file, op))
        if ttl_fields:
            ttl = command(sock, file,
                          ["HPEXPIRETIME", key, "FIELDS", str(len(ttl_fields))] + ttl_fields)
            return value + b"\x00field-ttl\x00" + ttl
        return value

    far = str(int(time.time() * 1000) + 24 * 60 * 60 * 1000)
    farther = str(int(far) + 70000)
    definitions = [
        ("wd:string", "string", [["SET", "wd:string", "wire\x00value"]], ()),
        ("wd:string-lzf", "string", [["SET", "wd:string-lzf", "repeat-" * 800]], ()),
        ("wd:list", "list", [["RPUSH", "wd:list", "a", "2", "x" * 500]], ()),
        ("wd:list-large", "list", [["RPUSH", "wd:list-large", "q" * 10000]], ()),
        ("wd:hash", "hash", [["HSET", "wd:hash", "a", "1", "b", "value"]],
         ["a", "b"]),
        ("wd:hash-large", "hash", [["HSET", "wd:hash-large", "field", "x" * 1000]],
         ["field"]),
        ("wd:hash-ttl-some", "hash",
         [["HSET", "wd:hash-ttl-some", "a", "1", "b", "2", "c", "3"],
          ["HPEXPIREAT", "wd:hash-ttl-some", far, "FIELDS", "1", "a"],
          ["HPEXPIREAT", "wd:hash-ttl-some", farther, "FIELDS", "1", "c"]],
         ["a", "b", "c"]),
        ("wd:hash-ttl-all", "hash",
         [["HSET", "wd:hash-ttl-all", "a", "1", "b", "x" * 100],
          ["HPEXPIREAT", "wd:hash-ttl-all", far, "FIELDS", "2", "a", "b"]],
         ["a", "b"]),
        ("wd:set-int", "set", [["SADD", "wd:set-int", "-2", "1", "70000"]], ()),
        ("wd:set", "set", [["SADD", "wd:set", "a", "bb", "m" * 100]], ()),
        ("wd:zset", "zset", [["ZADD", "wd:zset", "-2", "lo", "1.5", "mid"]], ()),
        ("wd:zset-large", "zset", [["ZADD", "wd:zset-large", "3.25", "z" * 100]], ()),
    ]
    pairs = ((ts, tf), (os_, of))
    for sock, file in pairs:
        if command(sock, file, ["FLUSHALL"])[:1] != b"+":
            raise RuntimeError("wiredump FLUSHALL failed")
    diffs = 0
    checks = 0

    for _, _, setup, _ in definitions:
        for operation in setup:
            replies = [command(sock, file, operation) for sock, file in pairs]
            if replies[0] != replies[1]:
                diffs += 1

    cached = []
    for key, kind, _, ttl_fields in definitions:
        target_dump = payload(command(ts, tf, ["DUMP", key]))
        oracle_dump = payload(command(os_, of, ["DUMP", key]))
        if target_dump is None or oracle_dump is None:
            raise RuntimeError("wiredump seed DUMP failed for %s" % key)
        cached.append((key, kind, target_dump, oracle_dump, ttl_fields))

    for iteration in range(4200):
        key, kind, target_seed, oracle_seed, ttl_fields = rng.choice(cached)
        action = rng.randrange(5)
        if action == 0:
            # Cross the freshly produced payloads. The restored values have the same TTL and full
            # content even though their legal wire representations differ.
            target_dump = payload(command(ts, tf, ["DUMP", key]))
            oracle_dump = payload(command(os_, of, ["DUMP", key]))
            if target_dump is None or oracle_dump is None:
                diffs += 1
                continue
            target_reply = command(ts, tf, ["RESTORE", "wd:cross", "600000",
                                             oracle_dump, "REPLACE"])
            oracle_reply = command(os_, of, ["RESTORE", "wd:cross", "600000",
                                              target_dump, "REPLACE"])
            if target_reply != oracle_reply or full_read(ts, tf, kind, "wd:cross", ttl_fields) != \
                    full_read(os_, of, kind, "wd:cross", ttl_fields):
                diffs += 1
        elif action == 1:
            # Feed exactly the same randomly selected producer payload to both RESTORE parsers.
            wire = target_seed if rng.randrange(2) else oracle_seed
            options = ["REPLACE"]
            ttl = "600000"
            if rng.randrange(4) == 0:
                ttl = str(int(time.time() * 1000) + 600000)
                options.append("ABSTTL")
            target_reply = command(ts, tf, ["RESTORE", "wd:restore", ttl, wire] + options)
            oracle_reply = command(os_, of, ["RESTORE", "wd:restore", ttl, wire] + options)
            if target_reply != oracle_reply or full_read(ts, tf, kind, "wd:restore", ttl_fields) != \
                    full_read(os_, of, kind, "wd:restore", ttl_fields):
                diffs += 1
        elif action == 2:
            target_reply = command(ts, tf, ["EXISTS", key])
            oracle_reply = command(os_, of, ["EXISTS", key])
            if target_reply != oracle_reply:
                diffs += 1
        elif action == 3:
            if full_read(ts, tf, kind, key, ttl_fields) != \
                    full_read(os_, of, kind, key, ttl_fields):
                diffs += 1
        else:
            # This arm is a negative control until the first restore and a live-TTL check after it.
            target_reply = normalize("PTTL", command(ts, tf, ["PTTL", "wd:restore"]))
            oracle_reply = normalize("PTTL", command(os_, of, ["PTTL", "wd:restore"]))
            if target_reply != oracle_reply:
                diffs += 1
        checks += 1
        if diffs and diffs <= 12:
            print("  WIREDUMP DIFF op %d action=%d key=%s" % (iteration, action, key))

    ts.close(); os_.close()
    print("DIFFER wiredump: %d ops, %d diffs -> %s" %
          (checks, diffs, "PASS" if diffs == 0 else "FAIL"))
    return diffs

if SUITE == "wiredump":
    sys.exit(1 if run_wiredump_suite(rng) else 0)

# ---- Lane F: CLIENT connection-control + TRACKING differential ---------------------------------
# Structurally different from the command/reply suites: replies depend on connection identity, and
# invalidation arrives as an out-of-band push. So the suite drives a fixed pair of connections per
# side, byte-compares every reply of an identity-independent grammar stream, and then compares the
# invalidation frames a tracking connection receives for an identical write script.
def run_climon_suite(rng):
    import select as _select
    diffs = 0
    checks = 0
    ts, tf = conn(TH, TP); os_, of = conn(OH, OP)
    for cs, cf in ((ts, tf), (os_, of)):
        cs.sendall(enc(["FLUSHALL"])); read_reply(cf)

    def both(args, label=None):
        nonlocal diffs, checks
        payload = enc(args)
        ts.sendall(payload); os_.sendall(payload)
        a = read_reply(tf); b = read_reply(of)
        checks += 1
        if a != b:
            diffs += 1
            if diffs <= 15:
                print("  DIFF %s %r\n    target: %r\n    oracle: %r" %
                      (label or "", args[:14], a[:160], b[:160]))
        return a

    # ---- grammar stream. Every command below has an identity-independent reply. --------------
    on_off = ["ON", "OFF", "on", "off", "garbage", ""]
    modes = ["WRITE", "ALL", "write", "GARBAGE"]
    prefixes = ["a", "ab", "abc", "b", "user:", "user:1", ""]
    grammar = []
    for _ in range(4200):
        c = rng.randrange(16)
        if c == 0:   grammar.append(["CLIENT", "NO-TOUCH", rng.choice(on_off)])
        elif c == 1: grammar.append(["CLIENT", "NO-EVICT", rng.choice(on_off)])
        elif c == 2: grammar.append(["CLIENT", "GETREDIR"])
        elif c == 3: grammar.append(["CLIENT", "TRACKINGINFO"])
        elif c == 4: grammar.append(["CLIENT", "TRACKING", rng.choice(["on", "off", "garbage"])])
        elif c == 5:
            args = ["CLIENT", "TRACKING", rng.choice(["on", "off"])]
            if rng.randrange(2): args.append("BCAST")
            for _ in range(rng.randrange(3)):
                args += ["PREFIX", rng.choice(prefixes)]
            if rng.randrange(3) == 0: args.append(rng.choice(["OPTIN", "OPTOUT", "NOLOOP"]))
            grammar.append(args)
        elif c == 6: grammar.append(["CLIENT", "CACHING", rng.choice(["yes", "no", "maybe"])])
        elif c == 7:
            grammar.append(["CLIENT", "TRACKING", "on", "REDIRECT",
                            rng.choice(["999999", "0", "abc", "-1"])])
        elif c == 8: grammar.append(["CLIENT", "UNBLOCK", rng.choice(["999999", "0", "notanint", "-5"])])
        elif c == 9:
            grammar.append(["CLIENT", "UNBLOCK", "999999",
                            rng.choice(["TIMEOUT", "ERROR", "GARBAGE"])])
        elif c == 10: grammar.append(["CLIENT", "PAUSE", rng.choice(["0", "abc", "-1", "99999999999999999999"])])
        elif c == 11: grammar.append(["CLIENT", "PAUSE", "0", rng.choice(modes)])
        elif c == 12: grammar.append(["CLIENT", "UNPAUSE"])
        elif c == 13: grammar.append(["CLIENT", "REPLY", rng.choice(["ON", "on", "garbage"])])
        elif c == 14:
            # arity errors: the per-subcommand wrong-number-of-arguments strings
            grammar.append(rng.choice([
                ["CLIENT", "NO-TOUCH"], ["CLIENT", "REPLY"], ["CLIENT", "UNPAUSE", "X"],
                ["CLIENT", "PAUSE"], ["CLIENT", "UNBLOCK"], ["CLIENT", "CACHING"],
                ["CLIENT", "GETREDIR", "X"], ["CLIENT", "TRACKINGINFO", "X"],
                ["CLIENT", "TRACKING"], ["CLIENT", "NO-EVICT"],
            ]))
        else: grammar.append(["CLIENT", "TRACKING", "off"])
    grammar.append(["CLIENT", "TRACKING", "off"])
    grammar.append(["CLIENT", "UNPAUSE"])
    for op in grammar:
        both(op, "grammar")

    # ---- invalidation stream. RESP3 tracking client + a writer, per side. --------------------
    tt, ttf = conn(TH, TP); ot, otf = conn(OH, OP)
    for cs, cf in ((tt, ttf), (ot, otf)):
        cs.sendall(enc(["HELLO", "3"])); read_reply(cf)
    tw, twf = conn(TH, TP); ow, owf = conn(OH, OP)

    # Out-of-band pushes can arrive interleaved with a command reply, and the two servers place
    # them at different points relative to it (redis emits the reply first; this server can emit
    # the push first when the connection was idle). So a reply read PARKS any push it runs into
    # and the round-level comparison consumes the parked frames -- the assertion is on the set of
    # invalidations produced by a round, which is the actual contract.
    parked = {"t": b"", "o": b""}

    def read_command_reply(file, side):
        while True:
            frame = read_reply(file)
            if frame[:1] == b">":
                parked[side] += frame
                continue
            return frame

    def track(args, label):
        nonlocal diffs, checks
        payload = enc(args)
        tt.sendall(payload); ot.sendall(payload)
        a = read_command_reply(ttf, "t"); b = read_command_reply(otf, "o")
        checks += 1
        if a != b:
            diffs += 1
            print("  DIFF track %s %r\n    target: %r\n    oracle: %r" % (label, args[:5], a, b))

    def write_both(args):
        payload = enc(args)
        tw.sendall(payload); ow.sendall(payload)
        read_reply(twf); read_reply(owf)

    def compare_pushes(label, wait=1.2):
        nonlocal diffs, checks
        checks += 1
        def collect(sock):
            deadline = time.time() + wait
            out = b""
            while True:
                left = deadline - time.time()
                if left <= 0: break
                if not _select.select([sock], [], [], left)[0]: break
                chunk = sock.recv(65536)
                if not chunk: break
                out += chunk
                deadline = time.time() + 0.2
            return out
        a = parked["t"] + collect(tt); b = parked["o"] + collect(ot)
        parked["t"] = parked["o"] = b""
        if a != b:
            diffs += 1
            print("  DIFF invalidation %s\n    target: %r\n    oracle: %r" % (label, a, b))

    track(["CLIENT", "TRACKING", "on"], "on")
    keys = ["ck:%d" % i for i in range(12)]
    for round_ in range(40):
        k = rng.choice(keys)
        mode = rng.randrange(5)
        if mode == 0:
            write_both(["SET", k, "v%d" % round_])
            track(["GET", k], "read")
            write_both(["SET", k, "w%d" % round_])
        elif mode == 1:
            write_both(["SET", k, "v"])
            track(["GET", k], "read")
            write_both(["DEL", k])
        elif mode == 2:
            track(["GET", k], "read-miss")
            write_both(["SET", k, "created"])
        elif mode == 3:
            track(["GET", k], "read")
            write_both(["GET", k])            # a read must never invalidate
        else:
            track(["GET", k], "read")
            write_both(["APPEND", k, "z"])
        compare_pushes("round %d mode %d" % (round_, mode))
    track(["CLIENT", "TRACKING", "off"], "off")

    # NOLOOP + BCAST behaviours, byte-compared
    track(["CLIENT", "TRACKING", "on", "NOLOOP"], "noloop on")
    track(["SET", "ck:loop", "1"], "self set")
    track(["GET", "ck:loop"], "self read")
    track(["SET", "ck:loop", "2"], "self set again")
    compare_pushes("noloop self-write")
    track(["GET", "ck:loop"], "self read 2")
    write_both(["SET", "ck:loop", "3"])
    compare_pushes("noloop other-write")
    track(["CLIENT", "TRACKING", "off"], "noloop off")

    track(["CLIENT", "TRACKING", "on", "BCAST", "PREFIX", "bx:"], "bcast on")
    for i in range(12):
        write_both(["SET", "bx:%d" % i, "v"])
        compare_pushes("bcast hit %d" % i)
        write_both(["SET", "zz:%d" % i, "v"])
        compare_pushes("bcast miss %d" % i)
    track(["CLIENT", "TRACKING", "off"], "bcast off")

    for sock in (ts, os_, tt, ot, tw, ow): sock.close()
    print("DIFFER climon: %d ops, %d diffs -> %s" %
          (checks, diffs, "PASS" if diffs == 0 else "FAIL"))
    return diffs

if SUITE == "climon":
    sys.exit(1 if run_climon_suite(rng) else 0)


# ---- Lane t-compatintro: routing/introspection/ACL/notification differential -----------------
# Identity-bearing CLIENT rows and asynchronous notification frames make this a custom driver.
# Deterministic replies are byte-compared; COMMAND inventory/metadata and CLIENT counters are
# compared at their documented semantic boundary, with the raw known differences recorded in
# NOTES-COMPATINTRO.md.
def run_compatintro_suite(rng):
    import select as _select

    diffs = 0
    checks = 0
    fired = {"getkeys": 0, "config_multi": 0, "client_rows": 0,
             "acl_mutations": 0, "notify_events": 0, "silence_controls": 0}
    ts, tf = conn(TH, TP); os_, of = conn(OH, OP)

    def mismatch(label, target, oracle):
        nonlocal diffs
        diffs += 1
        if diffs <= 18:
            print("  DIFF %s\n    target: %r\n    oracle: %r" %
                  (label, target[:280] if isinstance(target, bytes) else target,
                   oracle[:280] if isinstance(oracle, bytes) else oracle))

    def raw_command(sock, file, argv):
        sock.sendall(enc(argv))
        return read_reply(file)

    def both(argv, label=None):
        nonlocal checks
        target = raw_command(ts, tf, argv)
        oracle = raw_command(os_, of, argv)
        checks += 1
        if target != oracle:
            mismatch(label or " ".join(argv[:3]), target, oracle)
        return target, oracle

    def semantic(label, target, oracle):
        nonlocal checks
        checks += 1
        if target != oracle: mismatch(label, target, oracle)

    # Clean only state this suite owns. The servers may be long-lived across seeds.
    for sock, file in ((ts, tf), (os_, of)):
        raw_command(sock, file, ["CONFIG", "SET", "notify-keyspace-events", ""])
        raw_command(sock, file, ["FLUSHALL"])
        raw_command(sock, file, ["ACL", "DELUSER", "ci:diff", "ci:bad"])

    # COMMAND: randomized exact GETKEYS coverage, especially movable-key forms. ----------------
    def keys(n): return ["ci:k:%d" % rng.randrange(64) for _ in range(n)]

    def getkeys_case():
        choice = rng.randrange(25)
        n = rng.randrange(1, 5)
        ks = keys(n)
        if choice == 0: cmd = ["GET", ks[0]]
        elif choice == 1: cmd = ["MGET"] + ks
        elif choice == 2: cmd = ["ZADD", ks[0], "NX", "1", "m1", "2", "m2"]
        elif choice == 3: cmd = ["EVAL", "return 1", str(n)] + ks + ["arg"]
        elif choice == 4: cmd = ["EVALSHA", "0" * 40, str(n)] + ks + ["arg"]
        elif choice == 5: cmd = ["FCALL", "ci_fn", str(n)] + ks + ["arg"]
        elif choice == 6: cmd = ["XREAD", "COUNT", "2", "STREAMS"] + ks + ["0"] * n
        elif choice == 7:
            cmd = ["XREADGROUP", "GROUP", "g", "c", "COUNT", "2", "STREAMS"] + ks + [">"] * n
        elif choice == 8: cmd = ["LMPOP", str(n)] + ks + ["LEFT", "COUNT", "2"]
        elif choice == 9: cmd = ["BLMPOP", "0", str(n)] + ks + ["RIGHT", "COUNT", "2"]
        elif choice == 10: cmd = ["ZMPOP", str(n)] + ks + ["MIN", "COUNT", "2"]
        elif choice == 11: cmd = ["BZMPOP", "0", str(n)] + ks + ["MAX", "COUNT", "2"]
        elif choice == 12: cmd = ["SINTERCARD", str(n)] + ks + ["LIMIT", "1"]
        elif choice == 13: cmd = ["ZINTERCARD", str(n)] + ks + ["LIMIT", "1"]
        elif choice in (14, 15, 16):
            cmd = [rng.choice(["ZUNION", "ZINTER", "ZDIFF"]), str(n)] + ks
            if cmd[0] != "ZDIFF": cmd += ["WEIGHTS"] + ["1"] * n
        elif choice == 17:
            cmd = [rng.choice(["ZUNIONSTORE", "ZINTERSTORE", "ZDIFFSTORE"]),
                   "ci:dest", str(n)] + ks
        elif choice == 18:
            cmd = ["GEORADIUS", ks[0], "0", "0", "1", "km", "STORE", "ci:dest"]
        elif choice == 19:
            cmd = ["GEORADIUSBYMEMBER", ks[0], "m", "1", "km", "STOREDIST", "ci:dest"]
        elif choice == 20:
            cmd = ["SORT", ks[0], "BY", "weight:*", "GET", "object:*", "STORE", "ci:dest"]
        elif choice == 21: cmd = ["MEMORY", "USAGE", ks[0], "SAMPLES", "3"]
        elif choice == 22: cmd = ["BITOP", "AND", "ci:dest"] + ks
        elif choice == 23:
            cmd = ["MSET"] + sum(([key, "v"] for key in ks), [])
        else: cmd = ["RENAME", ks[0], "ci:dest"]
        return ["COMMAND", "GETKEYS"] + cmd

    for iteration in range(1550):
        argv = getkeys_case()
        both(argv, "GETKEYS randomized %d %s" % (iteration, argv[2]))
        fired["getkeys"] += 1

    directed_getkeys = [
        ["COMMAND", "GETKEYS", "EVAL", "return 1", "0", "not-a-key"],
        ["COMMAND", "GETKEYS", "EVAL", "return 1", "-1"],
        ["COMMAND", "GETKEYS", "LMPOP", "0", "k", "LEFT"],
        ["COMMAND", "GETKEYS", "ZMPOP", "garbage", "k", "MIN"],
        ["COMMAND", "GETKEYS", "GET"],
        ["COMMAND", "GETKEYS", "PING"],
    ]
    for iteration in range(150):
        both(rng.choice(directed_getkeys), "GETKEYS validation %d" % iteration)
    for iteration in range(180):
        ks = keys(rng.randrange(1, 5))
        both(["COMMAND", "GETKEYSANDFLAGS", "MGET"] + ks,
             "GETKEYSANDFLAGS readonly %d" % iteration)

    # Inventory order is dictionary/registry order. Compare the returned set and prove it fired.
    for args, label in ((["COMMAND", "LIST", "FILTERBY", "PATTERN", "*POP"], "LIST PATTERN"),
                        (["COMMAND", "LIST", "FILTERBY", "ACLCAT", "STRING"], "LIST ACLCAT")):
        target = parse_reply(raw_command(ts, tf, args))
        oracle = parse_reply(raw_command(os_, of, args))
        target_set = sorted(target) if isinstance(target, list) else target
        oracle_set = sorted(oracle) if isinstance(oracle, list) else oracle
        if not target_set or not oracle_set:
            mismatch(label + " non-vacuity", repr(target_set).encode(), repr(oracle_set).encode())
        semantic(label + " members", repr(target_set).encode(), repr(oracle_set).encode())
    both(["COMMAND", "LIST", "FILTERBY", "MODULE", "no-such-module"], "LIST MODULE empty")

    # COMMAND INFO's name/arity/legacy key range is the declared compatibility boundary.
    info = ["COMMAND", "INFO", "GET", "MGET", "ZADD", "not-a-command"]
    target = normalize_introspection("COMMAND", info, raw_command(ts, tf, info))
    oracle = normalize_introspection("COMMAND", info, raw_command(os_, of, info))
    semantic("COMMAND INFO routing fields", target, oracle)
    for sock, file, side in ((ts, tf, "target"), (os_, of, "oracle")):
        count = parse_reply(raw_command(sock, file, ["COMMAND", "COUNT"]))
        value = int(count[1:]) if isinstance(count, bytes) and count[:1] == b":" else 0
        if value < 200: mismatch("COMMAND COUNT %s positive" % side, str(value).encode(), b">=200")
    checks += 1

    # CONFIG: maps are order-independent, but every name and rendered value remains byte-exact. --
    config_names = ["save", "appendonly", "appendfsync", "maxmemory", "maxmemory-policy",
                    "maxmemory-samples", "maxclients", "timeout", "tcp-keepalive",
                    "client-output-buffer-limit", "notify-keyspace-events"]

    def normalized_config(reply):
        value = parse_reply(reply)
        if not isinstance(value, list) or len(value) % 2: return reply
        pairs = sorted((value[i], value[i + 1]) for i in range(0, len(value), 2))
        return repr(pairs).encode()

    def config_get(patterns, label):
        argv = ["CONFIG", "GET"] + patterns
        target = normalized_config(raw_command(ts, tf, argv))
        oracle = normalized_config(raw_command(os_, of, argv))
        semantic(label, target, oracle)
        if len(patterns) > 1: fired["config_multi"] += 1

    for iteration in range(1050):
        choice = rng.randrange(10)
        if choice < 6:
            count = rng.randrange(1, 4)
            names = rng.sample(config_names, count)
            config_get(names, "CONFIG GET %d" % iteration)
        elif choice < 8:
            timeout = str(rng.choice([0, 30, 60, 300]))
            keepalive = str(rng.choice([0, 60, 300]))
            both(["CONFIG", "SET", "timeout", timeout, "tcp-keepalive", keepalive],
                 "CONFIG SET multi %d" % iteration)
            config_get(["timeout", "tcp-keepalive"], "CONFIG SET verify %d" % iteration)
        elif choice == 8:
            # Negative control for all-or-nothing validation: the valid first pair must not apply.
            config_get(["timeout"], "CONFIG invalid before %d" % iteration)
            both(["CONFIG", "SET", "timeout", "17", "ci-no-such-knob", "x"],
                 "CONFIG invalid multi %d" % iteration)
            config_get(["timeout"], "CONFIG invalid after %d" % iteration)
        else:
            both(["CONFIG", "RESETSTAT"], "CONFIG RESETSTAT %d" % iteration)
    both(["CONFIG", "SET", "timeout", "0", "tcp-keepalive", "300"], "CONFIG restore")
    config_get(["no-such-config-*", "still-no-such-*"], "CONFIG unknown multi")

    # CLIENT: exact grammar plus normalized INFO/LIST field names and stable state values. --------
    client_fields_seen = None
    expected_name = ""
    no_evict = no_touch = False
    lib_name = lib_ver = ""

    def client_info_fields(raw):
        body = parse_reply(raw)
        if not isinstance(body, bytes): return [], {}
        if body.startswith(b"txt:"): body = body[4:]
        words = body.strip().split()
        pairs = [(word.split(b"=", 1)[0], word.split(b"=", 1)[1])
                 for word in words if b"=" in word]
        return [key for key, _ in pairs], dict(pairs)

    def verify_client_info(label):
        nonlocal client_fields_seen
        target_raw = raw_command(ts, tf, ["CLIENT", "INFO"])
        oracle_raw = raw_command(os_, of, ["CLIENT", "INFO"])
        tnames, tmap = client_info_fields(target_raw)
        onames, omap = client_info_fields(oracle_raw)
        semantic(label + " field order", repr(tnames).encode(), repr(onames).encode())
        stable = (b"name", b"flags", b"db", b"sub", b"psub", b"ssub", b"multi", b"watch",
                  b"events", b"user", b"redir", b"resp", b"lib-name", b"lib-ver")
        semantic(label + " stable values", repr([(key, tmap.get(key)) for key in stable]).encode(),
                 repr([(key, omap.get(key)) for key in stable]).encode())
        if len(tnames) < 31: mismatch(label + " non-vacuity", repr(tnames).encode(), b">=31 fields")
        client_fields_seen = tnames
        fired["client_rows"] += 1

    for iteration in range(950):
        choice = rng.randrange(12)
        if choice == 0:
            expected_name = "ci_%d" % rng.randrange(100000)
            both(["CLIENT", "SETNAME", expected_name], "CLIENT SETNAME %d" % iteration)
        elif choice == 1:
            target, oracle = both(["CLIENT", "GETNAME"], "CLIENT GETNAME %d" % iteration)
            wanted = b"$-1\r\n" if not expected_name else \
                     b"$%d\r\n%s\r\n" % (len(expected_name), expected_name.encode())
            if target != wanted or oracle != wanted: mismatch("CLIENT GETNAME model", target, wanted)
        elif choice == 2:
            no_evict = bool(rng.randrange(2))
            both(["CLIENT", "NO-EVICT", "ON" if no_evict else "OFF"],
                 "CLIENT NO-EVICT %d" % iteration)
        elif choice == 3:
            no_touch = bool(rng.randrange(2))
            both(["CLIENT", "NO-TOUCH", "ON" if no_touch else "OFF"],
                 "CLIENT NO-TOUCH %d" % iteration)
        elif choice == 4:
            both(["CLIENT", "UNPAUSE"], "CLIENT UNPAUSE %d" % iteration)
        elif choice == 5:
            if rng.randrange(5) == 0:
                # Redis strings are binary-safe: NUL is accepted even though line/control
                # delimiters are rejected. This row caught TomoKV's former ch <= ' ' check.
                expected_name = "bad\x00name"
                both(["CLIENT", "SETNAME", expected_name], "CLIENT binary name %d" % iteration)
            else:
                bad = rng.choice(["bad name", "bad\nname", "bad\rname", "bad\tname", "bad\x7fname"])
                both(["CLIENT", "SETNAME", bad], "CLIENT invalid name %d" % iteration)
        elif choice == 6:
            lib_name = "lib%d" % rng.randrange(100)
            both(["CLIENT", "SETINFO", "LIB-NAME", lib_name], "CLIENT SETINFO name")
        elif choice == 7:
            lib_ver = "%d.%d" % (rng.randrange(10), rng.randrange(10))
            both(["CLIENT", "SETINFO", "LIB-VER", lib_ver], "CLIENT SETINFO version")
        elif choice in (8, 9):
            verify_client_info("CLIENT INFO %d" % iteration)
        elif choice == 10:
            tids = parse_reply(raw_command(ts, tf, ["CLIENT", "ID"]))
            oids = parse_reply(raw_command(os_, of, ["CLIENT", "ID"]))
            tid = int(tids[1:]) if isinstance(tids, bytes) and tids[:1] == b":" else 0
            oid = int(oids[1:]) if isinstance(oids, bytes) and oids[:1] == b":" else 0
            checks += 1
            if tid <= 0 or oid <= 0: mismatch("CLIENT ID positive", str(tid).encode(), str(oid).encode())
        else:
            # LIST is issued with each side's own id; compare one-row schema and stable state.
            tid_raw = parse_reply(raw_command(ts, tf, ["CLIENT", "ID"]))
            oid_raw = parse_reply(raw_command(os_, of, ["CLIENT", "ID"]))
            tid, oid = int(tid_raw[1:]), int(oid_raw[1:])
            traw = raw_command(ts, tf, ["CLIENT", "LIST", "ID", str(tid)])
            oraw = raw_command(os_, of, ["CLIENT", "LIST", "ID", str(oid)])
            tnames, tmap = client_info_fields(traw)
            onames, omap = client_info_fields(oraw)
            semantic("CLIENT LIST field order", repr(tnames).encode(), repr(onames).encode())
            stable = (b"name", b"flags", b"db", b"sub", b"psub", b"ssub", b"multi",
                      b"watch", b"user", b"redir", b"resp", b"lib-name", b"lib-ver")
            semantic("CLIENT LIST stable values",
                     repr([(key, tmap.get(key)) for key in stable]).encode(),
                     repr([(key, omap.get(key)) for key in stable]).encode())
            fired["client_rows"] += 1
    verify_client_info("CLIENT INFO final")
    if not client_fields_seen: mismatch("CLIENT INFO fired", b"none", b"fields")

    # ACL: exact user serialization, patterns, categories, and malformed-rule errors. ------------
    rules = [
        ["reset", "on", "nopass", "~ci:*", "&chan:*", "+get", "+set"],
        ["reset", "off", ">secret", "~*", "&*", "+@all"],
        ["reset", "on", "nopass", "resetkeys", "~a:*", "~b:*", "resetchannels",
         "&c:*", "+get", "+subscribe"],
    ]
    malformed = [["+nosuchcommand"], [">"], ["&"], ["~"], ["reset", "+@nosuchcategory"]]
    both(["ACL", "CAT"], "ACL CAT")
    both(["ACL", "WHOAMI"], "ACL WHOAMI")
    for iteration in range(1000):
        choice = rng.randrange(10)
        if choice < 3:
            both(["ACL", "SETUSER", "ci:diff"] + rng.choice(rules),
                 "ACL SETUSER %d" % iteration)
            fired["acl_mutations"] += 1
        elif choice == 3: both(["ACL", "GETUSER", "ci:diff"], "ACL GETUSER %d" % iteration)
        elif choice == 4: both(["ACL", "LIST"], "ACL LIST %d" % iteration)
        elif choice == 5: both(["ACL", "USERS"], "ACL USERS %d" % iteration)
        elif choice == 6:
            category = rng.choice(["string", "list", "set", "sortedset", "hash", "bitmap", "geo"])
            target = parse_reply(raw_command(ts, tf, ["ACL", "CAT", category]))
            oracle = parse_reply(raw_command(os_, of, ["ACL", "CAT", category]))
            semantic("ACL CAT %s" % category, repr(sorted(target)).encode(),
                     repr(sorted(oracle)).encode())
        elif choice == 7:
            both(["ACL", "SETUSER", "ci:bad"] + rng.choice(malformed),
                 "ACL malformed %d" % iteration)
        elif choice == 8:
            both(["ACL", "DELUSER", "ci:diff"], "ACL DELUSER %d" % iteration)
            fired["acl_mutations"] += 1
        else: both(["ACL", "GETUSER", "no-such-user"], "ACL missing user %d" % iteration)
    both(["ACL", "DELUSER", "ci:diff", "ci:bad"], "ACL cleanup")

    # Notification flags, routes, event names/order, and no-change controls. --------------------
    tks, tksf = conn(TH, TP); oks, oksf = conn(OH, OP)
    tke, tkef = conn(TH, TP); oke, okef = conn(OH, OP)
    subscribers = ((tks, tksf, "__keyspace@0__:ci:n:*"),
                   (oks, oksf, "__keyspace@0__:ci:n:*"),
                   (tke, tkef, "__keyevent@0__:*"),
                   (oke, okef, "__keyevent@0__:*"))
    for sock, file, pattern in subscribers:
        raw_command(sock, file, ["PSUBSCRIBE", pattern])

    def set_notify(flags):
        both(["CONFIG", "SET", "notify-keyspace-events", flags], "notify flags " + flags)

    def quiet(sock): return not _select.select([sock], [], [], 0.08)[0]

    def assert_silence(label, target_sock, oracle_sock):
        target_quiet = quiet(target_sock)
        oracle_quiet = quiet(oracle_sock)
        semantic(label, str(target_quiet).encode(), str(oracle_quiet).encode())
        if not target_quiet or not oracle_quiet:
            mismatch(label + " expected zero", str(target_quiet).encode(), b"True/True")
        fired["silence_controls"] += 1

    def notify_op(argv, keyspace_events, keyevent_events, label):
        both(argv, label + " reply")
        for index in range(keyspace_events):
            target = read_reply(tksf); oracle = read_reply(oksf)
            semantic(label + " keyspace event %d" % index, target, oracle)
            fired["notify_events"] += 1
        for index in range(keyevent_events):
            target = read_reply(tkef); oracle = read_reply(okef)
            semantic(label + " keyevent event %d" % index, target, oracle)
            fired["notify_events"] += 1

    set_notify("K$"); notify_op(["SET", "ci:n:string-k", "v"], 1, 0, "K string")
    assert_silence("K string keyevent silence", tke, oke)
    set_notify("E$"); notify_op(["SET", "ci:n:string-e", "v"], 0, 1, "E string")
    assert_silence("E string keyspace silence", tks, oks)
    set_notify("Eh"); notify_op(["HSET", "ci:n:hash", "f", "v"], 0, 1, "hash class")
    set_notify("El"); notify_op(["LPUSH", "ci:n:list", "v"], 0, 1, "list class")
    set_notify(""); notify_op(["SET", "ci:n:generic", "v"], 0, 0, "generic setup")
    set_notify("Eg"); notify_op(["DEL", "ci:n:generic"], 0, 1, "generic class")
    set_notify("Es"); notify_op(["SADD", "ci:n:set", "m"], 0, 1, "set class")
    notify_op(["SADD", "ci:n:set", "m"], 0, 0, "set no change")
    assert_silence("set no-change keyevent silence", tke, oke)
    set_notify("Ez"); notify_op(["ZADD", "ci:n:zset", "1", "m"], 0, 1, "zset class")
    set_notify("Et"); notify_op(["XADD", "ci:n:stream", "1-0", "f", "v"], 0, 1, "stream class")
    set_notify("Ex"); notify_op(["SET", "ci:n:expired", "v", "PX", "1"], 0, 0, "expired setup")
    time.sleep(0.02)
    notify_op(["GET", "ci:n:expired"], 0, 1, "expired class")
    set_notify("Em"); notify_op(["GET", "ci:n:missing"], 0, 1, "keymiss class")
    set_notify("En"); notify_op(["SET", "ci:n:new", "v"], 0, 1, "new class")
    set_notify("KEA")
    notify_op(["SET", "ci:n:ordered", "v", "EX", "1000"], 2, 2, "set-expire ordering")
    set_notify(""); notify_op(["SET", "ci:n:disabled", "v"], 0, 0, "disabled control")
    assert_silence("disabled keyspace silence", tks, oks)
    assert_silence("disabled keyevent silence", tke, oke)

    for sock in (tks, oks, tke, oke): sock.close()
    for sock in (ts, os_): sock.close()

    minimums = {"getkeys": 1500, "config_multi": 100, "client_rows": 50,
                "acl_mutations": 100, "notify_events": 10, "silence_controls": 5}
    for name, minimum in minimums.items():
        if fired[name] < minimum:
            mismatch("non-vacuity " + name, str(fired[name]).encode(), str(minimum).encode())
    print("DIFFER compatintro: %d checks, %d diffs -> %s (%s)" %
          (checks, diffs, "PASS" if diffs == 0 else "FAIL",
           ", ".join("%s=%d" % item for item in fired.items())))
    return diffs


if SUITE == "compatintro":
    sys.exit(1 if run_compatintro_suite(rng) else 0)


# ---- Lane t-aclsel: ACL selector grammar/reporting/enforcement differential -------------------
def run_cmdmeta_suite(rng):
    """Cold command metadata: 4,200 byte comparisons plus inventory/category properties."""
    subcommands = """
acl|cat acl|deluser acl|dryrun acl|genpass acl|getuser acl|help acl|list acl|load acl|log
acl|save acl|setuser acl|users acl|whoami client|caching client|getname client|getredir
client|help client|id client|info client|kill client|list client|no-evict client|no-touch
client|pause client|reply client|setinfo client|setname client|tracking client|trackinginfo
client|unblock client|unpause cluster|addslots cluster|addslotsrange cluster|bumpepoch
cluster|count-failure-reports cluster|countkeysinslot cluster|delslots cluster|delslotsrange
cluster|failover cluster|flushslots cluster|forget cluster|getkeysinslot cluster|help cluster|info
cluster|keyslot cluster|links cluster|meet cluster|myid cluster|myshardid cluster|nodes
cluster|replicas cluster|replicate cluster|reset cluster|saveconfig cluster|set-config-epoch
cluster|setslot cluster|shards cluster|slaves cluster|slots command|count command|docs
command|getkeys command|getkeysandflags command|help command|info command|list config|get
config|help config|resetstat config|rewrite config|set function|delete function|dump
function|flush function|help function|kill function|list function|load function|restore
function|stats latency|doctor latency|graph latency|help latency|histogram latency|history
latency|latest latency|reset memory|doctor memory|help memory|malloc-stats memory|purge
memory|stats memory|usage module|help module|list module|load module|loadex module|unload
object|encoding object|freq object|help object|idletime object|refcount pubsub|channels
pubsub|help pubsub|numpat pubsub|numsub pubsub|shardchannels pubsub|shardnumsub script|debug
script|exists script|flush script|help script|kill script|load slowlog|get slowlog|help
slowlog|len slowlog|reset xgroup|create xgroup|createconsumer xgroup|delconsumer
xgroup|destroy xgroup|help xgroup|setid xinfo|consumers xinfo|groups xinfo|help xinfo|stream
""".split()
    key_cases = [
        ["GET", "k"], ["MGET", "a", "b"], ["EXISTS", "a", "b"], ["DEL", "a", "b"],
        ["SET", "k", "v"], ["SET", "k", "v", "GET"], ["SETNX", "k", "v"],
        ["MSET", "a", "1", "b", "2"], ["MSETNX", "a", "1", "b", "2"],
        ["GETDEL", "k"], ["GETEX", "k", "PERSIST"], ["APPEND", "k", "x"],
        ["EXPIRE", "k", "1"], ["ZADD", "z", "1", "m"], ["ZREM", "z", "m"],
        ["SREM", "s", "m"], ["LPUSH", "l", "v"], ["HSET", "h", "f", "v"],
        ["XADD", "x", "*", "f", "v"], ["XDEL", "x", "1-0"],
        ["RENAME", "a", "b"], ["RENAMENX", "a", "b"], ["COPY", "a", "b"],
        ["SMOVE", "a", "b", "m"], ["LMOVE", "a", "b", "LEFT", "RIGHT"],
        ["BRPOP", "a", "b", "0"], ["LMPOP", "2", "a", "b", "LEFT"],
        ["BLMPOP", "0", "2", "a", "b", "LEFT"], ["ZUNION", "2", "a", "b"],
        ["ZUNIONSTORE", "d", "2", "a", "b"], ["SINTERSTORE", "d", "a", "b"],
        ["PFCOUNT", "a", "b"], ["PFMERGE", "d", "a", "b"],
        ["BITOP", "AND", "d", "a", "b"], ["BITFIELD", "k", "GET", "u8", "0"],
        ["BITFIELD", "k", "SET", "u8", "0", "1"], ["LCS", "a", "b"],
        ["EVAL", "return 1", "2", "a", "b"], ["EVAL_RO", "return 1", "2", "a", "b"],
        ["XREAD", "STREAMS", "a", "b", "0", "0"],
        ["XREADGROUP", "GROUP", "g", "c", "STREAMS", "a", "b", ">", ">"],
        ["GEORADIUS", "g", "0", "0", "1", "km", "STORE", "d"],
        ["SORT", "s", "BY", "w:*", "STORE", "d"], ["SORT_RO", "s", "BY", "w:*"],
        ["OBJECT", "ENCODING", "k"], ["MEMORY", "USAGE", "k"],
        ["XGROUP", "CREATE", "x", "g", "$"], ["XINFO", "STREAM", "x"],
        ["ZUNION", "+1", "a", "b"], ["LMPOP", "01", "a", "b", "LEFT"],
        ["SORT", "s", "STORE", "d", "STORE", "e"], ["OBJECT", "BOGUS", "k"],
    ]
    ts, tf = conn(TH, TP)
    os_, of = conn(OH, OP)
    diffs = 0
    compared = 0
    fired = {"info": 0, "intent": 0, "pipes": 0, "categories": 0,
             "docs_boundary": 0, "zero_controls": 0}

    def command(sock, file, argv):
        sock.sendall(enc(argv))
        return read_reply(file)

    def mismatch(label, target, oracle):
        nonlocal diffs
        diffs += 1
        if diffs <= 12:
            print("  DIFF %s\n    target: %r\n    oracle: %r" %
                  (label, target[:500], oracle[:500]))

    def compare(argv, label):
        nonlocal compared
        target = command(ts, tf, argv)
        oracle = command(os_, of, argv)
        compared += 1
        if target != oracle:
            mismatch(label, target, oracle)

    # Compare the pipe-qualified inventory as a set: raw order is explicitly unordered.
    lists = []
    for sock, file in ((ts, tf), (os_, of)):
        value = parse_reply(command(sock, file, ["COMMAND", "LIST"]))
        lists.append({item for item in value if b"|" in item} if isinstance(value, list) else set())
    expected = {name.encode() for name in subcommands}
    if lists[0] != expected or lists[1] != expected:
        mismatch("129 pipe-qualified LIST rows", repr(sorted(lists[0])).encode(),
                 repr(sorted(lists[1])).encode())
    fired["pipes"] = len(lists[0])

    for category in ("stream", "pubsub", "admin", "connection"):
        values = []
        for sock, file in ((ts, tf), (os_, of)):
            parsed = parse_reply(command(sock, file, ["ACL", "CAT", category]))
            values.append(sorted(item for item in parsed if b"|" in item))
        if values[0] != values[1]:
            mismatch("ACL CAT %s pipe rows" % category,
                     repr(values[0]).encode(), repr(values[1]).encode())
        fired["categories"] += len(values[0])

    # COUNT excludes subcommands on both servers. The unrelated nine top-level command gaps are
    # outside this lane, so compare each side against its own non-pipe LIST cardinality.
    for side, sock, file in (("target", ts, tf), ("oracle", os_, of)):
        listing = parse_reply(command(sock, file, ["COMMAND", "LIST"]))
        count = parse_reply(command(sock, file, ["COMMAND", "COUNT"]))
        value = int(count[1:]) if isinstance(count, bytes) and count[:1] == b":" else -1
        top = sum(b"|" not in name for name in listing)
        if value != top:
            mismatch("COUNT top-level property " + side, str(value).encode(), str(top).encode())

    # The prose corpus is intentionally not embedded. Prove the boundary actually differs and
    # that both sides returned documentation rather than letting a vacuous omission pass.
    target_docs = command(ts, tf, ["COMMAND", "DOCS", "config|get"])
    oracle_docs = command(os_, of, ["COMMAND", "DOCS", "config|get"])
    if target_docs == oracle_docs or b"tomokv compatible config|get command" not in target_docs or \
            b"Returns the effective values" not in oracle_docs:
        mismatch("DOCS explicit minimal-prose boundary", target_docs, oracle_docs)
    else:
        fired["docs_boundary"] = 1

    # Every direct subcommand row is deterministic; container child order is intentionally not.
    # Mix these with 48 key-spec shapes for 4,200 raw byte comparisons.
    for iteration in range(4200):
        if rng.randrange(2):
            name = rng.choice(subcommands)
            # Vary request case without changing the canonical lower-case reply.
            requested = "".join(ch.upper() if rng.randrange(2) else ch for ch in name)
            compare(["COMMAND", "INFO", requested], "INFO %d %s" % (iteration, requested))
            fired["info"] += 1
        else:
            case = list(rng.choice(key_cases))
            case[0] = "".join(ch.lower() if rng.randrange(2) else ch for ch in case[0])
            compare(["COMMAND", "GETKEYSANDFLAGS"] + case,
                    "intent %d %s" % (iteration, case[0]))
            fired["intent"] += 1

    compare(["COMMAND", "INFO", "nope|nope"], "unknown INFO zero control")
    compare(["COMMAND", "GETKEYSANDFLAGS", "GET", "k"], "RO/access zero control")
    fired["zero_controls"] = 2
    ts.close()
    os_.close()
    if fired["pipes"] != 129 or fired["info"] < 1900 or fired["intent"] < 1900 or \
            fired["categories"] < 40 or fired["docs_boundary"] != 1:
        mismatch("cmdmeta non-vacuity", repr(fired).encode(), b"required counters")
    print("DIFFER cmdmeta: %d ops, %d diffs -> %s (%s)" %
          (compared, diffs, "PASS" if diffs == 0 else "FAIL",
           ", ".join("%s=%d" % item for item in fired.items())))
    return diffs


def run_aclsel_suite(rng):
    """Byte-compare selector control-plane replies and commands admitted through selectors."""
    diffs = 0
    checks = 0
    fired = {"profiles": 0, "reporting": 0, "syntax": 0, "grants": 0,
             "command_denials": 0, "key_denials": 0, "channel_denials": 0}
    ts, tf = conn(TH, TP); os_, of = conn(OH, OP)

    def mismatch(label, target, oracle):
        nonlocal diffs
        diffs += 1
        if diffs <= 18:
            print("  DIFF %s\n    target: %r\n    oracle: %r" %
                  (label, target[:280], oracle[:280]))

    def raw(sock, file, argv):
        sock.sendall(enc(argv))
        return read_reply(file)

    def both(argv, label):
        nonlocal checks
        target = raw(ts, tf, argv)
        oracle = raw(os_, of, argv)
        checks += 1
        if target != oracle: mismatch(label, target, oracle)
        return target

    def user_both(target_pair, oracle_pair, argv, label):
        nonlocal checks
        target = raw(target_pair[0], target_pair[1], argv)
        oracle = raw(oracle_pair[0], oracle_pair[1], argv)
        checks += 1
        if target != oracle: mismatch(label, target, oracle)
        if target.startswith(b"-NOPERM"):
            if b"has no permissions to run" in target: fired["command_denials"] += 1
            elif b"access a key" in target: fired["key_denials"] += 1
            elif b"access a channel" in target: fired["channel_denials"] += 1
        elif not target.startswith(b"-"):
            fired["grants"] += 1
        return target

    # Only cleanup suite-owned state; neither cleanup reply is part of the differential stream.
    for sock, file in ((ts, tf), (os_, of)):
        raw(sock, file, ["ACL", "DELUSER", "aclsel:diff", "aclsel:bad"])
        raw(sock, file, ["FLUSHALL"])
    for key, value in (("as:a:1", "A"), ("as:b:1", "B"),
                       ("as:root:1", "R"), ("as:cat:1", "CAT")):
        both(["SET", key, value], "seed " + key)
    stats_before = target_stats()

    profiles = [
        {
            "rules": ["reset", "on", "nopass", "-@all", "resetkeys", "resetchannels",
                      "(~as:a:* +get +strlen +mget)", "(~as:b:* +set)",
                      "(&as:news:* +publish)"],
            "ops": [["GET", "as:a:1"], ["SET", "as:b:w", "v"],
                    ["GET", "as:b:1"], ["DEL", "as:a:1"],
                    ["PUBLISH", "as:news:x", "v"], ["PUBLISH", "as:other", "v"]],
        },
        {
            "rules": ["reset", "on", "nopass", "-@all", "resetkeys", "~as:root:*",
                      "+get", "allchannels", "(~as:a:* +set)",
                      "(&as:news:* +publish)"],
            "ops": [["GET", "as:root:1"], ["SET", "as:a:w", "v"],
                    ["GET", "as:a:1"], ["SET", "as:root:w", "v"],
                    ["PUBLISH", "as:news:x", "v"], ["PUBLISH", "as:other", "v"]],
        },
        {
            "rules": ["reset", "on", "nopass", "-@all", "resetkeys", "resetchannels",
                      "(~as:cat:* +@string -set)"],
            "ops": [["GET", "as:cat:1"], ["STRLEN", "as:cat:1"],
                    ["SET", "as:cat:w", "v"], ["GET", "as:a:1"],
                    ["PUBLISH", "as:news:x", "v"]],
        },
        {
            # Two selectors arrive fragmented across RESP arguments. A multi-key request must
            # be authorized by one whole selector, not by combining their key patterns.
            "rules": ["reset", "on", "nopass", "-@all", "resetkeys", "resetchannels",
                      "(", "~as:a:*", "+mget", ")", "(~as:b:* +mget)"],
            "ops": [["MGET", "as:a:1"], ["MGET", "as:b:1"],
                    ["MGET", "as:a:1", "as:b:1"], ["GET", "as:a:1"]],
        },
    ]

    # Establish an authenticated connection pair once; immutable ACL images update underneath it.
    both(["ACL", "SETUSER", "aclsel:diff"] + profiles[0]["rules"], "initial profile")
    fired["profiles"] += 1
    tus, tuf = conn(TH, TP); ous, ouf = conn(OH, OP)
    user_both((tus, tuf), (ous, ouf), ["AUTH", "aclsel:diff", "unused"], "AUTH")

    # Directed pass guarantees every profile and every denial family fires for every seed.
    for profile_index, profile in enumerate(profiles):
        both(["ACL", "SETUSER", "aclsel:diff"] + profile["rules"],
             "directed profile %d" % profile_index)
        fired["profiles"] += 1
        both(["ACL", "GETUSER", "aclsel:diff"], "directed GETUSER %d" % profile_index)
        fired["reporting"] += 1
        for operation in profile["ops"]:
            user_both((tus, tuf), (ous, ouf), operation,
                      "directed profile %d %s" % (profile_index, operation[0]))

    invalid = [
        ["ACL", "SETUSER", "aclsel:bad", "reset", "(on +get)"],
        ["ACL", "SETUSER", "aclsel:bad", "reset", "(>pw +get)"],
        ["ACL", "SETUSER", "aclsel:bad", "reset", "((~x:* +get))"],
        ["ACL", "SETUSER", "aclsel:bad", "reset", "(~x:*", "+get"],
        ["ACL", "SETUSER", "aclsel:bad", "reset", "~x:* +get)"],
    ]

    current = 0
    for iteration in range(4200):
        choice = rng.randrange(15)
        if choice < 2:
            current = rng.randrange(len(profiles))
            both(["ACL", "SETUSER", "aclsel:diff"] + profiles[current]["rules"],
                 "random profile %d" % iteration)
            fired["profiles"] += 1
        elif choice == 2:
            both(["ACL", "GETUSER", "aclsel:diff"], "random GETUSER %d" % iteration)
            fired["reporting"] += 1
        elif choice == 3:
            both(rng.choice(invalid), "random invalid selector %d" % iteration)
            fired["syntax"] += 1
        elif choice == 4:
            both(["ACL", "GETUSER", "aclsel:bad"], "invalid atomicity %d" % iteration)
            fired["syntax"] += 1
        else:
            operation = rng.choice(profiles[current]["ops"])
            user_both((tus, tuf), (ous, ouf), operation,
                      "random profile %d %s" % (current, operation[0]))

    after = target_stats()
    counter_minimums = {"acl_access_denied_cmd": 50,
                        "acl_access_denied_key": 50,
                        "acl_access_denied_channel": 50}
    for name, minimum in counter_minimums.items():
        delta = after.get(name, 0) - stats_before.get(name, 0)
        if delta < minimum:
            mismatch("target counter delta " + name, str(delta).encode(),
                     (">=%d" % minimum).encode())
    minimums = {"profiles": 100, "reporting": 100, "syntax": 200, "grants": 300,
                "command_denials": 100, "key_denials": 100, "channel_denials": 50}
    for name, minimum in minimums.items():
        if fired[name] < minimum:
            mismatch("non-vacuity " + name, str(fired[name]).encode(), str(minimum).encode())

    for sock, file in ((ts, tf), (os_, of)):
        raw(sock, file, ["ACL", "DELUSER", "aclsel:diff", "aclsel:bad"])
    for sock in (tus, ous, ts, os_): sock.close()
    print("DIFFER aclsel: %d checks, %d diffs -> %s (%s)" %
          (checks, diffs, "PASS" if diffs == 0 else "FAIL",
           ", ".join("%s=%d" % item for item in fired.items())))
    return diffs


if SUITE == "aclsel":
    sys.exit(1 if run_aclsel_suite(rng) else 0)
if SUITE == "cmdmeta":
    sys.exit(1 if run_cmdmeta_suite(rng) else 0)


def run_s6fix_suite(rng):
    """A4-A7 differential properties plus 4,000 byte-compared mixed operations.

    RANDOMKEY values and LASTSAVE seconds cannot be compared pairwise, and TomoKV's outer SCAN
    cursor deliberately carries a shard id. Compare their observable properties instead; retain
    byte comparison for every deterministic validation reply and for the randomized command mix.
    Both servers must be purpose-booted with appendonly disabled so WAITAOF's [0,0] control is
    meaningful.
    """
    ts, tf = conn(TH, TP)
    os_, of = conn(OH, OP)
    diffs = 0
    logical_ops = 0

    def command(sock, file, argv):
        nonlocal logical_ops
        sock.sendall(enc(argv))
        logical_ops += 1
        return read_reply(file)

    def mismatch(label, target, oracle):
        nonlocal diffs
        diffs += 1
        if diffs <= 12:
            print("  DIFF %s\n    target: %r\n    oracle: %r" % (label, target, oracle))

    def compare(argv, label=None):
        target = command(ts, tf, argv)
        oracle = command(os_, of, argv)
        if target != oracle:
            mismatch(label or " ".join(argv[:4]), target, oracle)
        return target, oracle

    for sock, file in ((ts, tf), (os_, of)):
        if command(sock, file, ["FLUSHALL"])[:1] != b"+":
            raise RuntimeError("FLUSHALL failed on s6fix clean-slate")
        configured = parse_reply(command(sock, file, ["CONFIG", "GET", "appendonly"]))
        if configured != [b"appendonly", b"no"]:
            raise RuntimeError("s6fix differ must be purpose-booted with appendonly no")

    # A4: values are random, so compare reachability and the zero-valued controls on each side.
    expected = {("s6d:rk:%03d" % i).encode() for i in range(200)}
    for key in sorted(expected):
        compare(["SET", key.decode(), "v"], "A4 setup %s" % key.decode())
    counts = []
    for sock, file in ((ts, tf), (os_, of)):
        seen = {}
        for base in range(0, 20000, 64):
            width = min(64, 20000 - base)
            sock.sendall(enc(["RANDOMKEY"]) * width)
            logical_ops += width
            for _ in range(width):
                value = parse_reply(read_reply(file))
                seen[value] = seen.get(value, 0) + 1
        counts.append(seen)
    for side, seen in zip(("target", "oracle"), counts):
        missing = sorted(expected - set(seen))
        unexpected = sorted(set(seen) - expected - {None}, key=repr)
        nulls = seen.get(None, 0)
        if missing:
            mismatch("A4 %s missing" % side, missing, [])
        if unexpected:
            mismatch("A4 %s unexpected-key control" % side, unexpected, [])
        if nulls:
            mismatch("A4 %s null control" % side, nulls, 0)
        print("  A4 %s distinct=%d missing=%d unexpected=%d nulls=%d" %
              (side, len(set(seen) & expected), len(missing), len(unexpected), nulls))

    # A5: independently exhaust each server's cursor, then compare the complete key sets.
    for sock, file in ((ts, tf), (os_, of)):
        command(sock, file, ["FLUSHALL"])
        command(sock, file, ["XADD", "s6d:stream", "1-0", "f", "v"])
        command(sock, file, ["SET", "s6d:string", "v"])

    def scan_type(sock, file, type_name):
        cursor = b"0"
        keys = []
        calls = 0
        while True:
            raw = command(sock, file,
                          ["SCAN", cursor.decode(), "COUNT", "10000", "TYPE", type_name])
            calls += 1
            if raw[:1] == b"-":
                return raw, keys, calls
            parsed = parse_reply(raw)
            if not isinstance(parsed, list) or len(parsed) != 2:
                return b"invalid reply shape", keys, calls
            cursor, page = parsed
            keys.extend(page)
            if cursor == b"0":
                return None, keys, calls
            if calls > 300:
                return b"cursor did not terminate", keys, calls

    stream_results = [scan_type(sock, file, "stream") for sock, file in ((ts, tf), (os_, of))]
    unknown_results = [scan_type(sock, file, "not-a-real-type")
                       for sock, file in ((ts, tf), (os_, of))]
    for label, results, want in (("A5 stream", stream_results, [b"s6d:stream"]),
                                 ("A5 unknown control", unknown_results, [])):
        for side, (error, keys, calls) in zip(("target", "oracle"), results):
            if error is not None:
                mismatch("%s %s error" % (label, side), error, None)
            if sorted(keys) != want:
                mismatch("%s %s keys" % (label, side), sorted(keys), want)
            print("  %s %s calls=%d keys=%r" % (label, side, calls, sorted(keys)))

    # A6: exact seconds differ because the processes start separately; positivity and no-future
    # are the oracle properties, and LASTSAVE=0 on the old target fails them.
    now = int(time.time())
    for side, sock, file in (("target", ts, tf), ("oracle", os_, of)):
        raw = command(sock, file, ["LASTSAVE"])
        try:
            value = int(raw[1:-2]) if raw[:1] == b":" else -1
        except ValueError:
            value = -1
        if value <= 0:
            mismatch("A6 %s positive" % side, value, "> 0")
        future_control = int(value > now + 1)
        if future_control:
            mismatch("A6 %s future control" % side, future_control, 0)
        print("  A6 %s lastsave=%d future_control=%d" % (side, value, future_control))

    # A7 and the two cheap SCAN cosmetic errors are byte-comparable.
    compare(["WAITAOF", "0", "0", "0"], "A7 AOF-off zero control")
    compare(["WAITAOF", "0", "-1", "0"], "A7 negative numreplicas")
    compare(["WAITAOF", "0", "0", "-1"], "A7 negative-timeout control")
    compare(["SCAN", "0", "COUNT", "abc"], "SCAN COUNT abc")
    compare(["SCAN", "0", "NOVALUES"], "SCAN NOVALUES")

    # Stable randomized byte comparisons keep this a real >=4k differ, not a handful of directed
    # probes wearing a suite name. Validation rows recur throughout the mix.
    keys = ["s6d:mix:%02d" % i for i in range(32)]
    for iteration in range(4000):
        key = rng.choice(keys)
        choice = rng.randrange(8)
        if choice == 0:
            argv = ["SET", key, "v%d" % rng.randrange(1000)]
        elif choice == 1:
            argv = ["GET", key]
        elif choice == 2:
            argv = ["DEL", key]
        elif choice == 3:
            argv = ["EXISTS", key]
        elif choice == 4:
            argv = ["TYPE", key]
        elif choice == 5:
            argv = ["WAITAOF", "0", "-1", str(rng.randrange(3))]
        elif choice == 6:
            argv = ["SCAN", "0", "COUNT", "abc"]
        else:
            argv = ["SCAN", "0", "NOVALUES"]
        compare(argv, "mixed op %d %s" % (iteration, argv[0]))

    ts.close()
    os_.close()
    print("DIFFER s6fix: %d logical ops, %d diffs -> %s" %
          (logical_ops, diffs, "PASS" if diffs == 0 else "FAIL"))
    return diffs


if SUITE == "s6fix":
    sys.exit(1 if run_s6fix_suite(rng) else 0)


def gen_infofix(rng):
    """Stable byte stream surrounding INFO's non-byte-comparable telemetry properties."""
    keys = ["ifxd:%02d" % index for index in range(32)]
    ops = []
    for iteration in range(4200):
        key = rng.choice(keys)
        choice = rng.randrange(9)
        if choice < 2:
            ops.append(["SET", key, "v:%d:%d" % (iteration, rng.randrange(100000))])
        elif choice == 2:
            ops.append(["GET", key])
        elif choice == 3:
            ops.append(["DEL", key])
        elif choice == 4:
            ops.append(["EXISTS", key])
        elif choice == 5:
            ops.append(["TYPE", key])
        elif choice == 6:
            ops.append(["APPEND", key, "x%d" % rng.randrange(100)])
        elif choice == 7:
            ops.append(["STRLEN", key])
        else:
            ops.append(["PING"])
    return ops

gens = {"string": gen_string, "list": gen_list, "set": gen_set, "zset": gen_zset,
        "hash": gen_hash, "hexpire": gen_hexpire, "edgetime": gen_edgetime,
        "xshard": gen_xshard, "xmove": gen_xmove, "bitmap": gen_bitmap,
        "hll": gen_hll, "bitfield": gen_bitfield, "cgaps": gen_cgaps, "stream": gen_stream,
        "script": gen_script,
        "streamgrp": gen_streamgrp,
        "zsetops": gen_zsetops, "geo": gen_geo, "doubles": gen_doubles,
        "scan": gen_scan, "multi": gen_multi,
        "edgeenc": gen_edgeenc, "edgeproto": gen_edgeproto, "cmdgap": gen_cmdgap,
        "cmdgap2": gen_cmdgap2,
        "sort": gen_sort,
        "servertail": gen_servertail, "arity": gen_arity,
        "storeorder": gen_storeorder, "infofix": gen_infofix}
if LIST_GENERATORS:
    # This is the single suite inventory. Property suites live outside `gens` because their
    # replies are not byte-comparable, but the gate discovers them from this same list.
    print("\n".join(list(gens) + ["blocking", "pubsub", "fanout", "spubsub", "notify",
                                   "wiredump", "climon", "compatintro", "aclsel", "cmdmeta", "s6fix"]))
    sys.exit(0)
ops = gens[SUITE](rng)

script_multi_declared = 0
if SUITE == "script":
    for command in ops:
        if command[0].upper() not in ("EVAL", "EVALSHA", "EVAL_RO", "EVALSHA_RO",
                                      "FCALL", "FCALL_RO"):
            continue
        try: script_multi_declared += int(command[2]) >= 2
        except (ValueError, IndexError): pass
    if script_multi_declared == 0:
        raise RuntimeError("script generator emitted zero multi-key activations")

ts, tf = conn(TH, TP)
os_, of = conn(OH, OP)
secondary = (conn(TH, TP), conn(OH, OP)) if SUITE == "script" else None
script_cross_generated = 0
if SUITE == "script":
    # Resolve geometry from the target on every boot: its hash seed is randomized. DEBUG is never
    # sent to Redis, and a declared-key count alone is not proof that an activation crossed an
    # owner boundary.
    routed = {}
    activations = []
    for command in ops:
        if command[0].upper() not in ("EVAL", "EVALSHA", "EVAL_RO", "EVALSHA_RO",
                                      "FCALL", "FCALL_RO"):
            continue
        try: count = int(command[2])
        except (ValueError, IndexError): continue
        declared = command[3:3 + count]
        activations.append(declared)
        for key in declared:
            if key in routed: continue
            ts.sendall(enc(["DEBUG", "SHARD", key]))
            geometry_reply = read_reply(tf)
            if not geometry_reply.startswith(b":"):
                raise RuntimeError("script differ requires target DEBUG SHARD, got %r" %
                                   (geometry_reply,))
            owner = int(geometry_reply[1:-2])
            routed[key] = owner
    script_cross_generated = sum(
        len({routed[key] for key in declared}) >= 2 for declared in activations)
    # A single-owner boot is the CONTROL leg, not a failure: the identical stream must still be
    # byte-exact against redis while driving none of the cross-owner engine. Anything else that
    # produces zero proven cross-owner activations is a blind generator, and a gate built on it
    # would pass while testing nothing.
    script_single_owner = len(set(routed.values())) <= 1
    if script_cross_generated == 0 and not script_single_owner:
        raise RuntimeError("script generator emitted zero proven cross-owner activations")
if SUITE == "sort":
    # Exact lane geometry, checked on the target only. DEBUG SHARD reports shard IDs; the gate uses
    # the default shard-home map, whose two executor slots own sid % 2. Do not let a same-owner or
    # one-executor boot turn this semantic differ into a vacuous cross-owner check.
    ts.sendall(enc(["INFO", "LB"]))
    placement_reply = parse_reply(read_reply(tf))
    if not isinstance(placement_reply, bytes):
        raise RuntimeError("sort differ requires target INFO LB")
    placement = {}
    for line in placement_reply.split(b"\r\n"):
        if line.startswith((b"lb_io_threads:", b"lb_ex_threads:")):
            name, value = line.split(b":", 1)
            placement[name] = int(value)
    if placement.get(b"lb_io_threads") != 6 or placement.get(b"lb_ex_threads") != 2:
        raise RuntimeError("sort differ requires --shards 16 --ratio 6:2, got %r" % placement)

    routed = {}
    def sort_shard(key):
        if key not in routed:
            ts.sendall(enc(["DEBUG", "SHARD", key]))
            reply = read_reply(tf)
            if not reply.startswith(b":"):
                raise RuntimeError("sort differ requires target DEBUG SHARD, got %r" % reply)
            routed[key] = int(reply[1:-2])
        return routed[key]

    observed = {sort_shard("sort:geometry:%04d" % candidate) for candidate in range(2048)}
    if observed != set(range(16)):
        raise RuntimeError("sort differ requires 16 observed shards, got %r" % sorted(observed))

    # Redis installs LC_COLLATE from its process environment while TomoKV deliberately retains the
    # C locale. Mixed-case/punctuation ALPHA weights below are a useful comparison only when both
    # sides therefore use byte order. Check the actual relation instead of trusting a boot note:
    # without LC_ALL=C, every so:a_* row becomes a locale artefact that looks like a bad gather.
    locale_key = "sort:locale:probe"
    locale_expected = [b"Echo!068", b"char-lie050"]
    for label, (sock, file) in (("target", (ts, tf)), ("oracle", (os_, of))):
        for command in (["DEL", locale_key],
                        ["RPUSH", locale_key, "char-lie050", "Echo!068"]):
            sock.sendall(enc(command))
            read_reply(file)
        sock.sendall(enc(["SORT_RO", locale_key, "ALPHA"]))
        locale_reply = parse_reply(read_reply(file))
        sock.sendall(enc(["DEL", locale_key]))
        read_reply(file)
        if locale_reply != locale_expected:
            raise RuntimeError("sort differ requires C collation on %s (boot Redis with "
                               "LC_ALL=C), got %r" % (label, locale_reply))

    # Reconstruct collection membership while walking the generated stream, then expand exactly
    # the patterns a SORT sees at that point. This proves a real generated BY and a real generated
    # GET cross the executor boundary; arbitrary probe names would prove only the hash function.
    members = {}
    by_candidates = []
    get_candidates = []

    def pattern_key(pattern, element):
        star = pattern.find("*")
        if star < 0: return None
        suffix = pattern[star + 1:]
        arrow = suffix.find("->")
        if arrow >= 0 and arrow + 2 < len(suffix): suffix = suffix[:arrow]
        return pattern[:star] + element + suffix

    for command in ops:
        name = command[0].upper()
        if name == "DEL":
            for key in command[1:]: members.pop(key, None)
        elif name in ("RPUSH", "SADD") and len(command) >= 3:
            members.setdefault(command[1], []).extend(command[2:])
        elif name == "ZADD" and len(command) >= 4:
            members.setdefault(command[1], []).extend(command[3::2])
        elif name in ("SORT", "SORT_RO") and len(command) >= 2:
            source = command[1]
            elements = members.get(source, [])
            by_pattern = None
            get_patterns = []
            valid = True
            index = 2
            while index < len(command):
                option = command[index].upper()
                if option in ("ALPHA", "ASC", "DESC"): index += 1
                elif option == "LIMIT" and index + 2 < len(command): index += 3
                elif option == "BY" and index + 1 < len(command):
                    by_pattern = command[index + 1]
                    index += 2
                elif option == "GET" and index + 1 < len(command):
                    get_patterns.append(command[index + 1])
                    index += 2
                elif option == "STORE" and index + 1 < len(command) and name == "SORT":
                    index += 2
                else:
                    valid = False
                    break
            if not valid: continue
            if by_pattern is not None:
                for element in elements:
                    derived = pattern_key(by_pattern, element)
                    if derived is not None:
                        by_candidates.append((source, derived, element, by_pattern))
            for pattern in get_patterns:
                if pattern == "#": continue
                for element in elements:
                    derived = pattern_key(pattern, element)
                    if derived is not None:
                        get_candidates.append((source, derived, element, pattern))

    def cross_owner(candidates):
        for source, derived, element, pattern in candidates:
            source_shard = sort_shard(source)
            derived_shard = sort_shard(derived)
            if source_shard % 2 != derived_shard % 2:
                return source, derived, element, pattern, source_shard, derived_shard
        return None

    sort_by_cross = cross_owner(by_candidates)
    sort_get_cross = cross_owner(get_candidates)
    if sort_by_cross is None or sort_get_cross is None:
        raise RuntimeError("sort generator found no cross-owner BY/GET pair: BY=%r GET=%r" %
                           (sort_by_cross, sort_get_cross))
    print("  sort geometry BY source=%s[%d] pattern=%s element=%s derived=%s[%d]" %
          (sort_by_cross[0], sort_by_cross[4], sort_by_cross[3], sort_by_cross[2],
           sort_by_cross[1], sort_by_cross[5]))
    print("  sort geometry GET source=%s[%d] pattern=%s element=%s derived=%s[%d]" %
          (sort_get_cross[0], sort_get_cross[4], sort_get_cross[3], sort_get_cross[2],
           sort_get_cross[1], sort_get_cross[5]))
# clean slates on BOTH sides: the oracle is long-lived across runs; residue there while the
# target boots fresh makes every op diff from op 0 (bit us on zset 2026-08-24). FLUSHALL (the
# target grew one with i-compat) rather than a DEL preamble: DEL-by-harvested-arg-1 missed any
# key a command names elsewhere (RENAME/STORE destinations live in arg 2+), which would re-open
# the residue trap for cross-shard suites. Replies are drained, NOT diffed.
for cs, cf in ((ts, tf), (os_, of)):
    cs.sendall(enc(["FLUSHALL"]))
    if read_reply(cf)[:1] != b"+": raise RuntimeError("FLUSHALL failed on clean-slate")
script_stats_before = target_stats() if SUITE == "script" else None
# OBJECT ENCODING is only comparable once both servers promote at the same sizes, and the two
# spell those knobs differently, so the alignment cannot ride in the diffed op stream. Replies are
# drained, NOT diffed -- the knob NAMES differ by design.
if SUITE in ("servertail", "edgeenc"):
    alignment = {
        0: [["CONFIG", "SET", "hash-max-compact-entries", "128"],
            ["CONFIG", "SET", "hash-max-compact-value", "64"],
            ["CONFIG", "SET", "set-max-compact-entries", "128"],
            ["CONFIG", "SET", "set-max-compact-value", "64"],
            ["CONFIG", "SET", "zset-max-compact-entries", "128"],
            ["CONFIG", "SET", "zset-max-compact-value", "64"],
            ["CONFIG", "SET", "list-max-compact-entries", "128"],
            ["CONFIG", "SET", "list-max-compact-value", "64"]],
        1: [["CONFIG", "SET", "hash-max-listpack-entries", "128"],
            ["CONFIG", "SET", "hash-max-listpack-value", "64"],
            ["CONFIG", "SET", "set-max-listpack-entries", "128"],
            ["CONFIG", "SET", "set-max-listpack-value", "64"],
            ["CONFIG", "SET", "set-max-intset-entries", "128"],
            ["CONFIG", "SET", "zset-max-listpack-entries", "128"],
            ["CONFIG", "SET", "zset-max-listpack-value", "64"],
            ["CONFIG", "SET", "list-max-listpack-size", "128"]],
    }
    for side, (cs, cf) in enumerate(((ts, tf), (os_, of))):
        for command in alignment[side]:
            cs.sendall(enc(command))
            if read_reply(cf)[:1] != b"+":
                raise RuntimeError("encoding alignment failed: %r" % command)
diffs = 0
# HLL's directed promotion stream uses many-argument PFADDs and byte-sized GET oracles, and the
# cgaps suite carries wide numkeys forms; keep their pipeline chunks below the target's fixed
# read-buffer rollover so the suites test semantics, not an unrelated transport boundary.
# storeorder needs a DEEP chunk on purpose: the destination install and the connection's later
# operations only overlap while the client is not waiting for replies, so a 64-op chunk would
# hide the very window the suite exists to cover.
BATCH = (1 if SUITE == "script" else
         16 if SUITE in ("hll", "cgaps", "cmdgap2") else
         512 if SUITE == "storeorder" else 64)
for i in range(0, len(ops), BATCH):
    chunk = ops[i:i + BATCH]
    if chunk[0][0] == "SECOND":
        command = chunk[0][1:]
        (tss, tsf), (oss, osf) = secondary
        tss.sendall(enc(command)); oss.sendall(enc(command))
        a = normalize(command[0], read_reply(tsf))
        b = normalize(command[0], read_reply(osf))
        if a != b:
            diffs += 1
            if diffs <= 12:
                print("  DIFF op %d secondary %r\n    target: %r\n    oracle: %r" %
                      (i, command[:4], a[:256], b[:256]))
        continue
    payload = b"".join(enc(o) for o in chunk)
    ts.sendall(payload); os_.sendall(payload)
    for j, o in enumerate(chunk):
        try:
            a = normalize(o[0], read_reply(tf))
        except TimeoutError:
            print("  TIMEOUT target op %d: %r" % (i + j, o[:8]), flush=True)
            raise
        try:
            b = normalize(o[0], read_reply(of))
        except TimeoutError:
            print("  TIMEOUT oracle op %d: %r" % (i + j, o[:8]), flush=True)
            raise
        a = normalize_introspection(o[0].upper(), o, a)
        b = normalize_introspection(o[0].upper(), o, b)
        if a != b:
            diffs += 1
            if diffs <= 12:
                shown_a = a if o[0].upper() == "KEYS" else a[:256]
                shown_b = b if o[0].upper() == "KEYS" else b[:256]
                shown_o = o if o[0].upper() in ("SORT", "SORT_RO") else o[:4]
                print("  DIFF op %d %r\n    target: %r\n    oracle: %r" %
                      (i + j, shown_o, shown_a, shown_b))
                # Scan-vs-point discriminator: when a KEYS listing disagrees, immediately probe
                # every disputed key with EXISTS and re-run KEYS on the target. A key that EXISTS
                # but was absent from the listing is a scan-visibility race; EXISTS==0 is loss;
                # a clean second KEYS marks the divergence transient.
                if o[0].upper() == "KEYS" and b":" in a and b":" in b:
                    seta = set(a.split(b"SORTED:")[-1].split(b","))
                    setb = set(b.split(b"SORTED:")[-1].split(b","))
                    # FRESH-connection probes only: probing on the differ's own pipelined
                    # connection desynchronizes the reply stream and self-inflicts a cascade
                    # (learned the hard way). A fresh conn also discriminates the mechanism:
                    # same-client RYOW-gate miss vs global publish-ordering gap.
                    try:
                        ps, pf = conn(TH, TP)
                        for miss in sorted((seta ^ setb))[:4]:
                            side = "target-missing" if miss in setb else "oracle-missing"
                            ps.sendall(enc(["EXISTS", miss.decode()]))
                            print("    PROBE(freshconn) %s %s EXISTS=%r" %
                                  (side, miss.decode()[:24], read_reply(pf)), flush=True)
                        ps.sendall(enc(["KEYS", "xs:*"]))
                        rekeys = normalize("KEYS", read_reply(pf))
                        print("    PROBE(freshconn) re-KEYS match-oracle=%r" % (rekeys == b),
                              flush=True)
                        ps.sendall(enc(["INFO", "STATS"]))
                        stats = read_reply(pf)
                        for want in (b"atomic_entries", b"atomic_pending_entries",
                                     b"atomic_promotions"):
                            row = [l for l in stats.split(b"\r\n") if l.startswith(want)]
                            if row:
                                print("    PROBE stat %s" % row[0].decode(), flush=True)
                        ps.close()
                    except Exception as probe_err:
                        print("    PROBE error: %r" % (probe_err,), flush=True)

if SUITE == "stream":
    # Auto IDs are clock-derived, and TomoKV intentionally refreshes its owner clock more
    # coarsely. Validate each server structurally and monotonically instead of hiding the entire
    # downstream command stream behind normalization.
    prior = [None, None]
    now_ms = int(time.time() * 1000)
    for iteration in range(8):
        for side, (sock, file) in enumerate(((ts, tf), (os_, of))):
            sock.sendall(enc(["XADD", "stream:auto", "*", "f", str(iteration)]))
            reply = read_reply(file)
            match = re.fullmatch(br"\$([0-9]+)\r\n([0-9]+)-([0-9]+)\r\n", reply)
            ok = match is not None
            if ok:
                ident = (int(match.group(2)), int(match.group(3)))
                ok = abs(ident[0] - now_ms) < 10000 and (prior[side] is None or ident > prior[side])
                prior[side] = ident
            if not ok:
                diffs += 1
                print("  AUTO-ID PROPERTY FAIL side=%s iteration=%d reply=%r" %
                      ("target" if side == 0 else "oracle", iteration, reply))

    # '~' is deliberately exact on TomoKV and node-approximate on Redis. The compatibility
    # contract is the property, not byte equality.
    for sock, file in ((ts, tf), (os_, of)):
        sock.sendall(enc(["DEL", "stream:approx"])); read_reply(file)
        for ident in range(1, 251):
            sock.sendall(enc(["XADD", "stream:approx", "%d-0" % ident, "f", "v"]))
            read_reply(file)
        sock.sendall(enc(["XTRIM", "stream:approx", "MAXLEN", "~", "25"]))
        read_reply(file)
        sock.sendall(enc(["XLEN", "stream:approx"]))
        length_reply = read_reply(file)
        try: length = int(length_reply[1:-2]) if length_reply[:1] == b":" else -1
        except ValueError: length = -1
        if length < 25:
            diffs += 1
            print("  APPROX-TRIM PROPERTY FAIL reply=%r" % length_reply)
if SUITE == "scan":
    # Cursor VALUES and emission ORDER are implementation-defined, so the walk cannot be byte
    # diffed. What is contractual, and what this checks, is the SET a completed walk yields: with
    # identical keyspaces on both servers it must be identical, at every COUNT. A COUNT small
    # enough to force many calls is the interesting one -- it is the number of resumptions, not
    # the number of keys, that exposes a cursor that cannot survive its own table.
    def read_value(file):
        line = file.readline()
        if not line: raise EOFError
        kind, body = line[:1], line[1:-2]
        if kind in b"+-:,#(_": return body
        if kind in b"$=!":
            n = int(body)
            return None if n == -1 else file.read(n + 2)[:-2]
        if kind in b"*~>":
            n = int(body)
            return None if n == -1 else [read_value(file) for _ in range(n)]
        if kind == b"%":
            return [read_value(file) for _ in range(int(body) * 2)]
        raise RuntimeError("scan property: unexpected reply %r" % line)

    def walk_to_end(sock, file, args, count, step):
        cursor, out, calls = b"0", [], 0
        while True:
            sock.sendall(enc(args + [cursor, "COUNT", str(count)]))
            reply = read_value(file)
            calls += 1
            cursor, batch = reply[0], reply[1]
            out.extend(batch[i] for i in range(0, len(batch), step))
            if cursor == b"0": break
            if calls > 200000:
                raise RuntimeError("%s cursor never returned to 0" % args[0])
        return sorted(set(out)), calls, len(out)

    for label, args, step in (("SCAN", ["SCAN"], 1),
                              ("HSCAN", ["HSCAN", "s:hbig"], 2),
                              ("SSCAN", ["SSCAN", "s:sbig"], 1),
                              ("ZSCAN", ["ZSCAN", "s:zbig"], 2)):
        for count in (7, 50, 400):
            tset, tcalls, temitted = walk_to_end(ts, tf, args, count, step)
            oset, ocalls, oemitted = walk_to_end(os_, of, args, count, step)
            if tset != oset:
                diffs += 1
                missing = [x for x in oset if x not in set(tset)]
                extra = [x for x in tset if x not in set(oset)]
                print("  SCAN-COMPLETENESS FAIL %s COUNT=%d: target %d unique in %d calls "
                      "(%d emitted), oracle %d unique in %d calls; MISSING from target %r; "
                      "EXTRA %r" % (label, count, len(tset), tcalls, temitted, len(oset), ocalls,
                                    missing[:6], extra[:6]))
            elif not tset:
                diffs += 1
                print("  SCAN-COMPLETENESS FAIL %s COUNT=%d: both sides empty, the check is "
                      "vacuous" % (label, count))

if SUITE == "infofix":
    # INFO is telemetry, so its values cannot be byte-compared across two implementations. Keep
    # the 4200-command state stream byte-exact above, then validate the same invariants on each side.
    def issue(sock, file, argv):
        sock.sendall(enc(argv))
        return read_reply(file)

    def fields(sock, file, section):
        parsed = parse_reply(issue(sock, file, ["INFO", section]))
        if not isinstance(parsed, bytes): return {}
        out = {}
        for line in parsed.split(b"\r\n"):
            if b":" in line:
                name, value = line.split(b":", 1)
                out[name.decode()] = value.decode(errors="replace")
        return out

    def property_fail(label, detail):
        global diffs
        diffs += 1
        print("  INFOFIX PROPERTY FAIL %s: %s" % (label, detail))

    # RESETSTAT byte baselines: before the INFO reply itself is sent, the only post-reset output is
    # +OK and the only input is this exact INFO request. This is the zero-delta control for both
    # byte detectors.
    info_request_bytes = len(enc(["INFO", "STATS"]))
    for side, (sock, file) in (("target", (ts, tf)), ("oracle", (os_, of))):
        reset = issue(sock, file, ["CONFIG", "RESETSTAT"])
        stats = fields(sock, file, "stats")
        got = (int(stats.get("total_net_input_bytes", "-1")),
               int(stats.get("total_net_output_bytes", "-1")))
        want = (info_request_bytes, len(reset))
        if got != want: property_fail(side + " byte control", "got=%r want=%r" % (got, want))
    print("  infofix byte controls: target+oracle exact")

    # Commandstats deliberately exposes only the one member TomoKV measures.
    issue(ts, tf, ["PING"])
    commandstats = fields(ts, tf, "commandstats")
    members = dict(item.split("=", 1)
                   for item in commandstats.get("cmdstat_ping", "").split(",") if "=" in item)
    if int(members.get("calls", "0")) < 1 or set(members) != {"calls"}:
        property_fail("commandstats members", repr(members))

    # Unsupported telemetry is absent, while the useful sampled and byte counters stay present.
    persistence = fields(ts, tf, "persistence")
    issue(ts, tf, ["SET", "ifxd:ttl", "v", "PX", "60000"])
    keyspace = fields(ts, tf, "keyspace")
    stats = fields(ts, tf, "stats")
    if "aof_delayed_fsync" in persistence:
        property_fail("delayed fsync omission", "row present")
    if "avg_ttl=" in keyspace.get("db0", ""):
        property_fail("avg_ttl omission", keyspace.get("db0", ""))
    for required in ("instantaneous_ops_per_sec", "total_net_input_bytes",
                     "total_net_output_bytes"):
        if required not in stats: property_fail("required metric", required + " absent")

    # Both implementations must retain a peak after current memory falls. TomoKV additionally
    # follows this lane's explicit RESETSTAT re-base requirement.
    memory_points = {}
    for side, (sock, file) in (("target", (ts, tf)), ("oracle", (os_, of))):
        issue(sock, file, ["FLUSHALL"])
        issue(sock, file, ["CONFIG", "RESETSTAT"])
        base = fields(sock, file, "memory")
        issue(sock, file, ["SET", "ifxd:peak", "x" * 512000])
        high = fields(sock, file, "memory")
        issue(sock, file, ["DEL", "ifxd:peak"])
        low = fields(sock, file, "memory")
        point = tuple(int(rows[name]) for rows in (base, high, low)
                      for name in ("used_memory", "used_memory_peak"))
        memory_points[side] = point
        base_used, _, high_used, high_peak, low_used, low_peak = point
        if not (high_used > base_used and low_used < high_used and
                high_peak >= high_used and low_peak >= high_peak):
            property_fail(side + " memory peak", repr(point))
    issue(ts, tf, ["CONFIG", "RESETSTAT"])
    reset_memory = fields(ts, tf, "memory")
    if reset_memory.get("used_memory_peak") != reset_memory.get("used_memory"):
        property_fail("target peak reset", repr(reset_memory))
    print("  infofix peak points: %r" % memory_points)

    # Sampled-rate controls: RESETSTAT+idle is zero, a byte-compared PING burst is positive, and a
    # second RESETSTAT returns the detector to zero.
    for sock, file in ((ts, tf), (os_, of)):
        issue(sock, file, ["CONFIG", "RESETSTAT"])
    time.sleep(.15)
    target_control = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "-1"))
    if target_control != 0: property_fail("target rate control", str(target_control))
    payload = enc(["PING"]) * 1000
    ts.sendall(payload); os_.sendall(payload)
    for iteration in range(1000):
        target_reply, oracle_reply = read_reply(tf), read_reply(of)
        if target_reply != oracle_reply:
            property_fail("rate burst byte compare", "iteration=%d" % iteration)
            break
    time.sleep(.12)
    target_rate = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "0"))
    oracle_rate = int(fields(os_, of, "stats").get("instantaneous_ops_per_sec", "0"))
    if target_rate <= 0: property_fail("target rate fired", str(target_rate))
    if oracle_rate <= 0: property_fail("oracle rate fired", str(oracle_rate))
    issue(ts, tf, ["CONFIG", "RESETSTAT"])
    time.sleep(.15)
    reset_rate = int(fields(ts, tf, "stats").get("instantaneous_ops_per_sec", "-1"))
    if reset_rate != 0: property_fail("target rate reset", str(reset_rate))
    print("  infofix sampled rates: control=%d target=%d oracle=%d reset=%d" %
          (target_control, target_rate, oracle_rate, reset_rate))

ts.close(); os_.close()
if secondary:
    secondary[0][0].close(); secondary[1][0].close()
if SUITE == "script":
    after = target_stats()
    required = ("script_stage_owner_tasks", "script_run_attempts",
                "script_validate_owner_tasks", "script_apply_owner_tasks",
                "script_crossshard_activations", "script_group_commits",
                "script_staged_bytes_total", "script_keys_armed")
    deltas = {name: after.get(name, 0) - script_stats_before.get(name, 0)
              for name in required}
    released = after.get("script_keys_released", 0) - \
        script_stats_before.get("script_keys_released", 0)
    live = after.get("script_intents_live", -1)
    if script_single_owner:
        # CONTROL LEG. Same stream, one owner: every counter above must stay at zero, so a build
        # that silently routed single-owner scripts through the staged engine fails here.
        if any(value != 0 for value in deltas.values()):
            diffs += 1
            print("  MECHANISM FAIL single-owner control drove the cross engine deltas=%r" %
                  (deltas,))
        else:
            print("  script mechanism SINGLE-OWNER CONTROL: cross engine idle, deltas all zero")
    elif any(value <= 0 for value in deltas.values()):
        diffs += 1
        print("  MECHANISM FAIL generated_cross=%d deltas=%r" %
              (script_cross_generated, deltas))
    elif after.get("script_group_occ_giveups", 0) != \
            script_stats_before.get("script_group_occ_giveups", 0):
        diffs += 1
        print("  MECHANISM FAIL unexpected OCC giveup")
    elif deltas["script_keys_armed"] != released or live != 0:
        # Reservations must balance: an armed key that was never released would arm every later
        # write to it for the life of the process.
        diffs += 1
        print("  MECHANISM FAIL reservations leaked armed=%d released=%d live=%d" %
              (deltas["script_keys_armed"], released, live))
    else:
        print("  script mechanism generated_cross=%d deltas=%r live=%d" %
              (script_cross_generated, deltas, live))
print("DIFFER %s: %d ops, %d diffs -> %s" % (SUITE, len(ops), diffs, "PASS" if diffs == 0 else "FAIL"))
sys.exit(1 if diffs else 0)
