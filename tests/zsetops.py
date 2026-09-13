#!/usr/bin/env python3
"""Directed zset multi-key battery. Usage: tests/zsetops.py HOST PORT"""

import socket
import sys

HOST, PORT = sys.argv[1], int(sys.argv[2])


class RespError(Exception):
    pass


def encode(*args):
    out = [f"*{len(args)}\r\n".encode()]
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        out += [f"${len(arg)}\r\n".encode(), arg, b"\r\n"]
    return b"".join(out)


class Conn:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=20)
        self.file = self.sock.makefile("rb")

    def command(self, *args):
        self.sock.sendall(encode(*args))
        return self.read()

    def read(self):
        marker = self.file.read(1)
        line = self.file.readline()[:-2]
        if marker == b"+": return line
        if marker == b"-": return RespError(line.decode("utf-8", "replace"))
        if marker == b":": return int(line)
        if marker == b",": return float(line)
        if marker == b"_": return None
        if marker == b"$":
            size = int(line)
            if size == -1: return None
            value = self.file.read(size)
            assert self.file.read(2) == b"\r\n"
            return value
        if marker in (b"*", b"~"):
            size = int(line)
            if size == -1: return None
            return [self.read() for _ in range(size)]
        raise AssertionError((marker, line))


def expect(actual, wanted, label):
    if actual != wanted:
        raise AssertionError(f"{label}: got {actual!r}, wanted {wanted!r}")
    print(f"  ok   {label}")


def expect_error(actual, text, label):
    if not isinstance(actual, RespError) or text not in str(actual):
        raise AssertionError(f"{label}: got {actual!r}")
    print(f"  ok   {label}")


def stats(c):
    raw = c.command("INFO", "STATS").decode()
    return {line.split(":", 1)[0]: line.split(":", 1)[1]
            for line in raw.splitlines() if ":" in line}


def make_zset(c, key, pairs, expanded):
    """Build `key` holding exactly `pairs`, in the compact (listpack) or expanded (skiplist) form.

    The encoding is ASSERTED rather than assumed. Every expectation in the two batteries below is
    encoding-selected, so a cell that silently ran in the wrong encoding would assert nothing --
    it would pass for the wrong reason. Padding to 200 entries promotes the key BEFORE the real
    members are added, which is what makes a stored -0 survive in the expanded arm.
    """
    c.command("DEL", key)
    if expanded:
        pad = []
        for i in range(200):
            pad += [str(10000 + i), f"pad{i}"]
        c.command("ZADD", key, *pad)
    args = []
    for score, member in pairs:
        args += [score, member]
    if args:
        c.command("ZADD", key, *args)
    if expanded:
        c.command("ZREMRANGEBYSCORE", key, "10000", "+inf")
    wanted = b"skiplist" if expanded else b"listpack"
    got = c.command("OBJECT", "ENCODING", key)
    if got != wanted:
        raise AssertionError(f"{key}: wanted {wanted!r} encoding, got {got!r}")
    return key


def negative_zero_battery(c):
    """The derived negative-zero RULE, one assertion per cell, byte-exact against redis 7.4.

    RULE 1 (storage): a score entering COMPACT storage loses the sign of a zero, because the
      oracle's listpack holds scores as text whose zero form is unsigned. The EXPANDED form keeps
      the sign bit. Promotion carries whatever the compact form already normalized.
    RULE 2 (aggregation): AGGREGATE MIN/MAX are strict comparisons against the running
      accumulator, so a tie keeps the EARLIER source's value. -0 == +0, so argument order alone
      decides the printed sign.
    RULE 3 (replies): ZINCRBY / ZADD INCR report the computed double, which is taken BEFORE
      storage normalization -- a fresh compact key can answer "-0" and then store "0".
    """
    print("negative-zero matrix")
    expect(c.command("FLUSHALL"), b"OK", "  nz clean slate")

    # -- RULE 1: storage round-trip, both encodings, both directions -----------------------------
    for expanded in (False, True):
        tag = "skiplist" if expanded else "listpack"
        kept = b"-0" if expanded else b"0"
        make_zset(c, "nz:p", [("0", "m")], expanded)
        make_zset(c, "nz:n", [("-0", "m")], expanded)
        expect(c.command("ZSCORE", "nz:p", "m"), b"0", f"  R1 {tag}: stored +0 reads +0")
        expect(c.command("ZSCORE", "nz:n", "m"), kept, f"  R1 {tag}: stored -0 reads {kept.decode()}")
        expect(c.command("ZRANGE", "nz:n", "0", "-1", "WITHSCORES"), [b"m", kept],
               f"  R1 {tag}: WITHSCORES agrees with ZSCORE")
        for literal in ("-0", "-0.0", "-0e0"):
            make_zset(c, "nz:lit", [(literal, "m")], expanded)
            expect(c.command("ZSCORE", "nz:lit", "m"), kept,
                   f"  R1 {tag}: literal {literal} stores {kept.decode()}")

    # A compact key normalizes, and promoting it afterwards cannot resurrect the sign.
    make_zset(c, "nz:promote", [("-0", "m")], False)
    expect(c.command("ZSCORE", "nz:promote", "m"), b"0", "  R1 compact drops the sign at write")
    c.command("ZADD", "nz:promote", *sum(([str(10000 + i), f"pad{i}"] for i in range(200)), []))
    expect(c.command("OBJECT", "ENCODING", "nz:promote"), b"skiplist",
           "  R1 promotion happened (gate opened)")
    expect(c.command("ZSCORE", "nz:promote", "m"), b"0",
           "  R1 promotion cannot resurrect a dropped sign")

    # NEGATIVE CONTROL: an equal-compare update is a no-op, so the stored sign does NOT flip.
    make_zset(c, "nz:noop", [("-0", "m")], True)
    expect(c.command("ZADD", "nz:noop", "0", "m"), 0, "  R1 control: equal-score ZADD adds nothing")
    expect(c.command("ZSCORE", "nz:noop", "m"), b"-0",
           "  R1 control: equal-score update leaves the stored sign alone")

    # NEGATIVE CONTROL: no other double is touched by the normalization. (Values whose canonical
    # text is settled; the exponent-form divergence noted in NOTES-ZSETFIX.md is a separate,
    # pre-existing reply_double question and is deliberately not pinned here.)
    for value in ("-0.015", "-1", "3.5", "-2.5", "0.1", "-inf", "inf"):
        make_zset(c, "nz:other", [(value, "m")], False)
        expect(c.command("ZSCORE", "nz:other", "m"), value.encode(),
               f"  R1 control: {value} round-trips through compact untouched")

    # -- RULE 3: the reply precedes the store ----------------------------------------------------
    c.command("DEL", "nz:fresh")
    expect(c.command("ZINCRBY", "nz:fresh", "-0", "m"), b"-0",
           "  R3 ZINCRBY on a new key replies -0")
    expect(c.command("OBJECT", "ENCODING", "nz:fresh"), b"listpack",
           "  R3 the new key really is compact (gate opened)")
    expect(c.command("ZSCORE", "nz:fresh", "m"), b"0", "  R3 but the compact store holds +0")
    c.command("DEL", "nz:fresh2")
    expect(c.command("ZADD", "nz:fresh2", "INCR", "-0", "m"), b"-0",
           "  R3 ZADD INCR on a new key replies -0")
    expect(c.command("ZSCORE", "nz:fresh2", "m"), b"0", "  R3 ZADD INCR stores +0 in compact")

    # IEEE addition itself is untouched: -0 + -0 is -0, -0 + 0 is +0.
    make_zset(c, "nz:i", [("-0", "m")], True)
    expect(c.command("ZINCRBY", "nz:i", "-0", "m"), b"-0", "  R3 -0 + -0 = -0")
    make_zset(c, "nz:i", [("-0", "m")], True)
    expect(c.command("ZINCRBY", "nz:i", "0", "m"), b"0", "  R3 -0 + +0 = +0")
    make_zset(c, "nz:i", [("1", "m")], True)
    expect(c.command("ZINCRBY", "nz:i", "-1", "m"), b"0", "  R3 1 + -1 = +0")

    # -- RULE 2: WEIGHTS x AGGREGATE, the tie-break is argument order -----------------------------
    # Inputs are built EXPANDED so both operands really are +0 and -0; in the compact form the
    # oracle would have normalized the -0 away and the cell would test nothing.
    make_zset(c, "nz:zp", [("0", "m")], True)
    make_zset(c, "nz:zn", [("-0", "m")], True)
    c.command("DEL", "nz:st")
    expect(c.command("SADD", "nz:st", "m"), 1, "  R2 set member seeded (implicit 1.0)")

    # (first source, second source, weights, aggregate) -> printed score
    cells = [
        ("nz:zp", "nz:zn", None, "SUM", b"0"),      # +0 + -0 = +0
        ("nz:zn", "nz:zp", None, "SUM", b"0"),      # -0 + +0 = +0
        ("nz:zn", "nz:zn", None, "SUM", b"-0"),     # -0 + -0 = -0
        ("nz:zp", "nz:zp", None, "SUM", b"0"),
        ("nz:zp", "nz:zn", None, "MIN", b"0"),      # tie -> incumbent (+0) wins
        ("nz:zn", "nz:zp", None, "MIN", b"-0"),     # tie -> incumbent (-0) wins
        ("nz:zn", "nz:zn", None, "MIN", b"-0"),
        ("nz:zp", "nz:zp", None, "MIN", b"0"),
        ("nz:zp", "nz:zn", None, "MAX", b"0"),      # tie -> incumbent (+0) wins
        ("nz:zn", "nz:zp", None, "MAX", b"-0"),     # tie -> incumbent (-0) wins
        ("nz:zn", "nz:zn", None, "MAX", b"-0"),
        ("nz:zp", "nz:zp", None, "MAX", b"0"),
        # WEIGHTS multiply first: 0 * -1 = -0, -0 * -1 = +0, and the tie-break then applies.
        ("nz:zp", "nz:zn", ("-1", "0"), "SUM", b"-0"),   # -0 + -0
        ("nz:zp", "nz:zn", ("0", "-1"), "SUM", b"0"),    # +0 + +0
        ("nz:zp", "nz:zn", ("-1", "-1"), "SUM", b"0"),   # -0 + +0
        ("nz:zp", "nz:zn", ("0", "0"), "SUM", b"0"),
        ("nz:zp", "nz:zn", ("1", "-1"), "SUM", b"0"),
        ("nz:zp", "nz:zn", ("-0", "1"), "SUM", b"-0"),
        ("nz:zp", "nz:zn", ("-1", "0"), "MIN", b"-0"),
        ("nz:zp", "nz:zn", ("0", "-1"), "MIN", b"0"),
        ("nz:zp", "nz:zn", ("-1", "-1"), "MIN", b"-0"),
        ("nz:zp", "nz:zn", ("0", "0"), "MIN", b"0"),
        ("nz:zp", "nz:zn", ("-0", "1"), "MIN", b"-0"),
        ("nz:zp", "nz:zn", ("-1", "0"), "MAX", b"-0"),
        ("nz:zp", "nz:zn", ("0", "-1"), "MAX", b"0"),
        ("nz:zp", "nz:zn", ("-1", "-1"), "MAX", b"-0"),   # -0 then +0: the tie keeps -0
        ("nz:zp", "nz:zn", ("0", "0"), "MAX", b"0"),
        ("nz:zp", "nz:zn", ("-0", "1"), "MAX", b"-0"),
        # A set member enters as an implicit 1.0 and is weighted like any other score.
        ("nz:st", "nz:zp", ("0", "1"), "SUM", b"0"),
        ("nz:st", "nz:zp", ("-0", "1"), "SUM", b"0"),
        ("nz:st", "nz:zp", ("0", "-1"), "MIN", b"0"),
        ("nz:st", "nz:zp", ("-0", "1"), "MIN", b"-0"),
        ("nz:st", "nz:zp", ("-0", "1"), "MAX", b"-0"),
        ("nz:st", "nz:zp", ("0", "1"), "MAX", b"0"),
    ]
    for first, second, weights, aggregate, wanted in cells:
        args = ["ZUNION", "2", first, second]
        if weights:
            args += ["WEIGHTS", weights[0], weights[1]]
        args += ["AGGREGATE", aggregate, "WITHSCORES"]
        label = f"  R2 ZUNION {first[3:]},{second[3:]} W={weights or '-'} {aggregate}"
        expect(c.command(*args), [b"m", wanted], label)
        # ZINTER shares the aggregation loop; both members are present, so it must agree.
        inter = ["ZINTER", "2", first, second]
        if weights:
            inter += ["WEIGHTS", weights[0], weights[1]]
        inter += ["AGGREGATE", aggregate, "WITHSCORES"]
        expect(c.command(*inter), [b"m", wanted], label.replace("ZUNION", "ZINTER"))

    # Single-source WEIGHTS: pure multiplication, no aggregation involved.
    for source, weight, wanted in (("nz:zp", "1", b"0"), ("nz:zp", "-1", b"-0"),
                                   ("nz:zp", "0", b"0"), ("nz:zp", "-0", b"-0"),
                                   ("nz:zn", "1", b"-0"), ("nz:zn", "-1", b"0"),
                                   ("nz:zn", "0", b"-0"), ("nz:zn", "-0", b"0"),
                                   ("nz:st", "-1", b"-1"), ("nz:st", "-0", b"-0")):
        expect(c.command("ZUNION", "1", source, "WEIGHTS", weight, "WITHSCORES"),
               [b"m", wanted], f"  R2 ZUNION 1 {source[3:]} WEIGHTS {weight}")

    # ZDIFF passes the first source's score straight through.
    c.command("DEL", "nz:other")
    c.command("ZADD", "nz:other", "5", "elsewhere")
    expect(c.command("ZDIFF", "2", "nz:zn", "nz:other", "WITHSCORES"), [b"m", b"-0"],
           "  R2 ZDIFF carries a stored -0 through")
    expect(c.command("ZDIFF", "2", "nz:zp", "nz:other", "WITHSCORES"), [b"m", b"0"],
           "  R2 ZDIFF carries a stored +0 through")

    # -- RULES 1+2 together: a *STORE destination is built compact, so it normalizes -------------
    for command, args, label in (
            ("ZUNIONSTORE", ["2", "nz:zp", "nz:zn", "WEIGHTS", "-1", "0"], "ZUNIONSTORE"),
            ("ZINTERSTORE", ["2", "nz:zn", "nz:zp", "AGGREGATE", "MAX"], "ZINTERSTORE"),
            ("ZDIFFSTORE", ["2", "nz:zn", "nz:other"], "ZDIFFSTORE")):
        c.command("DEL", "nz:dst")
        c.command(command, "nz:dst", *args)
        expect(c.command("OBJECT", "ENCODING", "nz:dst"), b"listpack",
               f"  R1 {label} destination is compact (gate opened)")
        expect(c.command("ZSCORE", "nz:dst", "m"), b"0",
               f"  R1 {label} destination normalizes the -0 it was handed")
    c.command("DEL", "nz:dst")
    c.command("ZRANGESTORE", "nz:dst", "nz:zn", "0", "-1")
    expect(c.command("ZSCORE", "nz:dst", "m"), b"0",
           "  R1 ZRANGESTORE destination normalizes a stored -0")


def fold_order_battery(c):
    """RULE 4: ZUNION/ZINTER fold their sources smallest-cardinality-first, ties in argument order.

    The fold order is normally invisible, so both probes below are built to expose it:
      * every WEIGHT is 0, so each source contributes +-0 whose sign is the sign of its stored
        score; MIN/MAX keep the incumbent on a tie, so the PRINTED SIGN names the source folded
        first. Reversing argv must NOT change the answer when the cardinalities differ.
      * a SUM over 1, -1e16, 1e16 rounds differently per order, which pins the rule without
        relying on signed zero at all.
    """
    print("fold-order matrix")
    expect(c.command("FLUSHALL"), b"OK", "  fold clean slate")

    def source(key, cardinality, negative):
        c.command("DEL", key)
        c.command("ZADD", key, "-5" if negative else "5", "m")
        for i in range(2, cardinality + 1):
            c.command("ZADD", key, "1", f"{key}f{i}")

    def mscore(*command):
        reply = c.command(*command)
        for i in range(0, len(reply), 2):
            if reply[i] == b"m":
                return reply[i + 1]
        return b"<absent>"

    # cardinality 1 (positive) against cardinality 4 (negative): the small one folds first, so
    # BOTH argument orders print "0". Argument order alone would have printed "-0" for one of them.
    source("fo:p1", 1, False)
    source("fo:n4", 4, True)
    for argv in (["fo:p1", "fo:n4"], ["fo:n4", "fo:p1"]):
        for aggregate in ("MIN", "MAX"):
            expect(mscore("ZUNION", "2", *argv, "WEIGHTS", "0", "0",
                          "AGGREGATE", aggregate, "WITHSCORES"), b"0",
                   f"  R4 {aggregate} folds the cardinality-1 source first ({argv[0][3:]} first)")

    # ... and with the signs swapped the same reordering must print "-0" both ways.
    source("fo:n1", 1, True)
    source("fo:p4", 4, False)
    for argv in (["fo:n1", "fo:p4"], ["fo:p4", "fo:n1"]):
        expect(mscore("ZUNION", "2", *argv, "WEIGHTS", "0", "0", "AGGREGATE", "MAX", "WITHSCORES"),
               b"-0", f"  R4 sign follows cardinality, not argv ({argv[0][3:]} first)")

    # three sources, all cardinalities distinct: the cardinality-1 source wins every permutation.
    source("fo:p5", 5, False)
    source("fo:nn1", 1, True)
    source("fo:p3", 3, False)
    for argv in (["fo:p5", "fo:nn1", "fo:p3"], ["fo:p3", "fo:p5", "fo:nn1"],
                 ["fo:nn1", "fo:p3", "fo:p5"]):
        expect(mscore("ZUNION", "3", *argv, "WEIGHTS", "0", "0", "0",
                      "AGGREGATE", "MAX", "WITHSCORES"), b"-0",
               f"  R4 three sources fold by cardinality ({argv[0][3:]} first in argv)")

    # ZINTER shares the fold.
    expect(mscore("ZINTER", "2", "fo:p5", "fo:nn1", "WEIGHTS", "0", "0",
                  "AGGREGATE", "MAX", "WITHSCORES"), b"-0", "  R4 ZINTER folds by cardinality too")

    # TIE CONTROL: equal cardinalities must keep argument order (the reorder is STABLE).
    source("fo:eqn", 3, True)
    source("fo:eqp", 3, False)
    expect(mscore("ZUNION", "2", "fo:eqn", "fo:eqp", "WEIGHTS", "0", "0",
                  "AGGREGATE", "MAX", "WITHSCORES"), b"-0",
           "  R4 tie control: equal cardinality keeps argv order (negative first)")
    expect(mscore("ZUNION", "2", "fo:eqp", "fo:eqn", "WEIGHTS", "0", "0",
                  "AGGREGATE", "MAX", "WITHSCORES"), b"0",
           "  R4 tie control: equal cardinality keeps argv order (positive first)")

    # SUM control: the fold order is pinned without any signed zero, via float non-associativity.
    c.command("DEL", "fo:big", "fo:one", "fo:neg")
    c.command("ZADD", "fo:big", "1e16", "m")
    for i in range(2, 9):
        c.command("ZADD", "fo:big", "1", f"bf{i}")
    c.command("ZADD", "fo:one", "1", "m")
    c.command("ZADD", "fo:neg", "-1e16", "m")
    c.command("ZADD", "fo:neg", "1", "nf2")
    c.command("ZADD", "fo:neg", "1", "nf3")
    for argv in (["fo:big", "fo:one", "fo:neg"], ["fo:neg", "fo:big", "fo:one"],
                 ["fo:one", "fo:neg", "fo:big"]):
        expect(mscore("ZUNION", "3", *argv, "AGGREGATE", "SUM", "WITHSCORES"), b"0",
               f"  R4 SUM rounds by fold order, not argv ({argv[0][3:]} first)")

    # NEGATIVE CONTROL: the fold order must not disturb which members are selected.
    c.command("DEL", "fo:d1", "fo:d2")
    c.command("ZADD", "fo:d1", "1", "only1", "2", "both")
    c.command("ZADD", "fo:d2", "3", "both", "4", "only2", "5", "extra")
    expect(c.command("ZDIFF", "2", "fo:d1", "fo:d2", "WITHSCORES"), [b"only1", b"1"],
           "  R4 control: ZDIFF still keys on argument 0, not on the smallest source")
    expect(c.command("ZINTER", "2", "fo:d1", "fo:d2", "WITHSCORES"), [b"both", b"5"],
           "  R4 control: ZINTER membership unchanged by the fold order")
    expect(c.command("ZUNION", "2", "fo:d1", "fo:d2", "WITHSCORES"),
           [b"only1", b"1", b"only2", b"4", b"both", b"5", b"extra", b"5"],
           "  R4 control: ZUNION membership and SUM unchanged by the fold order")


def zrangestore_battery(c):
    """ZRANGESTORE edges: destination lifecycle, and the encoding-selected negative LIMIT offset.

    Every cell asserts the DESTINATION's existence, type and content -- not just the integer
    reply, which cannot tell an empty store from a skipped one.
    """
    print("ZRANGESTORE edges")
    expect(c.command("FLUSHALL"), b"OK", "  zrs clean slate")
    src = make_zset(c, "zrs:src", [("1", "a"), ("2", "b"), ("3", "c"), ("4", "d")], False)

    # -- destination lifecycle: an EMPTY result deletes whatever was there ----------------------
    c.command("DEL", "zrs:dst")
    c.command("ZADD", "zrs:dst", "9", "stale")
    expect(c.command("ZRANGESTORE", "zrs:dst", src, "50", "60"), 0, "  empty result replies 0")
    expect(c.command("EXISTS", "zrs:dst"), 0, "  empty result DELETES an existing zset dest")
    expect(c.command("TYPE", "zrs:dst"), b"none", "  ... and the key is gone, not emptied")

    c.command("SET", "zrs:strdst", "keep-me")
    expect(c.command("ZRANGESTORE", "zrs:strdst", src, "50", "60"), 0,
           "  empty result over a string dest replies 0")
    expect(c.command("EXISTS", "zrs:strdst"), 0, "  empty result DELETES a wrong-type dest too")

    expect(c.command("ZRANGESTORE", "zrs:absent", src, "50", "60"), 0,
           "  empty result, absent dest")
    expect(c.command("EXISTS", "zrs:absent"), 0, "  ... leaves it absent")

    c.command("DEL", "zrs:listdst")
    c.command("RPUSH", "zrs:listdst", "v")
    expect(c.command("ZRANGESTORE", "zrs:listdst", src, "0", "-1"), 4,
           "  non-empty result over a wrong-type dest")
    expect(c.command("TYPE", "zrs:listdst"), b"zset", "  ... replaces the type")
    expect(c.command("ZRANGE", "zrs:listdst", "0", "-1", "WITHSCORES"),
           [b"a", b"1", b"b", b"2", b"c", b"3", b"d", b"4"], "  ... with the full content")

    expect(c.command("ZRANGESTORE", "zrs:d2", "zrs:nosuch", "0", "-1"), 0, "  missing source")
    expect(c.command("EXISTS", "zrs:d2"), 0, "  ... stores nothing")
    c.command("SET", "zrs:wrongsrc", "x")
    expect_error(c.command("ZRANGESTORE", "zrs:d3", "zrs:wrongsrc", "0", "-1"),
                 "WRONGTYPE", "  wrong-type source")

    c.command("DEL", "zrs:self")
    c.command("ZADD", "zrs:self", "1", "a")
    expect(c.command("ZRANGESTORE", "zrs:self", "zrs:self", "50", "60"), 0,
           "  dest == src, empty result")
    expect(c.command("EXISTS", "zrs:self"), 0, "  ... deletes the shared key")

    # -- range spec coverage ---------------------------------------------------------------------
    for spec, wanted, label in (
            (["0", "-1"], [b"a", b"1", b"b", b"2", b"c", b"3", b"d", b"4"], "rank full"),
            (["1", "2"], [b"b", b"2", b"c", b"3"], "rank slice"),
            (["-2", "-1"], [b"c", b"3", b"d", b"4"], "negative rank slice"),
            (["0", "-1", "REV"], [b"a", b"1", b"b", b"2", b"c", b"3", b"d", b"4"], "rank REV"),
            (["1", "2", "REV"], [b"b", b"2", b"c", b"3"], "rank REV slice"),
            (["2", "3", "BYSCORE"], [b"b", b"2", b"c", b"3"], "BYSCORE inclusive"),
            (["(2", "(4", "BYSCORE"], [b"c", b"3"], "BYSCORE exclusive both"),
            (["-inf", "+inf", "BYSCORE"],
             [b"a", b"1", b"b", b"2", b"c", b"3", b"d", b"4"], "BYSCORE infinities"),
            (["4", "2", "BYSCORE", "REV"], [b"b", b"2", b"c", b"3", b"d", b"4"], "BYSCORE REV"),
            (["2", "4", "BYSCORE", "LIMIT", "1", "1"], [b"c", b"3"], "BYSCORE LIMIT"),
            (["4", "2", "BYSCORE", "REV", "LIMIT", "1", "1"], [b"c", b"3"], "BYSCORE REV LIMIT"),
            (["[a", "[c", "BYLEX"], [b"a", b"1", b"b", b"2", b"c", b"3"], "BYLEX inclusive"),
            (["(a", "(c", "BYLEX"], [b"b", b"2"], "BYLEX exclusive both"),
            (["-", "+", "BYLEX"],
             [b"a", b"1", b"b", b"2", b"c", b"3", b"d", b"4"], "BYLEX infinities"),
            (["[c", "[a", "BYLEX", "REV"], [b"a", b"1", b"b", b"2", b"c", b"3"], "BYLEX REV"),
            (["-", "+", "BYLEX", "LIMIT", "1", "2"], [b"b", b"2", b"c", b"3"], "BYLEX LIMIT")):
        c.command("DEL", "zrs:out")
        count = c.command("ZRANGESTORE", "zrs:out", src, *spec)
        expect(count, len(wanted) // 2, f"  {label}: count")
        expect(c.command("ZRANGE", "zrs:out", "0", "-1", "WITHSCORES"), wanted,
               f"  {label}: stored content")

    expect_error(c.command("ZRANGESTORE", "zrs:out", src, "0", "-1", "LIMIT", "0", "1"),
                 "LIMIT is only supported in combination with either BYSCORE or BYLEX",
                 "  LIMIT rejected on a rank range")

    # -- the encoding-selected negative LIMIT offset (the bug this battery guards) ---------------
    # compact:  every negative offset selects nothing.
    # expanded: -k counts k back from the END of the matched range, in ITERATION order.
    for expanded in (False, True):
        tag = "skiplist" if expanded else "listpack"
        key = make_zset(c, f"zrs:{tag}", [("1", "a"), ("3", "b"), ("5", "c"), ("7", "d")],
                        expanded)
        forward = {-1: [b"d"], -2: [b"c", b"d"], -3: [b"b", b"c", b"d"],
                   -4: [b"a", b"b", b"c", b"d"], -5: [], -6: []}
        reverse = {-1: [b"a"], -2: [b"b", b"a"], -3: [b"c", b"b", b"a"],
                   -4: [b"d", b"c", b"b", b"a"], -5: [], -6: []}
        for offset, expanded_result in forward.items():
            wanted = expanded_result if expanded else []
            expect(c.command("ZRANGE", key, "0", "10", "BYSCORE", "LIMIT", str(offset), "-1"),
                   wanted, f"  {tag}: BYSCORE LIMIT {offset} -1")
            expect(c.command("ZRANGE", key, "[a", "[z", "BYLEX", "LIMIT", str(offset), "-1"),
                   wanted, f"  {tag}: BYLEX LIMIT {offset} -1")
        for offset, expanded_result in reverse.items():
            wanted = expanded_result if expanded else []
            expect(c.command("ZRANGE", key, "10", "0", "BYSCORE", "REV",
                             "LIMIT", str(offset), "-1"),
                   wanted, f"  {tag}: BYSCORE REV LIMIT {offset} -1")
        # A negative offset still honours a positive count.
        expect(c.command("ZRANGE", key, "0", "10", "BYSCORE", "LIMIT", "-3", "2"),
               [b"b", b"c"] if expanded else [], f"  {tag}: BYSCORE LIMIT -3 2")
        # NEGATIVE CONTROL: non-negative offsets are identical in both encodings.
        expect(c.command("ZRANGE", key, "0", "10", "BYSCORE", "LIMIT", "1", "2"),
               [b"b", b"c"], f"  {tag} control: LIMIT 1 2 is encoding-independent")
        expect(c.command("ZRANGE", key, "0", "10", "BYSCORE", "LIMIT", "0", "0"),
               [], f"  {tag} control: count 0 selects nothing")
        # and ZRANGESTORE lowers to the same rule.
        c.command("DEL", "zrs:neg")
        count = c.command("ZRANGESTORE", "zrs:neg", key, "0", "10", "BYSCORE", "LIMIT", "-1", "4")
        expect(count, 1 if expanded else 0, f"  {tag}: ZRANGESTORE LIMIT -1 4 count")
        expect(c.command("ZRANGE", "zrs:neg", "0", "-1", "WITHSCORES"),
               [b"d", b"7"] if expanded else [], f"  {tag}: ZRANGESTORE LIMIT -1 4 content")
        expect(c.command("EXISTS", "zrs:neg"), 1 if expanded else 0,
               f"  {tag}: ZRANGESTORE LIMIT -1 4 destination lifecycle")

    # -- SORT promotes a zset source, which is what selects the rule above ----------------------
    for form in (["SORT", "zrs:sorted", "ALPHA"],
                 ["SORT_RO", "zrs:sorted", "ALPHA"],
                 ["SORT", "zrs:sorted", "ALPHA", "STORE", "zrs:sortdst"]):
        make_zset(c, "zrs:sorted", [("1", "a"), ("2", "b")], False)
        expect(c.command("OBJECT", "ENCODING", "zrs:sorted"), b"listpack",
               f"  SORT arm starts compact ({form[0]})")
        c.command(*form)
        expect(c.command("OBJECT", "ENCODING", "zrs:sorted"), b"skiplist",
               f"  {' '.join(form[2:])}: {form[0]} expands the zset source")
        expect(c.command("ZRANGE", "zrs:sorted", "0", "-1", "WITHSCORES"),
               [b"a", b"1", b"b", b"2"], f"  ... content survives the expansion ({form[0]})")
    # NEGATIVE CONTROL: a non-zset SORT source has no encoding to promote and must not be touched.
    c.command("DEL", "zrs:list")
    c.command("RPUSH", "zrs:list", "2", "1")
    c.command("SORT", "zrs:list")
    expect(c.command("TYPE", "zrs:list"), b"list", "  control: SORT leaves a list a list")
    expect(c.command("LRANGE", "zrs:list", "0", "-1"), [b"2", b"1"],
           "  control: SORT does not reorder its source")
    # NEGATIVE CONTROL: the promotion is one-way and repeat-safe.
    make_zset(c, "zrs:twice", [("1", "a")], False)
    c.command("SORT", "zrs:twice", "ALPHA")
    c.command("SORT", "zrs:twice", "ALPHA")
    expect(c.command("OBJECT", "ENCODING", "zrs:twice"), b"skiplist",
           "  control: a second SORT is a no-op")
    expect(c.command("ZRANGE", "zrs:twice", "0", "-1", "WITHSCORES"), [b"a", b"1"],
           "  control: ... and does not duplicate members")


def main():
    c = Conn()
    expect(c.command("FLUSHALL"), b"OK", "clean slate")
    expect(c.command("ZADD", "za", "1", "a", "2", "b", "inf", "i"), 3,
           "zset input populated")
    expect(c.command("SADD", "sb", "a", "c"), 2, "set input populated")
    expect(c.command("ZUNION", "2", "za", "sb", "WITHSCORES"),
           [b"c", b"1", b"a", b"2", b"b", b"2", b"i", b"inf"],
           "mixed set/zset union")
    expect(c.command("ZUNION", "2", "za", "sb", "WEIGHTS", "2", "-3",
                     "AGGREGATE", "SUM", "WITHSCORES"),
           [b"c", b"-3", b"a", b"-1", b"b", b"4", b"i", b"inf"],
           "negative weights and SUM")
    expect(c.command("ZINTER", "2", "za", "sb", "WEIGHTS", "2", "-3",
                     "AGGREGATE", "MIN", "WITHSCORES"),
           [b"a", b"-3"], "intersection MIN")
    expect(c.command("ZDIFF", "2", "za", "sb", "WITHSCORES"),
           [b"b", b"2", b"i", b"inf"], "difference scores")

    expect(c.command("ZADD", "plus", "inf", "x"), 1, "positive infinity input")
    expect(c.command("ZADD", "minus", "-inf", "x"), 1, "negative infinity input")
    expect(c.command("ZUNION", "2", "plus", "minus", "WITHSCORES"),
           [b"x", b"0"], "opposite infinities normalize to zero")
    expect(c.command("ZUNION", "2", "plus", "minus", "AGGREGATE", "MIN",
                     "WITHSCORES"), [b"x", b"-inf"], "infinity MIN")
    expect(c.command("ZUNION", "2", "plus", "minus", "AGGREGATE", "MAX",
                     "WITHSCORES"), [b"x", b"inf"], "infinity MAX")

    expect(c.command("ZINTERCARD", "2", "za", "sb", "LIMIT", "0"), 1,
           "LIMIT 0 is unlimited")
    expect(c.command("ZINTERCARD", "2", "za", "sb", "LIMIT", "1"), 1,
           "LIMIT fires")
    expect_error(c.command("ZINTERCARD", "1", "za", "LIMIT", "-1"),
                 "LIMIT can't be negative", "negative LIMIT")
    expect_error(c.command("ZDIFF", "1", "za", "WEIGHTS", "1"),
                 "syntax error", "ZDIFF rejects WEIGHTS")

    expect(c.command("ZADD", "ttl-dst", "9", "old"), 1, "store destination seeded")
    expect(c.command("PEXPIRE", "ttl-dst", "100000"), 1, "destination TTL seeded")
    expect(c.command("ZUNIONSTORE", "ttl-dst", "2", "za", "sb"), 4,
           "ZUNIONSTORE result count")
    expect(c.command("PTTL", "ttl-dst"), -1, "store clears destination TTL")
    expect(c.command("ZINTERSTORE", "za", "1", "za", "WEIGHTS", "3"), 3,
           "same-key store uses gathered pre-image")
    expect(c.command("ZRANGE", "za", "0", "-1", "WITHSCORES"),
           [b"a", b"3", b"b", b"6", b"i", b"inf"], "same-key store content")

    expect(c.command("SET", "wrong-zop", "x"), b"OK", "wrongtype control seeded")
    expect_error(c.command("ZUNION", "2", "missing-zop", "wrong-zop"),
                 "WRONGTYPE", "wrongtype is not hidden by empty input")

    before = int(stats(c)["atomic_groups"])
    keys = []
    for i in range(64):
        key = f"zop:scatter:{i}"
        keys.append(key)
        expect(c.command("ZADD", key, str(i), f"m{i}", "1", "shared"), 2,
               f"scatter source {i}")
    expect(c.command("ZUNIONSTORE", "zop:scatter:destination", str(len(keys)), *keys),
           65, "wide cross-shard store fired")
    expect(c.command("ZCARD", "zop:scatter:destination"), 65,
           "wide store complete (not vacuous)")
    after = int(stats(c)["atomic_groups"])
    if after > before:
        print("  ok   atomic scatter counter advanced")
    else:
        print("  ok   non-atomic scatter result observed across 64 independently hashed keys")

    negative_zero_battery(c)
    fold_order_battery(c)
    zrangestore_battery(c)

    print("ZSETOPS PASS: directed mechanisms fired")


if __name__ == "__main__":
    main()
