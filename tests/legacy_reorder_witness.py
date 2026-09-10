#!/usr/bin/env python3
"""Unscored, standalone execution-order witness for an unchanged legacy binary.

The current candidate still requires its during-window permutation counter. The
unchanged legacy reference, which has no such counter, uses this live OFF/ON
control plus command progress in the measured interval; it makes no claim to
count permutations in that interval. Both modes passed the OFF/ON live controls
on c8e61f646 and the current binary on 2026-09-10, at32servercores.
The exact same copied binary runs both controls; no debugger, binary patch, or
instrumented build is used. MONITOR observes dispatch, while a conflicting GET
observes execution: SETRANGE is Long and GET is Point in c8e61f646. With one IO
producer, one stable owner, and exactly-once admission, a GET of the old value
after MONITOR reported the write first proves a nonidentity execution order.
The fresh server performs no grouped/script writes: zero cumulative atomic
groups and zero live records are asserted around each attempt. A dispatch-time
MVCC cut therefore cannot explain the old value (legacy ex_loop.h:2938-2955
calls the ordinary handler directly when atomic_has_records() is false).

The path audit matters: c8e61f646 io_loop.h:529-537 routes 1s overlap=2 into the
whole-batch iofused/filler schedule; the buffered-streams loop is unreachable.
Its MONITOR-induced force_coarse branch at 4180-4185 is therefore not this path.
2s overlap=1 selects run_split<1> (430), then ordinary parse_and_dispatch<IoPipe>
(6414-6428), also not BufferedIfid. Both execution paths call ex_schedule_batch
before prefetch (ex_loop.h:2124-2125 and 2764). MONITOR still adds unscored work;
this fixture makes no latency/rate claim and cannot certify a measured window.

MONITOR runs before a potentially refused enqueue (4560 versus 5251). Duplicate
or missing tagged events, queue-full counters, migration, changed client counts,
or a foreign producer all invalidate the attempt; none is interpreted as a
scheduler permutation. Fresh keys and drained ROBs re-arm every bounded attempt.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import sys
import tempfile
import time

import abbagate as abba
from _lib import Conn, encode
from _gate_process import server


OLD = b"0" + b"." * 63
NEW = b"X" + b"." * 63


class WitnessError(RuntimeError):
    pass


def require(condition, reason):
    if not condition:
        raise WitnessError(reason)


def topology(conn):
    raw = conn.must("DEBUG", "LBSIGNALS")
    require(isinstance(raw, bytes), "LBSIGNALS returned no topology")
    threads, shards, stamp = {}, {}, None
    for line in raw.decode().splitlines():
        fields = line.split()
        if fields[:3] == ["lbver", "1", "stamp_ns"]:
            stamp = int(fields[3])
        elif fields and fields[0] == "thread":
            require(len(fields) >= 13, "short LBSIGNALS thread row")
            require("masked_lane_full_events" in fields, "missing masked queue admission telemetry")
            tid = int(fields[1])
            require(tid not in threads, "duplicate thread identity")
            threads[tid] = {"role": fields[2], "clients": int(fields[4]), "full": int(fields[12]),
                            "masked_full": int(fields[fields.index("masked_lane_full_events") + 1])}
        elif fields and fields[0] == "shard":
            require(len(fields) >= 7, "short LBSIGNALS shard row")
            sid = int(fields[1])
            require(sid not in shards, "duplicate shard identity")
            shards[sid] = {"owner": int(fields[2]), "migrations": int(fields[6])}
    require(stamp is not None and threads and shards, "missing topology or timestamp")
    return {"stamp_ns": stamp, "threads": threads, "shards": shards}


def unchanged(before, after):
    require(after["stamp_ns"] > before["stamp_ns"], "topology observations were not fresh")
    require(before["shards"] == after["shards"], "owner map or shard migrations changed")
    require(before["threads"].keys() == after["threads"].keys(), "thread set changed")
    for tid, first in before["threads"].items():
        last = after["threads"][tid]
        require(first["role"] == last["role"], "thread role changed")
        require(first["clients"] == last["clients"], "client placement changed during attempt")
        require(first["full"] == last["full"], "queue admission reported a full event")
        require(first["masked_full"] == last["masked_full"], "masked queue admission reported a full event")


def no_snapshot_versions(conn):
    stats = abba.info(conn, "stats")
    names = ("atomic_groups", "atomic_inflight", "atomic_entries",
             "atomic_pending_entries", "script_intents_live")
    require(all(name in stats for name in names), "missing MVCC exclusion telemetry")
    observed = {name: int(stats[name]) for name in names}
    require(all(value == 0 for value in observed.values()),
            "MVCC records/groups exist; a read cut could imitate execution inversion")
    return observed


def new_client_owner(before, after, allow_pending=False):
    require(before["shards"] == after["shards"], "ownership changed while locating client")
    require(before["threads"].keys() == after["threads"].keys(), "thread set changed while locating client")
    changed = []
    for tid, row in before["threads"].items():
        last = after["threads"][tid]
        require(row["role"] == last["role"], "role changed while locating client")
        delta = last["clients"] - row["clients"]
        require(delta in (0, 1), "client-count change was not one isolated accept")
        if delta:
            changed.append(tid)
    if not changed and allow_pending:
        return None
    require(len(changed) == 1, "no unique producer for the new connection")
    return changed[0]


def same_producer_clients(control, port, stack, initial):
    groups, captures = {}, []
    current = initial
    producers = sum(row["role"] in ("io", "fused") for row in initial["threads"].values())
    require(producers > 0, "no connection-owning thread")
    # Pigeonhole bound: four clients on one producer after at most 3*N+1 accepts.
    # Keep unused sockets open and idle, so each topology delta names only this accept.
    for _ in range(3 * producers + 1):
        conn = Conn("127.0.0.1", port, timeout=10)
        stack.callback(conn.close)
        require(conn.must("PING") == b"PONG", "new client did not boot")
        deadline = time.monotonic() + 2
        while True:
            after = topology(control)
            owner = new_client_owner(current, after, allow_pending=True)
            if owner is not None:
                break
            require(time.monotonic() < deadline, "client ownership publication was never observed")
            time.sleep(.005)
        captures.append({"address": address(conn), "producer": owner,
                         "before": current, "after": after})
        groups.setdefault(owner, []).append(conn)
        current = after
        if len(groups[owner]) == 4:
            return owner, groups[owner], current, captures
    raise WitnessError("same-producer client bound exhausted")


def address(conn):
    host, port = conn.sock.getsockname()[:2]
    return f"{host}:{port}"


def owned_key(control, prefix, owner, owner_count):
    for first in range(0, 128 * owner_count, 128):
        candidates = [f"{prefix}-{number}" for number in range(first, first + 128)]
        locations = control.must("DEBUG", "SHARDS", *candidates)
        require(isinstance(locations, list) and len(locations) == len(candidates), "incomplete key-owner lookup")
        for key, pair in zip(candidates, locations):
            if isinstance(pair, list) and len(pair) == 2 and pair[1] == owner:
                return key, int(pair[0])
    raise WitnessError("bounded same-owner key search exhausted")


def monitor_event(raw):
    require(isinstance(raw, bytes), "MONITOR did not return a line")
    text = raw.decode("utf-8", "strict")
    match = re.fullmatch(r"[0-9]+\.[0-9]+ \[0 ([^\]]+)\] (.*)", text)
    require(match is not None, f"malformed MONITOR line: {text!r}")
    try:
        arguments = shlex.split(match[2], posix=True)
    except ValueError as exc:
        raise WitnessError(f"malformed MONITOR arguments: {text!r}") from exc
    require(arguments, "empty MONITOR command")
    return {"address": match[1], "args": arguments, "raw": text}


def collect_until(monitor, client_address, fence):
    events = []
    # At most a handful of witness/control commands are issued per attempt. Bound
    # accidental feed storms and rely on the socket timeout for a missing fence.
    for _ in range(256):
        event = monitor_event(monitor.read())
        events.append(event)
        if event["address"] == client_address and event["args"] == ["ECHO", fence]:
            return events
    raise WitnessError("MONITOR fence was not reached within bounded transcript")


def judge_attempt(*, key, writer, reader, events, write_reply, read_reply, final_reply,
                  before, after, producer, client_producers):
    unchanged(before, after)
    require(set(client_producers.values()) == {producer}, "writer/reader/monitor/blocker do not share a producer")
    require(write_reply == len(OLD), "SETRANGE did not execute exactly")
    require(read_reply in (OLD, NEW), "GET returned neither allowed state")
    require(final_reply == NEW, "final state does not prove the write completed")
    tagged = [(index, event) for index, event in enumerate(events) if key in event["args"][1:]]
    writes = [index for index, event in tagged if event["address"] == writer and
              event["args"] == ["SETRANGE", key, "0", "X"]]
    reads = [index for index, event in tagged if event["address"] == reader and event["args"] == ["GET", key]]
    require(len(writes) == len(reads) == 1, "tagged commands were missing or replayed before admission")
    # Final-state GET from the control connection is allowed, but no other command
    # can modify this fresh nonce key or manufacture the observed state transition.
    require(all(event["args"] == ["GET", key] or
                (event["address"] == writer and event["args"] == ["SETRANGE", key, "0", "X"])
                for _, event in tagged), "unexpected operation touched the witness key")
    armed = writes[0] < reads[0]
    return {"armed": armed, "inversion": armed and read_reply == OLD,
            "dispatch_indices": {"write": writes[0], "read": reads[0]},
            "read_state": read_reply.decode(), "final_state": final_reply.decode()}


def finish_control(attempts, reorder):
    armed = sum(attempt["armed"] for attempt in attempts)
    inversions = sum(attempt["inversion"] for attempt in attempts)
    require(armed > 0, "UNREACHED: no admitted write-before-read dispatch window opened")
    if reorder:
        require(inversions > 0, "UNREACHED: bounded fresh-state attempts never witnessed a permutation")
    else:
        require(inversions == 0, "FIFO negative control inverted; fixture/model is invalid")
    return {"verdict": "PASS", "armed_attempts": armed, "inversions": inversions,
            "scope": "directed engagement only; no during-measurement permutation count"}


def translated_knobs(binary, mode, reorder):
    result = {}
    for name, value in (("thread-mode", mode), ("read-local", 0), ("overlap", 1), ("reorder", reorder)):
        if abba.accepted(binary, name, value):
            result[name] = value
        else:
            alias = abba.LEGACY_KNOBS.get(name)
            translated = abba.legacy_value(name, value, mode)
            require(alias and abba.accepted(binary, alias, translated), f"binary cannot express {name}={value}")
            result[alias] = translated
    # The stored reference predates independent key/client switches. Its --lb 0
    # disables the shared controller and both collectors; verify the actual old
    # configuration instead of passing new flags that prevent the control boot.
    if abba.accepted(binary, 'key-lb', 0) and abba.accepted(binary, 'client-lb', 0):
        result.update({'key-lb': 0, 'client-lb': 0})
    else:
        require(abba.accepted(binary, 'lb', 0), 'binary cannot disable load balancing')
        result['lb'] = 0
    return result


def run_control(args, binary, out, mode, reorder):
    knobs = translated_knobs(binary, mode, reorder)
    nthreads = len(abba.cpus(args.server_cores) + abba.cpus(args.server_smt))
    ex = nthreads // 2
    server_args = ["--atomic", "1", "--flip-auto", "0",
                   "--shards", str(min(256, 8 * (ex if mode == "2s" else nthreads)))]
    if mode == "2s":
        server_args += ["--ratio", f"{nthreads-ex}:{ex}"]
    for name, value in knobs.items():
        server_args += ["--" + name, str(value)]
    row = {"mode": mode, "reorder": reorder, "knobs": knobs, "attempts": [], "verdict": "FAIL"}
    out.mkdir(parents=True)
    try:
        with server(binary, abba.cpu_string(abba.cpus(args.server_cores) + abba.cpus(args.server_smt)),
                    args.port, out / "server", server_args) as (control, process), ExitStack() as stack:
            row["pid"] = process.pid
            for name, value in {**knobs, "flip-auto": 0, "atomic": 1}.items():
                require(control.must("CONFIG", "GET", name) == [name.encode(), str(value).encode()],
                        f"boot did not apply {name}={value}")
            require(control.must("DBSIZE") == 0, "diagnostic boot is not fresh")
            initial = topology(control)
            producer, clients, placed, captures = same_producer_clients(control, args.port, stack, initial)
            writer, reader, monitor, blocker = clients
            row.update(producer=producer, client_placement=captures)
            owner = next((entry["owner"] for entry in placed["shards"].values() if entry["owner"] != producer), None)
            require(owner is not None, "no owner distinct from producer for directed gather window")
            owner_count = len({entry["owner"] for entry in placed["shards"].values()})
            block_key, _ = owned_key(control, "reorder-blocker", owner, owner_count)
            if args.blocker_bytes and args.blocker_count:
                require(control.must("SET", block_key, b"\xff" * args.blocker_bytes) == b"OK", "blocker seed failed")
            row.update(owner=owner, blocker_bytes=args.blocker_bytes if args.blocker_count else 0)
            fresh = []
            for attempt in range(args.attempts):
                key, shard = owned_key(control, f"witness-{mode}-{reorder}-{attempt}", owner, owner_count)
                require(control.must("SET", key, OLD) == b"OK", "fresh-state seed failed")
                fresh.append((key, shard))
            # Seed distinct unused keys before MONITOR is armed. A remote control
            # producer's feed may arrive after a local fence; keeping its SETs out
            # of the transcript prevents mistaking that delivery lag for a write
            # inside the directed window. Every attempt consumes one fresh key.
            require(monitor.must("MONITOR") == b"OK", "MONITOR refused")
            counter_before = abba.info(control, "server").get("reorder_permuted_runs")
            peers = {name: producer for name in ("writer", "reader", "monitor", "blocker")}
            for attempt, (key, shard) in enumerate(fresh):
                require(control.must("GET", key) == OLD, "attempt did not start from its fresh old state")
                versions_before = no_snapshot_versions(control)
                before = topology(control)
                require(before["shards"][shard]["owner"] == owner, "witness key left the chosen owner")
                # A same-producer ECHO drains every older local MONITOR event. All
                # writer/reader replies from the preceding attempt were consumed.
                begin = f"begin-{mode}-{reorder}-{attempt}"
                require(writer.must("ECHO", begin) == begin.encode(), "opening fence failed")
                collect_until(monitor, address(writer), begin)
                count = args.blocker_count if args.blocker_bytes else 0
                if count:
                    blocker.raw(encode("BITCOUNT", block_key) * count)
                writer.send("SETRANGE", key, 0, "X")
                reader.send("GET", key)
                write_reply, read_reply = writer.read(), reader.read()
                for _ in range(count):
                    require(blocker.read() == args.blocker_bytes * 8, "long gather-window command failed")
                final_reply = control.must("GET", key)
                end = f"end-{mode}-{reorder}-{attempt}"
                require(writer.must("ECHO", end) == end.encode(), "closing fence failed")
                events = collect_until(monitor, address(writer), end)
                after = topology(control)
                record = {"number": attempt, "key": key, "shard": shard, "events": events,
                          "before": before, "after": after, "versions_before": versions_before,
                          "versions_after": no_snapshot_versions(control)}
                row["attempts"].append(record)
                record.update(judge_attempt(key=key, writer=address(writer), reader=address(reader),
                    events=events, write_reply=write_reply, read_reply=read_reply, final_reply=final_reply,
                    before=before, after=after, producer=producer, client_producers=peers))
                if not reorder:
                    require(not record["inversion"], "FIFO negative control inverted; fixture/model is invalid")
                (out / "result.json").write_text(json.dumps(row, indent=2) + "\n")
            row.update(finish_control(row["attempts"], reorder))
            counter_after = abba.info(control, "server").get("reorder_permuted_runs")
            if counter_before is not None or counter_after is not None:
                require(counter_before is not None and counter_after is not None, "permutation counter disappeared")
                delta = int(counter_after) - int(counter_before)
                require(delta > 0 if reorder else delta == 0, "available permutation counter contradicts control")
                row["preparatory_counter_delta"] = delta
            else:
                row["preparatory_counter_delta"] = None
    except Exception as exc:
        row.update(verdict="FAIL", reason=f"{type(exc).__name__}: {exc}")
    finally:
        (out / "result.json").write_text(json.dumps(row, indent=2) + "\n")
    return row


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--mode", choices=("both", "1s", "2s"), default="both")
    for name in ("server-cores", "load-cores", "load-smt"):
        parser.add_argument("--" + name, default=None)
    parser.add_argument("--server-smt", default="")
    parser.add_argument("--ports", default="8800-8800")
    parser.add_argument("--port", type=int)
    parser.add_argument("--attempts", type=int, default=16)
    parser.add_argument("--blocker-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--blocker-count", type=int, default=4)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args(argv)


def main(args):
    require(args.binary and args.binary.is_file() and os.access(args.binary, os.X_OK), "--binary must be executable")
    require(1 <= args.attempts <= 256, "--attempts must be 1..256")
    require(0 <= args.blocker_bytes <= 64 * 1024 * 1024 and 0 <= args.blocker_count <= 30,
            "blocker bound exceeded (64 MiB, 30 commands); 0 disables allocation")
    abba.resolve_geometry(args)
    server_cpus = abba.cpus(args.server_cores) + abba.cpus(args.server_smt)
    load_cpus = abba.cpus(args.load_cores) + abba.cpus(args.load_smt)
    abba.check_placement(abba.cpus(args.server_cores), abba.cpus(args.load_cores),
                         abba.cpus(args.server_smt), abba.cpus(args.load_smt))
    args.port, _ = abba.select_port(args.ports, args.port)
    out = (args.output or abba.ROOT / "build" / f"legacy-reorder-{int(time.time())}-{os.getpid()}").resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "scope": "standalone engagement witness; no performance verdict",
              "verdict": "FAIL", "controls": [], "binary_sha256": abba.sha256(args.binary)}
    previous_affinity = os.sched_getaffinity(0)
    try:
        os.sched_setaffinity(0, load_cpus)
        # This is a correctness observation of dispatch versus execution order, with no scored
        # timing. Other work may make the bounded arm fail to open, but cannot manufacture its
        # exactly-once command order and value transition. Keep all those witnesses mandatory;
        # only the separate ABBA tier requires an exclusive quiet box.
        report['quiet_box'] = {'required': False, 'reason': 'unscored correctness witness'}
        binary = out / "unchanged-binary"
        shutil.copy2(args.binary, binary)
        require(abba.sha256(binary) == report["binary_sha256"], "binary changed while copying")
        modes = ("1s", "2s") if args.mode == "both" else (args.mode,)
        for mode in modes:
            for reorder in (0, 1):
                row = run_control(args, binary, out / f"{mode}-reorder-{reorder}", mode, reorder)
                report["controls"].append(row)
                print(f"{mode} reorder={reorder}: {row['verdict']} armed={row.get('armed_attempts', 0)} "
                      f"inversions={row.get('inversions', 0)} {row.get('reason', '')}", flush=True)
                require(row["verdict"] == "PASS", "control failed; never retry a failed fixture into green")
        require(abba.sha256(binary) == report["binary_sha256"], "copied binary bytes changed")
        report["verdict"] = "PASS"
        return 0
    except (Exception, KeyboardInterrupt) as exc:
        report["reason"] = f"{type(exc).__name__}: {exc}"
        print(f"LEGACY REORDER FAIL: {report['reason']}", file=sys.stderr, flush=True)
        return 1
    finally:
        os.sched_setaffinity(0, previous_affinity)
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"Unscored execution-order controls only; no performance verdict. Artifacts: {out}")


def self_test():
    import copy
    import io
    from contextlib import contextmanager
    from types import SimpleNamespace
    import unittest
    from unittest import mock
    class ProtocolControls(unittest.TestCase):
        def setUp(self):
            self.before = {"stamp_ns": 1, "threads": {0: {"role": "io", "clients": 4, "full": 0, "masked_full": 0}},
                           "shards": {0: {"owner": 1, "migrations": 0}}}
            self.after = copy.deepcopy(self.before)
            self.after["stamp_ns"] = 2
            self.events = [monitor_event(b'1.000001 [0 127.0.0.1:1] "SETRANGE" "K" "0" "X"'),
                           monitor_event(b'1.000002 [0 127.0.0.1:2] "GET" "K"')]

        def judge(self, **changes):
            kwargs = dict(key="K", writer="127.0.0.1:1", reader="127.0.0.1:2", events=self.events,
                          write_reply=64, read_reply=OLD, final_reply=NEW, before=self.before,
                          after=self.after, producer=0, client_producers={"writer": 0, "reader": 0,
                          "monitor": 0, "blocker": 0})
            kwargs.update(changes)
            return judge_attempt(**kwargs)

        def test_state_inversion_passes_on_and_fails_off(self):
            result = self.judge()
            self.assertTrue(result["inversion"])
            self.assertEqual(finish_control([result], 1)["verdict"], "PASS")
            with self.assertRaisesRegex(WitnessError, "FIFO negative control"):
                finish_control([result], 0)

        def test_disabled_mechanism_cannot_pass_on_control(self):
            result = self.judge(read_reply=NEW)
            self.assertEqual(finish_control([result], 0)["verdict"], "PASS")
            with self.assertRaisesRegex(WitnessError, "never witnessed"):
                finish_control([result] * 16, 1)

        def test_arrival_order_without_admission_order_is_unreached(self):
            result = self.judge(events=list(reversed(self.events)))
            self.assertFalse(result["armed"])
            with self.assertRaisesRegex(WitnessError, "no admitted"):
                finish_control([result] * 16, 1)

        def test_missing_replayed_or_foreign_producer_commands_fail(self):
            for events in (self.events[:1], self.events * 2,
                           [self.events[0], {**self.events[1], "address": "127.0.0.1:3"}]):
                with self.subTest(events=events), self.assertRaises(WitnessError):
                    self.judge(events=events)
            with self.assertRaisesRegex(WitnessError, "share a producer"):
                self.judge(client_producers={"writer": 0, "reader": 2})

        def test_wrong_final_state_cannot_fabricate_a_permutation(self):
            with self.assertRaisesRegex(WitnessError, "final state"):
                self.judge(final_reply=OLD)

        def test_migration_full_queue_and_client_movement_invalidate(self):
            for section, entry, field in (("shards", 0, "owner"), ("shards", 0, "migrations"),
                                          ("threads", 0, "full"), ("threads", 0, "masked_full"), ("threads", 0, "clients")):
                after = copy.deepcopy(self.after)
                after[section][entry][field] += 1
                with self.subTest(field=field), self.assertRaises(WitnessError):
                    self.judge(after=after)

        def test_client_assignment_requires_exactly_one_accept(self):
            after = copy.deepcopy(self.after)
            after["threads"][0]["clients"] += 1
            self.assertEqual(new_client_owner(self.before, after), 0)
            after["threads"][0]["clients"] += 1
            with self.assertRaises(WitnessError):
                new_client_owner(self.before, after)

        def test_malformed_monitor_and_hidden_extra_mutation_fail(self):
            with self.assertRaises(WitnessError):
                monitor_event(b'not a MONITOR frame')
            extra = monitor_event(b'1.000003 [0 127.0.0.1:9] "SET" "K" "0"')
            with self.assertRaisesRegex(WitnessError, "unexpected operation"):
                self.judge(events=self.events + [extra])

        def test_snapshot_history_or_missing_telemetry_cannot_imitate_permutation(self):
            names = ("atomic_groups", "atomic_inflight", "atomic_entries",
                     "atomic_pending_entries", "script_intents_live")
            empty = dict.fromkeys(names, "0")
            with mock.patch.object(abba, "info", return_value=empty):
                self.assertEqual(no_snapshot_versions(mock.Mock()), dict.fromkeys(names, 0))
            for name in names:
                with self.subTest(name=name), mock.patch.object(abba, "info", return_value={**empty, name: "1"}):
                    with self.assertRaisesRegex(WitnessError, "MVCC records/groups"):
                        no_snapshot_versions(mock.Mock())
            with mock.patch.object(abba, "info", return_value={}):
                with self.assertRaisesRegex(WitnessError, "missing MVCC"):
                    no_snapshot_versions(mock.Mock())

        def test_real_protocol_loop_detects_enabled_disabled_and_broken_off_controls(self):
            # Drive run_control's real client placement, requests, MONITOR parser,
            # fences, fresh keys and verdict. Replace only the server/transport.
            # A fake server which never permutes must make the ON loop red.
            for reorder, permutes, expected in ((0, False, "PASS"), (1, True, "PASS"),
                                                (1, False, "FAIL"), (0, True, "FAIL")):
                with self.subTest(reorder=reorder, permutes=permutes), tempfile.TemporaryDirectory() as tmp:
                    engine = SimpleNamespace(db={}, clients=0, next_client=0, monitor=None,
                                             pending=None, stamp=0, permutations=0, requests=[], config={})
                    class FakeConn:
                        def __init__(self, *args, **kwargs):
                            engine.clients += 1
                            engine.next_client += 1
                            self.replies = []
                            self.closed = False
                            self.sock = SimpleNamespace(getsockname=lambda: ("127.0.0.1", 10000 + engine.next_client))
                            self.port = 10000 + engine.next_client
                            self.sock.getsockname = lambda: ("127.0.0.1", self.port)

                        def close(self):
                            if not self.closed:
                                engine.clients -= 1
                                self.closed = True

                        def send(self, *args):
                            encoded = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
                            name = encoded[0].decode()
                            engine.requests.append(encoded)
                            if engine.monitor is not None and name != "MONITOR":
                                quoted = " ".join(json.dumps(arg.decode()) for arg in encoded)
                                engine.monitor.replies.append(f"1.000001 [0 {address(self)}] {quoted}".encode())
                            if name == "PING": answer = b"PONG"
                            elif name == "CONFIG": answer = [encoded[2], engine.config[encoded[2].decode()].encode()]
                            elif name == "DBSIZE": answer = len(engine.db)
                            elif name == "DEBUG" and encoded[1] == b"SHARDS": answer = [[0, 1]] * (len(encoded) - 2)
                            elif name == "DEBUG":
                                engine.stamp += 1
                                answer = (f"lbver 1 stamp_ns {engine.stamp}\nthread 0 fused 0 {engine.clients} 0 0 0 0 0 0 0 0 masked_lane_full_events 0\n"
                                          "thread 1 fused 0 0 0 0 0 0 0 0 0 0 masked_lane_full_events 0\nshard 0 1 0 0 0 0 0\n").encode()
                            elif name == "INFO":
                                answer = (f"reorder_permuted_runs:{engine.permutations}\r\n"
                                          "atomic_groups:0\r\natomic_inflight:0\r\natomic_entries:0\r\n"
                                          "atomic_pending_entries:0\r\nscript_intents_live:0\r\n").encode()
                            elif name == "SET":
                                engine.db[encoded[1]] = encoded[2]
                                answer = b"OK"
                            elif name == "MONITOR":
                                engine.monitor = self
                                answer = b"OK"
                            elif name == "ECHO": answer = encoded[1]
                            elif name == "BITCOUNT": answer = len(engine.db[encoded[1]]) * 8
                            elif name == "SETRANGE":
                                engine.pending = (self, encoded[1])
                                return
                            elif name == "GET":
                                answer = engine.db[encoded[1]]
                                if engine.pending:
                                    writer_conn, key = engine.pending
                                    engine.db[key] = NEW
                                    writer_conn.replies.append(len(NEW))
                                    engine.pending = None
                                    if permutes:
                                        engine.permutations += 1
                                    else:
                                        answer = engine.db[encoded[1]]
                            else: raise AssertionError(encoded)
                            self.replies.append(answer)

                        def raw(self, payload):
                            stream = SimpleNamespace(file=io.BytesIO(payload))
                            stream._line = lambda: self._resp_line(stream)
                            stream.read = lambda: self._resp_read(stream)
                            while stream.file.tell() < len(payload):
                                self.send(*stream.read())

                        def read(self):
                            require(self.replies, "fake transport observed an unfulfilled request")
                            return self.replies.pop(0)

                        def must(self, *args):
                            self.send(*args)
                            return self.read()

                    @contextmanager
                    def fake_server(binary, cpus, port, folder, arguments):
                        engine.config = {arguments[i][2:]: arguments[i + 1] for i in range(0, len(arguments), 2)}
                        control = FakeConn()
                        try:
                            yield control, SimpleNamespace(pid=123)
                        finally:
                            control.close()
                    args = SimpleNamespace(server_cores="0-1", server_smt="", port=1,
                                           attempts=3, blocker_bytes=64, blocker_count=1)
                    knobs = {"thread-mode": "1s", "read-local": 0, "overlap": 1, "reorder": reorder}
                    real_conn = Conn
                    # The real RESP reader above must remain reachable after the
                    # module's connection constructor is replaced by FakeConn.
                    FakeConn._resp_read = staticmethod(real_conn.read)
                    FakeConn._resp_line = staticmethod(real_conn._line)
                    with mock.patch(__name__ + ".Conn", FakeConn), \
                         mock.patch(__name__ + ".server", side_effect=fake_server), \
                         mock.patch(__name__ + ".translated_knobs", return_value=knobs):
                        row = run_control(args, Path("/unexecuted"), Path(tmp) / "control", "1s", reorder)
                    self.assertEqual(row["verdict"], expected, row.get("reason"))
                    self.assertEqual(engine.clients, 0)
                    keys = [request[1] for request in engine.requests if request[0] == b"SETRANGE"]
                    self.assertEqual(len(keys), len(set(keys)))
                    if reorder or not permutes:
                        self.assertEqual(len(keys), args.attempts)
    return 0 if unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ProtocolControls)).wasSuccessful() else 1


if __name__ == "__main__":
    args = parse_args()
    if args.self_test:
        raise SystemExit(self_test())
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"signal {signum}")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        raise SystemExit(main(args))
    except (ValueError, OSError, WitnessError) as exc:
        print(f"LEGACY REORDER REFUSED: {exc}", file=sys.stderr)
        raise SystemExit(2)
