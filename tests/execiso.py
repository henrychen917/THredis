#!/usr/bin/env python3
"""EXEC is never weaker than the same command run bare -- the in-EXEC cross-shard read gate.

Usage: execiso.py HOST PORT

THE DEFECT THIS GATE LOCKS
--------------------------
A cross-shard MGET wrapped in MULTI/EXEC TORE, while the identical BARE MGET against the same
server, the same keys and the same concurrent writer did not.  MULTI therefore gave a cross-shard
read LESS isolation than running the command without MULTI.

Mechanism: a MULTI child ran with `!force_atomic`, which is the term that keeps a fragment out of
the read-cut machinery, and every child then bound the UNBOUND read context (multi.inc,
`atomic_set_read_context(UINT64_MAX, ...)`).  Each of the read's eight fragments therefore answered
"newest committed at the instant THIS fragment runs".  A foreign transaction publishing its one
ticket while the fan-out was in flight was seen by the fragments that ran after it and missed by
those that ran before: one MGET, two generations, e.g. [8,8,8,8,8,8,8,7].

The counter is the proof of the mechanism: on the clean bare arm `atomic_fanout_cuts` advanced by
one per read, and on the tearing in-EXEC arm it advanced by ZERO -- the MULTI children provably
never entered the machinery at all.

THE CONTRACT, STATED NARROWLY
-----------------------------
EXEC must never be weaker than the same commands executed bare.  A bare cross-shard read is atomic,
so a read inside MULTI must be at least as atomic.  That is the whole bar.  It does NOT promise a
transaction-wide snapshot and it does NOT change write visibility -- see NOTES-EXECISO.md.

NOT VACUOUS, BY CONSTRUCTION
----------------------------
- GEOMETRY.  The hash seed is drawn from the kernel at every boot, so this battery asks DEBUG SHARD
  for the real owner of every key and refuses to pass unless the read spans more than one owner.
- DETERMINISM.  DEBUG ATOMIC-FANOUT-DEFER now parks MULTI-child fragments as well as ordinary
  scatter fragments: every fragment of the transaction except the one on its lead shard is
  re-queued until the deadline.  A park, not a stall -- the executor stays free, so the foreign
  transaction really does commit BETWEEN two fragments of one in-EXEC read.
- THE WINDOW REALLY OPENED.  Every armed round asserts the foreign transaction's reply landed
  BEFORE the parked read's, and that the read was held for a real fraction of the park.  An unarmed
  control asserts the same shape completes in a small fraction of that time, so a passing armed
  round cannot be explained by "the hook did nothing".
- COUNTER.  atomic_exec_read_cuts counts transactions that published a read cut because they carry
  a multi-owner read.  It is separate from atomic_fanout_cuts precisely so background bare traffic
  cannot satisfy the assertion.  Every armed arm demands it advanced, in BOTH atomic modes.
- BARE REFERENCE.  The same straddle shape is run against a BARE read in the same round, because
  bare is the bar the contract names.
- NEGATIVE CONTROLS.  A same-owner read and a write-only transaction must register NO cut: the fix
  is scoped to multi-owner read fragments and must cost nothing outside them.  A cut must be a cut
  and not a freeze, and it must never hide the reading connection's own writes.

Boot: --enable-debug-command yes.  Without it the armed arms cannot run and the battery says so.
"""

import socket
import sys
import threading
import time


HOST, PORT = sys.argv[1], int(sys.argv[2])
FAIL = 0
HOLD_US = 300000          # 300ms park for the in-EXEC fan-out window
ROUNDS = 3


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
        self.sock = socket.create_connection((HOST, PORT), timeout=60)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def close(self):
        try:
            self.file.close()
            self.sock.close()
        except OSError:
            pass

    def send(self, *args):
        self.sock.sendall(frame(*args))

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed")
        kind = line[:1]
        if kind == b"+":
            return line[1:-2]
        if kind == b"-":
            return RespError(line[1:-2].decode(errors="replace"))
        if kind == b":":
            return int(line[1:-2])
        if kind == b"$":
            size = int(line[1:-2])
            if size == -1:
                return None
            data = self.file.read(size)
            if self.file.read(2) != b"\r\n":
                raise ValueError("bad bulk trailer")
            return data
        if kind == b"*":
            count = int(line[1:-2])
            if count == -1:
                return None
            return [self.read() for _ in range(count)]
        raise ValueError("bad RESP type %r" % line[:20])

    def cmd(self, *args):
        self.send(*args)
        return self.read()


ADMIN = Resp()


def info_field(field):
    text = ADMIN.cmd("INFO", "stats")
    if not isinstance(text, (bytes, bytearray)):
        return 0
    for line in text.decode(errors="replace").split("\r\n"):
        if line.startswith(field + ":"):
            return int(line.split(":", 1)[1])
    return 0


def set_atomic(value):
    return ADMIN.cmd("CONFIG", "SET", "atomic", str(value)) == b"OK"


def arm(microseconds):
    return ADMIN.cmd("DEBUG", "ATOMIC-FANOUT-DEFER", str(microseconds)) == b"OK"


def transaction(client, commands):
    """MULTI + every command + EXEC on one connection; returns the EXEC reply."""
    if client.cmd("MULTI") != b"OK":
        raise AssertionError("MULTI refused")
    for command in commands:
        if client.cmd(*command) != b"QUEUED":
            raise AssertionError("%r was not queued" % (command,))
    return client.cmd("EXEC")


def string_writer(keys, value):
    return [("SET", key, value) for key in keys]


def set_writer(keys, old, new):
    # One transaction, one ticket: every set must flip generation together.
    commands = []
    for key in keys:
        commands.append(("SREM", key, old))
        commands.append(("SADD", key, new))
    return commands


# ---- geometry oracle -----------------------------------------------------------------------
KEYS = ["execiso:%d" % i for i in range(8)]
SETS = ["execisoset:%d" % i for i in range(8)]
SAME = []
HAVE_DEBUG = not isinstance(ADMIN.cmd("DEBUG", "SHARD", KEYS[0]), (RespError, Exception))
SPAN, SET_SPAN, SAME_OWNER = [], [], None
if HAVE_DEBUG:
    def pick_spanning(prefix, wanted=8, limit=8000):
        picked, seen, probe = [], set(), 0
        while len(picked) < wanted and probe < limit:
            key = "%s:%d" % (prefix, probe)
            owner = int(ADMIN.cmd("DEBUG", "SHARD", key))
            if owner not in seen:
                seen.add(owner)
                picked.append(key)
            probe += 1
        return picked, sorted(seen)

    picked, SPAN = pick_spanning("execiso")
    if len(picked) == 8:
        KEYS = picked
    picked, SET_SPAN = pick_spanning("execisoset")
    if len(picked) == 8:
        SETS = picked
    # A same-owner set of eight keys: its read resolves inside ONE owner task, so it cannot
    # straddle anything and must therefore never pay for a cut.
    buckets, probe = {}, 0
    while probe < 20000 and not SAME:
        key = "execisosame:%d" % probe
        owner = int(ADMIN.cmd("DEBUG", "SHARD", key))
        buckets.setdefault(owner, []).append(key)
        if len(buckets[owner]) >= 8:
            SAME_OWNER, SAME = owner, buckets[owner][:8]
        probe += 1
note("geometry: in-EXEC reads fan out over more than one owner",
     HAVE_DEBUG and len(SPAN) > 1 and len(SET_SPAN) > 1 and len(SAME) == 8,
     "strings=%d over %s; sets=%d over %s; same-owner=%d on shard %s%s"
     % (len(KEYS), SPAN, len(SETS), SET_SPAN, len(SAME), SAME_OWNER,
        "" if HAVE_DEBUG else " (DEBUG disabled)"))


# ---- 1. deterministic in-EXEC straddle --------------------------------------------------------
def straddle_arm(mode, kind):
    """Park an in-EXEC cross-shard read across a whole foreign transaction; demand ONE generation.

    `kind` also selects the BARE reference arm, which runs the identical shape without MULTI: the
    contract is stated relative to bare, so bare is measured in the same battery.
    """
    label = "deterministic in-EXEC straddle: %s (atomic %d)" % (kind, mode)
    if not HAVE_DEBUG:
        note(label, False, "needs --enable-debug-command yes")
        return
    keys = SETS if kind == "SUNION" else KEYS
    torn, detail, opened = 0, [], 0
    before_cuts = info_field("atomic_exec_read_cuts")
    for round_id in range(ROUNDS):
        old, new = "g%d-old" % round_id, "g%d-new" % round_id
        if kind == "EXISTS":
            for key in keys:
                ADMIN.cmd("DEL", key)
            commands = string_writer(keys, new)
        elif kind == "SUNION":
            for key in keys:
                ADMIN.cmd("DEL", key)
                ADMIN.cmd("SADD", key, old)
            commands = set_writer(keys, old, new)
        else:
            for key in keys:
                ADMIN.cmd("SET", key, old)
            commands = string_writer(keys, new)

        if not arm(HOLD_US):
            note(label, False, "arm refused")
            return
        reader = Resp()
        started = time.time()
        # Issue MULTI/<read>/EXEC and DO NOT read the EXEC reply yet: the lead fragment answers
        # immediately, the rest are parked, and the foreign transaction runs inside that window.
        if reader.cmd("MULTI") != b"OK":
            note(label, False, "MULTI refused")
            reader.close()
            return
        read_cmd = {"MGET": ("MGET",) + tuple(keys),
                    "EXISTS": ("EXISTS",) + tuple(keys),
                    "SUNION": ("SUNION",) + tuple(keys)}[kind]
        if reader.cmd(*read_cmd) != b"QUEUED":
            note(label, False, "read was not queued")
            reader.close()
            return
        reader.send("EXEC")
        time.sleep(0.05)                     # the lead fragment has answered; the rest are parked
        writer = Resp()
        reply = transaction(writer, commands)
        exec_done = time.time()
        writer.close()
        outer = reader.read()
        elapsed = time.time() - started
        reader.close()
        arm(0)

        expected_writes = len(commands)
        if not isinstance(reply, list) or len(reply) != expected_writes or \
                any(isinstance(r, RespError) for r in reply):
            detail.append("round %d foreign EXEC=%r" % (round_id, reply))
            torn += 1
            continue
        if not isinstance(outer, list) or len(outer) != 1:
            detail.append("round %d in-EXEC reply=%r" % (round_id, outer))
            torn += 1
            continue
        values = outer[0]
        # The window really opened: the whole foreign transaction completed while the read was held.
        if exec_done < started or elapsed < HOLD_US / 4e6:
            detail.append("round %d window did not open (read %.3fs, foreign exec ended %.3fs in)"
                          % (round_id, elapsed, exec_done - started))
        else:
            opened += 1
        if kind == "MGET":
            ok = bool(values) and all(value == values[0] for value in values[1:])
            # The cut was pinned before the foreign transaction arrived: the answer is the OLD world.
            ok = ok and values[0] == old.encode()
        elif kind == "EXISTS":
            ok = values in (0, len(keys))
        else:
            ok = isinstance(values, list) and len(values) == 1 and values[0] == old.encode()
        if not ok:
            torn += 1
            if len(detail) < 3:
                detail.append("round %d saw %r" % (round_id, values))
    cuts = info_field("atomic_exec_read_cuts") - before_cuts
    ok = torn == 0 and opened == ROUNDS and cuts >= ROUNDS
    note(label, ok, "rounds=%d torn=%d windows_opened=%d exec_read_cuts=+%d %r"
         % (ROUNDS, torn, opened, cuts, detail[:2]))


# ---- 2. the bar itself: the same shape run BARE ------------------------------------------------
def bare_reference_arm(mode):
    """EXEC is measured against bare, so bare is measured here, under the same armed park."""
    label = "bare reference: the same parked MGET straddle is clean (atomic %d)" % mode
    if not HAVE_DEBUG:
        note(label, False, "needs --enable-debug-command yes")
        return
    torn, detail, opened = 0, [], 0
    for round_id in range(ROUNDS):
        old, new = "b%d-old" % round_id, "b%d-new" % round_id
        for key in KEYS:
            ADMIN.cmd("SET", key, old)
        if not arm(HOLD_US):
            note(label, False, "arm refused")
            return
        reader = Resp()
        started = time.time()
        reader.send("MGET", *KEYS)
        time.sleep(0.05)
        writer = Resp()
        reply = transaction(writer, string_writer(KEYS, new))
        exec_done = time.time()
        writer.close()
        values = reader.read()
        elapsed = time.time() - started
        reader.close()
        arm(0)
        if reply != [b"OK"] * len(KEYS):
            detail.append("round %d foreign EXEC=%r" % (round_id, reply))
            torn += 1
            continue
        if exec_done >= started and elapsed >= HOLD_US / 4e6:
            opened += 1
        else:
            detail.append("round %d window did not open (%.3fs)" % (round_id, elapsed))
        if not values or any(v != values[0] for v in values[1:]) or values[0] != old.encode():
            torn += 1
            if len(detail) < 3:
                detail.append("round %d saw %r" % (round_id, values))
    note(label, torn == 0 and opened == ROUNDS,
         "rounds=%d torn=%d windows_opened=%d %r" % (ROUNDS, torn, opened, detail[:2]))


# ---- 3. the park is what parks (unarmed control) ----------------------------------------------
def unarmed_control(mode):
    label = "unarmed in-EXEC read completes far inside the park budget (atomic %d)" % mode
    if not HAVE_DEBUG:
        note(label, False, "needs --enable-debug-command yes")
        return
    arm(0)
    for key in KEYS:
        ADMIN.cmd("SET", key, "control")
    reader = Resp()
    started = time.time()
    reply = transaction(reader, [("MGET",) + tuple(KEYS)])
    elapsed = time.time() - started
    reader.close()
    ok = elapsed < HOLD_US / 4e6 and reply == [[b"control"] * len(KEYS)]
    note(label, ok, "elapsed=%.4fs budget=%.3fs" % (elapsed, HOLD_US / 1e6))


# ---- 4. a cut is a cut, not a freeze -----------------------------------------------------------
def visibility_control(mode):
    """A transaction that commits BEFORE the reader's EXEC is issued must be fully visible to it."""
    label = "committed transaction is fully visible to a later in-EXEC read (atomic %d)" % mode
    if not HAVE_DEBUG:
        note(label, False, "needs --enable-debug-command yes")
        return
    for key in KEYS:
        ADMIN.cmd("SET", key, "stale")
    writer = Resp()
    reply = transaction(writer, string_writer(KEYS, "fresh"))
    writer.close()
    arm(HOLD_US)
    reader = Resp()
    outer = transaction(reader, [("MGET",) + tuple(KEYS)])
    reader.close()
    arm(0)
    ok = reply == [b"OK"] * len(KEYS) and outer == [[b"fresh"] * len(KEYS)]
    note(label, ok, "values=%r" % (outer[0][:3] if isinstance(outer, list) and outer else outer,))


# ---- 5. read-your-writes survives the pinned cut ------------------------------------------------
def ryow_control(mode):
    """The cut must never hide the reading connection's own writes -- plain, or earlier in the
    SAME transaction. The in-transaction case is the load-bearing one: those writes are private
    epoch-zero candidates, visible only through the read context's origin_conn_id overlay."""
    client = Resp()
    try:
        for index, key in enumerate(KEYS):
            client.cmd("SET", key, "own%d" % index)
        outer = transaction(client, [("MGET",) + tuple(KEYS)])
        ok = outer == [[("own%d" % i).encode() for i in range(len(KEYS))]]
        note("own plain writes stay visible to a later in-EXEC read (atomic %d)" % mode, ok,
             "values=%r" % (outer[0][:2] if isinstance(outer, list) and outer else outer,))
        # SET k0 then MGET, inside ONE transaction: the MGET must see the write it follows.
        inner = transaction(client, [("SET", KEYS[0], "mine"), ("MGET",) + tuple(KEYS)])
        ok = (isinstance(inner, list) and len(inner) == 2 and inner[0] == b"OK" and
              inner[1][0] == b"mine" and inner[1][1:] ==
              [("own%d" % i).encode() for i in range(1, len(KEYS))])
        note("own in-transaction write is visible to a later read in the same EXEC (atomic %d)"
             % mode, ok, "values=%r" % (inner[1][:2] if isinstance(inner, list) and
                                        len(inner) == 2 else inner,))
    finally:
        client.close()


# ---- 6. scope: a same-owner in-EXEC read must pay nothing ---------------------------------------
def scope_control(mode):
    label = "same-owner in-EXEC read registers NO cut (atomic %d)" % mode
    if not SAME:
        note(label, False, "no 8-key single-owner set found")
        return
    for key in SAME:
        ADMIN.cmd("SET", key, "same")
    before = info_field("atomic_exec_read_cuts")
    client = Resp()
    outer = transaction(client, [("MGET",) + tuple(SAME)])
    client.close()
    cuts = info_field("atomic_exec_read_cuts") - before
    ok = cuts == 0 and outer == [[b"same"] * len(SAME)]
    note(label, ok, "exec_read_cuts=+%d owner=%s" % (cuts, SAME_OWNER))


def cost_control(mode):
    """A write-only transaction must register no cut: the fix is off for everything it does not
    need to be on for, and this is the arm that would catch it being on."""
    before = info_field("atomic_exec_read_cuts")
    client = Resp()
    reply = transaction(client, string_writer(KEYS, "writeonly"))
    client.close()
    cuts = info_field("atomic_exec_read_cuts") - before
    ok = cuts == 0 and reply == [b"OK"] * len(KEYS)
    note("write-only transaction registers NO cut (atomic %d)" % mode, ok,
         "exec_read_cuts=+%d" % cuts)


# ---- 7. unarmed stress: the shape that reproduced the defect ------------------------------------
def stress_arm(mode, seconds=2.0, readers=3):
    for key in KEYS:
        ADMIN.cmd("SET", key, "0")
    before_cuts = info_field("atomic_exec_read_cuts")
    stop = threading.Event()
    start = threading.Barrier(readers + 1)
    lock = threading.Lock()
    state = {"torn": 0, "reads": 0, "commits": 0}
    errors, samples = [], []

    def writer():
        client = Resp()
        seq = 1
        try:
            start.wait()
            while not stop.is_set():
                reply = transaction(client, string_writer(KEYS, str(seq)))
                if reply != [b"OK"] * len(KEYS):
                    raise AssertionError("bad EXEC %r" % (reply,))
                with lock:
                    state["commits"] += 1
                seq += 1
        except Exception as exc:
            with lock:
                errors.append("writer:%s" % exc)
        finally:
            client.close()

    def reader(rid):
        client = Resp()
        try:
            start.wait()
            while not stop.is_set():
                outer = transaction(client, [("MGET",) + tuple(KEYS)])
                values = outer[0] if isinstance(outer, list) and outer else None
                with lock:
                    state["reads"] += 1
                    if not values or any(v != values[0] for v in values[1:]):
                        state["torn"] += 1
                        if len(samples) < 2:
                            samples.append(repr(values))
        except Exception as exc:
            with lock:
                errors.append("reader%d:%s" % (rid, exc))
        finally:
            client.close()

    threads = [threading.Thread(target=writer)] + [
        threading.Thread(target=reader, args=(i,)) for i in range(readers)
    ]
    for thread in threads:
        thread.start()
    time.sleep(seconds)
    stop.set()
    for thread in threads:
        thread.join(60)
    alive = [t.name for t in threads if t.is_alive()]
    cuts = info_field("atomic_exec_read_cuts") - before_cuts
    ok = (not errors and not alive and state["torn"] == 0 and
          state["reads"] > 100 and state["commits"] > 10 and cuts >= state["reads"])
    note("concurrent in-EXEC MGET never straddles a transaction (atomic %d)" % mode, ok,
         "reads=%d commits=%d torn=%d exec_read_cuts=+%d errors=%r %s"
         % (state["reads"], state["commits"], state["torn"], cuts, errors[:1], samples[:1]))


for atomic_mode in (0, 1):
    note("CONFIG SET atomic %d" % atomic_mode, set_atomic(atomic_mode))
    unarmed_control(atomic_mode)
    straddle_arm(atomic_mode, "MGET")
    straddle_arm(atomic_mode, "EXISTS")
    straddle_arm(atomic_mode, "SUNION")
    bare_reference_arm(atomic_mode)
    visibility_control(atomic_mode)
    ryow_control(atomic_mode)
    scope_control(atomic_mode)
    cost_control(atomic_mode)
    stress_arm(atomic_mode)

if HAVE_DEBUG:
    arm(0)
for key in KEYS + SETS + SAME:
    ADMIN.cmd("DEL", key)
ADMIN.close()

if FAIL:
    raise SystemExit("%d in-EXEC isolation checks failed" % FAIL)
print("in-EXEC isolation battery passed", flush=True)
