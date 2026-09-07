#!/usr/bin/env python3
"""Directed split local-read activation, owner routing, RYOW and FLIP checks.

Run against a maintainer-started, otherwise idle server:
  tests/rl2s.py HOST PORT
  --thread-mode 2s --read-local 1 --overlap 0 --shards 16 --ratio 6:2
  --enable-debug-command yes --key-lb 0 --client-lb 0 --flip-auto 0

This script never launches a server. Wrong geometry or a lane that never fires
is a failure, including failure to reach every IO thread after bounded fresh
connections. FLIP is restored in finally. Keys use a unique test prefix.
"""
import time

import _lib


def require(ok, detail):
    if not ok:
        raise AssertionError(detail)


def pipeline(conn, commands):
    conn.raw(b"".join(_lib.encode(*command) for command in commands))
    return [conn.read() for _ in commands]


def rows(conn):
    # Boot/FLIP readiness precedes the first instruction in a new IO loop. Give
    # that entry a bounded chance to publish; a configured-but-dark lane fails.
    for _ in range(200):
        info = _lib.info(conn, "server")
        if int(info["read_local_active_threads"]) == int(info["io_threads"]):
            break
        time.sleep(0.01)
    require(info["thread_mode"] == "2s" and info["read_local"] == "1",
            "requires an effective split local-read lane")
    result = {}
    for key, value in info.items():
        if key.startswith("read_local_thread_"):
            row = dict(part.split("=", 1) for part in value.split(","))
            result[int(key.removeprefix("read_local_thread_"))] = row
    io = {tid for tid, row in result.items() if row["role"] == "ifid"}
    ex = {tid for tid, row in result.items() if row["role"] == "ex"}
    require(len(io) == int(info["io_threads"]) and len(ex) == int(info["ex_threads"]),
            "INFO role rows disagree with split geometry")
    require(len(io) == int(info["read_local_active_threads"]),
            "some IO reader loops never activated")
    require(all(row["active"] == ("1" if tid in io else "0")
                and (tid not in io or row["shards"] == "0")
                for tid, row in result.items()),
            "IO owns shards, or lane activation disagrees with roles")
    topology = _lib.topology(conn)
    require(set(topology.shard_owner.values()) <= ex, "a shard owner is in the IO tier")
    for tid, row in result.items():
        require(int(row["shards"]) == list(topology.shard_owner.values()).count(tid),
                "INFO shard counts disagree with published ownership")
    return result, io, ex


def counters(conn):
    info = _lib.info(conn, "stats")
    names = ("read_local_hits", "read_local_mget_local_hits", "read_local_fallbacks",
             "read_local_arms", "read_local_write_ring_sidecars", "read_local_write_ring_records")
    return {name: int(info[name]) for name in names}


def owner_ops(conn):
    return sum(shard.ops for shard in _lib.lbsignals(conn).shards)


def local_round(conn, ctl, a, b):
    # Warm arming and retire any setup writes before the measured clean run.
    require(conn.cmd("GET", a) == b"a", "GET warmup failed")
    before = counters(ctl)
    ex_before = owner_ops(ctl)
    commands = [("GET", a)] * 32 + [("MGET", a, b)] * 32
    require(pipeline(conn, commands) == [b"a"] * 32 + [[b"a", b"b"]] * 32,
            "clean GET/MGET replies differ")
    after = counters(ctl)
    require(after["read_local_hits"] - before["read_local_hits"] == 64,
            "clean commands silently used owner tasks")
    require(after["read_local_mget_local_hits"] - before["read_local_mget_local_hits"] == 32,
            "MGET never entered the lane")
    require(after["read_local_fallbacks"] == before["read_local_fallbacks"],
            "uncontended clean reads fell back")
    require(owner_ops(ctl) == ex_before, "local reads executed on shard owners")


def exercise_readers(host, port, ctl, a, b):
    initial, io, _ = rows(ctl)
    found = {}
    previous = initial
    try:
        # Each retry is a fresh connection, so SO_REUSEPORT gets another independent choice.
        # Exhaustion fails; we never bless a subset of the IO tier as full activation.
        for _ in range(32 + 16 * len(io)):
            conn = _lib.Conn(host, port, timeout=20)
            try:
                local_round(conn, ctl, a, b)
                current, current_io, _ = rows(ctl)
                require(current_io == io, "unexpected role change during directed reads")
                advanced = [tid for tid in io
                            if int(current[tid]["mget_hits_total"]) >
                            int(previous[tid]["mget_hits_total"])]
                require(len(advanced) == 1, "cannot attribute reads to exactly one IO thread")
                tid = advanced[0]
                previous = current
                if tid not in found:
                    found[tid] = conn
                    conn = None
                if set(found) == io:
                    break
            finally:
                if conn is not None:
                    conn.close()
        require(set(found) == io, "fresh connections never fired lanes on IO tids %r" % (io - set(found)))

        # Overlapping writes, reads and MGETs across several ROB windows in one
        # wire burst. This checks exact RYOW replies; it does not claim a write-ring
        # overflow, which also depends on concurrent owner progress.
        for conn in found.values():
            commands, expected = [], []
            value_a, value_b = b"a", b"b"
            for n in range(40):
                value_a = ("a-%d" % n).encode()
                commands += [("SET", a, value_a), ("GET", a), ("MGET", a, b)]
                expected += [b"OK", value_a, [value_a, value_b]]
                value_b = ("b-%d" % n).encode()
                commands += [("SET", b, value_b), ("MGET", a, b)]
                expected += [b"OK", [value_a, value_b]]
            require(pipeline(conn, commands) == expected, "pipelined RYOW or reply order broke")
            require(pipeline(conn, [("SET", a, b"a"), ("SET", b, b"b")]) == [b"OK"] * 2,
                    "restore after RYOW failed")
            local_round(conn, ctl, a, b)
    finally:
        for conn in found.values():
            conn.close()


def main():
    host, port = _lib.host_port()
    ctl = _lib.Conn(host, port, timeout=30)
    for knob in ("key-lb", "client-lb", "flip-auto"):
        require(ctl.cmd("CONFIG", "GET", knob) == [knob.encode(), b"0"],
                "directed RL2S test requires --%s 0" % knob)
    _, io, ex = rows(ctl)
    unit = int(_lib.info(ctl, "server")["flip_unit_threads"])
    require(len(ex) > unit, "requires at least two EX scheduling units for the FLIP round trip")
    prefix = "rl2s:%d:" % time.time_ns()
    a, b, _, _ = _lib.cross_owner_pair(ctl, prefix)
    require(pipeline(ctl, [("SET", a, b"a"), ("SET", b, b"b")]) == [b"OK"] * 2,
            "seed failed")
    try:
        # A new pure-writer connection must neither arm nor allocate an RYOW sidecar.
        writer = _lib.Conn(host, port, timeout=20)
        try:
            before, ex_before = counters(ctl), owner_ops(ctl)
            require(pipeline(writer, [("SET", a, b"a")] * 64) == [b"OK"] * 64,
                    "pure SET failed")
            require(counters(ctl) == before, "pure SET armed or entered a reader lane")
            require(owner_ops(ctl) - ex_before == 64, "SET did not execute on owners")
        finally:
            writer.close()
        exercise_readers(host, port, ctl, a, b)
        require(ctl.cmd("FLIP", len(io) + unit, len(ex) - unit) == b"OK", "EX -> IO failed")
        _, grown_io, grown_ex = rows(ctl)
        require(len(grown_io) == len(io) + unit and len(grown_ex) == len(ex) - unit,
                "FLIP did not change actual geometry")
        exercise_readers(host, port, ctl, a, b)
        require(ctl.cmd("FLIP", len(io), len(ex)) == b"OK", "IO -> EX failed")
        _, restored_io, restored_ex = rows(ctl)
        require(len(restored_io) == len(io) and len(restored_ex) == len(ex), "roles not restored")
        exercise_readers(host, port, ctl, a, b)
        print("ok: RL2S owner routing, every IO lane, GET/MGET, RYOW and FLIP round trip")
    finally:
        try:
            require(ctl.cmd("FLIP", len(io), len(ex)) == b"OK", "cleanup could not restore roles")
            ctl.cmd("DEL", a, b)
        finally:
            ctl.close()


if __name__ == "__main__":
    main()
