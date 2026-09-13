#!/usr/bin/env python3
"""Removed CONFIG names and actual gate geometry, on each persistence/atomic boot."""
import sys

import _lib


host, port, engine, atomic = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
retired = (
    "read-local-prefetch-capture", "read-local-atomic-filter", "read-local-interleave",
    "flip-auto-band", "shard-home", "l3-domains", "smt-mode", "genthread-schedule",
    "atomic-window", "persist-io", "lru-clock-shift", "script-crossshard-max-bytes",
    "script-crossshard-workbench-bytes", "script-crossshard-conflict-retries",
    "script-crossshard-cut-slots", "tls-ktls", "key-lb", "client-lb",
    "lb-sample-rate", "lb-age-sample-rate", "lb-tick-ms", "lb-imbalance-pct",
    "lb-move-cap", "lb-cooldown-ms", "ex-sched", "overlap", "thread-pipeline",
)
conn = _lib.Conn(host, port)
try:
    config = conn.must("CONFIG", "GET", "*")
    values = dict(zip(config[::2], config[1::2]))
    for name in retired:
        if name.encode() in values or conn.must("CONFIG", "GET", name) != []:
            raise AssertionError("retired CONFIG name is visible: " + name)
        if not isinstance(conn.cmd("CONFIG", "SET", name, "0"), _lib.RespError):
            raise AssertionError("retired CONFIG name remains writable: " + name)
    for name, expected in (("thread-mode", "2s"), ("net-io", engine), ("read-local", "0"),
                           ("atomic", atomic), ("lb", "1")):
        if values.get(name.encode()) != expected.encode():
            raise AssertionError("CONFIG %s differs: %r" % (name, values.get(name.encode())))
    for name in ("read-local", "lb", "net-io"):
        result = conn.cmd("CONFIG", "SET", name, values[name.encode()])
        if not isinstance(result, _lib.RespError) or "immutable" not in str(result):
            raise AssertionError("boot-only knob was mutable: " + name)
    server = _lib.info(conn, "server")
    for name, expected in (("thread_mode", "2s"), ("shards", "16"),
                           ("read_local", "0"), ("atomic", atomic)):
        if server.get(name) != expected:
            raise AssertionError("INFO server %s differs: %r" % (name, server.get(name)))
    snapshot = _lib.lbsignals(conn)
    if len(snapshot.shards) != int(server["shards"]):
        raise AssertionError("INFO shards differs from the actual shard inventory")
    if (sum(t.role == "io" for t in snapshot.threads) != int(server["io_threads"]) or
            sum(t.role == "ex" for t in snapshot.threads) != int(server["ex_threads"])):
        raise AssertionError("INFO role counts differ from actual thread inventory")
    print("configuration reduction and actual geometry verified")
finally:
    conn.close()
