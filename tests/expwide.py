#!/usr/bin/env python3
"""One expiry cut per logical operation. Usage: tests/expwide.py HOST PORT [MODE]

A key deadline is the one mutation with no command behind it, so it is the one thing a fan-out
cannot pin by taking a commit ticket.  Before this lane, every owner of a cross-shard command
compared deadlines against its OWN executor's per-pass clock, so one operation could observe a key
alive on one owner and elapsed on another -- a keyspace that existed at no instant.  Redis takes a
single time snapshot per command, and a transaction or a script is one command, so every access
inside it reads against that one instant.  This battery pins that behaviour.

MODES
  ``all``            the gate battery (default).  Runs unchanged against the vanilla redis oracle:
                     boot it with ``--save "" --enable-debug-command yes`` and point this at it.
                     Sections needing TomoKV's own DEBUG hooks announce themselves as skipped.
  ``repro-natural``  the statistical, hook-free exposure probe.  Not a gate mode -- it reports a
                     rate rather than a verdict, and it takes minutes.

WHY NO SECTION CAN PASS VACUOUSLY
  Every armed check is bracketed by two controls that must answer the other way: a far-deadline
  control (nothing elapses) and an already-elapsed control (everything has).  A server that
  ignored deadlines entirely would fail the elapsed control; one that treated everything as dead
  would fail the far control.  Sections that widen a window assert the window really opened -- the
  hook's own measured elapsed time in S1, the measured length of the transaction in S2/S3.
"""

import socket
import sys
import time

HOST = sys.argv[1]
PORT = int(sys.argv[2])
MODE = sys.argv[3] if len(sys.argv) > 3 else "all"
FAILURES = []
CHECKS = 0
NOTES = []


def encode(*args):
    out = bytearray(b"*%d\r\n" % len(args))
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        out += b"$%d\r\n" % len(arg) + arg + b"\r\n"
    return bytes(out)


class RespError:
    def __init__(self, message):
        self.message = message

    def __eq__(self, other):
        return isinstance(other, RespError) and other.message == self.message

    def __repr__(self):
        return "RespError(%r)" % self.message


class Resp:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=120)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed the connection")
        kind, body = line[:1], line[1:-2]
        if kind == b"+":
            return body
        if kind == b"-":
            return RespError(body.decode())
        if kind == b":":
            return int(body)
        if kind == b"$":
            n = int(body)
            return None if n < 0 else self.file.read(n + 2)[:-2]
        if kind == b"*":
            n = int(body)
            return None if n < 0 else [self.read() for _ in range(n)]
        if kind == b"_":
            return None
        raise AssertionError("unhandled RESP type %r" % line)

    def cmd(self, *args):
        self.sock.sendall(encode(*args))
        return self.read()

    def pipeline(self, commands):
        """Send every command in one write, then read exactly that many replies."""
        self.sock.sendall(b"".join(encode(*c) for c in commands))
        return [self.read() for _ in commands]


def check(name, got, want):
    global CHECKS
    CHECKS += 1
    if got != want:
        FAILURES.append("%s: got %r, want %r" % (name, got, want))
    return got == want


def check_true(name, ok, detail=""):
    global CHECKS
    CHECKS += 1
    if not ok:
        FAILURES.append("%s%s" % (name, (" (%s)" % detail) if detail else ""))
    return ok


def info_counter(conn, field):
    text = conn.cmd("INFO", "stats")
    if not isinstance(text, (bytes, bytearray)):
        return -1
    for line in text.decode(errors="replace").splitlines():
        if line.startswith(field + ":"):
            return int(line.split(":", 1)[1])
    return -1


# --------------------------------------------------------------------------------------------
# geometry and arming


def sharded(conn):
    return isinstance(conn.cmd("DEBUG", "SHARD", "expwide:probe"), int)


def owner_keys(conn, prefix, count=8):
    """One key per distinct owner, so a fan-out over them really reaches `count` executors."""
    if not sharded(conn):
        return ["%s:%04d" % (prefix, i) for i in range(count)], None
    by_shard = {}
    for index in range(6000):
        key = "%s:%04d" % (prefix, index)
        shard = conn.cmd("DEBUG", "SHARD", key)
        if isinstance(shard, int) and shard not in by_shard:
            by_shard[shard] = key
        if len(by_shard) == count:
            break
    if len(by_shard) != count:
        raise AssertionError("DEBUG SHARD found only %d distinct owners" % len(by_shard))
    return [by_shard[s] for s in sorted(by_shard)], sorted(by_shard)


def key_on_other_owner(conn, prefix, source):
    """Find a destination that DEBUG SHARD proves is not owned with `source`.

    Redis has one keyspace and no DEBUG SHARD route, so its oracle leg uses an arbitrary distinct
    name. A TomoKV leg is not allowed that fallback: if the live hash seed cannot produce a split
    pair, the cross-shard regression has tested nothing and must fail loudly.
    """
    if not sharded(conn):
        return "%s:oracle" % prefix, None
    source_owner = conn.cmd("DEBUG", "SHARD", source)
    if not isinstance(source_owner, int):
        raise AssertionError("DEBUG SHARD failed for source %r: %r" % (source, source_owner))
    for index in range(8000):
        candidate = "%s:%04d" % (prefix, index)
        owner = conn.cmd("DEBUG", "SHARD", candidate)
        if isinstance(owner, int) and owner != source_owner:
            return candidate, (source_owner, owner)
    raise AssertionError("DEBUG SHARD found no cross-owner destination for %r" % source)


def set_active(conn, enabled):
    conn.cmd("DEBUG", "SET-ACTIVE-EXPIRE", str(enabled))


# offset_ms sentinel: the keys must already be past their deadline when the operation starts
ELAPSED = "elapsed"
FAR = 3600000


def arm(conn, deadline_keys, offset_ms, plain_keys=(), value=b"v", make=None):
    """(Re)create every key and give `deadline_keys` ONE shared absolute deadline.

    `plain_keys` are recreated with no deadline at all, which is what lets a check isolate the
    liveness of a rename/copy DESTINATION from that of its source.  `make` builds a key when the
    value under test is not a string (SADD for the *STORE family).

    PEXPIREAT with a deadline already in the past deletes the key on the spot on both servers, so
    the elapsed arm uses a short deadline and waits past it.  With active expiry disabled nothing
    reaps them afterwards, which is what makes them a control for LAZY expiry.
    """
    build = []
    for key in list(deadline_keys) + list(plain_keys):
        build.append(("DEL", key))
        build += make(key) if make else [("SET", key, value)]
    conn.pipeline(build)
    offset = 60 if offset_ms is ELAPSED else offset_ms
    armed_at = time.time()
    deadline = int(armed_at * 1000) + offset
    conn.pipeline([("PEXPIREAT", k, str(deadline)) for k in deadline_keys])
    if offset_ms is ELAPSED:
        time.sleep(0.25)
    return armed_at, deadline


# --------------------------------------------------------------------------------------------
# S1. The bare cross-shard fan-out, widened by the tree's own DEBUG ATOMIC-FANOUT-DEFER.
# The hook arms cross-shard READS only, which is why the write half of the family is covered in
# S2 by a window made of real work instead.


def test_fanout_hook(conn):
    if not sharded(conn):
        NOTES.append("S1 fan-out hook: skipped (one keyspace, nothing to fan out)")
        return
    set_active(conn, 0)
    conn.cmd("FLUSHALL")
    keys, shards = owner_keys(conn, "expwide:hop")
    check_true("S1 geometry spans eight owners", shards is not None and len(shards) == 8,
               "shards=%r" % (shards,))
    defer_us = 400000

    def widened(command, offset_ms):
        arm(conn, keys, offset_ms)
        if conn.cmd("DEBUG", "ATOMIC-FANOUT-DEFER", str(defer_us)) != b"OK":
            raise AssertionError("fan-out defer hook rejected")
        start = time.monotonic()
        reply = conn.cmd(*command)
        elapsed = time.monotonic() - start
        conn.cmd("DEBUG", "ATOMIC-FANOUT-DEFER", "0")
        return reply, elapsed

    for name, command, alive, dead in (
            ("MGET", ("MGET",) + tuple(keys), [b"v"] * 8, [None] * 8),
            ("EXISTS", ("EXISTS",) + tuple(keys), 8, 0)):
        reply, elapsed = widened(command, defer_us // 2000)
        check_true("S1 %s: the hook really widened the fan-out" % name,
                   elapsed >= defer_us / 2.0e6, "elapsed=%.3fs" % elapsed)
        check("S1 %s across a deadline inside the fan-out" % name, reply, alive)
        reply, _ = widened(command, FAR)
        check("S1 %s control, deadline an hour out" % name, reply, alive)
        reply, _ = widened(command, ELAPSED)
        check("S1 %s control, elapsed before the command" % name, reply, dead)
    set_active(conn, 1)


# --------------------------------------------------------------------------------------------
# S2. A transaction is one instant, for the WHOLE multi-key family.
#
# The window is real work, not a hook: BITCOUNT over a large string, repeated until the block runs
# far longer than the deadline is placed into it.  That keeps the section runnable against the
# oracle unchanged and reaches the write members of the family.


STRETCH_PREFIX = "expwide:stretch"
STRETCH_BYTES = 33554431           # 32 MB per owner
TARGET_BLOCK_MS = 150
DEADLINE_IN = 25                   # the shared deadline lands this far into the block


def keys_on_owners(conn, prefix, owners):
    """One key per owner in `owners`, so a per-owner workload really reaches all of them.

    Keeping every participating executor busy for the whole block is what makes the section
    discriminating: TomoKV's transactions are ordered per owner and NOT across owners, so an owner
    with nothing else queued would run the command under test immediately -- before the deadline
    -- and report the pinned answer for the wrong reason.
    """
    if owners is None:
        return ["%s:%04d" % (prefix, i) for i in range(8)]
    found = {}
    for index in range(8000):
        key = "%s:%04d" % (prefix, index)
        shard = conn.cmd("DEBUG", "SHARD", key)
        if shard in owners and shard not in found:
            found[shard] = key
        if len(found) == len(owners):
            break
    if len(found) != len(owners):
        raise AssertionError("could not place a key on every owner (%d/%d)"
                             % (len(found), len(owners)))
    return [found[s] for s in owners]


def build_stretcher(conn, owners):
    """A BITCOUNT-per-owner block, sized so it runs for TARGET_BLOCK_MS."""
    stretch = keys_on_owners(conn, STRETCH_PREFIX, owners)
    for key in stretch:
        conn.cmd("SETRANGE", key, str(STRETCH_BYTES), "x")
    conn.pipeline([("BITCOUNT", k) for k in stretch])
    start = time.monotonic()
    conn.pipeline([("BITCOUNT", k) for k in stretch])
    # sharded: the owners scan in parallel, so one round costs one scan.  One keyspace: they are
    # serial, and the measurement above already includes all eight.
    per_round = (time.monotonic() - start) * 1000.0
    if owners is not None:
        per_round /= len(stretch)
    reps = max(1, min(400, int(TARGET_BLOCK_MS / max(per_round, 0.2)) + 1))
    return stretch, reps, per_round


def run_block(conn, stretch, reps, commands, armed_at, offset_ms):
    """Queue and EXEC in ONE write so the transaction starts a round trip after arming.

    `commands` are the tail of the transaction, after the per-owner BITCOUNT block.  Returns
    (replies_to_those_commands, exec_ms, started_inside_window).  The last element is what stops a
    slow client from turning "the cut held" into "the deadline had already passed when EXEC ran";
    an EXEC-level error is returned as a single-element list so callers see the shape.
    """
    if not isinstance(commands, list):
        commands = [commands]
    body = [("MULTI",)] + [("BITCOUNT", k) for k in stretch] * reps + commands + [("EXEC",)]
    inside = offset_ms is ELAPSED or offset_ms == FAR or \
        time.time() < armed_at + offset_ms / 1000.0
    start = time.monotonic()
    replies = conn.pipeline(body)
    took = (time.monotonic() - start) * 1000.0
    exec_reply = replies[-1]
    if not isinstance(exec_reply, list):
        return [exec_reply] * len(commands), took, inside
    return exec_reply[-len(commands):], took, inside


def test_transaction_family(conn):
    set_active(conn, 0)
    conn.cmd("FLUSHALL")
    keys, owners = owner_keys(conn, "expwide:fam")
    dst, _ = owner_keys(conn, "expwide:dst", 2)
    move_dst, move_owners = key_on_other_owner(conn, "expwide:move-dst", keys[0])
    if move_owners is not None:
        check_true("S2 two-hop geometry uses different owners",
                   move_owners[0] != move_owners[1], "owners=%r" % (move_owners,))
    stretch, reps, unit = build_stretcher(conn, owners)
    NOTES.append("S2/S3 window: %d rounds x BITCOUNT(32MB) on each of %d owners, %.2fms a round"
                 % (reps, len(stretch), unit))

    def family(name, command, alive, dead, deadline_keys, plain_keys=(), make=None):
        for offset, want, tag in ((DEADLINE_IN, alive, "across a deadline inside the transaction"),
                                  (FAR, alive, "control, deadline an hour out"),
                                  (ELAPSED, dead, "control, elapsed before the transaction")):
            armed_at, _ = arm(conn, deadline_keys, offset, plain_keys, make=make)
            (reply,), took, inside = run_block(conn, stretch, reps, command, armed_at, offset)
            if offset == DEADLINE_IN:
                check_true("S2 %s: EXEC started before the deadline it must pin" % name, inside)
                check_true("S2 %s: the transaction really outlived the deadline" % name,
                           took >= DEADLINE_IN * 3,
                           "exec=%.0fms, deadline was %dms in" % (took, DEADLINE_IN))
            check("S2 %s %s" % (name, tag), reply, want)

    kk = tuple(keys)
    family("MGET", ("MGET",) + kk, [b"v"] * 8, [None] * 8, keys)
    family("EXISTS", ("EXISTS",) + kk, 8, 0, keys)
    family("TOUCH", ("TOUCH",) + kk, 8, 0, keys)
    family("DEL", ("DEL",) + kk, 8, 0, keys)
    family("UNLINK", ("UNLINK",) + kk, 8, 0, keys)

    # MSETNX refuses when ANY target exists, so it folds the liveness of all eight owners into one
    # bit -- the sharpest straddle in the write half of the family.
    pairs = ()
    for key in keys:
        pairs += (key, "w")
    family("MSETNX", ("MSETNX",) + pairs, 0, 1, keys)

    # MSET carries no liveness test of its own; what it must not do is disagree with the read that
    # follows it in the same transaction.  Checked as MSET-then-MGET in one block.
    mset_args = ()
    for key in keys:
        mset_args += (key, "z")
    armed_at, _ = arm(conn, keys, DEADLINE_IN)
    body = [("MULTI",)] + [("BITCOUNT", k) for k in stretch] * reps + \
           [("MSET",) + mset_args, ("MGET",) + kk, ("EXEC",)]
    replies = conn.pipeline(body)
    check("S2 MSET then MGET inside one transaction", replies[-1][-1], [b"z"] * 8)

    # RENAME reads the SOURCE's liveness on one owner and writes on another. Its elapsed control is
    # an execution-time `ERR no such key` element, followed by a successful GET element that proves
    # EXEC ran instead of discarding the transaction.
    def rename_round(offset):
        armed_at, _ = arm(conn, [keys[0]], offset)   # source carries the shared deadline
        conn.cmd("SET", move_dst, "d")               # destination is plain and distinguishable
        # RENAME transfers the source's deadline to the destination, so the destination is read
        # INSIDE the transaction: under one cut the renamed value is there, and reading it after
        # EXEC would only re-measure the deadline that has since passed.
        return run_block(conn, stretch, reps,
                         [("RENAME", keys[0], move_dst), ("GET", move_dst)], armed_at, offset)

    for offset, tag in ((DEADLINE_IN, "across a deadline inside the transaction"),
                        (FAR, "control, deadline an hour out")):
        (reply, moved), took, inside = rename_round(offset)
        if offset == DEADLINE_IN:
            check_true("S2 RENAME: EXEC started before the deadline it must pin", inside)
            check_true("S2 RENAME: the transaction really outlived the deadline",
                       took >= DEADLINE_IN * 3, "exec=%.0fms" % took)
        check("S2 RENAME %s" % tag, reply, b"OK")
        check("S2 RENAME %s moved the value" % tag, moved, b"v")
    (reply, moved), _, _ = rename_round(ELAPSED)
    check("S2 RENAME control returns its runtime error inside EXEC",
          reply, RespError("ERR no such key"))
    check("S2 RENAME runtime error does not stop the following EXEC element", moved, b"d")
    check("S2 RENAME control, elapsed source left the destination untouched",
          conn.cmd("GET", move_dst), b"d")

    # RENAMENX and COPY read the DESTINATION's liveness, which sits on a different owner from the
    # source's -- the sharpest two-hop straddle in the family. They are ordinary runtime
    # conditionals inside EXEC: a live destination returns 0, while one already absent at the
    # transaction's cut permits the write and returns 1.
    family("RENAMENX", ("RENAMENX", keys[0], move_dst), 0, 1,
           [move_dst], [keys[0]])
    family("COPY", ("COPY", keys[0], move_dst), 0, 1,
           [move_dst], [keys[0]])

    # A physically missing source exercises the same RENAME outcome without expiry. Commands on
    # both sides prove this is an EXEC array element, not EXECABORT: the transaction really ran.
    before = "expwide:rename-before"
    after = "expwide:rename-after"
    conn.pipeline([("DEL", keys[0]), ("SET", move_dst, "d"),
                   ("DEL", before), ("DEL", after)])
    missing_exec = conn.pipeline([
        ("MULTI",), ("SET", before, "1"), ("RENAME", keys[0], move_dst),
        ("SET", after, "2"), ("EXEC",)])[-1]
    check("S2 missing-source RENAME is one error element and EXEC continues",
          missing_exec, [b"OK", RespError("ERR no such key"), b"OK"])
    check("S2 command before missing-source RENAME committed", conn.cmd("GET", before), b"1")
    check("S2 command after missing-source RENAME committed", conn.cmd("GET", after), b"2")
    check("S2 missing-source RENAME left destination untouched", conn.cmd("GET", move_dst), b"d")

    # Bare parity: the lowering change is MULTI-only. Conditional zero/success and RENAME's runtime
    # error must retain their existing outside-transaction answers on the same proven split pair.
    for name, command, live_dst, dead_dst in (
            ("RENAMENX", ("RENAMENX", keys[0], move_dst), 0, 1),
            ("COPY", ("COPY", keys[0], move_dst), 0, 1)):
        arm(conn, [move_dst], FAR, [keys[0]])
        check("S2 bare %s sees a live destination" % name, conn.cmd(*command), live_dst)
        arm(conn, [move_dst], ELAPSED, [keys[0]])
        check("S2 bare %s sees an elapsed destination" % name, conn.cmd(*command), dead_dst)
    conn.pipeline([("DEL", keys[0]), ("SET", move_dst, "d")])
    check("S2 bare RENAME missing source", conn.cmd("RENAME", keys[0], move_dst),
          RespError("ERR no such key"))
    check("S2 bare RENAME missing source left destination untouched",
          conn.cmd("GET", move_dst), b"d")

    # The *STORE family folds several sources on several owners into one cardinality.
    sadd = lambda key: [("SADD", key, "m")]
    src = keys[:3]
    family("SINTERSTORE", ("SINTERSTORE", dst[1]) + tuple(src), 1, 0, src, make=sadd)
    family("SUNIONSTORE", ("SUNIONSTORE", dst[1]) + tuple(src), 1, 0, src, make=sadd)
    family("SDIFFSTORE", ("SDIFFSTORE", dst[1], src[0]), 1, 0, [src[0]], make=sadd)
    set_active(conn, 1)


def test_transaction_agreement(conn):
    """Two IDENTICAL reads in one transaction, either side of the deadline, must agree."""
    set_active(conn, 0)
    conn.cmd("FLUSHALL")
    keys, owners = owner_keys(conn, "expwide:agree")
    stretch, reps, _ = build_stretcher(conn, owners)

    def two_reads(offset_ms):
        armed_at, _ = arm(conn, keys, offset_ms)
        body = [("MULTI",), ("MGET",) + tuple(keys)] + \
               [("BITCOUNT", k) for k in stretch] * reps + [("MGET",) + tuple(keys), ("EXEC",)]
        replies = conn.pipeline(body)
        block = replies[-1]
        return (sum(v is not None for v in block[0]),
                sum(v is not None for v in block[-1]))

    check("S3 two reads either side of the deadline agree", two_reads(DEADLINE_IN), (8, 8))
    check("S3 control, deadline an hour out", two_reads(FAR), (8, 8))
    check("S3 control, elapsed before the transaction", two_reads(ELAPSED), (0, 0))

    # The cut works the other way too, and this is the risk a pinned clock CREATES: a relative
    # deadline set inside a transaction is measured from the same instant, so a PTTL read after a
    # long block still reports the full span.  Redis behaves this way for the same reason (its
    # EXPIRE family adds to commandTimeSnapshot()), so the exact figure is comparable; a server
    # whose clock moved under the block would report the span minus the block.
    conn.cmd("DEL", "expwide:reltimer")
    body = [("MULTI",), ("SET", "expwide:reltimer", "v", "PX", "60000")] + \
           [("BITCOUNT", k) for k in stretch] * reps + [("PTTL", "expwide:reltimer"), ("EXEC",)]
    start = time.monotonic()
    replies = conn.pipeline(body)
    block_ms = (time.monotonic() - start) * 1000.0
    check_true("S3 the relative-deadline block really ran", block_ms >= 3 * DEADLINE_IN,
               "block=%.0fms" % block_ms)
    check("S3 a relative deadline set inside a transaction is measured from its one instant",
          replies[-1][-1], 60000)
    conn.cmd("DEL", "expwide:reltimer")
    set_active(conn, 1)


# --------------------------------------------------------------------------------------------
# S4. A cross-shard script compares deadlines against the wall clock like everything else.
# The single-owner geometry is the control: it exercised a different code path and always agreed
# with the oracle, so a failure that appears only in the split geometry is attributable.


SCRIPT_TWO = "return {redis.call('GET', KEYS[1]) or 'NIL', redis.call('GET', KEYS[2]) or 'NIL'}"


def test_script_clock(conn):
    set_active(conn, 0)
    conn.cmd("FLUSHALL")
    pair_split = None
    if not sharded(conn):
        pair_same = ("expwide:s:a", "expwide:s:b")
        NOTES.append("S4 script: one keyspace, only the single-owner geometry exists")
    else:
        base_key = "expwide:s:0000"
        base = conn.cmd("DEBUG", "SHARD", base_key)
        pair_same = None
        for index in range(1, 6000):
            key = "expwide:s:%04d" % index
            shard = conn.cmd("DEBUG", "SHARD", key)
            if shard == base and pair_same is None:
                pair_same = (base_key, key)
            elif shard != base and pair_split is None:
                pair_split = (base_key, key)
            if pair_same and pair_split:
                break
        check_true("S4 found a same-owner and a split-owner pair",
                   pair_same is not None and pair_split is not None)

    def probe(pair, offset_ms):
        conn.cmd("FLUSHALL")
        arm(conn, list(pair), offset_ms)
        return conn.cmd("DBSIZE"), conn.cmd("EVAL", SCRIPT_TWO, "2", *pair)

    for label, pair in (("single-owner", pair_same), ("cross-shard", pair_split)):
        if pair is None:
            continue
        resident, reply = probe(pair, ELAPSED)
        check("S4 %s script hides an elapsed key" % label, reply, [b"NIL", b"NIL"])
        check("S4 %s elapsed key was still physically resident" % label, resident, 2)
        _, reply = probe(pair, FAR)
        check("S4 %s control, live keys" % label, reply, [b"v", b"v"])
    set_active(conn, 1)


# --------------------------------------------------------------------------------------------
# S5. The cut must not become a licence to ignore deadlines.


def test_ordinary_expiry_still_works(conn):
    set_active(conn, 0)
    conn.cmd("FLUSHALL")
    keys, _ = owner_keys(conn, "expwide:plain")
    before = info_counter(conn, "expired_keys")
    arm(conn, keys, ELAPSED)
    check("S5 an ordinary cross-shard MGET still hides elapsed keys",
          conn.cmd("MGET", *keys), [None] * 8)
    if before >= 0:
        check("S5 the reap counter moved by exactly eight",
              info_counter(conn, "expired_keys") - before, 8)
    conn.cmd("FLUSHALL")
    before = info_counter(conn, "expired_keys")
    conn.pipeline([("SET", k, "v") for k in keys])
    check("S5 control, TTL-free keys are all present", conn.cmd("MGET", *keys), [b"v"] * 8)
    if before >= 0:
        check("S5 control, the same reap counter can report zero",
              info_counter(conn, "expired_keys") - before, 0)
    set_active(conn, 1)


# --------------------------------------------------------------------------------------------
# repro-natural: the hook-free exposure rate.  Not a gate mode.


def repro_natural(conn, rounds=6000, horizon_ms=5, burst=2000000):
    """The hook-free exposure rate.  Not a gate mode: it reports a rate, not a verdict.

    Each trial is one pipelined round trip that arms a shared deadline `horizon_ms` out, then fires
    an eight-owner MGET right at it.  The only widener is ORDINARY PIPELINED LOAD from a second
    connection aimed at one of the eight owners, which is real client traffic, not a hook.

    ARMING GUARD: after reading all sixteen arming replies the client checks its own clock.  A
    reply already read was produced by a command the server had already run, so a clean guard
    proves every PEXPIREAT ran before the deadline and none could take the "deadline already past
    -> delete it now" branch.  Without the guard a partial MGET cannot be told apart from keys that
    were removed at arming time -- an unguarded draft of this probe "reproduced" tearing on the
    vanilla redis oracle, 95 times in 20000, purely from that.

    Arms: armed (load on), ctrl-idle (load off), ctrl-far (load on, deadline an hour out).  A
    non-zero ctrl-far means the load alone explains the tears and the armed number means nothing.
    """
    import threading
    set_active(conn, 0)
    conn.cmd("FLUSHALL")
    keys, shards = owner_keys(conn, "expwide:nat")
    victim = shards[-1] if shards else None
    filler = keys_on_owners(conn, "expwide:fill", [victim] * 1 if victim is not None else None)
    filler = (filler * 64)[:64]
    conn.pipeline([("SET", k, "x") for k in filler])
    sets = b"".join(encode("SET", k, "v") for k in keys)
    load = b"".join(encode("GET", filler[i % len(filler)]) for i in range(burst))
    load_bytes = burst * 7                      # "$1\r\nx\r\n" per GET
    stop = threading.Event()

    def pump():
        peer = Resp()
        try:
            while not stop.is_set():
                peer.sock.sendall(load)
                got = 0
                while got < load_bytes:
                    chunk = peer.sock.recv(1 << 20)
                    if not chunk:
                        return
                    got += len(chunk)
        except Exception:
            pass
        finally:
            peer.close()

    def run(offset_ms, contend, trials):
        thread = None
        if contend:
            stop.clear()
            thread = threading.Thread(target=pump, daemon=True)
            thread.start()
            time.sleep(0.05)
        torn = present = absent = guard = 0
        example = None
        for _ in range(trials):
            armed_at = time.time()
            deadline = int(armed_at * 1000) + offset_ms
            conn.sock.sendall(
                sets + b"".join(encode("PEXPIREAT", k, str(deadline)) for k in keys))
            for _ in range(16):
                conn.read()
            if time.time() >= armed_at + horizon_ms / 1000.0:
                guard += 1
                conn.cmd("MGET", *keys)
                continue
            while time.time() < armed_at + horizon_ms / 1000.0 - 0.0002:
                pass
            conn.sock.sendall(encode("MGET", *keys))
            reply = conn.read()
            live = sum(v is not None for v in reply)
            if live == len(keys):
                present += 1
            elif live == 0:
                absent += 1
            else:
                torn += 1
                if example is None:
                    example = reply
        if thread is not None:
            stop.set()
            thread.join(120)
        return torn, present, absent, guard, example

    print("geometry: owners %r, load aimed at owner %r" % (shards, victim))
    for label, offset, contend, trials in (
            ("armed     (D=now+%dms, load)" % horizon_ms, horizon_ms, True, rounds),
            ("ctrl-idle (D=now+%dms, idle)" % horizon_ms, horizon_ms, False, rounds),
            ("ctrl-far  (D=now+1h,   load)", FAR, True, max(1000, rounds // 5))):
        torn, present, absent, guard, example = run(offset, contend, trials)
        print("  %-30s TORN %d/%d  (all-present %d, all-absent %d, guard-skipped %d)"
              % (label, torn, trials - guard, present, absent, guard))
        if example:
            print("        example torn reply: %r" % (example,))
    set_active(conn, 1)
    return 0


def main():
    conn = Resp()
    try:
        if MODE == "repro-natural":
            return repro_natural(conn)
        if MODE != "all":
            raise SystemExit("unknown mode %s" % MODE)
        test_fanout_hook(conn)
        test_transaction_family(conn)
        test_transaction_agreement(conn)
        test_script_clock(conn)
        test_ordinary_expiry_still_works(conn)
    finally:
        try:
            conn.cmd("DEBUG", "ATOMIC-FANOUT-DEFER", "0")
            conn.cmd("DEBUG", "SET-ACTIVE-EXPIRE", "1")
            conn.cmd("FLUSHALL")
        except Exception:
            pass
        conn.close()

    for note in NOTES:
        print("  note " + note)
    for failure in FAILURES:
        print("  FAIL " + failure)
    print("expwide: %d checks, %d failures -> %s"
          % (CHECKS, len(FAILURES), "PASS" if not FAILURES else "FAIL"))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
