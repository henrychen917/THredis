#!/usr/bin/env python3
"""Withdrawn-candidate battery: an aborted MSETNX must not reach a later EXEC.
Usage: tests/multirace.py HOST PORT

WHAT THIS PINS DOWN
-------------------
Under --atomic 1 a cross-shard MSETNX is a SINGLE-HOP atomic write: every owner installs its
candidate physically first and only then discovers whether some OTHER owner already holds one of
the keys. When it does, the group sets `aborted`, answers :0 and its installed candidates stay
"forever invisible at epoch zero" -- masked by the pending chain and unwound by atomic_collapse().
MSETNX is therefore the one write kind that publishes a candidate it may still WITHDRAW.

atomic_resolve_internal()'s RYOW overlay is keyed on the CONNECTION, not on the logical unit: an
epoch-zero record whose origin_conn_id matches the reader is `own_private` and wins outright.
That is required -- a transaction must see its own still-private installs -- but it also exposed
the in-flight MSETNX's candidate to any LATER command of the same connection. Ordinary pipelined
commands never reach it (xshard_task_should_defer holds them behind their own undecided
predecessor); a MULTI/EXEC fragment did, because it was dispatched without that hold.

So `prepare_write_key()` cloned the withdrawn value as the transaction's OWN candidate and the
transaction committed it with a real ticket. A `MSETNX` that answered :0 and wrote nothing left
`hello` permanently installed, visible to every connection:

    DEL B v0..v5 ; SET B blocker
    MSETNX v0 hello ... v5 hello B hello      -> :0        (B exists, so nothing may be written)
    MULTI ; INCRBY v0 -2 ; INCRBY v1 -2 ; EXEC
      -> pre-fix: -ERR value is not an integer   (the transaction read the withdrawn "hello")
      -> redis:   :-2 :-2
    MGET v0 v1                                  -> pre-fix: "hello" "hello", from ANY connection

The fix parks the transaction fragment behind an older same-connection unit that has installed on
this owner but has not yet decided, so the EXEC runs against the group's verdict rather than its
guess.

This is a RACE -- the owner of a victim key must install before the owner of the blocker vetoes --
so every case runs many rounds and reports a hit count. Pre-fix rates on this box were roughly
40-80% of rounds for the armed case in the first rounds after a boot.

The COMMIT control is what stops the fix from degenerating into "make the group invisible": the
same shape with no blocker must let the EXEC see the MSETNX's value, i.e. read-your-own-writes
across two units of one connection still holds.

Non-vacuity: `atomic_exec_order_holds` counts a transaction meeting an undecided same-connection
unit on an owner -- exactly this window. Under atomic 1 the armed cases must advance it, or the
run never entered the window and its pass proves nothing. Entering it is a race, and under the
gate's own geometry it is bimodal: 8 of 10 runs open the window hundreds to thousands of times and
the rest open it exactly zero times. The armed pair is therefore re-armed on fresh connections up
to ARM_ATTEMPTS times, which re-rolls the dynamic placement that decides it; a tree that has lost
the park opens the window on no attempt and still fails. Under atomic 0 MSETNX is two-hop and
decides before it installs, so the window cannot open and the counter must read exactly zero;
that arm asserts zero rather than merely tolerating it.

Correctness of the fix's PLACEMENT is a second, separate claim with its own counter.
`atomic_exec_order_late` counts the same question re-asked at install time, and must read zero in
both modes: the park is only safe at dispatch because nothing can appear on this owner between the
dispatch check and the installs. The two counters are deliberately NOT summed -- `holds` is
non-zero by design in exactly the runs that matter, so a shared counter could never be checked
against zero and a violation would hide inside it.

BOTH MODES RUN FROM ONE BOOT, driven by this battery through CONFIG SET (restored at exit). The
armed debug-surface boot it joins is shared with scriptatomic/execatomic/execiso/execfix, each of
which flips `atomic` itself and leaves it flipped, so the boot flag does not decide the mode a
later battery runs under -- measured: the gate's "multirace (atomic 0)" row reported atomic=1.

Boot requirement: --enable-debug-command yes for DEBUG SHARD, so the key set provably spans
distinct owners.
"""

import socket
import sys


HOST, PORT = sys.argv[1], int(sys.argv[2])

ROUNDS = 200
VICTIMS = 6
# Attempts allowed to get the armed pair INTO the hazard window before the vacuity gate calls
# the run vacuous. See run_mode() for the measurement this number comes from.
ARM_ATTEMPTS = 4
TAG = "mrace"


class RespError(Exception):
    pass


def encode(*args):
    out = [f"*{len(args)}\r\n".encode()]
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        out.extend((f"${len(arg)}\r\n".encode(), arg, b"\r\n"))
    return b"".join(out)


class Conn:
    def __init__(self, timeout=30):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect((HOST, PORT))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def send(self, *args):
        self.sock.sendall(encode(*args))

    def send_raw(self, payload):
        self.sock.sendall(payload)

    def command(self, *args):
        self.send(*args)
        return self.read()

    def read(self):
        prefix = self.file.read(1)
        if not prefix:
            raise EOFError("server closed the connection")
        line = self.file.readline()
        if not line.endswith(b"\r\n"):
            raise AssertionError(f"bad RESP line: {prefix + line!r}")
        value = line[:-2]
        if prefix == b"+":
            return value
        if prefix == b"-":
            return RespError(value.decode("utf-8", "replace"))
        if prefix == b":":
            return int(value)
        if prefix == b"$":
            size = int(value)
            if size == -1:
                return None
            payload = self.file.read(size)
            if self.file.read(2) != b"\r\n":
                raise AssertionError("bad bulk trailer")
            return payload
        if prefix == b"*":
            size = int(value)
            if size == -1:
                return None
            return [self.read() for _ in range(size)]
        raise AssertionError(f"unknown RESP prefix: {prefix!r}")

    def close(self):
        if self.sock is None:
            return
        self.file.close()
        self.sock.close()
        self.sock = None


def ok(label):
    print(f"  ok   {label}", flush=True)


def stats(conn):
    raw = conn.command("INFO", "STATS")
    if not isinstance(raw, bytes):
        raise AssertionError(f"INFO STATS returned {raw!r}")
    out = {}
    for line in raw.decode().splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            out[key] = value
    return out


def holds(conn):
    table = stats(conn)
    if "atomic_exec_order_holds" not in table:
        raise AssertionError("INFO STATS has no atomic_exec_order_holds counter")
    return int(table["atomic_exec_order_holds"])


def late(conn):
    """INFO atomic_exec_order_late -- the install-time ordering violations the dispatch park is
    supposed to make impossible. A BUG counter: it must read zero at every point in every mode.

    Returns None on a build that predates the counter. This battery is also run against the PRE
    binary to show the defect, and there the question cannot be asked at all -- which must be
    reported as its own distinct row rather than being allowed to look like the leak."""
    table = stats(conn)
    if "atomic_exec_order_late" not in table:
        return None
    return int(table["atomic_exec_order_late"])


def atomic_setting(conn):
    reply = conn.command("CONFIG", "GET", "atomic")
    if not isinstance(reply, list) or len(reply) != 2:
        raise AssertionError(f"CONFIG GET atomic returned {reply!r}")
    value = reply[1]
    return value.decode() if isinstance(value, bytes) else str(value)


def set_atomic(conn, value):
    """Both modes are driven from ONE boot, on purpose. The armed debug-surface boot this battery
    joins is shared with scriptatomic/execatomic/execiso/execfix, every one of which flips `atomic`
    itself and leaves it flipped -- so the boot's --atomic flag says nothing about the mode a later
    battery actually runs under, and the gate's "(atomic 0)" row was in fact running atomic 1.
    Setting the mode here is what makes each arm's claim true rather than merely labelled."""
    reply = conn.command("CONFIG", "SET", "atomic", value)
    if isinstance(reply, RespError):
        raise AssertionError(f"CONFIG SET atomic {value} refused: {reply}")
    live = atomic_setting(conn)
    if live != value:
        raise AssertionError(f"CONFIG SET atomic {value} did not take (reads {live})")


def owner_spread(admin, wanted):
    """One key per distinct owner, so the MSETNX provably spans shards and its blocker lives on
    an owner other than the victims'. A single-owner boot cannot race install against veto."""
    per_shard = {}
    for i in range(4000):
        if len(per_shard) >= wanted:
            break
        key = f"{TAG}:{i:04d}:" + "z" * 30
        shard = admin.command("DEBUG", "SHARD", key)
        if isinstance(shard, RespError):
            raise AssertionError(
                f"DEBUG SHARD refused ({shard}); boot with --enable-debug-command yes")
        per_shard.setdefault(int(shard), key)
    if len(per_shard) < 3:
        raise AssertionError(
            f"only {len(per_shard)} distinct owner(s); this battery needs a blocker and at least "
            "two victims on separate owners")
    keys = [per_shard[s] for s in sorted(per_shard)]
    return keys[0], keys[1:]


def build_round(blocker, victims, block, multi_blocker):
    """The armed shape, as one pipelined write. Returns (setup_commands, body_commands).

    `setup` clears the victims, optionally installs the blocker that forces the abort, and issues
    the cross-shard MSETNX. `body` is the MULTI/EXEC that follows it. They are separated only so
    the second-connection control can send the body somewhere else; in the armed case both go out
    in ONE write, which is what keeps the transaction being parsed and dispatched while the MSETNX
    group is still deciding."""
    setup = [("DEL", blocker) + tuple(victims)]
    if block and multi_blocker:
        setup += [("MULTI",), ("SETNX", blocker, "blocker"),
                  ("SET", f"{TAG}:pad", "pad"), ("EXEC",)]
    elif block:
        setup.append(("SET", blocker, "blocker"))
    pairs = [x for v in victims for x in (v, "hello")]
    setup.append(("MSETNX", *pairs, blocker, "hello"))
    body = [("MULTI",), ("INCRBY", victims[0], "-2"),
            ("INCRBY", victims[1], "-2"), ("EXEC",)]
    return setup, body


def run_case(label, blocker, victims, block, probe_when,
             second_conn=False, multi_blocker=False):
    """ROUNDS rounds on ONE long-lived connection. Yields (msetnx, exec_reply, mget, foreign).

    The keyspace is cleared ONCE, here, and each round then clears only its own keys with the DEL
    it already carries. A FLUSHALL per round was measured to close the window almost completely --
    it quiesces every owner's pending list between rounds, so the MSETNX group has nothing left to
    race against -- and dropped the unpatched binary's per-round hit rate from ~8% to ~1%."""
    conn = Conn()
    foreign = Conn()
    txn = Conn() if second_conn else None
    try:
        conn.command("FLUSHALL")
        for _ in range(ROUNDS):
            setup, body = build_round(blocker, victims, block, multi_blocker)
            if second_conn:
                conn.send_raw(b"".join(encode(*c) for c in setup))
                msetnx_reply = [conn.read() for _ in setup][-1]
                txn.send_raw(b"".join(encode(*c) for c in body))
                exec_reply = [txn.read() for _ in body][-1]
                mget_after = conn.command("MGET", *victims)
            else:
                pipe = setup + body + [("MGET",) + tuple(victims)]
                conn.send_raw(b"".join(encode(*c) for c in pipe))
                replies = [conn.read() for _ in pipe]
                msetnx_reply = replies[len(setup) - 1]
                exec_reply = replies[len(setup) + len(body) - 1]
                mget_after = replies[-1]
            # A DIFFERENT connection carries a different origin_conn_id, so it can never be
            # answered from the originating connection's RYOW overlay. If it sees the value too,
            # the withdrawn candidate reached the shared table -- which is what the defect did.
            # Probed only when this connection already disagrees with `probe_when`: an extra
            # round trip on every round quiesces the owners between rounds and measurably closes
            # the window (it took the unpatched binary from ~8% of rounds to under 1%). Rounds are
            # drained, so the next round's DEL has not been sent yet and the probe still sees the
            # disputed state.
            foreign_after = (foreign.command("MGET", *victims)
                             if mget_after != probe_when else mget_after)
            yield msetnx_reply, exec_reply, mget_after, foreign_after
    finally:
        conn.close()
        foreign.close()
        if txn is not None:
            txn.close()


def case_abort(admin, label, blocker, victims, second_conn=False, multi_blocker=False,
               seen=None):
    """The MSETNX must abort, so no victim may ever hold its value. The two victims the
    transaction increments end at -2 because the aborted MSETNX left them absent and INCRBY
    created them; every other victim must stay absent. `hello` anywhere is the leak."""
    before = holds(admin)
    expected = [b"-2", b"-2"] + [None] * (len(victims) - 2)
    leaked = foreign_leaked = wrong_exec = rounds = 0
    sample = None
    for msetnx_reply, exec_reply, mget_after, foreign_after in run_case(
            label, blocker, victims, True, expected, second_conn, multi_blocker):
        if msetnx_reply != 0:
            raise AssertionError(
                f"{label}: MSETNX answered {msetnx_reply!r}, not :0; the blocker did not block "
                "and the case is not the one this battery covers")
        rounds += 1
        if exec_reply != [-2, -2]:
            wrong_exec += 1
            if sample is None:
                sample = exec_reply
        if mget_after != expected:
            leaked += 1
            if sample is None:
                sample = mget_after
        if foreign_after != expected:
            foreign_leaked += 1
            if sample is None:
                sample = ("foreign connection", foreign_after)
    delta = holds(admin) - before
    if seen is not None:
        seen.append(delta)
    detail = (f"rounds={rounds} leaked={leaked} foreign_leaked={foreign_leaked} "
              f"bad_exec={wrong_exec} holds+{delta}")
    if leaked or foreign_leaked or wrong_exec:
        raise AssertionError(
            f"{label}: {detail}; an MSETNX that answered :0 wrote a value anyway "
            f"(sample={sample!r})")
    ok(f"{label}: {detail}")
    return delta


def case_commit(admin, label, blocker, victims, seen=None):
    """CONTROL, and the one that stops the fix from being 'hide the group'. With no blocker the
    MSETNX COMMITS, so the EXEC that follows it on the same connection MUST see hello: INCRBY
    answers an error and every victim still reads hello afterwards, on this connection and on a
    foreign one. A fix that made the group's candidate invisible instead of ordering the reader
    would show :-2 here."""
    before = holds(admin)
    wanted = [b"hello"] * len(victims)
    stale = rounds = 0
    sample = None
    for msetnx_reply, exec_reply, mget_after, foreign_after in run_case(
            label, blocker, victims, False, wanted):
        if msetnx_reply != 1:
            raise AssertionError(
                f"{label}: MSETNX answered {msetnx_reply!r}, not :1; every key was absent so it "
                "was required to install")
        rounds += 1
        errored = (isinstance(exec_reply, list) and len(exec_reply) == 2
                   and all(isinstance(r, RespError) for r in exec_reply))
        if not errored or mget_after != wanted or foreign_after != wanted:
            stale += 1
            if sample is None:
                sample = (exec_reply, mget_after, foreign_after)
    delta = holds(admin) - before
    if seen is not None:
        seen.append(delta)
    detail = f"rounds={rounds} stale={stale} holds+{delta}"
    if stale:
        raise AssertionError(
            f"{label}: {detail}; the transaction did not read the committed MSETNX it followed "
            f"(sample={sample!r})")
    ok(f"{label}: {detail}")
    return delta


def case_liveness(admin, label, keys, rounds=150):
    """LIVENESS, and the one case that is about the FIX rather than the defect.

    The repair makes a transaction fragment WAIT on an older same-connection unit. A wait can
    deadlock, and NOTES-MULTIRES.md §5(a) records an earlier attempt that did. The shape that
    could still close a cycle is a TWO-PHASE cross-shard unit -- RENAME, LMPOP, SMOVE -- whose
    second-phase fragment lands on an owner where the younger transaction has already installed:
    the unit would then be waiting on the transaction through atomic_group_has_own_undecided()
    while the transaction waits on the unit through the new park.

    So: send all three of those, cross-owner, with a MULTI/EXEC touching each of their
    destinations, in ONE pipelined write, and require the whole round to ANSWER. The replies are
    fully determined, so this is a correctness assertion too -- but its job is that a wedge shows
    up as a red row in seconds rather than as a hung gate."""
    before = holds(admin)
    src, dst, list_a, list_b, set_a, set_b = keys[:6]
    conn = Conn(timeout=20)
    pipe = [("DEL", src, dst, list_a, list_b, set_a, set_b),
            ("SET", src, "1"), ("RENAME", src, dst),
            ("RPUSH", list_a, "x"), ("RPUSH", list_b, "y"),
            ("LMPOP", "2", list_a, list_b, "LEFT"),
            ("SADD", set_a, "m"), ("SMOVE", set_a, set_b, "m"),
            ("MULTI",), ("INCR", dst), ("RPUSH", list_b, "z"), ("SADD", set_b, "q"), ("EXEC",)]
    payload = b"".join(encode(*command) for command in pipe)
    want = [2, 2, 1]
    try:
        conn.command("FLUSHALL")
        for round_index in range(1, rounds + 1):
            conn.send_raw(payload)
            try:
                replies = [conn.read() for _ in pipe]
            except (socket.timeout, TimeoutError) as timed_out:
                raise AssertionError(
                    f"{label}: round {round_index} never answered ({timed_out}); a transaction "
                    "fragment and an older same-connection two-phase unit are waiting on each "
                    "other") from None
            for position, reply in enumerate(replies):
                if isinstance(reply, RespError):
                    raise AssertionError(
                        f"{label}: round {round_index} answered {reply} to {pipe[position]}")
            if replies[-1] != want:
                raise AssertionError(
                    f"{label}: round {round_index} EXEC answered {replies[-1]!r}, want {want!r}")
    finally:
        conn.close()
    delta = holds(admin) - before
    ok(f"{label}: rounds={rounds} holds+{delta}")
    return delta


def run_mode(admin, mode, blocker, victims):
    """The three cases plus the vacuity gate, under ONE value of `atomic`. Returns a failure
    count; never raises for a case failure, so the other mode still runs."""
    failures = 0
    print(f"multirace: atomic={mode}", flush=True)

    # THE ARMED PAIR, RE-ARMED WHILE THE WINDOW STAYS SHUT. Both cases below must enter the hazard
    # window for their pass to mean anything, and entering it is a race this test cannot force: the
    # EXEC fragment has to reach an owner while an older same-connection unit is still undecided
    # there. Measured 2026-09-07 under the gate's OWN geometry (--shards 16 --ratio 6:2, cores 0-7,
    # which is 6 io + 2 ex, not the default this battery used to be checked under): a healthy tree
    # opens the window in 8 of 10 runs, and opens it 268-8777 times when it does. The outcome is
    # bimodal per run -- hundreds of hits or exactly zero, never a thin tail -- and the key set is
    # deterministic (TAG is a constant), so what varies between runs is dynamic placement, not which
    # owners are involved. Each attempt therefore runs on FRESH connections, which re-rolls it.
    # Four attempts put a vacuous false alarm at roughly 1 in 600. A tree that has genuinely lost
    # the park opens the window on NO attempt and still fails the gate below, which is the property
    # this guard exists to protect. To induce that failure: return early from the dispatch-time
    # hold so no unit is ever parked, and every attempt reports holds+0.
    attempts = 0
    while True:
        attempts += 1
        armed_deltas = []
        armed_failures = 0

        try:
            case_abort(admin, "aborted MSETNX then EXEC write on the same connection",
                       blocker, victims, seen=armed_deltas)
        except AssertionError as failure:
            armed_failures += 1
            print(f"  FAIL {failure}", flush=True)

        try:
            case_commit(admin, "control: committed MSETNX then EXEC write (RYOW must hold)",
                        blocker, victims, seen=armed_deltas)
        except AssertionError as failure:
            armed_failures += 1
            print(f"  FAIL {failure}", flush=True)

        # Never retry past a real case failure. A correctness FAIL is the answer, and re-running
        # until it goes away is precisely the vacuity this battery refuses to commit.
        if armed_failures or mode != "1" or sum(armed_deltas) or attempts >= ARM_ATTEMPTS:
            break
        print(f"  note the hazard window stayed shut on attempt {attempts} of {ARM_ATTEMPTS}; "
              "re-arming the pair on fresh connections", flush=True)

    failures += armed_failures

    # NEGATIVE CONTROL. A foreign connection is never answered from another connection's RYOW
    # overlay, so it must be clean before AND after the fix, and it must leave the hazard
    # counter alone -- a control that opened the window would not be controlling for anything.
    try:
        delta = case_abort(admin, "control: aborted MSETNX, transaction on a second connection",
                           blocker, victims, second_conn=True)
        if delta:
            raise AssertionError(
                f"control: second connection opened the hazard window {delta}x; it is meant "
                "to stay outside it")
    except AssertionError as failure:
        failures += 1
        print(f"  FAIL {failure}", flush=True)

    # Its holds are reported but deliberately kept OUT of the vacuity sum below: this case exists
    # to prove the fix cannot wedge, and its counter reading is a by-product, not its claim.
    try:
        case_liveness(admin, "liveness: two-phase RENAME/LMPOP/SMOVE then EXEC on their targets",
                      [blocker] + list(victims))
    except AssertionError as failure:
        failures += 1
        print(f"  FAIL {failure}", flush=True)

    # VACUOUS-VALIDATION GATE, and under atomic 0 its mirror image. Under atomic 1 the armed cases
    # MUST have met an undecided same-connection unit or the run never entered the window this
    # battery exists to close. Under atomic 0 MSETNX is two-hop -- it decides before it installs --
    # so no unit of this shape is ever undecided on an owner and the counter MUST stay at zero;
    # a non-zero reading there would mean the two-hop path had grown an install-then-decide window
    # of its own, which is the same defect in the other mode.
    window_holds = sum(armed_deltas)
    if mode == "1" and window_holds == 0:
        failures += 1
        print(f"  FAIL the armed cases recorded 0 atomic_exec_order_holds across {attempts} "
              f"attempt(s) on fresh connections: no transaction fragment ever met an undecided "
              "same-connection unit, so this run never entered the window it exists to close "
              "and its pass is vacuous", flush=True)
    elif mode == "1":
        armed_on = "" if attempts == 1 else f", armed on attempt {attempts} of {ARM_ATTEMPTS}"
        ok(f"hazard window opened {window_holds}x across the armed cases{armed_on}")
    elif window_holds:
        failures += 1
        print(f"  FAIL atomic 0 opened the hazard window {window_holds}x: a two-hop MSETNX must "
              "decide before it installs, so no same-connection unit can be undecided on an "
              "owner in this mode", flush=True)
    else:
        ok("atomic 0: MSETNX is two-hop and installs nothing before it decides, window shut")

    # THE PARK FIRED EARLY ENOUGH. Separate counter, separate claim: `holds` above proves the
    # transaction was held, this proves it was held BEFORE it installed. prepare_write_key() re-asks
    # the park's question at install time, and only the owner thread may publish into its own
    # pending list, so the answer must be no -- in BOTH modes, unconditionally, from boot. A
    # non-zero reading means a record became visible between dispatch and install, which is the
    # premise of the acyclicity argument in NOTES-MULTIRACE.md §5 and the reason the hold is safe
    # to take at dispatch at all. This is asserted as an absolute rather than a delta because the
    # honest value is zero forever, not merely unchanged across this battery.
    violations = late(admin)
    if violations is None:
        failures += 1
        print("  FAIL INFO STATS has no atomic_exec_order_late counter, so this build cannot show "
              "whether the hold ran BEFORE the install; the placement claim is unfalsifiable "
              "here. Expected on a pre-fix binary, where the rows above carry the verdict",
              flush=True)
    elif violations:
        failures += 1
        print(f"  FAIL atomic_exec_order_late={violations}: an EXEC write reached its install "
              "with an undecided same-connection unit still on this owner. The dispatch-time park "
              "did not foreclose the window, so the transaction could clone a withdrawable "
              "candidate -- the defect this battery exists to close, one step later", flush=True)
    else:
        ok("no install-time ordering violation: the park fired before every install")
    return failures


def main():
    if len(sys.argv) != 3:
        print("usage: tests/multirace.py HOST PORT", file=sys.stderr)
        return 2
    admin = Conn()
    failures = 0
    booted = None
    try:
        booted = atomic_setting(admin)
        blocker, victims = owner_spread(admin, VICTIMS + 1)
        victims = victims[:VICTIMS]
        print(f"  note blocker + {len(victims)} victims, each on its own owner", flush=True)
        for mode in ("1", "0"):
            set_atomic(admin, mode)
            failures += run_mode(admin, mode, blocker, victims)
    except (AssertionError, EOFError, OSError) as failure:
        failures += 1
        print(f"  FAIL {failure}", flush=True)
    finally:
        try:
            if booted is not None:
                admin.command("CONFIG", "SET", "atomic", booted)
            admin.command("FLUSHALL")
        except (EOFError, OSError, AssertionError):
            pass
        admin.close()
    print(f"multirace: {'FAIL' if failures else 'PASS'} ({failures} failing case(s))", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
