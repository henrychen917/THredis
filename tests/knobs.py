#!/usr/bin/env python3
"""Configuration grammar, live encoding controls and actual gate geometry."""
import sys

import _lib


host, port, engine, atomic = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
retired = (
    "read-local-prefetch-capture", "read-local-atomic-filter", "read-local-interleave",
    "flip-auto-band", "l3-domains", "smt-mode", "genthread-schedule",
    "atomic-window", "persist-io", "lru-clock-shift", "script-crossshard-max-bytes",
    "script-crossshard-workbench-bytes", "script-crossshard-conflict-retries",
    "script-crossshard-cut-slots", "tls-ktls", "lb", "flip-work-window",
    "script-instruction-limit",
    "load", "conf", "hash-max-compact-entries", "hash-max-compact-value",
    "list-max-compact-entries", "list-max-compact-value", "set-max-compact-entries",
    "set-max-compact-value", "zset-max-compact-entries", "zset-max-compact-value",
    "lb-sample-rate", "lb-age-sample-rate", "lb-tick-ms", "lb-imbalance-pct",
    "lb-move-cap", "lb-cooldown-ms", "ex-sched", "x-ex-sched", "x-overlap", "thread-pipeline",
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
                           ("atomic", atomic), ("key-lb", "1"), ("client-lb", "1"),
                           ("flip-auto", "0"), ("overlap", "0"), ("reorder", "0")):
        if values.get(name.encode()) != expected.encode():
            raise AssertionError("CONFIG %s differs: %r" % (name, values.get(name.encode())))
    for name in ("read-local", "key-lb", "client-lb", "flip-auto", "net-io", "overlap", "reorder"):
        result = conn.cmd("CONFIG", "SET", name, values[name.encode()])
        if not isinstance(result, _lib.RespError) or "immutable" not in str(result):
            raise AssertionError("boot-only knob was mutable: " + name)
    server = _lib.info(conn, "server")
    for name, expected in (("thread_mode", "2s"), ("shards", "16"),
                           ("read_local", "0"), ("atomic", atomic), ("overlap", "0"), ("reorder", "0"),
                           ("key_lb", "1"), ("client_lb", "1"), ("flip_auto", "0"),
                           ("flip_fingerprint_window", "0"), ("net_io", engine), ("hash", "mix64"),
                           ("zc_min", values[b"zc-min"].decode()),
                           ("multiplexing_api", "epoll" if engine == "epoll" else "io_uring")):
        if server.get(name) != expected:
            raise AssertionError("INFO server %s differs: %r" % (name, server.get(name)))
    snapshot = _lib.lbsignals(conn)
    if len(snapshot.shards) != int(server["shards"]):
        raise AssertionError("INFO shards differs from the actual shard inventory")
    for field in ("shard_home", "shard_owners"):
        entries = [tuple(map(int, pair.split(":"))) for pair in server[field].split(",")]
        mapping = dict(entries)
        if len(entries) != len(mapping) or set(mapping) != set(range(int(server["shards"]))):
            raise AssertionError("INFO %s is not a complete shard map" % field)
        if any(owner not in {t.tid for t in snapshot.threads if t.role == "ex"}
               for owner in mapping.values()):
            raise AssertionError("INFO %s names a non-executor" % field)
    lb = _lib.info(conn, "lb")
    if (lb.get("tomokv_keylb_enabled") != "1" or
            lb.get("tomokv_clientlb_enabled") != "1" or "tomokv_lb_enabled" in lb):
        raise AssertionError("INFO LB does not report the independent switches")
    if (sum(t.role == "io" for t in snapshot.threads) != int(server["io_threads"]) or
            sum(t.role == "ex" for t in snapshot.threads) != int(server["ex_threads"])):
        raise AssertionError("INFO role counts differ from actual thread inventory")
    # A boot-only echo would pass the initial comparison. Prove INFO follows a completed live
    # update too, including zero-copy's zero/off arm, and restore the caller's setting.
    original_zc = values[b"zc-min"].decode()
    try:
        for threshold in ("0", "1" if original_zc == "0" else original_zc):
            conn.must("CONFIG", "SET", "zc-min", threshold)
            if _lib.info(conn, "server").get("zc_min") != threshold:
                raise AssertionError("INFO zc_min echoed the boot request after CONFIG SET")
    finally:
        conn.must("CONFIG", "SET", "zc-min", original_zc)
    # Fresh keys on EVERY shard prove CONFIG's fan-out, not just the global GET echo.
    by_shard = {}
    for n in range(8192):
        key = "knob:encoding:%d" % n
        by_shard.setdefault(_lib.shard_of(conn, key), key)
        if len(by_shard) == int(server["shards"]):
            break
    if len(by_shard) != int(server["shards"]):
        raise AssertionError("could not cover the complete shard inventory")
    encoding_names = ["%s-max-listpack-%s" % (kind, axis)
                      for kind in ("hash", "set", "zset") for axis in ("entries", "value")]
    encoding_names.append("list-max-listpack-size")
    original = {name: values[name.encode()] for name in encoding_names}
    created = set(by_shard.values())

    def encoding(key, expected):
        actual = conn.must("OBJECT", "ENCODING", key)
        if actual != expected:
            raise AssertionError("%s encoding %r, expected %r" % (key, actual, expected))

    def add(kind, key, n, member):
        if kind == "hash":
            return conn.must("HSET", key, "f%d" % n, member)
        if kind == "set":
            return conn.must("SADD", key, member)
        return conn.must("ZADD", key, n, member)

    try:
        for kind, expanded in (("hash", b"hashtable"), ("set", b"hashtable"),
                                ("zset", b"skiplist")):
            entries, size = (kind + "-max-listpack-" + axis for axis in ("entries", "value"))
            conn.must("CONFIG", "SET", entries, 4, size, 64)
            for key in by_shard.values():
                conn.must("DEL", key)
                for n in range(4):
                    add(kind, key, n, "m%d" % n)
                encoding(key, b"listpack")
                add(kind, key, 4, "m4")
                encoding(key, expanded)
                # RESTORE must use the destination's live limits too.
                dump = conn.must("DUMP", key)
                conn.must("CONFIG", "SET", entries, 8)
                conn.must("RESTORE", key, 0, dump, "REPLACE")
                encoding(key, b"listpack")
                conn.must("CONFIG", "SET", entries, 4)
            conn.must("CONFIG", "SET", entries, 8, size, 4)
            for key in by_shard.values():
                conn.must("DEL", key)
                add(kind, key, 0, "xxxx")
                encoding(key, b"listpack")
                add(kind, key, 1, "xxxxx")
                encoding(key, expanded)
            conn.must("CONFIG", "SET", entries, 0)
            key = next(iter(by_shard.values()))
            conn.must("DEL", key)
            add(kind, key, 0, "x")
            encoding(key, expanded)
            conn.must("CONFIG", "SET", entries, original[entries], size, original[size])

        # Redis's listpack controls must not act as the missing intset count control.
        conn.must("CONFIG", "SET", "set-max-listpack-entries", 0, "set-max-listpack-value", 0)
        key = next(iter(by_shard.values()))
        conn.must("DEL", key)
        conn.must("SADD", key, *range(128))
        encoding(key, b"intset")
        conn.must("SADD", key, 128)
        encoding(key, b"hashtable")

        for fill, count in ((0, 1), (4, 4)):
            conn.must("CONFIG", "SET", "list-max-listpack-size", fill)
            for key in by_shard.values():
                conn.must("DEL", key)
                conn.must("RPUSH", key, *(["x"] * count))
                encoding(key, b"listpack")
                conn.must("RPUSH", key, "y")
                encoding(key, b"quicklist")
                if conn.must("LRANGE", key, 0, -1) != [b"x"] * count + [b"y"]:
                    raise AssertionError("list promotion lost content")

        # Both lists exceed either compact budget: different memory use witnesses that the
        # expanded node builder also consumes the knob. A promotion-only repair fails here.
        memory = []
        for fill in (-1, -3):
            conn.must("CONFIG", "SET", "list-max-listpack-size", fill)
            key = "knob:node:%d" % fill
            created.add(key)
            conn.must("DEL", key)
            conn.must("RPUSH", key, *(["x" * 128] * 512))
            encoding(key, b"quicklist")
            memory.append(conn.must("MEMORY", "USAGE", key))
        if memory[0] <= memory[1]:
            raise AssertionError("list node budgets had no observable effect: %r" % memory)

        for canonical, alias in (("hash-max-listpack-entries", "hash-max-ziplist-entries"),
                                 ("hash-max-listpack-value", "hash-max-ziplist-value"),
                                 ("zset-max-listpack-entries", "zset-max-ziplist-entries"),
                                 ("zset-max-listpack-value", "zset-max-ziplist-value"),
                                 ("list-max-listpack-size", "list-max-ziplist-size")):
            conn.must("CONFIG", "SET", alias, 7)
            for spelling in (canonical, alias):
                if conn.must("CONFIG", "GET", spelling) != [spelling.encode(), b"7"]:
                    raise AssertionError("encoding alias is not one shared value: " + spelling)
            if not isinstance(conn.cmd("CONFIG", "SET", canonical, 8, alias, 9), _lib.RespError):
                raise AssertionError("duplicate alias accepted in one CONFIG SET")
        for name, value, expected in (("hash-max-listpack-value", "1kb", b"1024"),
                                      ("zset-max-listpack-value", "2k", b"2000"),
                                      ("hash-max-listpack-entries", "9223372036854775807",
                                       b"9223372036854775807"),
                                      ("list-max-listpack-size", "-2147483648", b"-2147483648")):
            conn.must("CONFIG", "SET", name, value)
            if conn.must("CONFIG", "GET", name)[1] != expected:
                raise AssertionError("reference grammar did not round-trip: " + name)
        for name, bad in (("set-max-listpack-value", "1kb"),
                          ("set-max-listpack-entries", "01"),
                          ("hash-max-listpack-entries", "9223372036854775808"),
                          ("list-max-listpack-size", "2147483648")):
            before = conn.must("CONFIG", "GET", name)
            if not isinstance(conn.cmd("CONFIG", "SET", name, bad), _lib.RespError):
                raise AssertionError("invalid reference grammar accepted: " + name)
            if conn.must("CONFIG", "GET", name) != before:
                raise AssertionError("invalid encoding update changed state")
    finally:
        conn.must("CONFIG", "SET", *(item for pair in original.items() for item in pair))
        conn.must("DEL", *sorted(created))
    print("configuration grammar, live encoding controls and actual geometry verified")
finally:
    conn.close()
