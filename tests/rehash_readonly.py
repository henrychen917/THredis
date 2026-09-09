#!/usr/bin/env python3
"""Resize completion under GET-only traffic; an existing DEBUG-enabled server is required.

Run on a fresh 16-shard boot, in split/fused with read-local 0/1. No server is started here.
The owner's DEBUG REHASH-STATE returns its actual cursor, capacities, live counts and starts.
Arming disables expiry maintenance, grows past the normal trigger, and observes the live old
table. Re-enabling maintenance is the last state-changing command before the three-second
read phase. All subsequent requests are GET or the observational DEBUG REHASH-STATE.

Negative control: remove active_expire()'s rehash_step() in a throwaway CLEAN build. This
battery must time out with the old table still present; working reads alone cannot pass it.
Restoring find()'s rehash_step() instead must fail the frozen-maintenance control.
"""
import sys
import time
from dataclasses import dataclass

from _lib import Conn, encode, info, shards_of, topology


BOUND_SECONDS = 3.0
TARGET_OLD_CAPACITY = 4096
MAX_KEYS = 8192
VALUE = b"rehash-readonly-value"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


@dataclass(frozen=True)
class State:
    shard: int
    starts: int
    capacity: int
    old_capacity: int
    cursor: int
    old_live: int
    keys: int

    @property
    def remaining(self):
        return self.old_capacity - self.cursor


def state(conn):
    row = conn.cmd("DEBUG", "REHASH-STATE")
    require(isinstance(row, list) and len(row) == 7 and
            all(isinstance(n, int) and n >= 0 for n in row),
            "DEBUG REHASH-STATE unavailable/malformed (no skip): %r" % (row,))
    result = State(*row)
    require(result.shard == 0 and result.cursor <= result.old_capacity,
            "diagnostic must observe shard 0 on its owner: %r" % (result,))
    return result


def read_batch(conn, keys):
    conn.raw(b"".join(encode("GET", key) for key in keys))
    for key in keys:
        require(conn.read() == VALUE, "GET lost/corrupted %s during resize" % key)


def candidates(conn):
    # Hash seeds differ across boots: ask the router in bounded batches, never guess affinity.
    for first in range(0, MAX_KEYS * 64, 512):
        keys = ["rehash-readonly:%d" % n for n in range(first, first + 512)]
        for key, (sid, _) in zip(keys, shards_of(conn, keys)):
            if sid == 0:
                yield key
    raise AssertionError("bounded shard-0 key discovery exhausted (no skip)")


def run(host, port, expected_mode=None, expected_local=None):
    conn = Conn(host, port, timeout=3.0)
    try:
        topo = topology(conn)
        meta = info(conn, "server")
        lane = int(meta["read_local"])
        require(lane in (0, 1) and meta["thread_mode"] == topo.mode, "reported read-local/mode")
        if expected_mode is not None:
            require(topo.mode == {"split": "2s", "fused": "1s"}[expected_mode] and
                    lane == expected_local, "boot did not enable the requested mode/read-local posture")
        require(len(topo.shard_owner) == 16 and len(topo.roles) == 8,
                "battery requires the gate's 16 shards on eight threads")
        require(conn.cmd("FLUSHDB") == b"OK", "fresh arming state")
        require(conn.cmd("DEBUG", "SET-ACTIVE-EXPIRE", 0) == b"OK",
                "maintenance pause must arm (no skip)")
        baseline = state(conn)
        require(baseline.keys == 0 and baseline.old_capacity == 0, "empty, stable baseline")
        source = candidates(conn)
        inserted = []
        for _ in range(MAX_KEYS // 64):
            batch = [next(source) for _ in range(64)]
            conn.raw(b"".join(encode("SET", key, VALUE) for key in batch))
            for key in batch:
                require(conn.read() == b"OK", "grow insertion failed: %s" % key)
            inserted.extend(batch)
            armed = state(conn)
            if armed.old_capacity >= TARGET_OLD_CAPACITY:
                break
        else:
            raise AssertionError("never entered the required resize (no skip): %r" % (armed,))
        require(armed.starts > baseline.starts and armed.remaining > 0 and armed.old_live > 0,
                "must witness actual growth with live keys and unexamined old slots")
        require(armed.capacity == 2 * armed.old_capacity and armed.keys == len(inserted),
                "must be a growth resize with every inserted key present")
        print("ARMED mode=%s read_local=%d bound=%.1fs %s" %
              (topo.mode, lane, BOUND_SECONDS, armed), flush=True)

        # Reads alone must preserve the cursor while owner maintenance is paused. This is also
        # a control for the observer: querying the counters must never do maintenance itself.
        read_batch(conn, inserted[:32] + inserted[-32:])
        require(state(conn) == armed, "a lookup/diagnostic advanced the paused resize")

        # This is the LAST mutation of test/control state. The client now sends only reads.
        require(conn.cmd("DEBUG", "SET-ACTIVE-EXPIRE", 1) == b"OK",
                "maintenance resume must succeed")
        start = time.monotonic()
        deadline = start + BOUND_SECONDS
        previous = armed
        reads = 0
        samples = 0
        while time.monotonic() < deadline:
            conn.sock.settimeout(max(0.001, deadline - time.monotonic()))
            keys = [inserted[(reads + i) % len(inserted)] for i in range(32)]
            read_batch(conn, keys)
            reads += len(keys)
            current = state(conn)
            samples += 1
            require(current.starts == armed.starts and current.keys == armed.keys and
                    current.capacity == armed.capacity,
                    "unexpected mutation/new resize during reads: %r -> %r" % (armed, current))
            require(current.remaining <= previous.remaining and current.old_live <= previous.old_live,
                    "resize counters went backwards")
            previous = current
            if current.old_capacity == 0:
                break
        elapsed = time.monotonic() - start
        print("RESULT mode=%s reads=%d samples=%d elapsed=%.6fs remaining=%d->%d "
              "old_live=%d->%d final=%s" %
              (topo.mode, reads, samples, elapsed, armed.remaining, previous.remaining,
               armed.old_live, previous.old_live, previous), flush=True)
        require(elapsed <= BOUND_SECONDS and previous.old_capacity == 0 and
                previous.cursor == 0 and previous.old_live == 0,
                "resize did not COMPLETE within %.1fs of read-only traffic" % BOUND_SECONDS)
        require(reads > 0 and armed.remaining > previous.remaining,
                "reads and store-counter progress must both fire")
        conn.sock.settimeout(3.0)
        for first in range(0, len(inserted), 128):
            read_batch(conn, inserted[first:first + 128])
        print("PASS: completed resize and verified all %d values" % len(inserted), flush=True)
    finally:
        # The control connection must not leave active expiry disabled even on an arming failure.
        conn.sock.settimeout(3.0)
        try:
            require(conn.cmd("DEBUG", "SET-ACTIVE-EXPIRE", 1) == b"OK", "restore maintenance")
        finally:
            conn.close()


if __name__ == "__main__":
    require(len(sys.argv) in (3, 5), "usage: rehash_readonly.py HOST PORT [split|fused 0|1]")
    posture = (sys.argv[3], int(sys.argv[4])) if len(sys.argv) == 5 else (None, None)
    run(sys.argv[1], int(sys.argv[2]), *posture)
