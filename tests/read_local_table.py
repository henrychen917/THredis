#!/usr/bin/env python3
"""Directed publication/QSBR battery against an already booted server.

Usage: read_local_table.py HOST PORT fused|disabled|split [SCENARIO]
SCENARIO is all (default), prepublication, retirement, churn, or oom.

Hold a reader after acquiring its complete table root. Complete its GET while the
writer is paused before publication, or after two publications and a failed QSBR
grace scan. The retirement case also replaces the value in the new image, so a
reader that refreshes its root cannot pass by returning identical bytes.
"""

import sys
import time

import _lib

PREFIX = "read_local_table_"
VALUE = b"table-retained:" + bytes(range(256)) * 4


def expect(got, want, label):
    if got != want:
        raise AssertionError("%s: got %r, want %r" % (label, got, want))


def metrics(conn):
    info = _lib.info(conn, "stats")
    required = ("probes", "served", "atomic_validations", "published", "retired", "reclaimed",
                "retired_bytes", "current_bytes", "build_bytes", "sidecar_bytes", "grows",
                "shrinks", "clears", "debug_phase", "debug_held", "debug_grace_blocked",
                "debug_retired_reads", "debug_prepublication_reads", "debug_timeouts")
    for name in required:
        if PREFIX + name not in info:
            raise AssertionError("missing INFO counter " + PREFIX + name)
    result = {key[len(PREFIX):]: int(value) for key, value in info.items()
              if key.startswith(PREFIX)}
    result["local_hits"] = int(info["read_local_hits"])
    result["mget_hits"] = int(info["read_local_mget_local_hits"])
    result["topology_fallbacks"] = int(info["read_local_fallback_seq_churn"])
    return result


def wait_metric(conn, predicate, label, timeout=4):
    deadline = time.monotonic() + timeout
    while True:
        current = metrics(conn)
        if predicate(current):
            return current
        if time.monotonic() >= deadline:
            raise AssertionError("window never opened: %s; %r" % (label, current))
        time.sleep(0.005)


def connection_on(host, port, predicate):
    # Fresh accept state, bounded: failure is a red battery, never a skip or a widened timeout.
    for _ in range(128):
        candidate = _lib.Conn(host, port, timeout=5)
        tid = candidate.must("DEBUG", "IO-THREAD")
        if isinstance(tid, int) and predicate(tid):
            return candidate, tid
        candidate.close()
    raise AssertionError("could not obtain the required independent running participant")


def same_shard_keys(conn, sid, count):
    found = []
    for batch in range(256):
        keys = ["nowait:battery:%d" % (batch * 512 + i) for i in range(512)]
        routes = _lib.shards_of(conn, keys)
        found.extend(key for key, (shard, _) in zip(keys, routes) if shard == sid)
        if len(found) >= count:
            return found[:count]
    raise AssertionError("bounded same-shard key discovery exhausted")


def set_keys(conn, keys, value=VALUE):
    for start in range(0, len(keys), 32):
        batch = keys[start:start + 32]
        conn.raw(b"".join(_lib.encode("SET", key, value) for key in batch))
        for key in batch:
            expect(conn.read(), b"OK", "SET " + key)


def directed(ctl, reader, writer, keys, mode):
    expect(ctl.must("FLUSHALL"), b"OK", "fresh image")
    set_keys(writer, keys[:716])
    expect(reader.must("GET", keys[0]), VALUE, "warm local connection")
    wait_metric(ctl, lambda m: m["retired_bytes"] == 0, "pre-arm grace complete")
    before = metrics(ctl)
    expect(ctl.must("DEBUG", "READ-LOCAL-TABLE", keys[0], mode), b"OK", "arm")
    try:
        reader.send("GET", keys[0])
        held = wait_metric(ctl, lambda m: m["debug_phase"] == 2 and m["debug_held"] == 1,
                           "reader owns the pre-publication root")
        expect(held["served"], before["served"], "held GET has not been served")
        writer.send("SET", keys[716], VALUE)  # exact 70% growth edge of the reset table
        if mode == 2:
            prepared = wait_metric(ctl, lambda m: m["debug_phase"] == 3,
                                   "writer prepared a private destination")
            expect(prepared["published"], before["published"], "writer has not published")
            expect(ctl.must("DEBUG", "READ-LOCAL-TABLE", keys[0], 3), b"OK", "release reader")
            expect(reader.read(), VALUE, "GET while writer was paused before publication")
            expect(writer.read(), b"OK", "writer resumes after private reply")
        else:
            expect(writer.read(), b"OK", "first publication")
            # Another complete growth with the first root still held. No replacements, so the
            # head of this owner's deferred queue is the exact image named by the held root.
            set_keys(writer, keys[717:1500])
            pending = wait_metric(ctl, lambda m: m["published"] >= before["published"] + 2
                                  and m["debug_grace_blocked"] > before["debug_grace_blocked"],
                                  "two publications and a real failed grace scan")
            expect(pending["debug_held"], 1, "reader still holds first image")
            expect(pending["reclaimed"], before["reclaimed"], "no old image reclaimed")
            if pending["retired_bytes"] <= before["retired_bytes"]:
                raise AssertionError("no retained storage despite held reader")
            # Replace only in current after freezing the held image. A root refresh would
            # return the new bytes; early value reclamation would invalidate the old reply.
            expect(writer.must("SET", keys[0], b"new-image:" + VALUE), b"OK",
                   "replace current while old image and value are held")
            expect(ctl.must("DEBUG", "READ-LOCAL-TABLE", keys[0], 3), b"OK", "release reader")
            expect(reader.read(), VALUE, "GET from image held across two publications")

        after = wait_metric(ctl, lambda m: m["debug_phase"] == 6, "reply copy completed locally")
        expect(after["served"] - before["served"], 1, "exactly one published-table GET")
        expect(after["local_hits"] - before["local_hits"], 1, "no owner fallback")
        expect(after["topology_fallbacks"], before["topology_fallbacks"], "no topology decline")
        expect(after["debug_retired_reads"] - before["debug_retired_reads"], int(mode == 1),
               "completed reply consumed the expected held image")
        expect(after["debug_timeouts"], before["debug_timeouts"], "no hook timeout")
        if mode == 2:
            expect(after["debug_prepublication_reads"] - before["debug_prepublication_reads"],
                   1, "read needed no writer progress")
    finally:
        expect(ctl.must("DEBUG", "READ-LOCAL-TABLE", keys[0], 0), b"OK", "disarm")
    drained = wait_metric(ctl, lambda m: m["retired_bytes"] == 0, "post-reader grace reclaim")
    if drained["reclaimed"] <= before["reclaimed"]:
        raise AssertionError("test never exercised the reclaim callback")
    expect(reader.must("GET", keys[0]), b"new-image:" + VALUE if mode == 1 else VALUE,
           "current generation after reclaim")


def ordinary(ctl, reader, writer, keys, enabled):
    expect(ctl.must("FLUSHALL"), b"OK", "reset ordinary traffic")
    before = metrics(ctl)
    set_keys(writer, keys[:1500])
    # Above 128 keys MGET uses shard semantic generations, not the bounded cell-epoch cache.
    # Require a real local completion so the fallback route cannot hide a broken validator.
    large_before = metrics(ctl)
    expect(reader.must("MGET", *keys[:160]), [VALUE] * 160, "large MGET after growth")
    if enabled:
        expect(metrics(ctl)["mget_hits"] - large_before["mget_hits"], 1, "large MGET stayed local")
    # Every reply byte and order matters. The reader's own earlier writes fence its GETs through
    # the existing ROB machinery; a captured old root is never permission to bypass RYOW.
    for n in range(32):
        value = b"own:%d:" % n + VALUE
        reader.raw(_lib.encode("SET", keys[0], value) + _lib.encode("GET", keys[0]) +
                   _lib.encode("MGET", keys[0], keys[1], keys[0], "nowait:absent"))
        expect(reader.read(), b"OK", "pipelined SET")
        expect(reader.read(), value, "RYOW GET")
        expect(reader.read(), [value, VALUE, value, None], "RYOW MGET and duplicate key")
    # A stable key is never erased while the shard crosses multiple shrink thresholds.
    for start in range(1, 1500, 32):
        batch = keys[start:min(start + 32, 1500)]
        writer.raw(b"".join(_lib.encode("DEL", key) for key in batch))
        reader.send("GET", keys[0])
        for _ in batch:
            expect(writer.read(), 1, "erase churn")
        expect(reader.read(), b"own:31:" + VALUE, "stable key during shrink")
    expect(reader.must("MGET", *keys[:160]), [b"own:31:" + VALUE] + [None] * 159,
           "large MGET hits and misses after shrink")
    after = metrics(ctl)
    if enabled:
        if not (after["probes"] > before["probes"] and after["served"] > before["served"]
                and after["grows"] > before["grows"] and after["shrinks"] > before["shrinks"]):
            raise AssertionError("ordinary growth/shrink/new-read paths did not fire: %r" % after)
        expect(after["topology_fallbacks"], before["topology_fallbacks"],
               "ordinary topology churn never declines")
    else:
        for field in ("probes", "served", "published", "retired", "reclaimed", "sidecar_bytes",
                      "current_bytes", "retired_bytes", "build_bytes"):
            expect(after[field], 0, "inactive lane allocates/executes nothing: " + field)
    expect(ctl.must("FLUSHALL"), b"OK", "clear publication")
    expect(reader.must("GET", keys[0]), None, "clear does not resurrect a retired value")


def allocation_failures(ctl, reader, writer, keys, enabled):
    expect(ctl.must("FLUSHALL"), b"OK", "OOM fresh image")
    set_keys(writer, keys[:716])
    before = metrics(ctl)
    expect(ctl.must("DEBUG", "TABLE-ALLOC-FAIL", 1), b"OK", "arm growth allocation fault")
    try:
        if not isinstance(writer.cmd("SET", keys[716], VALUE), _lib.RespError):
            raise AssertionError("growth allocation failure was silently accepted")
        expect(reader.must("GET", keys[0]), VALUE, "allocation failure retains old image")
        expect(reader.must("GET", keys[716]), None, "failed insert stays absent")
        if enabled:
            failed = metrics(ctl)
            expect(failed["published"], before["published"], "no partial root on OOM")
            expect(failed["allocation_failures"] - before["allocation_failures"], 1,
                   "table allocation actually failed")
        expect(writer.must("SET", keys[716], VALUE), b"OK", "growth retry")
    finally:
        expect(ctl.must("DEBUG", "TABLE-ALLOC-FAIL", 0), b"OK", "clear allocation fault")

    # Every shard's clear allocation fails. An armed lane must publish the immutable empty sentinel, then
    # allocate writable storage on the next insertion. It must never zero/reuse a held array.
    topo = _lib.topology(ctl)
    owner = _lib.shards_of(ctl, [keys[0]])[0][1]
    other = None
    for batch in range(16):
        candidates = ["nowait:oom:remote:%d" % (batch * 64 + i) for i in range(64)]
        for key, (_, target) in zip(candidates, _lib.shards_of(ctl, candidates)):
            if target != owner:
                other = key
                break
        if other is not None:
            break
    if other is None:
        raise AssertionError("no second owner for the empty-table atomic capacity check")
    expect(ctl.must("DEBUG", "TABLE-ALLOC-FAIL", len(topo.shard_owner)), b"OK", "arm clear OOM")
    try:
        expect(ctl.must("FLUSHALL"), b"OK", "allocation-free clear")
    finally:
        expect(ctl.must("DEBUG", "TABLE-ALLOC-FAIL", 0), b"OK", "disarm clear OOM")
    expect(reader.must("GET", keys[0]), None, "empty sentinel")
    if enabled:
        expect(metrics(ctl)["current_bytes"], 0, "every shard uses the empty sentinel")
    expect(writer.must("MSET", keys[0], VALUE, other, VALUE), b"OK", "atomic capacity from empty")
    expect(reader.must("MGET", keys[0], other), [VALUE, VALUE], "atomic publish after clear OOM")


def main():
    host, port = _lib.host_port()
    posture = sys.argv[3]
    if posture not in ("fused", "disabled", "split"):
        raise AssertionError("expected fused|disabled|split")
    scenario = sys.argv[4] if len(sys.argv) > 4 else "all"
    if scenario not in ("all", "prepublication", "retirement", "churn", "oom"):
        raise AssertionError("unknown scenario: " + scenario)
    enabled = posture == "fused"
    if not enabled and scenario != "all":
        raise AssertionError("inactive controls must run all ordinary/OOM checks")
    ctl = _lib.Conn(host, port, timeout=5)
    server = _lib.info(ctl, "server")
    expect(server["thread_mode"], "2s" if posture == "split" else "1s", "effective mode")
    expect(int(server["read_local_table"]), int(enabled), "effective publication path")
    expect(server["read_local_table_read_path"], "published" if enabled else "owner", "only path")
    expect(server["read_local_table_scope"], "topology_only" if enabled else "off", "honest scope")
    expect(ctl.must("CONFIG", "GET", "read-local-table"), [], "removed configuration option")
    if not isinstance(ctl.cmd("CONFIG", "SET", "read-local-table", 0), _lib.RespError):
        raise AssertionError("removed table option was accepted")
    expect(ctl.must("CONFIG", "GET", "lb"), [b"lb", b"0"], "directed placement")

    sid, owner = _lib.shards_of(ctl, ["nowait:target"])[0]
    keys = same_shard_keys(ctl, sid, 1500)
    if posture == "split":
        reader, _ = connection_on(host, port, lambda _: True)
        writer, _ = connection_on(host, port, lambda _: True)
    else:
        reader, reader_tid = connection_on(host, port, lambda tid: tid != owner)
        ctl.close()
        ctl, ctl_tid = connection_on(host, port, lambda tid: tid not in (owner, reader_tid))
        writer, _ = connection_on(host, port, lambda tid: tid == ctl_tid)
        expect(_lib.shards_of(ctl, [keys[0]])[0], (sid, owner), "stable shard owner")
    try:
        if enabled:
            if scenario in ("all", "prepublication"):
                directed(ctl, reader, writer, keys, 2)
            if scenario in ("all", "retirement"):
                directed(ctl, reader, writer, keys, 1)
        elif not isinstance(ctl.cmd("DEBUG", "READ-LOCAL-TABLE", keys[0], 1), _lib.RespError):
            raise AssertionError("inactive lane allocated the publication hook")
        if scenario in ("all", "churn"):
            ordinary(ctl, reader, writer, keys, enabled)
        if scenario in ("all", "oom"):
            allocation_failures(ctl, reader, writer, keys, enabled)
        print("PASS read_local_table %s/%s: topology, lifetime, path, and RYOW witnesses" %
              (posture, scenario))
    finally:
        reader.close()
        writer.close()
        ctl.close()


if __name__ == "__main__":
    main()
