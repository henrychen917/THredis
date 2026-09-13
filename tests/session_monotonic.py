#!/usr/bin/env python3
"""Session-monotonicity + cross-shard atomicity hammer.

Usage: session_monotonic.py HOST PORT [SECONDS] [READERS]

WHAT THIS PINS DOWN
-------------------
One connection pipelines `GET a` and `MGET a b0 b1 ...`. A writer atomically MSETs every one of
those keys to the same monotone counter, so each reply is its own consistency oracle:

  violation:  int(mget_a) < int(get_a)   -- the LATER reply answered with an OLDER world than the
                                            EARLIER reply on the SAME connection. Time ran backward
                                            inside one session.
  torn:       mget values disagree       -- one reply carried two generations of an atomic write.

Two distinct engine bugs produced these, and this battery keeps both closed:

1. THE READ CUT WAS NOT PINNED IN PROGRAM ORDER. A multi-key read pins its epoch on IO at prepare;
   a plain read used to sample the commit sequence at EXECUTION and answer with the newest
   committed world. The plain GET is posted first but executes after the MGET's pin, so a foreign
   atomic commit landing in between made the earlier reply the newer one. Widened by
   DEBUG ATOMIC-READ-DELAY, which holds a plain read on its owner before it resolves.

2. A GROUP'S TICKET BECAME VISIBLE BEFORE ITS RECORDS DID. The last owner drew the ticket out of
   the commit sequence and stored it into the group's shared epoch word two instructions later.
   A reader whose cut landed in that hole saw the group for whatever fragments it read after the
   store and missed it for the fragments it read before -- one MGET, two generations. Widened by
   DEBUG ATOMIC-COMMIT-DELAY, which holds the group between the draw and the store.

NOT VACUOUS, BY CONSTRUCTION
----------------------------
- GEOMETRY. The hash seed is drawn from the kernel at every boot, so a fixed key pair lands on one
  owner roughly one boot in `shards` -- and a same-owner run proves nothing at all, because a
  single owner serialises everything by itself. The key set is therefore wide enough that all keys
  sharing one owner is a ~1e-9 event, and where DEBUG SHARD exists the exact span is printed and
  asserted to be more than one owner.
- COUNTERS. With --atomic 1 the run asserts that atomic_read_cuts_held advanced (a read really was
  held to its pinned cut instead of "now") and, in the commit-delay arm, that atomic_commit_holds
  advanced (a read's cut really did exclude a drawn-but-unpublished ticket). Zero data with a gate
  that never opened is reported as a failure, not a pass.

Boot: --enable-debug-command yes enables the armed arms and the geometry oracle. Without it the
battery still runs its unarmed arm, which is the arm that fails on an unfixed engine.
"""

import socket
import sys
import threading
import time
from collections import Counter

HOST, PORT = sys.argv[1], int(sys.argv[2])
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0
READERS = int(sys.argv[4]) if len(sys.argv) > 4 else 2

# Seven partners plus the probe key. Every key of one MSET carries the same counter value, so the
# reply is self-checking; eight keys make "every key on one owner" a (1/shards)**7 accident.
PARTNERS = 7
PROBE = "sm:a"
KEYS = [PROBE] + ["sm:b%d" % i for i in range(PARTNERS)]


def enc(*args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        b = a if isinstance(a, bytes) else str(a).encode()
        out += [b"$%d\r\n" % len(b), b, b"\r\n"]
    return b"".join(out)


class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=20)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.f = self.s.makefile("rb")

    def command(self, *args):
        self.s.sendall(enc(*args))
        return self.read_reply()

    def read_reply(self):
        line = self.f.readline()
        if not line:
            raise EOFError("server closed the connection")
        k = line[:1]
        if k in b"+-:":
            return line[1:-2] if k != b"-" else RuntimeError(line[1:-2].decode())
        if k == b"$":
            n = int(line[1:-2])
            if n == -1:
                return None
            return self.f.read(n + 2)[:-2]
        if k == b"*":
            n = int(line[1:-2])
            if n == -1:
                return None
            return [self.read_reply() for _ in range(n)]
        raise AssertionError("marker %r" % k)


def stats(conn):
    raw = conn.command("INFO", "STATS")
    out = {}
    for line in raw.decode().splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            out[key] = value
    return out


def counter(conn, name):
    return int(stats(conn).get(name, "0"))


class Run:
    def __init__(self):
        self.lock = threading.Lock()
        self.stop = False
        self.batches = 0
        self.violations = 0
        self.torn = 0
        self.writes = 0
        self.lags = Counter()
        self.samples = []


def writer(run):
    c = Conn()
    n = 0
    args = []
    while not run.stop:
        n += 1
        args = []
        for key in KEYS:
            args += [key, n]
        c.command("MSET", *args)
        run.writes = n


def reader(run):
    c = Conn()
    c.command("MSET", *[x for key in KEYS for x in (key, 0)])
    payload = enc("GET", PROBE) + enc("MGET", *KEYS)
    nb = nv = nt = 0
    lags = Counter()
    samples = []
    while not run.stop:
        c.s.sendall(payload)
        g = c.read_reply()
        m = c.read_reply()
        nb += 1
        ga = int(g) if g else 0
        values = [int(v) if v else 0 for v in m]
        if min(values) != max(values):
            nt += 1
            if len(samples) < 4:
                samples.append(("TORN", nb, ga, values))
        if values[0] < ga:
            nv += 1
            lags[ga - values[0]] += 1
            if len(samples) < 4:
                samples.append(("VIOLATION", nb, ga, values))
    with run.lock:
        run.batches += nb
        run.violations += nv
        run.torn += nt
        run.lags.update(lags)
        run.samples.extend(samples)


def hammer(seconds):
    run = Run()
    threads = [threading.Thread(target=writer, args=(run,))]
    threads += [threading.Thread(target=reader, args=(run,)) for _ in range(READERS)]
    for t in threads:
        t.start()
    time.sleep(seconds)
    run.stop = True
    for t in threads:
        t.join()
    return run


def case_freshness_floor(failures, rounds=4000):
    """THE PROPERTY THAT MAKES THE PINNED CUT LEGAL.

    A pinned read may answer with a world older than "now", so it must never answer with a world
    older than its own ARRIVAL. Two disjoint-window shapes prove that:

      cross-connection  writer's +OK is fully received BEFORE the reader's bytes are sent, so the
                        reader's pass starts after the commit and must see it;
      read-your-writes  the same connection writes and then reads, which must see its own write
                        whatever epoch the read pinned.

    Both are the cases a stale cut would break, and neither tolerates a single miss.
    """
    w = Conn()
    r = Conn()
    stale_cross = stale_own = 0
    first = None
    for i in range(1, rounds + 1):
        w.command("MSET", *[x for key in KEYS for x in (key, i)])
        own = w.command("MGET", *KEYS)
        own_values = [int(v) if v else 0 for v in own]
        if min(own_values) < i:
            stale_own += 1
            first = first or ("own", i, own_values)
        got = r.command("GET", PROBE)
        seen = int(got) if got else 0
        cross = r.command("MGET", *KEYS)
        cross_values = [int(v) if v else 0 for v in cross]
        if seen < i or min(cross_values) < i:
            stale_cross += 1
            first = first or ("cross", i, [seen] + cross_values)
    print("freshness floor    rounds=%-8d acked-write-not-visible: cross-conn=%d own-conn=%d"
          % (rounds, stale_cross, stale_own))
    if stale_cross or stale_own:
        failures.append("freshness floor: %d cross-connection and %d read-your-writes miss(es) of "
                        "an already-acknowledged write, first %r" % (stale_cross, stale_own, first))
    w.s.close()
    r.s.close()


def main():
    admin = Conn()
    failures = []
    notes = []

    atomic_on = True
    mode = admin.command("CONFIG", "GET", "atomic")
    if isinstance(mode, list) and len(mode) == 2:
        atomic_on = mode[1] != b"0"

    # ---- geometry gate -----------------------------------------------------------------------
    shard_of = {}
    probe = admin.command("DEBUG", "SHARD", PROBE)
    have_debug = not isinstance(probe, Exception)
    if have_debug:
        for key in KEYS:
            shard_of[key] = int(admin.command("DEBUG", "SHARD", key))
        span = sorted(set(shard_of.values()))
        print("geometry: %d key(s) over %d owner(s) %s" % (len(KEYS), len(span), span))
        if len(span) < 2:
            failures.append(
                "every key landed on one owner: a single owner serialises the whole workload, so "
                "this run could not have entered either window (re-boot; the hash seed is random)")
    else:
        print("geometry: DEBUG SHARD unavailable (no --enable-debug-command); relying on %d keys, "
              "for which one-owner is a ~1e-9 accident" % len(KEYS))

    def arm(name, value):
        if not have_debug:
            return False
        reply = admin.command("DEBUG", name, str(value))
        if isinstance(reply, Exception):
            return False
        return True

    def run_arm(label, seconds, commit_delay=0, read_delay=0, expect_counter=None):
        armed = True
        if commit_delay or read_delay or have_debug:
            armed = arm("ATOMIC-COMMIT-DELAY", commit_delay) and \
                    arm("ATOMIC-READ-DELAY", read_delay)
        if (commit_delay or read_delay) and not armed:
            notes.append("%s skipped: DEBUG window hooks unavailable" % label)
            return
        before = stats(admin)
        run = hammer(seconds)
        after = stats(admin)
        arm("ATOMIC-COMMIT-DELAY", 0)
        arm("ATOMIC-READ-DELAY", 0)

        def delta(name):
            return int(after.get(name, "0")) - int(before.get(name, "0"))

        print("%-22s batches=%-9d writes=%-8d violations=%-7d torn=%-7d "
              "[groups+%d read_cuts_held+%d commit_holds+%d]" %
              (label, run.batches, run.writes, run.violations, run.torn,
               delta("atomic_groups"), delta("atomic_read_cuts_held"),
               delta("atomic_commit_holds")))
        if run.lags:
            print("      lag histogram (get_a - mget_a): %s" % dict(sorted(run.lags.items())))
        for kind, batch, ga, values in run.samples[:4]:
            print("      %s batch=%d get_a=%d mget=%s" % (kind, batch, ga, values))
        if run.violations:
            failures.append("%s: %d session-monotonicity violation(s) -- a later reply answered "
                            "with an older world than an earlier one on the same connection"
                            % (label, run.violations))
        if run.torn:
            message = "%s: %d torn read(s) -- one reply carried two generations of an atomic write"
            if atomic_on:
                failures.append(message % (label, run.torn))
            else:
                notes.append((message % (label, run.torn)) +
                             " (expected with --atomic 0: cross-shard atomicity is the feature "
                             "that is switched off)")
        if not atomic_on:
            return
        if delta("atomic_groups") == 0:
            failures.append("%s: no cross-shard atomic group committed, so the run never entered "
                            "the window it claims to close" % label)
        if expect_counter and delta(expect_counter) == 0:
            failures.append("%s: %s did not advance -- the guard never opened and the clean "
                            "result is vacuous" % (label, expect_counter))

    case_freshness_floor(failures)
    short = max(4.0, SECONDS / 4.0)
    # Unarmed is the arm that fails on an unfixed engine; the armed arms are the deterministic ones.
    run_arm("unarmed", SECONDS, expect_counter="atomic_read_cuts_held")
    run_arm("read-delay 20us", short, read_delay=20, expect_counter="atomic_read_cuts_held")
    run_arm("commit-delay 100us", short, commit_delay=100, expect_counter="atomic_commit_holds")

    for note in notes:
        print("  note %s" % note)
    for failure in failures:
        print("  FAIL %s" % failure)
    print("session_monotonic: %s" % ("PASS" if not failures else "FAIL"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
