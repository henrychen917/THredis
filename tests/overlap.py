#!/usr/bin/env python3
"""Check one externally booted overlap/read-local cell; never starts a server.

Usage: overlap.py HOST PORT 1s|2s OVERLAP READ_LOCAL [REORDER]
Boot with the requested mode/overlap/read-local, --key-lb 0 --client-lb 0, --enable-debug-command yes,
and two shard owners foreign to the reader. Repeat for all eight cells and atomic 0/1.
This is a correctness/engagement battery, not a throughput measurement. Existing
read_local_lane.py and bplus.py exercise the deterministic pressure/atomic windows.
"""
import sys
import time

import _lib


COUNTERS = ("read_local_hits", "read_local_mget_local_hits",
            "read_local_fallbacks", "read_local_fallback_missing")


def counters(conn):
    stats = _lib.info(conn, "stats")
    return {name: int(stats[name]) for name in COUNTERS}


def expect(actual, wanted, label):
    if actual != wanted:
        raise AssertionError("%s: got %r, wanted %r" % (label, actual, wanted))


def burst(conn, commands, replies):
    conn.raw(b"".join(_lib.encode(*command) for command in commands))
    for index, wanted in enumerate(replies):
        expect(conn.read(), wanted, "reply %d" % index)


def main():
    if (len(sys.argv) not in (6, 7) or sys.argv[3] not in ("1s", "2s") or
            any(value not in ("0", "1") for value in sys.argv[4:])):
        raise SystemExit(__doc__)
    host, port, mode, overlap, lane = sys.argv[1:6]
    reorder = sys.argv[6] if len(sys.argv) == 7 else "0"
    armed = lane == "1"
    control = _lib.Conn(host, port, timeout=10)
    reader = _lib.Conn(host, port, timeout=10)
    keys = []
    try:
        config = control.must("CONFIG", "GET", "*")
        config = dict(zip(config[::2], config[1::2]))
        for name, wanted in (("thread-mode", mode), ("overlap", overlap),
                             ("read-local", lane), ("key-lb", "0"), ("client-lb", "0"),
                             ("reorder", reorder)):
            expect(config.get(name.encode()), wanted.encode(), "CONFIG " + name)
        expect(control.must("CONFIG", "GET", "x-overlap"), [], "retired spelling")
        result = control.cmd("CONFIG", "SET", "overlap", overlap)
        if not isinstance(result, _lib.RespError) or "immutable" not in str(result):
            raise AssertionError("overlap must remain boot-only: %r" % result)
        server = _lib.info(control, "server")
        for name, wanted in (("thread_mode", mode), ("overlap", overlap),
                             ("read_local", str(int(armed)))):
            expect(server.get(name), wanted, "INFO " + name)
        if "x_overlap" in server:
            raise AssertionError("retired INFO spelling remains visible")

        # Select two different owners, both foreign to the reader. Never infer ownership
        # from names; fixed LB makes this witness valid through the last read below.
        reader_tid = reader.must("DEBUG", "IO-THREAD")
        topo = _lib.topology(control)
        prefix = "overlap:%d" % time.time_ns()
        chosen = {}
        for key, shard, owner in _lib.probe_keys(control, prefix, topo):
            if owner != reader_tid:
                chosen.setdefault(owner, (key, shard))
            if len(chosen) == 2:
                break
        if len(chosen) != 2:
            raise AssertionError("need two owners foreign to reader t%d" % reader_tid)
        keys = [key for key, _ in chosen.values()]
        left, right = keys
        missing = prefix + ":missing"
        value = b"immutable:" + b"v" * 256
        for key in keys:
            expect(control.must("SET", key, value), b"OK", "seed")

        before = counters(control)
        # More than two ROB wraps, then idle/resume, then another fresh burst. Exact hit
        # counts on clean state fail if admission was compiled out or silently declined.
        for _ in range(2):
            burst(reader, [("GET", left)] * 192, [value] * 192)
            burst(reader, [("MGET", left, right, missing)] * 16,
                  [[value, value, None]] * 16)
            time.sleep(0.06)
        after = counters(control)
        expect(after["read_local_hits"] - before["read_local_hits"],
               416 if armed else 0, "clean local GET/MGET engagement")
        expect(after["read_local_mget_local_hits"] - before["read_local_mget_local_hits"],
               32 if armed else 0, "local MGET engagement")
        expect(after["read_local_fallbacks"] - before["read_local_fallbacks"],
               0, "clean-read fallbacks")

        # Missing GET must lower to its owner while younger local replies still retire
        # in order. The fallback counter is the witness, not just the returned nil.
        for _ in range(80):
            # Drain each triplet before the next missing GET so a prior demoted miss
            # cannot legitimately classify the next one as ContextOwnerKey instead.
            burst(reader, [("GET", missing), ("GET", left), ("PING",)],
                  [None, value, b"PONG"])
        missing_after = counters(control)
        expect(missing_after["read_local_fallback_missing"] - after["read_local_fallback_missing"],
               80 if armed else 0, "missing-GET demotion")

        # A pending local read ahead of a conflicting write must precede that write on
        # demotion. Its younger read must see the write, across repeated ROB reuse.
        commands, replies = [], []
        old = value
        for index in range(160):
            new = ("version:%d:" % index).encode() + b"w" * 256
            commands.extend((("GET", left), ("SET", left, new), ("GET", left),
                             ("MGET", left, right), ("PING",)))
            replies.extend((old, b"OK", new, [new, value], b"PONG"))
            old = new
        burst(reader, commands, replies)
        expect(reader.must("DEBUG", "IO-THREAD"), reader_tid, "reader ownership")
        expect(_lib.topology(control).shard_owner, topo.shard_owner, "shard ownership")
        if not armed:
            expect(counters(control), before, "inactive lane counters")
        print("overlap cell PASS: mode=%s overlap=%s read-local=%s effective=%d; "
              "clean hits=%d MGET hits=%d missing demotions=%d" %
              (mode, overlap, lane, armed,
               after["read_local_hits"] - before["read_local_hits"],
               after["read_local_mget_local_hits"] - before["read_local_mget_local_hits"],
               missing_after["read_local_fallback_missing"] - after["read_local_fallback_missing"]))
    finally:
        reader.close()
        try:
            if keys:
                control.must("DEL", *keys)
        finally:
            control.close()


if __name__ == "__main__":
    main()
