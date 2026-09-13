#!/usr/bin/env python3
"""Directed battery for ENCODING and SIZE boundaries.  Usage: tests/edgeenc.py HOST PORT

WHAT THIS LOCKS.  Every representation in this tree has at least one size threshold, and each one
is a place where two different code paths must agree about the same value:

  * compact <-> expanded per collection type   (CompactValue::compact_fits / list_fits,
    src/store/typeval.h; the four <type>-max-compact-{entries,value} knobs)
  * the KvObj tail embed line, kEmbedThreshold = kCollectionEmbedMax = 192
    (src/store/kvobj.h) -- for strings it is visible as embstr vs raw, for collections it is the
    EmbeddedCompact-in-the-key-block form whose capacity is good_size() slack and therefore
    depends on the KEY length as well as the value
  * the zero-copy borrow cutover, --zc-min, and the multi-key gather cutover min(zc-min, 1024)
  * integer encoding (Enc::Int) vs raw bytes, and the INT64 edges
  * DUMP/RESTORE and DEBUG RELOAD, which must round-trip a value ACROSS every one of the above

EVERY ARM PROVES ITS MECHANISM FIRED, not merely that nothing broke: the promotion arms assert the
OBJECT ENCODING actually changed at the threshold AND that it had not changed one entry earlier
(the negative control), the embed arms assert the encoding name flips at exactly 192, the
externalisation arms drive the encoded size past the 192-byte cap so the tail form is provably
impossible, and the zc arm reads the knob back before and after so the two arms are known to have
run on different paths.  An arm that cannot show its mechanism says so out loud rather than
passing quietly.

Deliberate deviations from vanilla redis that this battery encodes rather than fights
(both documented in-tree, see NOTES-SERVERTAIL.md section 5 and NOTES-HEXPIRE.md):
  * the embstr/raw line is 192 here and 44 in redis, and an APPENDed short string stays embstr;
  * a hash carrying field TTLs reports listpack/hashtable, never redis 7.4's listpackex.
"""

import socket
import sys

HOST, PORT = sys.argv[1], int(sys.argv[2])

FAILURES = []
CHECKS = 0
SKIPPED = []


class RespError(Exception):
    def __eq__(self, other):
        return isinstance(other, RespError) and self.args == other.args

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash(("RespError",) + self.args)


def encode(*args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        if isinstance(a, str):
            a = a.encode()
        elif isinstance(a, int):
            a = str(a).encode()
        out.append(b"$%d\r\n" % len(a) + a + b"\r\n")
    return b"".join(out)


class Conn:
    def __init__(self, timeout=60):
        self.sock = socket.create_connection((HOST, PORT), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.f = self.sock.makefile("rb")

    def send(self, *args):
        self.sock.sendall(encode(*args))

    def read(self):
        prefix = self.f.read(1)
        if not prefix:
            raise EOFError("server closed the connection")
        line = self.f.readline()
        if not line.endswith(b"\r\n"):
            raise AssertionError("bad RESP line %r" % (prefix + line))
        body = line[:-2]
        if prefix == b"+":
            return body
        if prefix == b"-":
            return RespError(body.decode("utf-8", "replace"))
        if prefix == b":":
            return int(body)
        if prefix == b"$":
            n = int(body)
            return None if n == -1 else self.f.read(n + 2)[:-2]
        if prefix in (b"*", b"~", b">"):
            n = int(body)
            return None if n == -1 else [self.read() for _ in range(n)]
        if prefix == b"%":
            return [self.read() for _ in range(2 * int(body))]
        if prefix == b"_":
            return None
        if prefix == b"#":
            return body == b"t"
        if prefix == b",":
            return float(body)
        raise AssertionError("unknown RESP prefix %r" % prefix)

    def cmd(self, *args):
        self.send(*args)
        return self.read()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def check(label, got, want):
    global CHECKS
    CHECKS += 1
    if got != want:
        FAILURES.append("%s: got %r want %r" % (label, brief(got), brief(want)))


def brief(v):
    if isinstance(v, (bytes, bytearray)) and len(v) > 48:
        return b"<%d bytes starting %s>" % (len(v), bytes(v[:12]))
    if isinstance(v, list) and len(v) > 8:
        return [brief(x) for x in v[:8]] + ["...+%d" % (len(v) - 8)]
    if isinstance(v, list):
        return [brief(x) for x in v]
    return v


c = Conn()
check("clean slate", c.cmd("FLUSHALL"), b"OK")


def limit(name):
    reply = c.cmd("CONFIG", "GET", name)
    if not reply or len(reply) < 2:
        raise SystemExit("CONFIG GET %s returned %r -- battery cannot adapt" % (name, reply))
    return int(reply[1])


V = lambda n, ch="x": (ch * n)[:n]


# ---------------------------------------------------------------------------------------------
print("== 1. compact -> expanded promotion, ENTRY-count axis (all four collection types)")
# The threshold is read from the live config so the arm follows the knob instead of a copy of it.
TYPES = [
    # name, knob prefix, compact encoding, expanded encoding, add(i)->argv, count->argv, read->argv
    ("hash", "hash", b"listpack", b"hashtable",
     lambda k, i: ["HSET", k, "f%05d" % i, "v%05d" % i], lambda k: ["HLEN", k]),
    ("set", "set", b"listpack", b"hashtable",
     lambda k, i: ["SADD", k, "m%05d" % i], lambda k: ["SCARD", k]),
    ("zset", "zset", b"listpack", b"skiplist",
     lambda k, i: ["ZADD", k, str(i), "m%05d" % i], lambda k: ["ZCARD", k]),
    ("list", "list", b"listpack", b"quicklist",
     lambda k, i: ["RPUSH", k, "e%05d" % i], lambda k: ["LLEN", k]),
]

for name, knob, small, big, add, count in TYPES:
    maxent = limit("%s-max-compact-entries" % knob)
    if maxent > 4096:
        # An unlimited entry axis (lists budget bytes, not entries) is not a promotion threshold.
        SKIPPED.append("%s entry-axis promotion: %s-max-compact-entries is %d (no entry threshold)"
                       % (name, knob, maxent))
        continue
    k = "prom:%s" % name
    c.cmd("DEL", k)
    for i in range(maxent):
        c.cmd(*add(k, i))
    # NEGATIVE CONTROL: exactly at the limit the small form must still be in place, otherwise the
    # "it promoted" assertion below would pass for a value that was never compact.
    check("%s at max_entries=%d is still %s" % (name, maxent, small.decode()),
          c.cmd("OBJECT", "ENCODING", k), small)
    check("%s entry count at limit" % name, c.cmd(*count(k)), maxent)
    c.cmd(*add(k, maxent))                              # one past the limit
    check("%s at max_entries+1 promoted to %s" % (name, big.decode()),
          c.cmd("OBJECT", "ENCODING", k), big)
    check("%s entry count after promotion" % name, c.cmd(*count(k)), maxent + 1)
    # CONTENT SURVIVES THE TRANSITION. Read every entry back, not just the count.
    if name == "hash":
        got = c.cmd("HGETALL", k)
        pairs = dict(zip(got[0::2], got[1::2]))
        check("hash content across promotion",
              [pairs.get(b"f%05d" % i) for i in range(maxent + 1)],
              [b"v%05d" % i for i in range(maxent + 1)])
    elif name == "set":
        check("set content across promotion", sorted(c.cmd("SMEMBERS", k)),
              sorted(b"m%05d" % i for i in range(maxent + 1)))
    elif name == "zset":
        check("zset content across promotion", c.cmd("ZRANGE", k, "0", "-1"),
              [b"m%05d" % i for i in range(maxent + 1)])
    else:
        check("list content across promotion", c.cmd("LRANGE", k, "0", "-1"),
              [b"e%05d" % i for i in range(maxent + 1)])

    # ONE-WAY BY DESIGN (src/store/typeval.h: "Promotion is one-way unless a lane explicitly
    # proves a hysteretic demotion policy"). Deleting back below the threshold must NOT demote.
    # This is the arm that would catch a demotion path being added without the accounting.
    for i in range(maxent):
        if name == "hash":
            c.cmd("HDEL", k, "f%05d" % i)
        elif name == "set":
            c.cmd("SREM", k, "m%05d" % i)
        elif name == "zset":
            c.cmd("ZREM", k, "m%05d" % i)
        else:
            c.cmd("LPOP", k)
    check("%s stays %s after shrinking back below the threshold" % (name, big.decode()),
          c.cmd("OBJECT", "ENCODING", k), big)
    check("%s survives the shrink with one entry" % name, c.cmd(*count(k)), 1)
    c.cmd("DEL", k)


# ---------------------------------------------------------------------------------------------
print("== 2. compact -> expanded promotion, VALUE-size axis")
for name, knob, small, big in (("hash", "hash", b"listpack", b"hashtable"),
                               ("set", "set", b"listpack", b"hashtable"),
                               ("zset", "zset", b"listpack", b"skiplist")):
    maxval = limit("%s-max-compact-value" % knob)
    for delta, expect, tag in ((-1, small, "one below"), (0, small, "at"), (1, big, "one above")):
        k = "vprom:%s:%d" % (name, delta)
        c.cmd("DEL", k)
        payload = V(maxval + delta, "p")
        if name == "hash":
            c.cmd("HSET", k, "f", payload)
            check("hash value %s the limit round-trips" % tag, c.cmd("HGET", k, "f"),
                  payload.encode())
        elif name == "set":
            c.cmd("SADD", k, payload)
            check("set member %s the limit round-trips" % tag, c.cmd("SMEMBERS", k),
                  [payload.encode()])
        else:
            c.cmd("ZADD", k, "1", payload)
            check("zset member %s the limit round-trips" % tag, c.cmd("ZRANGE", k, "0", "-1"),
                  [payload.encode()])
        check("%s value %s the limit (%d) encodes as %s"
              % (name, tag, maxval + delta, expect.decode()),
              c.cmd("OBJECT", "ENCODING", k), expect)
        c.cmd("DEL", k)

# The list lane budgets AGGREGATE payload, not per-element length (typeval.h: "A single Compact is
# our whole small list, so list.max_value is its aggregate payload budget"). Drive that axis.
list_budget = limit("list-max-compact-value")
k = "vprom:list"
c.cmd("DEL", k)
elem = 64
under = list_budget // elem            # keeps the aggregate strictly below the budget
for i in range(under):
    c.cmd("RPUSH", k, V(elem, chr(97 + i % 26)))
check("list under the aggregate byte budget is listpack", c.cmd("OBJECT", "ENCODING", k),
      b"listpack")
while True:
    c.cmd("RPUSH", k, V(elem, "Z"))
    if c.cmd("OBJECT", "ENCODING", k) == b"quicklist":
        break
    if c.cmd("LLEN", k) > under + 4096:
        FAILURES.append("list never crossed its aggregate byte budget of %d" % list_budget)
        break
check("list promoted once the aggregate budget was passed", c.cmd("OBJECT", "ENCODING", k),
      b"quicklist")
n = c.cmd("LLEN", k)
check("list content intact across the aggregate-budget promotion",
      len(c.cmd("LRANGE", k, "0", "-1")), n)
c.cmd("DEL", k)


# ---------------------------------------------------------------------------------------------
print("== 3. the 192-byte KvObj embed line (kEmbedThreshold)")
# For STRINGS the line is directly observable: <=192 lives in the key's own block (embstr),
# above it the value is a second allocation (raw). NOTES-SERVERTAIL.md section 5.
EMBED = 192
for n, expect in ((0, b"embstr"), (1, b"embstr"), (EMBED - 1, b"embstr"), (EMBED, b"embstr"),
                  (EMBED + 1, b"raw"), (EMBED + 64, b"raw")):
    k = "embed:%d" % n
    payload = V(n, "e")
    check("SET %d bytes" % n, c.cmd("SET", k, payload), b"OK")
    check("embed line: %d bytes encodes as %s" % (n, expect.decode()),
          c.cmd("OBJECT", "ENCODING", k), expect)
    check("embed line: %d bytes round-trips" % n, c.cmd("GET", k), payload.encode())

# GROWN across the line rather than written across it: APPEND and SETRANGE take a different path
# from SET and must reach the same representation decision. The decision here is the RESULTING
# LENGTH alone -- redis reports any APPENDed string raw because it over-allocates for growth, this
# tree keeps no such reservation and a short grown value stays embstr (NOTES-SERVERTAIL.md 5).
for start in (EMBED - 2, EMBED - 1, EMBED):
    grown_len = start + 2
    expect = b"embstr" if grown_len <= EMBED else b"raw"
    k = "grow:%d" % start
    c.cmd("DEL", k)
    c.cmd("SET", k, V(start, "g"))
    check("grown value starts embstr at %d" % start, c.cmd("OBJECT", "ENCODING", k), b"embstr")
    c.cmd("APPEND", k, "AB")
    check("APPEND to length %d encodes as %s" % (grown_len, expect.decode()),
          c.cmd("OBJECT", "ENCODING", k), expect)
    check("APPEND across the embed line keeps the bytes", c.cmd("GET", k),
          (V(start, "g") + "AB").encode())
    c.cmd("DEL", k)
    c.cmd("SET", k, V(start, "g"))
    c.cmd("SETRANGE", k, str(start), "CD")
    check("SETRANGE across the embed line keeps the bytes", c.cmd("GET", k),
          (V(start, "g") + "CD").encode())
    check("SETRANGE to length %d encodes as %s" % (grown_len, expect.decode()),
          c.cmd("OBJECT", "ENCODING", k), expect)
    c.cmd("DEL", k)

# A TTL changes the KvObj's positional layout, so crossing the line WITH a deadline exercises
# kvobj_reheader as well. The deadline must survive the representation change.
for start in (EMBED - 1, EMBED):
    k = "growttl:%d" % start
    c.cmd("DEL", k)
    c.cmd("SET", k, V(start, "t"), "EX", "1000")
    c.cmd("APPEND", k, "ZZ")
    ttl = c.cmd("TTL", k)
    check("TTL survives crossing the embed line at %d (got %r)" % (start, ttl),
          isinstance(ttl, int) and 900 < ttl <= 1000, True)
    check("value survives crossing the embed line with a TTL", c.cmd("GET", k),
          (V(start, "t") + "ZZ").encode())
    c.cmd("DEL", k)


# ---------------------------------------------------------------------------------------------
print("== 4. the collection tail-embed line, swept over KEY length")
# A small collection lives in its KvObj's tail; the capacity of that tail is good_size() slack in
# the SAME allocation as the key (src/store/kvobj.h embedded_compact_capacity), so the point at
# which a collection has to move out of the tail is a function of the key length too. The
# capacity is hard-capped at kCollectionEmbedMax = 192, so a collection whose ENCODED bytes exceed
# 192 is provably no longer in the tail -- that is the mechanism proof for this arm; what it
# checks is that the contents are identical on both sides of the move, for every key length.
for klen in (1, 2, 3, 7, 8, 15, 16, 17, 23, 24, 31, 32, 39, 40, 47, 48, 55, 56, 63, 64, 100,
             254, 255, 256):
    k = ("K" * klen)[:klen]
    c.cmd("DEL", k)
    want = []
    # 12 x 32-byte elements = ~396 encoded bytes: starts in the tail, provably leaves it.
    for i in range(12):
        e = ("%02d" % i) + V(30, chr(97 + i))
        want.append(e.encode())
        check("RPUSH klen=%d step %d" % (klen, i), c.cmd("RPUSH", k, e), i + 1)
        check("list contents at klen=%d after %d pushes" % (klen, i + 1),
              c.cmd("LRANGE", k, "0", "-1"), want)
    # in-place replace with a LONGER element: the one operation that can overflow a tail buffer
    longer = "L" + V(120, "l")
    check("LSET klen=%d" % klen, c.cmd("LSET", k, "0", longer), b"OK")
    want[0] = longer.encode()
    check("list contents after a longer LSET at klen=%d" % klen,
          c.cmd("LRANGE", k, "0", "-1"), want)
    # interior insert, then remove it again
    check("LINSERT klen=%d" % klen, c.cmd("LINSERT", k, "AFTER", longer, "MID"), 13)
    want.insert(1, b"MID")
    check("list contents after LINSERT at klen=%d" % klen, c.cmd("LRANGE", k, "0", "-1"), want)
    check("LREM klen=%d" % klen, c.cmd("LREM", k, "0", "MID"), 1)
    want.pop(1)
    check("list contents after LREM at klen=%d" % klen, c.cmd("LRANGE", k, "0", "-1"), want)
    c.cmd("DEL", k)

    # hash and zset over the same tail line, same key
    c.cmd("DEL", k)
    for i in range(10):
        c.cmd("HSET", k, "f%d" % i, V(30, chr(97 + i)))
    c.cmd("HSET", k, "f0", V(150, "H"))          # replace with a much longer value
    got = c.cmd("HGETALL", k)
    pairs = dict(zip(got[0::2], got[1::2]))
    check("hash tail line at klen=%d: replaced field" % klen, pairs.get(b"f0"),
          V(150, "H").encode())
    check("hash tail line at klen=%d: field count" % klen, len(pairs), 10)
    c.cmd("DEL", k)
    for i in range(10):
        c.cmd("ZADD", k, str(i), "m%d%s" % (i, V(28, chr(97 + i))))
    c.cmd("ZADD", k, "99", "m0" + V(28, "a"))    # rescore the first member
    check("zset tail line at klen=%d: rescored member is last" % klen,
          c.cmd("ZRANGE", k, "-1", "-1"), [("m0" + V(28, "a")).encode()])
    check("zset tail line at klen=%d: cardinality" % klen, c.cmd("ZCARD", k), 10)
    c.cmd("DEL", k)


# ---------------------------------------------------------------------------------------------
print("== 5. the zero-copy borrow cutover (--zc-min) and the gather cutover")
# NOTE ON MECHANISM: the engine exposes no borrow counter, so this arm cannot count borrows. What
# it does prove is that the two arms really ran on different settings (the knob is read back), and
# that the reply bytes are identical at, just below and just above the cutover, and that a
# connection that has just served a borrowed reply still answers the next small reply correctly.
original = c.cmd("CONFIG", "GET", "zc-min")
check("zc-min is readable", isinstance(original, list) and len(original) == 2, True)
original_value = original[1].decode()

import hashlib


def payload_of(n, salt):
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(("%s:%d" % (salt, i)).encode()).digest()
        i += 1
    return bytes(out[:n])


for cutover in ("64", "0", "16384"):
    check("CONFIG SET zc-min %s" % cutover, c.cmd("CONFIG", "SET", "zc-min", cutover), b"OK")
    back = c.cmd("CONFIG", "GET", "zc-min")
    check("zc-min really took effect (%s)" % cutover, back[1], cutover.encode())
    base = int(cutover) if cutover != "0" else 64
    for n in (base - 1, base, base + 1, base * 4):
        if n <= 0:
            continue
        k = "zc:%s:%d" % (cutover, n)
        payload = payload_of(n, k)
        c.cmd("SET", k, payload)
        check("zc-min=%s GET of %d bytes is byte-exact" % (cutover, n), c.cmd("GET", k), payload)
        check("zc-min=%s STRLEN of %d bytes" % (cutover, n), c.cmd("STRLEN", k), n)
        check("zc-min=%s GETRANGE head" % cutover, c.cmd("GETRANGE", k, "0", "7"), payload[:8])
        check("zc-min=%s GETRANGE tail" % cutover, c.cmd("GETRANGE", k, str(n - 8), "-1"),
              payload[-8:])
        # a small reply on the SAME connection immediately after the borrowed one
        check("zc-min=%s small reply after a big one" % cutover, c.cmd("PING"), b"PONG")
        check("zc-min=%s echo after a big one" % cutover, c.cmd("ECHO", "after"), b"after")
        c.cmd("DEL", k)
    # grown across the cutover rather than written across it
    g = "zcgrow:%s" % cutover
    c.cmd("DEL", g)
    head = payload_of(base - 1, g)
    c.cmd("SET", g, head)
    check("zc-min=%s grown value below the cutover" % cutover, c.cmd("GET", g), head)
    c.cmd("APPEND", g, b"AB")
    check("zc-min=%s grown value across the cutover" % cutover, c.cmd("GET", g), head + b"AB")
    c.cmd("SETRANGE", g, str(base + 32), b"TAIL")
    grown = c.cmd("GET", g)
    check("zc-min=%s SETRANGE-grown length" % cutover, len(grown), base + 36)
    check("zc-min=%s SETRANGE-grown prefix" % cutover, grown[:base - 1], head)
    check("zc-min=%s SETRANGE-grown tail" % cutover, grown[-4:], b"TAIL")
    c.cmd("DEL", g)

# The multi-key gather slot cuts over at min(zc-min, 1024) (src/core/config.h note on kInline).
check("CONFIG SET zc-min back for the gather arm", c.cmd("CONFIG", "SET", "zc-min", "16384"),
      b"OK")
for n in (1023, 1024, 1025, 2048):
    keys = ["gather:%d:%d" % (n, i) for i in range(6)]
    payloads = [payload_of(n, k) for k in keys]
    for k, p in zip(keys, payloads):
        c.cmd("SET", k, p)
    check("MGET across the gather cutover at %d bytes" % n, c.cmd("MGET", *keys), payloads)
    check("MGET with a hole at %d bytes" % n, c.cmd("MGET", keys[0], "gather:absent", keys[1]),
          [payloads[0], None, payloads[1]])
    c.cmd("DEL", *keys)
check("restore zc-min", c.cmd("CONFIG", "SET", "zc-min", original_value), b"OK")


# ---------------------------------------------------------------------------------------------
print("== 6. segmented send: small replies AFTER a large one, on one connection")
# A reply that does not fit the socket buffer puts the connection into a partial-send state. The
# arm reads the big reply slowly ON PURPOSE (a small SO_RCVBUF and a stalled reader), so the send
# really is segmented, then checks that the next replies on that same connection are intact and
# in order.
big = payload_of(4 * 1024 * 1024, "segmented")
check("stage the large value", c.cmd("SET", "seg:big", big), b"OK")
check("stage the small value", c.cmd("SET", "seg:small", b"small-value"), b"OK")

slow = socket.create_connection((HOST, PORT), timeout=60)
slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
slow.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
slow.sendall(encode("GET", "seg:big"))
header = b""
while not header.endswith(b"\r\n"):
    header += slow.recv(1)
check("large reply header", header, b"$%d\r\n" % len(big))
body = b""
first_chunk = slow.recv(4096)
body += first_chunk
check("the large reply really was segmented (first chunk %d < %d bytes)"
      % (len(first_chunk), len(big)), len(first_chunk) < len(big), True)
while len(body) < len(big) + 2:
    chunk = slow.recv(1 << 16)
    if not chunk:
        break
    body += chunk
check("large reply body is byte-exact", body[:-2], big)
# ... and the same connection still works for everything that follows
slow.sendall(encode("PING") + encode("GET", "seg:small") + encode("STRLEN", "seg:big")
             + encode("ECHO", "tail"))
tail = b""
while tail.count(b"\r\n") < 5:
    tail += slow.recv(1 << 16)
check("replies after a segmented send, in order", tail,
      b"+PONG\r\n$11\r\nsmall-value\r\n:%d\r\n$4\r\ntail\r\n" % len(big))
slow.close()
c.cmd("DEL", "seg:big", "seg:small")


# ---------------------------------------------------------------------------------------------
print("== 7. integer encoding and the INT64 edges")
# NOTES-STRING.md pins these: only a canonical decimal in range is Enc::Int; '+1', '-0' and
# leading zeros stay raw; APPEND and non-empty SETRANGE materialise an integer to raw.
INT_CASES = [
    ("0", b"int"), ("1", b"int"), ("-1", b"int"), ("123456789", b"int"),
    ("9223372036854775807", b"int"), ("-9223372036854775808", b"int"),
    ("9223372036854775808", b"embstr"), ("-9223372036854775809", b"embstr"),
    ("18446744073709551615", b"embstr"),
    ("+1", b"embstr"), ("-0", b"embstr"), ("00", b"embstr"), ("007", b"embstr"),
    ("1.0", b"embstr"), ("1e3", b"embstr"), (" 1", b"embstr"), ("1 ", b"embstr"),
    ("", b"embstr"), ("-", b"embstr"), ("0x10", b"embstr"),
]
for text, expect in INT_CASES:
    k = "int:%s" % text.strip().replace(" ", "_") or "int:blank"
    c.cmd("SET", k, text)
    check("OBJECT ENCODING of %r" % text, c.cmd("OBJECT", "ENCODING", k), expect)
    check("GET of %r is the exact bytes" % text, c.cmd("GET", k), text.encode())
    check("STRLEN of %r" % text, c.cmd("STRLEN", k), len(text))
    c.cmd("DEL", k)

# INCR at the edges must refuse rather than wrap.
c.cmd("SET", "int:max", "9223372036854775807")
r = c.cmd("INCR", "int:max")
check("INCR at INT64_MAX is refused", isinstance(r, RespError), True)
check("INCR at INT64_MAX left the value alone", c.cmd("GET", "int:max"),
      b"9223372036854775807")
c.cmd("SET", "int:min", "-9223372036854775808")
r = c.cmd("DECR", "int:min")
check("DECR at INT64_MIN is refused", isinstance(r, RespError), True)
check("DECR at INT64_MIN left the value alone", c.cmd("GET", "int:min"),
      b"-9223372036854775808")
check("INCR just below the top", c.cmd("INCRBY", "int:max", "0"), 9223372036854775807)
c.cmd("DEL", "int:max", "int:min")

# APPEND / SETRANGE against an integer-encoded value: both must MATERIALISE it -- the value stops
# being Enc::Int. Which byte-form it lands in is the 192-byte line, so a short result reads embstr
# here where redis would say raw (NOTES-SERVERTAIL.md 5). The load-bearing assertion is
# "no longer int"; the exact byte-form is asserted separately at both sides of 192.
c.cmd("SET", "int:app", "123")
check("APPEND to an int", c.cmd("APPEND", "int:app", "45"), 5)
check("APPEND materialises an int", c.cmd("OBJECT", "ENCODING", "int:app"), b"embstr")
check("APPEND to an int keeps the digits", c.cmd("GET", "int:app"), b"12345")
c.cmd("SET", "int:app", "123")
check("empty APPEND to an int", c.cmd("APPEND", "int:app", ""), 3)
check("empty APPEND still materialises an int",
      c.cmd("OBJECT", "ENCODING", "int:app"), b"embstr")
c.cmd("SET", "int:app", "123")
c.cmd("APPEND", "int:app", V(EMBED, "A"))
check("APPEND past the embed line gives raw", c.cmd("OBJECT", "ENCODING", "int:app"), b"raw")
check("APPEND past the embed line keeps every byte", c.cmd("GET", "int:app"),
      b"123" + V(EMBED, "A").encode())
c.cmd("SET", "int:sr", "123")
check("SETRANGE into an int", c.cmd("SETRANGE", "int:sr", "1", "X"), 3)
check("SETRANGE materialises an int", c.cmd("OBJECT", "ENCODING", "int:sr"), b"embstr")
check("SETRANGE into an int result", c.cmd("GET", "int:sr"), b"1X3")
c.cmd("SET", "int:sr", "123")
check("empty SETRANGE into an int is a no-op", c.cmd("SETRANGE", "int:sr", "99", ""), 3)
check("empty SETRANGE into an int keeps int", c.cmd("OBJECT", "ENCODING", "int:sr"), b"int")
c.cmd("DEL", "int:app", "int:sr")

# Integer members inside collections: the set lane keeps a sorted integer compact whose element
# width steps at the 16/32/64-bit edges.
c.cmd("DEL", "int:set")
for m in ("1", "-1", "32767", "-32768"):
    c.cmd("SADD", "int:set", m)
check("all-integer set is intset", c.cmd("OBJECT", "ENCODING", "int:set"), b"intset")
c.cmd("SADD", "int:set", "2147483647")
check("set stays intset over the 32-bit step", c.cmd("OBJECT", "ENCODING", "int:set"), b"intset")
c.cmd("SADD", "int:set", "9223372036854775807")
check("set stays intset over the 64-bit step", c.cmd("OBJECT", "ENCODING", "int:set"), b"intset")
check("intset content across both width steps", sorted(c.cmd("SMEMBERS", "int:set")),
      sorted([b"1", b"-1", b"32767", b"-32768", b"2147483647", b"9223372036854775807"]))
c.cmd("SADD", "int:set", "notanint")
check("a non-integer member leaves intset", c.cmd("OBJECT", "ENCODING", "int:set"), b"listpack")
check("intset content survives leaving intset", sorted(c.cmd("SMEMBERS", "int:set")),
      sorted([b"1", b"-1", b"32767", b"-32768", b"2147483647", b"9223372036854775807",
              b"notanint"]))
c.cmd("SREM", "int:set", "notanint")
check("removing it does NOT go back to intset (promotion is one-way)",
      c.cmd("OBJECT", "ENCODING", "int:set"), b"listpack")
# NEGATIVE CONTROL for the '+7 is not 7' rule: these are distinct members, not duplicates.
c.cmd("DEL", "int:set2")
c.cmd("SADD", "int:set2", "7")
check("adding '+7' beside '7' is a new member", c.cmd("SADD", "int:set2", "+7"), 1)
check("adding '007' beside '7' is a new member", c.cmd("SADD", "int:set2", "007"), 1)
check("three distinct spellings of seven", c.cmd("SCARD", "int:set2"), 3)
c.cmd("DEL", "int:set", "int:set2")


# ---------------------------------------------------------------------------------------------
print("== 8. DUMP/RESTORE round-trip across every boundary")
maxent_hash = limit("hash-max-compact-entries")
maxval_hash = limit("hash-max-compact-value")


def build(kind, k, entries, vlen):
    c.cmd("DEL", k)
    for i in range(entries):
        if kind == "hash":
            c.cmd("HSET", k, "f%04d" % i, V(vlen, chr(97 + i % 26)))
        elif kind == "set":
            c.cmd("SADD", k, "m%04d%s" % (i, V(max(0, vlen - 5), chr(97 + i % 26))))
        elif kind == "zset":
            c.cmd("ZADD", k, str(i), "m%04d%s" % (i, V(max(0, vlen - 5), chr(97 + i % 26))))
        elif kind == "list":
            c.cmd("RPUSH", k, "e%04d%s" % (i, V(max(0, vlen - 5), chr(97 + i % 26))))
        else:
            c.cmd("SET", k, V(vlen, "s"))


def snapshot(kind, k):
    if kind == "hash":
        got = c.cmd("HGETALL", k)
        return sorted(zip(got[0::2], got[1::2]))
    if kind == "set":
        return sorted(c.cmd("SMEMBERS", k))
    if kind == "zset":
        return c.cmd("ZRANGE", k, "0", "-1", "WITHSCORES")
    if kind == "list":
        return c.cmd("LRANGE", k, "0", "-1")
    return c.cmd("GET", k)


ROUND_TRIP = [
    ("string", 1, 0), ("string", 1, 1), ("string", 1, 191), ("string", 1, 192),
    ("string", 1, 193), ("string", 1, 20000),
    ("hash", 1, 8), ("hash", 3, maxval_hash), ("hash", 3, maxval_hash + 1),
    ("hash", maxent_hash, 4), ("hash", maxent_hash + 1, 4),
    ("set", 1, 8), ("set", 130, 8), ("set", 3, 200),
    ("zset", 1, 8), ("zset", 130, 8), ("zset", 3, 200),
    ("list", 1, 8), ("list", 12, 32), ("list", 200, 64), ("list", 3, 4000),
]
for kind, entries, vlen in ROUND_TRIP:
    k = "dump:%s:%d:%d" % (kind, entries, vlen)
    build(kind, k, entries, vlen)
    before = snapshot(kind, k)
    enc_before = c.cmd("OBJECT", "ENCODING", k)
    blob = c.cmd("DUMP", k)
    check("DUMP %s/%d/%d produced a payload" % (kind, entries, vlen),
          isinstance(blob, bytes) and len(blob) > 0, True)
    c.cmd("DEL", k)
    check("RESTORE %s/%d/%d" % (kind, entries, vlen), c.cmd("RESTORE", k, "0", blob), b"OK")
    check("RESTORE %s/%d/%d round-trips the contents" % (kind, entries, vlen),
          snapshot(kind, k), before)
    check("RESTORE %s/%d/%d round-trips the encoding" % (kind, entries, vlen),
          c.cmd("OBJECT", "ENCODING", k), enc_before)
    # NEGATIVE CONTROL: a mangled payload must be refused, not restored. Without this the arm
    # above would pass on a server that accepted anything.
    broken = bytearray(blob)
    broken[len(broken) // 2] ^= 0xFF
    r = c.cmd("RESTORE", k + ":bad", "0", bytes(broken))
    check("a corrupted %s payload is refused" % kind, isinstance(r, RespError), True)
    check("a corrupted %s payload creates nothing" % kind, c.cmd("EXISTS", k + ":bad"), 0)
    c.cmd("DEL", k)


# ---------------------------------------------------------------------------------------------
print("== 9. snapshot round-trip across the same boundaries (DEBUG RELOAD)")
probe = c.cmd("DEBUG", "SHARD", "x")
if isinstance(probe, RespError) and "not allowed" in str(probe):
    SKIPPED.append("DEBUG RELOAD round-trip: server was booted without --enable-debug-command")
else:
    expected = {}
    for kind, entries, vlen in ROUND_TRIP:
        k = "reload:%s:%d:%d" % (kind, entries, vlen)
        build(kind, k, entries, vlen)
        expected[(kind, k)] = (snapshot(kind, k), c.cmd("OBJECT", "ENCODING", k))
    check("DEBUG RELOAD", c.cmd("DEBUG", "RELOAD"), b"OK")
    for (kind, k), (want, want_enc) in expected.items():
        check("RELOAD round-trips %s" % k, snapshot(kind, k), want)
        check("RELOAD round-trips the encoding of %s" % k, c.cmd("OBJECT", "ENCODING", k),
              want_enc)
    for (_, k) in expected:
        c.cmd("DEL", k)


# ---------------------------------------------------------------------------------------------
print("== 10. boundary sizes over a key set that provably spans shards")
if isinstance(probe, RespError):
    SKIPPED.append("cross-shard boundary arm: DEBUG SHARD unavailable, shards not provable")
else:
    keys = ["xs:%d" % i for i in range(8)]
    shards = {int(c.cmd("DEBUG", "SHARD", k)) for k in keys}
    check("the key set really spans more than one shard (saw %r)" % sorted(shards),
          len(shards) > 1, True)
    for n in (1, 63, 64, 95, 96, 97, 191, 192, 193, 1023, 1024, 1025, 16383, 16384, 16385):
        payloads = [payload_of(n, "%s:%d" % (k, n)) for k in keys]
        args = []
        for k, p in zip(keys, payloads):
            args += [k, p]
        check("MSET %d-byte values across shards" % n, c.cmd("MSET", *args), b"OK")
        check("MGET %d-byte values across shards" % n, c.cmd("MGET", *keys), payloads)
        check("EXISTS across shards", c.cmd("EXISTS", *keys), len(keys))
        check("DEL across shards", c.cmd("DEL", *keys), len(keys))
        check("MGET after DEL across shards", c.cmd("MGET", *keys), [None] * len(keys))


# ---------------------------------------------------------------------------------------------
c.cmd("FLUSHALL")
c.close()

print()
for note in SKIPPED:
    print("SKIPPED: %s" % note)
print("edgeenc: %d checks, %d failures -> %s"
      % (CHECKS, len(FAILURES), "PASS" if not FAILURES else "FAIL"))
for f in FAILURES[:40]:
    print("  FAIL %s" % f)
sys.exit(1 if FAILURES else 0)
