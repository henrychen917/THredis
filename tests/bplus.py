#!/usr/bin/env python3
"""Directed B+ per-key atomic-filter gate. Usage: tests/bplus.py HOST PORT

Boot requirement:
  --thread-mode 1s|2s --overlap 0|1 --read-local 1
  --atomic 1 --enable-debug-command yes

The test does not infer routing from key names. DEBUG SHARD/LBSIGNALS select a real cross-owner
MSET, DEBUG ATOMIC-FILTER-CELL supplies unrelated keys in provably different filter cells, and
DEBUG IO-THREAD keeps the observers off both participating owners. DEBUG ATOMIC-COMMIT-DELAY then
holds the group after every raw install but before its shared epoch publication. A concurrent
EXISTS advances atomic_commit_holds only after it samples that exact reserved/unpublished window.
"""

import select
import socket
import sys
import time


HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 6379
HOLD_US = 1_000_000
# The per-IO scatter snapshot window holds eight commands. Leave two slots for the held-window
# command under test and control traffic while still avoiding a single witness's head-of-line trap.
WITNESS_COUNT = 6
OPEN_CHECKPOINT_BUDGET = 0.35
COMMIT_CHECKPOINT_BUDGET = 0.60
CLASSIFY_BUDGET = 0.88


class RespError(Exception):
    def __init__(self, message):
        self.message = message

    def __repr__(self):
        return "RespError(%r)" % self.message


def frame(*arguments):
    values = [value if isinstance(value, bytes) else str(value).encode()
              for value in arguments]
    return (b"*%d\r\n" % len(values) +
            b"".join(b"$%d\r\n%s\r\n" % (len(value), value) for value in values))


class Conn:
    def __init__(self, timeout=10):
        self.sock = socket.create_connection((HOST, PORT), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")
        self.closed = False

    def send(self, *arguments):
        self.sock.sendall(frame(*arguments))

    def command(self, *arguments):
        self.send(*arguments)
        return self.read()

    def read(self):
        marker = self.file.read(1)
        if not marker:
            raise EOFError("server closed connection")
        line = self.file.readline()
        if not line.endswith(b"\r\n"):
            raise AssertionError("bad RESP line %r" % (marker + line,))
        payload = line[:-2]
        if marker == b"+":
            return payload
        if marker == b"-":
            return RespError(payload)
        if marker == b":":
            return int(payload)
        if marker in (b"$", b"="):
            length = int(payload)
            if length < 0:
                return None
            data = self.file.read(length)
            if self.file.read(2) != b"\r\n":
                raise AssertionError("bad bulk trailer")
            return data[4:] if marker == b"=" and data[3:4] == b":" else data
        if marker in (b"*", b"~", b">"):
            count = int(payload)
            return None if count < 0 else [self.read() for _ in range(count)]
        if marker == b"%":
            return [self.read() for _ in range(int(payload) * 2)]
        raise AssertionError("unsupported RESP marker %r" % marker)

    def readable(self, timeout=0):
        ready, _, _ = select.select([self.sock], [], [], timeout)
        return bool(ready)

    def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            self.file.close()
        finally:
            self.sock.close()


def expect(actual, wanted, label):
    if actual != wanted:
        raise AssertionError("%s: got %r, wanted %r" % (label, actual, wanted))
    print("  ok   " + label, flush=True)


def stats(connection):
    raw = connection.command("INFO", "STATS")
    if not isinstance(raw, bytes):
        raise AssertionError("INFO STATS returned %r" % (raw,))
    result = {}
    for line in raw.decode().splitlines():
        if ":" not in line:
            continue
        name, value = line.split(":", 1)
        try:
            result[name] = int(value)
        except ValueError:
            pass
    return result


REQUIRED_STATS = (
    "atomic_commit_holds",
    "atomic_groups",
    "atomic_gauge_underflows",
    "atomic_pending_entries",
    "foreign_read_unsafe_refs",
    "foreign_read_occupied_cells",
    "foreign_read_wildcard_cells",
    "foreign_read_saturated_cells",
    "foreign_read_poisoned_shards",
    "read_local_hits",
    "read_local_keyspace_hits",
    "read_local_fallback_atomic_pending",
    "read_local_mget_local_hits",
    "read_local_mget_generation_retries",
    "read_local_mget_fallback_atomic_pending",
    "read_local_mget_fallback_generation",
)


def require_stats(connection):
    current = stats(connection)
    missing = [name for name in REQUIRED_STATS if name not in current]
    if missing:
        raise AssertionError(
            "INFO STATS omitted %s; this build cannot prove the B+ mechanisms fired" %
            ", ".join(missing))
    return current


def wait_stats(connection, deadline, predicate, label):
    current = require_stats(connection)
    while not predicate(current) and time.monotonic() < deadline:
        time.sleep(0.002)
        current = require_stats(connection)
    if not predicate(current):
        raise AssertionError("%s did not fire before the deterministic deadline; last=%r" %
                             (label, {name: current[name] for name in REQUIRED_STATS}))
    return current


def config_value(connection, name):
    reply = connection.command("CONFIG", "GET", name)
    if not isinstance(reply, list) or len(reply) != 2 or reply[0] != name.encode():
        raise AssertionError("CONFIG GET %s returned %r" % (name, reply))
    return reply[1]


def topology(connection):
    raw = connection.command("DEBUG", "LBSIGNALS")
    if not isinstance(raw, bytes):
        raise AssertionError(
            "DEBUG LBSIGNALS unavailable; boot with --enable-debug-command yes: %r" % raw)
    threads = set()
    readers = set()
    shard_owner = {}
    for line in raw.splitlines():
        fields = line.split()
        # Roles identify client-serving readers (`io` or `fused`); shard rows below
        # remain the only ownership oracle, including for fused threads.
        if len(fields) >= 3 and fields[0] == b"thread":
            threads.add(int(fields[1]))
            if fields[2] in (b"io", b"fused"):
                readers.add(int(fields[1]))
        elif len(fields) >= 3 and fields[0] == b"shard":
            shard_owner[int(fields[1])] = int(fields[2])
    if len(threads) < 3:
        raise AssertionError(
            "B+ held-group geometry needs two participating owners and one independent local "
            "reader; found live threads %r" % sorted(threads))
    if not shard_owner:
        raise AssertionError("DEBUG LBSIGNALS reported no shard ownership")
    unknown = set(shard_owner.values()) - threads
    if unknown:
        raise AssertionError("shards name non-live owners %r" % sorted(unknown))
    return readers, shard_owner


def debug_shard(connection, key):
    shard = connection.command("DEBUG", "SHARD", key)
    if not isinstance(shard, int):
        raise AssertionError("DEBUG SHARD %r returned %r" % (key, shard))
    return shard


def debug_cell(connection, key):
    cell = connection.command("DEBUG", "ATOMIC-FILTER-CELL", key)
    if not isinstance(cell, int) or not 0 <= cell < 4096:
        raise AssertionError("DEBUG ATOMIC-FILTER-CELL %r returned %r" % (key, cell))
    return cell


def select_geometry(connection):
    threads, shard_owner = topology(connection)
    cells_by_shard = {}
    qualified = {}
    for index in range(20000):
        key = "bplus:geometry:%05d:%s" % (index, "k" * 24)
        shard = debug_shard(connection, key)
        if shard not in shard_owner:
            raise AssertionError("DEBUG SHARD returned unreported shard %d" % shard)
        cell = debug_cell(connection, key)
        cells = cells_by_shard.setdefault(shard, {})
        cells.setdefault(cell, key)
        if len(cells) >= 2:
            ordered = sorted(cells.items())
            qualified[shard] = (ordered[0], ordered[1])
        for source_shard, source in qualified.items():
            for partner_shard, partner in qualified.items():
                source_owner = shard_owner[source_shard]
                partner_owner = shard_owner[partner_shard]
                if source_owner == partner_owner:
                    continue
                safe = sorted(threads - {source_owner, partner_owner})
                if not safe:
                    continue
                (a_cell, a), (b_cell, b) = source
                (p_cell, p), (c_cell, c) = partner
                return {
                    "a": a, "b": b, "p": p, "c": c,
                    "a_shard": source_shard, "p_shard": partner_shard,
                    "a_owner": source_owner, "p_owner": partner_owner,
                    "a_cell": a_cell, "b_cell": b_cell,
                    "p_cell": p_cell, "c_cell": c_cell,
                    "reader_thread": safe[0],
                }
    raise AssertionError(
        "could not find two cross-owner shards with two distinct filter cells each after "
        "20000 boot-keyed probes")


def connections_on_thread(thread_id, count):
    retained = []
    for _ in range(4096):
        connection = Conn()
        owner = connection.command("DEBUG", "IO-THREAD")
        if owner == thread_id:
            retained.append(connection)
            if len(retained) == count:
                return retained
        else:
            connection.close()
    for connection in retained:
        connection.close()
    raise AssertionError("found only %d/%d connections on independent IO thread %d" %
                         (len(retained), count, thread_id))


def make_value(case, key, generation):
    return ("bplus:%s:%s:%s:" % (case, key, generation)).encode() + b"x" * 96


def wait_drained(connection, timeout=4.0):
    deadline = time.monotonic() + timeout
    current = require_stats(connection)
    fields = ("atomic_pending_entries", "foreign_read_unsafe_refs",
              "foreign_read_occupied_cells", "foreign_read_wildcard_cells",
              "foreign_read_saturated_cells", "foreign_read_poisoned_shards")
    while any(current[name] != 0 for name in fields) and time.monotonic() < deadline:
        time.sleep(0.01)
        current = require_stats(connection)
    nonzero = {name: current[name] for name in fields if current[name] != 0}
    if nonzero:
        raise AssertionError("atomic/filter state did not drain: %r" % nonzero)
    return current


def seed_case(connection, geometry, case):
    values = {}
    for role in ("a", "b", "p", "c"):
        values[(role, "old")] = make_value(case, role, "old")
        values[(role, "new")] = make_value(case, role, "new")
        expect(connection.command("SET", geometry[role], values[(role, "old")]), b"OK",
               "%s seed %s" % (case, role.upper()))
    return values


def assert_open_filter(current):
    wanted = {
        "atomic_pending_entries": 2,
        "foreign_read_unsafe_refs": 2,
        "foreign_read_occupied_cells": 2,
        "foreign_read_wildcard_cells": 0,
        "foreign_read_saturated_cells": 0,
        "foreign_read_poisoned_shards": 0,
    }
    got = {name: current[name] for name in wanted}
    if got != wanted:
        raise AssertionError("held two-key group has wrong filter state: got %r wanted %r" %
                             (got, wanted))


def held_group(control, witnesses, geometry, values, case, exercise):
    wait_drained(control)
    before = require_stats(control)
    writer = Conn()
    witness_replies = []
    writer_sent = False
    writer_read = False
    witnesses_sent = 0
    try:
        expect(control.command("DEBUG", "ATOMIC-COMMIT-DELAY", str(HOLD_US)), b"OK",
               "%s arm commit hold" % case)
        started = time.monotonic()
        writer.send("MSET", geometry["a"], values[("a", "new")],
                    geometry["p"], values[("p", "new")])
        writer_sent = True

        # AtomicEntry::live is published only after each shard's complete raw install. Waiting for
        # both linked entries before issuing the snapshot witness prevents that witness from being
        # queued ahead of the group and then stranded behind the committing owner. The filter
        # gauges simultaneously prove both unsafe keys were published before those installs.
        opened = wait_stats(
            control, started + OPEN_CHECKPOINT_BUDGET,
            lambda current:
                current["atomic_pending_entries"] == 2 and
                current["foreign_read_unsafe_refs"] == 2 and
                current["foreign_read_occupied_cells"] == 2,
            "%s installed open-group gauges" % case)
        if writer.readable(0):
            raise AssertionError("%s writer replied before its open-group checkpoint" % case)
        assert_open_filter(opened)

        # Keep several snapshot preparations in flight. A single synchronous EXISTS can prepare
        # just before ticket reserve and then block behind the delayed owner, leaving it unable to
        # issue the probe that would sample the reserve/publish hole. Independent connections avoid
        # that head-of-line trap. INFO round trips between sends stagger preparation without making
        # elapsed time the oracle; atomic_commit_holds remains the mandatory checkpoint.
        checkpoint = None
        for witness in witnesses:
            witness.send("EXISTS", geometry["a"], geometry["p"])
            witnesses_sent += 1
            sampled = require_stats(control)
            if sampled["atomic_commit_holds"] > before["atomic_commit_holds"]:
                checkpoint = sampled
        if checkpoint is None:
            checkpoint = wait_stats(
                control, started + COMMIT_CHECKPOINT_BUDGET,
                lambda current: current["atomic_commit_holds"] >
                    before["atomic_commit_holds"],
                "%s atomic_commit_holds" % case)
        expect(control.command("DEBUG", "ATOMIC-COMMIT-DELAY", "0"), b"OK",
               "%s disarm future commit holds" % case)
        if time.monotonic() >= started + COMMIT_CHECKPOINT_BUDGET:
            raise AssertionError("%s checkpoint exceeded %.2fs deterministic budget" %
                                 (case, COMMIT_CHECKPOINT_BUDGET))
        if writer.readable(0):
            raise AssertionError("%s writer replied before held-group reads began" % case)
        assert_open_filter(checkpoint)
        print("  ok   %s group is installed and held before epoch publication" % case,
              flush=True)

        exercise(checkpoint, started + CLASSIFY_BUDGET, writer)

        writer_reply = writer.read()
        writer_read = True
        expect(writer_reply, b"OK", "%s held MSET committed" % case)
        witness_replies = [witness.read() for witness in witnesses[:witnesses_sent]]
    finally:
        try:
            control.command("DEBUG", "ATOMIC-COMMIT-DELAY", "0")
        except Exception:
            pass
        if writer_sent and not writer_read:
            try:
                writer.read()
            except Exception:
                pass
        writer.close()

    if witnesses_sent != WITNESS_COUNT or len(witness_replies) != WITNESS_COUNT:
        raise AssertionError("%s checkpoint drained %d replies from %d/%d probes" %
                             (case, len(witness_replies), witnesses_sent, WITNESS_COUNT))
    if witness_replies != [2] * WITNESS_COUNT:
        raise AssertionError("%s checkpoint observed bad EXISTS replies %r" %
                             (case, witness_replies))
    print("  ok   %s all %d commit-window witnesses drained valid replies" %
          (case, WITNESS_COUNT), flush=True)

    drained = wait_drained(control)
    if drained["atomic_groups"] != before["atomic_groups"] + 1:
        raise AssertionError("%s did not admit exactly one atomic group: before=%d after=%d" %
                             (case, before["atomic_groups"], drained["atomic_groups"]))
    if drained["atomic_gauge_underflows"] != before["atomic_gauge_underflows"]:
        raise AssertionError("%s changed atomic_gauge_underflows: before=%d after=%d" %
                             (case, before["atomic_gauge_underflows"],
                              drained["atomic_gauge_underflows"]))


def get_case(control, witnesses, unrelated, touched, geometry):
    case = "get"
    values = seed_case(control, geometry, case)
    wait_drained(control)

    def exercise(opened, deadline, writer):
        unrelated.send("GET", geometry["b"])
        if not unrelated.readable(max(0.0, deadline - time.monotonic())):
            raise AssertionError("GET B did not complete inside the captured commit hold")
        expect(unrelated.read(), values[("b", "old")],
               "GET unrelated B serves the old value locally")
        after_b = wait_stats(
            control, deadline,
            lambda current: current["read_local_hits"] == opened["read_local_hits"] + 1,
            "GET B local-hit counter")
        if after_b["read_local_keyspace_hits"] != opened["read_local_keyspace_hits"] + 1:
            raise AssertionError("GET B did not count one local keyspace hit")
        if after_b["read_local_fallback_atomic_pending"] != \
                opened["read_local_fallback_atomic_pending"]:
            raise AssertionError("GET B retained the old whole-shard atomic fallback")
        if writer.readable(0) or time.monotonic() >= deadline:
            raise AssertionError("GET B was not proved local while the group remained held")

        touched.send("GET", geometry["a"])
        after_a = wait_stats(
            control, deadline,
            lambda current: current["read_local_fallback_atomic_pending"] ==
                after_b["read_local_fallback_atomic_pending"] + 1,
            "GET A pending-filter fallback counter")
        if after_a["read_local_hits"] != after_b["read_local_hits"]:
            raise AssertionError("unsafe GET A was incorrectly counted as a local hit")
        if writer.readable(0) or time.monotonic() >= deadline:
            raise AssertionError("GET A fallback was not classified inside the held group")
        expect(touched.read(), values[("a", "old")],
               "GET touched A falls back at its pinned old cut")

    held_group(control, witnesses, geometry, values, case, exercise)
    post = require_stats(control)
    expect(unrelated.command("GET", geometry["a"]), values[("a", "new")],
           "GET A sees committed value after filter drain")
    expect(unrelated.command("GET", geometry["p"]), values[("p", "new")],
           "GET P sees committed value after filter drain")
    expect(unrelated.command("GET", geometry["b"]), values[("b", "old")],
           "GET B remained unchanged")
    expect(unrelated.command("GET", geometry["c"]), values[("c", "old")],
           "GET C remained unchanged")
    after = require_stats(control)
    if after["read_local_hits"] != post["read_local_hits"] + 4:
        raise AssertionError("post-cleanup GET controls did not all return to the local path")


def mget_case(control, witnesses, unrelated, touched, geometry):
    case = "mget"
    values = seed_case(control, geometry, case)
    wait_drained(control)

    def exercise(opened, deadline, writer):
        unrelated.send("MGET", geometry["b"], geometry["c"])
        if not unrelated.readable(max(0.0, deadline - time.monotonic())):
            raise AssertionError("unrelated MGET did not complete inside the captured commit hold")
        expect(unrelated.read(), [values[("b", "old")], values[("c", "old")]],
               "MGET B/C spans pending shards and serves locally")
        after_unrelated = wait_stats(
            control, deadline,
            lambda current: current["read_local_mget_local_hits"] ==
                opened["read_local_mget_local_hits"] + 1,
            "unrelated MGET local-hit counter")
        if after_unrelated["read_local_hits"] != opened["read_local_hits"] + 1:
            raise AssertionError("unrelated MGET did not count exactly one aggregate local hit")
        if after_unrelated["read_local_mget_fallback_atomic_pending"] != \
                opened["read_local_mget_fallback_atomic_pending"]:
            raise AssertionError("unrelated MGET retained the old touched-shard pending gate")
        if after_unrelated["read_local_mget_generation_retries"] != \
                opened["read_local_mget_generation_retries"]:
            raise AssertionError("stable held-group MGET retried its publication generation")
        if writer.readable(0) or time.monotonic() >= deadline:
            raise AssertionError("unrelated MGET was not proved local during the hold")

        # A is deliberately last: admission and execution must inspect every key, not only the
        # prehashed first route, and one positive makes the complete MGET owner-side.
        touched.send("MGET", geometry["b"], geometry["c"], geometry["a"])
        after_touched = wait_stats(
            control, deadline,
            lambda current: current["read_local_mget_fallback_atomic_pending"] ==
                after_unrelated["read_local_mget_fallback_atomic_pending"] + 1,
            "last-key-touched MGET pending-filter fallback counter")
        if after_touched["read_local_mget_local_hits"] != \
                after_unrelated["read_local_mget_local_hits"]:
            raise AssertionError("partially unsafe MGET was counted as a local completion")
        if after_touched["read_local_fallback_atomic_pending"] != \
                after_unrelated["read_local_fallback_atomic_pending"] + 1:
            raise AssertionError("MGET pending fallback was not counted once per command")
        if writer.readable(0) or time.monotonic() >= deadline:
            raise AssertionError("touched MGET fallback was not classified inside the hold")
        expect(touched.read(),
               [values[("b", "old")], values[("c", "old")], values[("a", "old")]],
               "last-key-touched MGET falls back as one all-old command")

    held_group(control, witnesses, geometry, values, case, exercise)
    post = require_stats(control)
    expect(unrelated.command("MGET", geometry["a"], geometry["p"],
                             geometry["b"], geometry["c"]),
           [values[("a", "new")], values[("p", "new")],
            values[("b", "old")], values[("c", "old")]],
           "MGET sees committed group after filter drain")
    after = require_stats(control)
    if (after["read_local_hits"] != post["read_local_hits"] + 1 or
            after["read_local_mget_local_hits"] !=
            post["read_local_mget_local_hits"] + 1):
        raise AssertionError("post-cleanup MGET did not return to the local path")


def main():
    discovery = Conn()
    retained = []
    try:
        expected_config = {
            "read-local": b"1",
            "atomic": b"1",
        }
        if config_value(discovery, "thread-mode") not in (b"1s", b"2s"):
            raise AssertionError("B+ test requires 1s or 2s")
        for name, wanted in expected_config.items():
            got = config_value(discovery, name)
            if got != wanted:
                raise AssertionError(
                    "B+ test needs CONFIG %s=%s, got %r" %
                    (name, wanted.decode(), got))
        if config_value(discovery, "overlap") not in (b"0", b"1"):
            raise AssertionError("B+ test needs a supported overlap schedule")
        if discovery.command("CONFIG", "GET", "read-local-atomic-filter") != []:
            raise AssertionError("removed read-local atomic-filter knob is still exposed")

        geometry = select_geometry(discovery)
        if (geometry["a_shard"] != debug_shard(discovery, geometry["b"]) or
                geometry["p_shard"] != debug_shard(discovery, geometry["c"]) or
                geometry["a_shard"] == geometry["p_shard"] or
                geometry["a_owner"] == geometry["p_owner"] or
                geometry["a_cell"] == geometry["b_cell"] or
                geometry["p_cell"] == geometry["c_cell"] or
                geometry["reader_thread"] in
                (geometry["a_owner"], geometry["p_owner"])):
            raise AssertionError("selected invalid B+ geometry %r" % geometry)
        print("  ok   geometry A/B shard=%d owner=%d cells=%d/%d; "
              "P/C shard=%d owner=%d cells=%d/%d; reader-thread=%d" %
              (geometry["a_shard"], geometry["a_owner"],
               geometry["a_cell"], geometry["b_cell"],
               geometry["p_shard"], geometry["p_owner"],
               geometry["p_cell"], geometry["c_cell"],
               geometry["reader_thread"]), flush=True)

        retained = connections_on_thread(geometry["reader_thread"], 3 + WITNESS_COUNT)
        control, unrelated, touched, *witnesses = retained
        require_stats(control)
        get_case(control, witnesses, unrelated, touched, geometry)
        mget_case(control, witnesses, unrelated, touched, geometry)
    finally:
        for connection in retained:
            connection.close()
        discovery.close()

    print("BPLUS PASS: negative-cell GET/MGET stayed local; touched GET/MGET fell back",
          flush=True)


if __name__ == "__main__":
    main()
