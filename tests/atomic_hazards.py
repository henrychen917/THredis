#!/usr/bin/env python3
"""Owner-local read-context hazards at --atomic 1.  Usage: atomic_hazards.py HOST PORT

Boot: --atomic 1 --enable-debug-command yes.  The armed arms need DEBUG SHARD (geometry),
DEBUG ATOMIC-FANOUT-DEFER (floor pin) and DEBUG ATOMIC-COMMIT-DELAY (commit window); without
them the battery cannot open its windows and says so.

THE TWO DEFECTS THIS GATE LOCKS (AUDIT-ATOMICS.md S1 and S2)
-------------------------------------------------------------
S1  FLUSH READ-CUT LEAK.  FlatStore::atomic_tombstone_all (FLUSHDB/FLUSHALL/DEBUG RELOAD on a
    shard that still holds MVCC records) installed its tombstones through atomic_install_plain,
    whose last statement selects the freshly installed version as the store's read cut -- the
    right thing for its ordinary caller (a plain write's clone must be visible to the handler that
    follows), wrong for FLUSH, which runs under an unbound read context and restores nothing.  The
    store then resolved every later UNBOUND chain read at FLUSH's ticket with no origin connection.
    A direct cross-shard RENAME reads its source unbound (execute_atomic_direct_rename), so
    `MSET k...; FLUSHDB; MSET k...; RENAME k dest` on one connection answered "ERR no such key" for a
    key its own MSET had just acknowledged: the second MSET's ticket is newer than FLUSH's cut and
    the read carried conn 0, so own_committed could not rescue it.
    Needs the first MSET's records to survive to the FLUSH, which cleanup normally reclaims within
    a pass; the floor is therefore pinned by a parked cross-shard MGET on a second connection.

S2  XREAD TOUCHES NO KEY.  for_each_touched_key (atomics_glue.inc) handed the positional key table
    xread_count == 0, so a localfast XREAD/XREADGROUP named no key.  It therefore never deferred
    behind its own connection's still-undecided cross-shard DEL/EXEC on that stream, and
    xshard_plain_prepare bound no read context, so the handler resolved at (latest, no connection)
    and an own still-private tombstone or XADD was invisible: pipelined `DEL s x` then
    `XREAD STREAMS s 0` answered the deleted stream's entries.
    The window is the interval between the group's install and the publication of its ticket;
    DEBUG ATOMIC-COMMIT-DELAY holds exactly that interval open.

NOT VACUOUS, BY CONSTRUCTION
----------------------------
- GEOMETRY.  Keys are picked with DEBUG SHARD on THIS boot's hash seed so every group really is
  cross-shard (a same-owner MSET/DEL takes localfast and cannot exercise either path).
- THE WINDOW REALLY OPENED.  S1 asserts the whole write sequence finished while the pinning read
  was still parked, and that the FLUSH tombstoned live records (atomic_pending_entries holds at
  least one group entry plus one tombstone per key after the FLUSH).  S2 asserts the DEL/EXEC reply
  itself arrived no earlier than half the armed commit delay, so the group was held undecided while
  the XREAD was free to run; an unarmed control round asserts the same shape completes quickly.
"""

import socket
import sys
import time
import _lib


HOST, PORT = sys.argv[1], int(sys.argv[2])
FAIL = 0
HOLD_US = 600000        # S1: park of the floor-pinning cross-shard MGET
DELAY_US = 200000       # S2: stall between the ticket draw and its publication
FLUSH_ROUNDS = 3
FLUSH_ATTEMPTS = 3 * FLUSH_ROUNDS
XREAD_ROUNDS = 6


def note(name, ok, extra=""):
    global FAIL
    print(("  ok   " if ok else "  FAIL ") + name + (" " + extra if extra else ""), flush=True)
    if not ok:
        FAIL += 1


def frame(*args):
    out = bytearray(b"*%d\r\n" % len(args))
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        out += b"$%d\r\n" % len(arg) + arg + b"\r\n"
    return bytes(out)


class RespError:
    def __init__(self, message):
        self.message = message

    def __repr__(self):
        return "RespError(%r)" % self.message


class Resp:
    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=30)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed")
        kind = line[:1]
        if kind == b"+": return line[1:-2]
        if kind == b"-": return RespError(line[1:-2].decode(errors="replace"))
        if kind == b":": return int(line[1:-2])
        if kind == b"$":
            size = int(line[1:-2])
            if size == -1: return None
            data = self.file.read(size)
            if self.file.read(2) != b"\r\n": raise ValueError("bad bulk")
            return data
        if kind == b"*":
            count = int(line[1:-2])
            return None if count == -1 else [self.read() for _ in range(count)]
        raise ValueError(repr(line[:20]))

    def send(self, *frames):
        self.sock.sendall(b"".join(frames))

    def cmd(self, *args):
        self.send(frame(*args))
        return self.read()


def mset_args(keys, value):
    args = ["MSET"]
    for key in keys:
        args.extend((key, value))
    return args


ADMIN = Resp()
note("enable atomic lane", ADMIN.cmd("CONFIG", "SET", "atomic", "1") == b"OK")


def info_field(field):
    text = ADMIN.cmd("INFO")
    if not isinstance(text, (bytes, bytearray)):
        return None
    for line in text.decode(errors="replace").split("\r\n"):
        if line.startswith(field + ":"):
            try:
                return int(line.split(":", 1)[1])
            except ValueError:
                return None
    return None


def debug_set(name, value):
    return ADMIN.cmd("DEBUG", name, str(value)) == b"OK"


def shard_of(key):
    reply = ADMIN.cmd("DEBUG", "SHARD", key)
    return reply if isinstance(reply, int) else None


def pick_distinct(prefix, want, avoid=(), limit=6000, *, owners=None,
                  span_owners=False, lead_other_than=None):
    """Pick distinct SHARDS, optionally proving an owner crossing from LBSIGNALS.

    DEBUG SHARD returns a shard id, not a thread id: the gate has sixteen shards but
    only two owners. Distinct shard ids alone cannot prove the direct RENAME/EXEC geometry.
    """
    picked, seen, probe = [], set(), 0
    while len(picked) < want and probe < limit:
        key = "%s%d" % (prefix, probe)
        probe += 1
        shard = shard_of(key)
        if shard is None:
            return None
        if shard in seen or shard in avoid:
            continue
        if owners is not None:
            owner = owners[shard]
            if not picked and owner == lead_other_than:
                continue
            if span_owners and len(picked) == 1 and owner == owners[shard_of(picked[0])]:
                continue
        seen.add(shard)
        picked.append(key)
    return picked if len(picked) == want else None


HAVE_DEBUG = shard_of("ahz:probe") is not None
note("DEBUG command available (geometry + window hooks)", HAVE_DEBUG,
     "" if HAVE_DEBUG else "boot with --enable-debug-command yes")


# ---- S1. FLUSH must not leave a finite read cut behind -------------------------------------------
def flush_leak_round(round_id, keys, dest, pin_keys):
    old, new = "flush%d-old" % round_id, "flush%d-new" % round_id
    # FLUSHDB deletes the pinner's keys too. Every attempt needs its own populated
    # keys and new connections; a retry must not inherit a previous attempt's state.
    if ADMIN.cmd(*mset_args(pin_keys, "pin")) != b"OK":
        return "FAIL", "pin population refused"
    if not debug_set("ATOMIC-FANOUT-DEFER", HOLD_US):
        return "FAIL", "arm refused"
    pinner = writer = None
    try:
        pinner, writer = Resp(), Resp()
        pinner.send(frame("MGET", *pin_keys))
        started = time.monotonic()
        time.sleep(0.05)                 # lead fragment answered; the rest are parked with the cut
        writer.send(frame(*mset_args(keys, old)), frame("FLUSHDB"))
        r_mset1, r_flush = writer.read(), writer.read()
        live_after_flush = info_field("atomic_pending_entries")
        writer.send(frame(*mset_args(keys, new)), frame("RENAME", keys[0], dest), frame("GET", dest))
        r_mset2, r_rename, r_get = writer.read(), writer.read(), writer.read()
        writes_done = time.monotonic() - started
        pinned = pinner.read()
        park = time.monotonic() - started
    finally:
        if writer is not None:
            writer.close()
        if pinner is not None:
            pinner.close()
        disarmed = debug_set("ATOMIC-FANOUT-DEFER", 0)
    problems = []
    if r_mset1 != b"OK" or r_flush != b"OK" or r_mset2 != b"OK":
        problems.append("setup replies %r/%r/%r" % (r_mset1, r_flush, r_mset2))
    if r_rename != b"OK":
        problems.append("RENAME=%r" % r_rename)
    if r_get != new.encode():
        problems.append("GET dest=%r" % r_get)
    # FLUSH explicitly conflicts with this other connection's read. The pinner
    # supplies a floor, not a cross-client ordering promise for these plain keys.
    if (not isinstance(pinned, list) or len(pinned) != len(pin_keys) or
            any(value not in (None, b"pin") for value in pinned)):
        problems.append("pinned MGET=%r" % pinned)
    if live_after_flush is None:
        problems.append("missing atomic_pending_entries witness")
    if not disarmed:
        problems.append("disarm refused")
    # Semantic failures stay fatal even if the arm missed. Retrying a bad RENAME/GET
    # would discard precisely the read-cut defect this row exists to detect.
    if problems:
        return "FAIL", "; ".join(problems)
    # The window really opened: every write retired while the pinning read was still parked, and
    # the read was held for a real fraction of the requested park.
    if writes_done >= HOLD_US / 1e6 or park < HOLD_US / 4e6:
        problems.append("window did not open (writes %.3fs, park %.3fs)" % (writes_done, park))
    # The FLUSH really tombstoned live records: with the floor pinned nothing could be reclaimed, so
    # the pending lists hold the first MSET's group entries plus one tombstone per key.
    if live_after_flush < 2 * len(keys):
        problems.append("FLUSH found no live records (pending_entries=%r)" % live_after_flush)
    return ("MISS" if problems else "ARMED"), "; ".join(problems)


def flush_campaign(attempt):
    """Count three actually armed successes; never count an exhausted arm as a pass.

    A captured gate round finished its writes at 0.600 s, with the pin answering at
    0.601 s and no live records left: it supplied no evidence of an open window.
    Keep that 600 ms deadline and the live-record witness unchanged. Only a pure
    missed arm can be retried, on fresh state, with a fixed total attempt bound.
    """
    armed, misses = 0, []
    for index in range(FLUSH_ATTEMPTS):
        status, reason = attempt(index)
        if status == "FAIL":
            return False, "armed=%d/%d attempts=%d; %s" % (armed, FLUSH_ROUNDS, index + 1, reason)
        if status == "ARMED":
            armed += 1
        elif status == "MISS":
            misses.append("attempt %d: %s" % (index + 1, reason))
        else:
            raise AssertionError("invalid FLUSH attempt verdict %r" % status)
        if armed == FLUSH_ROUNDS:
            return True, "armed=%d/%d attempts=%d misses=%r" % (armed, FLUSH_ROUNDS, index + 1, misses)
    return False, "armed=%d/%d attempts=%d; window never armed enough times; misses=%r" % (
        armed, FLUSH_ROUNDS, FLUSH_ATTEMPTS, misses)


if HAVE_DEBUG:
    topology = _lib.topology(ADMIN)
    geometry = len(topology.owners) >= 2 and len(topology.shard_owner) >= 8
    note("S1 geometry: cross-shard MSET, RENAME across owners, cross-shard pin",
         geometry, "%d shards, %d owners; each attempt checks its actual keys" % (
             len(topology.shard_owner), len(topology.owners)))
    if geometry:
        campaign = time.monotonic_ns()
        def fresh_attempt(index):
            owners = _lib.topology(ADMIN).shard_owner
            prefix = "ahz:%d:%d:" % (campaign, index)
            keys = pick_distinct(prefix + "k", 8, owners=owners, span_owners=True)
            pins = pick_distinct(prefix + "pin", 4, owners=owners, span_owners=True)
            dest = pick_distinct(prefix + "dest", 1, owners=owners,
                                 lead_other_than=owners[shard_of(keys[0])] if keys else None)
            if not keys or not pins or not dest:
                return "FAIL", "could not select fresh cross-owner geometry"
            current = _lib.topology(ADMIN).shard_owner
            if (len({current[shard_of(key)] for key in keys}) < 2 or
                    len({current[shard_of(key)] for key in pins}) < 2 or
                    current[shard_of(keys[0])] == current[shard_of(dest[0])]):
                return "FAIL", "fresh keys lost their cross-owner geometry"
            return flush_leak_round(index, keys, dest[0], pins)
        passed, detail = flush_campaign(fresh_attempt)
        note("S1 FLUSH under a pinned floor leaves no finite read cut (RENAME after MSET sees "
             "its own write)", passed, detail)
else:
    note("S1 FLUSH read-cut leak", False, "needs DEBUG")


# ---- S2. a localfast XREAD names its stream key -------------------------------------------------
def xread_after_del_round(round_id, stream, others, armed):
    ADMIN.cmd("DEL", stream, *others)
    for i in range(3):
        ADMIN.cmd("XADD", stream, "*", "f", "v%d" % i)
    ADMIN.cmd(*mset_args(others, "x"))
    if armed and not debug_set("ATOMIC-COMMIT-DELAY", DELAY_US):
        return "arm refused"
    c = Resp()
    started = time.time()
    c.send(frame("DEL", stream, *others), frame("XREAD", "STREAMS", stream, "0"))
    deleted = c.read()
    t_del = time.time() - started
    entries = c.read()
    t_read = time.time() - started
    c.close()
    if armed:
        debug_set("ATOMIC-COMMIT-DELAY", 0)
    problems = []
    if deleted != 1 + len(others):
        problems.append("DEL=%r" % deleted)
    if entries not in (None, []):
        problems.append("XREAD after own DEL saw %r" % entries)
    if armed and t_del < DELAY_US / 2e6:
        problems.append("window did not open (DEL replied at %.3fs)" % t_del)
    if not armed and t_read > DELAY_US / 2e6:
        problems.append("unarmed control was slow (%.3fs)" % t_read)
    return "; ".join(problems)


def xread_after_exec_round(round_id, stream, other, armed):
    ADMIN.cmd("DEL", stream, other)
    if armed and not debug_set("ATOMIC-COMMIT-DELAY", DELAY_US):
        return "arm refused"
    c = Resp()
    problems = []
    if c.cmd("MULTI") != b"OK":
        problems.append("MULTI refused")
    if c.cmd("XADD", stream, "*", "f", "exec%d" % round_id) != b"QUEUED":
        problems.append("XADD not queued")
    if c.cmd("SET", other, "x") != b"QUEUED":
        problems.append("SET not queued")
    started = time.time()
    c.send(frame("EXEC"), frame("XREAD", "STREAMS", stream, "0"))
    execd = c.read()
    t_exec = time.time() - started
    entries = c.read()
    c.close()
    if armed:
        debug_set("ATOMIC-COMMIT-DELAY", 0)
    if not (isinstance(execd, list) and len(execd) == 2 and execd[1] == b"OK"):
        problems.append("EXEC=%r" % execd)
    ok = (isinstance(entries, list) and len(entries) == 1 and entries[0][0] == stream.encode() and
          len(entries[0][1]) == 1 and entries[0][1][0][1] == [b"f", ("exec%d" % round_id).encode()])
    if not ok:
        problems.append("XREAD after own EXEC saw %r" % entries)
    if armed and t_exec < DELAY_US / 2e6:
        problems.append("window did not open (EXEC replied at %.3fs)" % t_exec)
    return "; ".join(problems)


if HAVE_DEBUG:
    stream_key = "ahz:xread:s"
    owners = _lib.topology(ADMIN).shard_owner
    stream_shard = shard_of(stream_key)
    stream_owner = owners[stream_shard]
    others = pick_distinct("ahz:xread:o", 7, avoid={stream_shard}, owners=owners,
                           lead_other_than=stream_owner)
    geometry = bool(others) and len(others) >= 1
    note("S2 geometry: DEL/EXEC span the stream's owner and at least one other",
         geometry, "others=%d shards/%d owners; EXEC crosses owner %d" % (
             len(others or []), len({owners[shard_of(key)] for key in others or []}), stream_owner))
    if geometry:
        for armed, label in ((True, "armed"), (False, "unarmed control")):
            detail = []
            for round_id in range(XREAD_ROUNDS if armed else 2):
                problem = xread_after_del_round(round_id, stream_key, others, armed)
                if problem:
                    detail.append("round %d: %s" % (round_id, problem))
            note("S2 pipelined XREAD behind its own cross-shard DEL answers nil (%s)" % label,
                 not detail, "%r" % detail[:2])
        detail = []
        for round_id in range(XREAD_ROUNDS):
            problem = xread_after_exec_round(round_id, stream_key, others[0], True)
            if problem:
                detail.append("round %d: %s" % (round_id, problem))
        note("S2 pipelined XREAD behind its own EXEC{XADD} sees the entry (armed)",
             not detail, "%r" % detail[:2])
        ADMIN.cmd("DEL", stream_key, *others)
else:
    note("S2 XREAD touched-key fence", False, "needs DEBUG")

ADMIN.close()
print("FAIL=%d" % FAIL)
sys.exit(1 if FAIL else 0)
