#!/usr/bin/env python3
"""EXEC-vs-cross-shard-group commit-order battery. Usage: tests/multires.py HOST PORT

WHAT THIS PINS DOWN
-------------------
A cross-shard atomic write group (MSET/MSETNX/DEL/UNLINK) draws its commit ticket only once its
LAST fragment has installed. A MULTI/EXEC transaction the same connection sends AFTERWARDS can
therefore reach the ticket counter FIRST, so the two units of one connection end up with commit
tickets in the opposite order to the order the client sent them.

atomic_collapse() resolves an overlapping prefix by committed-ticket argmax. Ranking a
same-connection pair by raw ticket made the OLDER group the winner, and its value was then
spliced back into the physical slot on top of the transaction's -- destroying a write EXEC had
already ACKNOWLEDGED, and answering later reads from the group's generation. Program order, not
ticket order, is the truth within one connection: a connection's owner tasks are posted to a
shard in arrival order and drained in order, so the collapse list order already IS that
connection's program order for the key.

The two symptoms this battery separates, both starting from one acknowledged write:
  1. LOST WRITE   -- EXEC answered "the key now holds X" and the settled value is not X.
  2. STALE READ   -- reads issued after that EXEC answer from the group's generation instead.

Both are RACES, so every case runs many rounds and reports a hit count rather than a yes/no; the
pre-fix rates on this box were roughly 22% of rounds for (1) and 37% for (2). A single clean
round means nothing, which is why nothing here concludes from one.

Every case also proves its mechanism FIRED: `atomic_exec_order_holds` counts EXEC writes that
installed while an older same-connection group was still undecided on that owner, which is
exactly the hazard window. A run in which it never advances never entered the window, and is
reported as vacuous rather than as a pass. Negative controls (no group; a single-key non-group
predecessor; the transaction on a second connection) must record no loss AND are expected to
leave the counter alone.

Boot requirement: --enable-debug-command yes for DEBUG SHARD, so the key set provably spans
distinct owners. Runs under both --atomic 0 and --atomic 1; with atomics off there is no group
to invert against, so the window never opens and the counter legitimately stays at 0.
"""

import socket
import sys


HOST, PORT = sys.argv[1], int(sys.argv[2])

ROUNDS = 60
PROBES = 8
TAG = "mres"


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


def atomic_enabled(conn):
    reply = conn.command("CONFIG", "GET", "atomic")
    if isinstance(reply, list) and len(reply) == 2:
        return reply[1] not in (b"0", "0")
    return False


def owner_spread(admin):
    """One key per distinct owner, so a group over them provably spans shards."""
    per_shard = {}
    for i in range(4000):
        if len(per_shard) >= 16:
            break
        key = f"{TAG}:{i:04d}:" + "z" * 30
        shard = admin.command("DEBUG", "SHARD", key)
        if isinstance(shard, RespError):
            raise AssertionError(
                f"DEBUG SHARD refused ({shard}); boot with --enable-debug-command yes")
        per_shard.setdefault(int(shard), key)
    if len(per_shard) < 2:
        raise AssertionError(f"only {len(per_shard)} distinct owner(s); need a cross-shard spread")
    return [per_shard[s] for s in sorted(per_shard)]


def round_trip(keys, witness, group, second_conn=False, probes=0):
    """One pipelined round. Returns (exec_reply, settled_value, probe_values)."""
    conn = Conn()
    other = None
    try:
        conn.command("FLUSHALL")
        payload = b""
        if group is not None and not second_conn:
            payload += encode(*group)
        body = encode("MULTI") + encode("SET", witness, "hello") + encode("EXEC")
        if second_conn:
            if group is not None:
                conn.command(*group)
            other = Conn()
            other.send_raw(body)
            other.read()
            other.read()
            exec_reply = other.read()
        else:
            payload += body + encode("GET", witness) * probes
            conn.send_raw(payload)
            if group is not None:
                conn.read()
            conn.read()
            conn.read()
            exec_reply = conn.read()
        probe_values = [conn.read() for _ in range(probes)] if not second_conn else []
        settled = conn.command("GET", witness)
        return exec_reply, settled, probe_values
    finally:
        conn.close()
        if other is not None:
            other.close()


ACK = [b"OK"]


def case(admin, label, keys, group, second_conn=False, probes=0, seen=None):
    """Runs ROUNDS rounds. Returns the hazard-counter delta, and also records it into `seen`
    BEFORE raising, so a genuine failure cannot masquerade as a vacuous run."""
    before = holds(admin)
    losses = stale = acked = 0
    sample = None
    for _ in range(ROUNDS):
        exec_reply, settled, probe_values = round_trip(
            keys, keys[0], group, second_conn, probes)
        if exec_reply != ACK:
            continue                      # EXEC did not claim the write; not this test's case
        acked += 1
        if settled != b"hello":
            losses += 1
            if sample is None:
                sample = settled
        if any(v != b"hello" for v in probe_values):
            stale += 1
    delta = holds(admin) - before
    if seen is not None:
        seen.append(delta)
    detail = f"acked={acked} lost={losses} stale={stale} holds+{delta}"
    if losses or stale:
        raise AssertionError(
            f"{label}: {detail}; an acknowledged EXEC write did not survive "
            f"(settled={sample!r})")
    ok(f"{label}: {detail}")
    return delta


def main():
    if len(sys.argv) != 3:
        print("usage: tests/multires.py HOST PORT", file=sys.stderr)
        return 2
    admin = Conn()
    failures = 0
    armed_deltas = []
    try:
        atomic_on = atomic_enabled(admin)
        print(f"multires: atomic={'1' if atomic_on else '0'}", flush=True)
        keys = owner_spread(admin)
        print(f"  note key set spans {len(keys)} owners", flush=True)
        wide_mset = ["MSET"] + [x for k in keys for x in (k, "grp")]
        wide_del = ["DEL"] + keys

        for label, group, kwargs in (
            ("wide MSET then EXEC write (lost-write + stale-read)", wide_mset,
             {"probes": PROBES}),
            ("wide DEL then EXEC write (lost-write + stale-read)", wide_del,
             {"probes": PROBES}),
            ("wide UNLINK then EXEC write", ["UNLINK"] + keys, {"probes": PROBES}),
            ("wide MSETNX then EXEC write",
             ["MSETNX"] + [x for k in keys for x in (k, "grp")], {"probes": PROBES}),
        ):
            try:
                case(admin, label, keys, group, seen=armed_deltas, **kwargs)
            except AssertionError as failure:
                failures += 1
                print(f"  FAIL {failure}", flush=True)

        # NEGATIVE CONTROLS. Each must be clean, and each must leave the hazard counter alone --
        # a control that itself opened the window would not be controlling for anything.
        for label, group, kwargs in (
            ("control: no predecessor at all", None, {"probes": PROBES}),
            ("control: single-key DEL is not a group", ["DEL", keys[0]], {"probes": PROBES}),
            ("control: transaction on a second connection", wide_mset, {"second_conn": True}),
        ):
            try:
                delta = case(admin, label, keys, group, **kwargs)
                if delta:
                    raise AssertionError(
                        f"{label}: control opened the hazard window {delta}x; it is meant to "
                        "stay outside it")
            except AssertionError as failure:
                failures += 1
                print(f"  FAIL {failure}", flush=True)

        # VACUOUS-VALIDATION GATE. Clean data from a run that never entered the window proves
        # nothing. With atomics off there is no group to invert against, so 0 is correct there.
        window_holds = sum(armed_deltas)
        if atomic_on and window_holds == 0:
            failures += 1
            print("  FAIL the armed cases recorded 0 atomic_exec_order_holds: no EXEC write ever "
                  "installed against an undecided same-connection group, so this run never "
                  "entered the window it exists to close and its pass is vacuous", flush=True)
        elif atomic_on:
            ok(f"hazard window opened {window_holds}x across the armed cases")
        else:
            ok("atomic 0: no cross-shard group exists to invert against, window stays shut")
    except (AssertionError, EOFError, OSError) as failure:
        failures += 1
        print(f"  FAIL {failure}", flush=True)
    finally:
        try:
            admin.command("FLUSHALL")
        except (EOFError, OSError, AssertionError):
            pass
        admin.close()

    print(f"multires: {'PASS' if not failures else 'FAIL'}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
