#!/usr/bin/env python3
"""Replayable productive-role occupancy, one necessary saturation witness.

Neither CPU occupancy nor this score proves capacity alone. ABBA additionally
requires a stable per-arm throughput plateau under a higher-worker-capacity
probe. The diagnostic wrapper cannot turn retained experiments into gate proof.
"""
from dataclasses import dataclass
import json
import math
from pathlib import Path

SATURATION_FLOOR = 95.0


@dataclass(frozen=True)
class LbSnapshot:
    stamp_ns: int
    threads: dict[int, dict]


def parse_snapshot(raw: bytes) -> LbSnapshot:
    """Parse the stable LBSIGNALS schema-1 prefix emitted by cmd/lbsignals.cc."""
    stamp = None
    rows = {}
    for line in raw.decode().splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "lbver":
            if stamp is not None or len(fields) != 4 or fields[1:3] != ["1", "stamp_ns"]:
                raise RuntimeError("missing, duplicate, or unsupported LBSIGNALS schema")
            stamp = int(fields[3])
            if stamp < 0:
                raise RuntimeError("negative LBSIGNALS capture timestamp")
        elif fields[0] == "thread":
            if len(fields) < 10 or fields[2] not in ("io", "ex", "fused"):
                raise RuntimeError("malformed LBSIGNALS thread prefix")
            tid, domain, clients, iterations, ops, busy, idle, cpu = (
                int(fields[index]) for index in (1, 3, 4, 5, 6, 7, 8, 9))
            if min(tid, domain, clients, iterations, ops, busy, idle, cpu) < 0 or tid in rows:
                raise RuntimeError("negative or duplicate LBSIGNALS thread counter")
            if fields[2] == "ex" and clients:
                raise RuntimeError("executor unexpectedly owns client connections")
            rows[tid] = dict(role=fields[2], domain=domain, clients=clients,
                iterations=iterations, ops=ops, busy=busy, idle=idle, cpu=cpu)
    if stamp is None or not rows:
        raise RuntimeError("LBSIGNALS lacks capture time or thread counters")
    return LbSnapshot(stamp, rows)


def bottleneck_saturation(start: LbSnapshot, end: LbSnapshot, *, floor_pct: float) -> dict:
    """Preserve the exact counters behind a productive-role occupancy score.

    flipctl.cc:684 documents I/O submit/reap work outside busy_ns. Its wall-idle
    demand signal includes that missing work. ex_loop.h:788 books empty polling
    passes as idle; CPU time alone would misclassify those spinners as useful work.
    We therefore try min(wall-idle, CPU), with a workload-progress witness, and
    average over ALL threads in each role. Dropping inactive peers or taking the
    hottest thread could make a control thread certify an otherwise idle server.

    rl2s.cc:54 explicitly cancels local execution's second ops charge: split I/O
    counts parsed/dispatched commands, while EX counts owner TASKS (MGET may fan
    out). Fused counts combined work. Use ops only to establish progress; never
    divide work between roles by counters that have different units. Clients is
    an ownership gauge and EX always exports zero, not evidence of an idle owner.

    An idle role is legitimate: split read-local GET need not issue executor
    tasks. Averaging that idle role into the busy role caps a saturated server at
    50%. The maximum ROLE average identifies the bottleneck; it never drops idle
    peers within that role. The unchanged floor must hold separately in every
    reference and candidate measurement, alongside the independent plateau.
    """
    if not math.isfinite(floor_pct) or not 0 < floor_pct <= 100:
        raise ValueError("invalid diagnostic saturation floor")
    floor_pct = float(floor_pct)  # One JSON representation for the fixed numeric criterion.
    wall = end.stamp_ns - start.stamp_ns
    if wall <= 0 or start.threads.keys() != end.threads.keys():
        raise RuntimeError("changed LBSIGNALS topology or nonpositive capture interval")
    roles = {row["role"] for row in start.threads.values()}
    if roles not in ({"fused"}, {"io", "ex"}):
        raise RuntimeError("incomplete or mixed LBSIGNALS role geometry")
    thread_rows = {}
    role_rows = {role: dict(threads=0, productive_threads=0, ops=0,
                           work_pct=0.0, cpu_pct=0.0, score_pct=0.0) for role in sorted(roles)}
    clamp = lambda value: min(1.0, max(0.0, value))
    # JSON writers may sort string thread IDs lexically (0,1,10,...,2). Sum in
    # numeric TID order so receipt replay has identical floating-point reductions.
    for tid, before in sorted(start.threads.items()):
        after = end.threads[tid]
        role = before["role"]
        if after["role"] != role:
            raise RuntimeError("LBSIGNALS role changed during diagnostic window")
        delta = {key: after[key] - before[key] for key in ("ops", "busy", "idle", "cpu")}
        if any(value < 0 for value in delta.values()):
            raise RuntimeError("LBSIGNALS progress/time counter reset during diagnostic window")
        work_fraction = 1 - delta["idle"] / wall
        cpu_fraction = delta["cpu"] / wall
        has_clients = role == "ex" or (before["clients"] > 0 and after["clients"] > 0)
        productive = delta["ops"] > 0 and has_clients
        credit = min(clamp(work_fraction), clamp(cpu_fraction)) if productive else 0.0
        # Spans publish only at their end; a boundary can include/exclude part of
        # one span. Keep the raw out-of-range value and an explicit clamping flag
        # so live validation can quantify this error, not silently excuse it.
        thread_rows[tid] = dict(role=role, clients_before=before["clients"],
            clients_after=after["clients"], ops_delta=delta["ops"],
            busy_ns_delta=delta["busy"], idle_ns_delta=delta["idle"], cpu_ns_delta=delta["cpu"],
            productive=productive, work_pct=100 * work_fraction, cpu_pct=100 * cpu_fraction,
            score_pct=100 * credit,
            clipped=not (0 <= work_fraction <= 1 and 0 <= cpu_fraction <= 1),
            excluded_reason="" if productive else "no operation progress" if delta["ops"] == 0
                            else "no client ownership at both snapshots")
        summary = role_rows[role]
        summary["threads"] += 1
        summary["productive_threads"] += productive
        summary["ops"] += delta["ops"]
        summary["work_pct"] += 100 * work_fraction
        summary["cpu_pct"] += 100 * cpu_fraction
        summary["score_pct"] += 100 * credit
    for summary in role_rows.values():
        for key in ("work_pct", "cpu_pct", "score_pct"):
            summary[key] /= summary["threads"]
    role = max(role_rows, key=lambda name: role_rows[name]["score_pct"])
    score = role_rows[role]["score_pct"]
    return dict(schema=1, criterion="productive-role-v1",
        stamp_before_ns=start.stamp_ns, stamp_after_ns=end.stamp_ns, window_seconds=wall / 1e9,
        floor_pct=floor_pct, score_pct=score, floor_met=score >= floor_pct,
        highest_scoring_role=role, roles=role_rows, threads=thread_rows)


def productive_saturation(start: LbSnapshot, end: LbSnapshot, *, floor_pct: float) -> dict:
    """Historical diagnostic shape; replay never retroactively certifies a run."""
    result = bottleneck_saturation(start, end, floor_pct=floor_pct)
    result.pop("schema")
    result["proposed_floor_met"] = result.pop("floor_met")
    return dict(result, validation="UNVALIDATED", decision_input=False)


def replay_saturation(record, *, floor_pct, mode=None, thread_count=None):
    """Recompute from raw same-window deltas; cached scores are never evidence.

    JSON changes integer dictionary keys to strings. Reconstruct the counters
    with zero baselines, preserving both client gauges and the actual stamps.
    A reset, missing thread, foreign role, or changed derived value fails closed.
    No legacy busy ratio or process CPU value substitutes for missing evidence.
    """
    integer = lambda value: type(value) is int and value >= 0
    try:
        if (not isinstance(record, dict) or record.get("schema") != 1 or
                record.get("criterion") != "productive-role-v1" or
                record.get("floor_pct") != floor_pct or
                not integer(record.get("stamp_before_ns")) or
                not integer(record.get("stamp_after_ns"))):
            raise ValueError("missing or unsupported saturation evidence")
        rows = record.get("threads")
        if not isinstance(rows, dict) or not rows:
            raise ValueError("missing saturation thread evidence")
        before, after = {}, {}
        for key, row in rows.items():
            tid = int(key)
            if (str(tid) != str(key) or tid < 0 or tid in before or not isinstance(row, dict) or
                    row.get("role") not in ("io", "ex", "fused")):
                raise ValueError("invalid or duplicate saturation thread")
            fields = ("clients_before", "clients_after", "ops_delta", "busy_ns_delta",
                      "idle_ns_delta", "cpu_ns_delta")
            if any(not integer(row.get(field)) for field in fields):
                raise ValueError("invalid/reset saturation counter")
            if row["role"] == "ex" and (row["clients_before"] or row["clients_after"]):
                raise ValueError("executor saturation evidence owns clients")
            before[tid] = dict(role=row["role"], clients=row["clients_before"],
                               ops=0, busy=0, idle=0, cpu=0)
            after[tid] = dict(role=row["role"], clients=row["clients_after"],
                **{name: row[field] for name, field in
                   (("ops", "ops_delta"), ("busy", "busy_ns_delta"),
                    ("idle", "idle_ns_delta"), ("cpu", "cpu_ns_delta"))})
        if set(before) != set(range(len(before))) or thread_count is not None and len(before) != thread_count:
            raise ValueError("saturation thread inventory differs from server geometry")
        roles = {row["role"] for row in before.values()}
        if mode is not None and (mode not in ("1s", "2s") or
                roles != ({"fused"} if mode == "1s" else {"io", "ex"})):
            raise ValueError("saturation roles differ from measured cell")
        if mode == "2s" and sum(row["role"] == "ex" for row in before.values()) != len(before) // 2:
            raise ValueError("saturation roles differ from fixed split geometry")
        replayed = bottleneck_saturation(LbSnapshot(record["stamp_before_ns"], before),
            LbSnapshot(record["stamp_after_ns"], after), floor_pct=floor_pct)
        # Compare canonical JSON after normalizing thread keys; this rejects unknown
        # fields and forged role averages/flags as well as a forged overall score.
        canonical = lambda value: json.dumps(value, sort_keys=True, allow_nan=False)
        normalized = lambda value: dict(value, threads={str(k): v for k, v in value["threads"].items()})
        if canonical(normalized(record)) != canonical(normalized(replayed)):
            raise ValueError("saturation summary differs from raw same-window counters")
        return replayed
    except (KeyError, TypeError, OverflowError, RuntimeError) as error:
        raise ValueError(f"invalid saturation evidence: {error}") from error


def require_saturation_window(record, run):
    # src/core/signal.h now_ns() and Python time.monotonic() both use
    # CLOCK_MONOTONIC. Bind the interval as well as its length: population or
    # another run's occupied interval cannot certify this run's workload.
    midpoint, seconds = run.get("midpoint_monotonic"), run.get("window_seconds")
    if any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0
           for value in (midpoint, seconds)):
        raise ValueError("missing/invalid saturation workload interval")
    first_ns = round((midpoint - seconds / 2) * 1e9)
    last_ns = round((midpoint + seconds / 2) * 1e9)
    if not record["stamp_before_ns"] <= first_ns < last_ns <= record["stamp_after_ns"]:
        raise ValueError("saturation snapshots do not span the measured workload window")
    raw_ns = record["stamp_after_ns"] - record["stamp_before_ns"]
    central_ns = last_ns - first_ns
    outside_ns = raw_ns - central_ns
    roles = {role: dict(threads=0, score_pct=0.0) for role in sorted(record["roles"])}
    threads = {}
    # Containment alone is insufficient: INFO/proc capture or a scheduling pause
    # can widen the bracket. Charge ALL time outside the central interval against
    # every thread's observed work AND CPU. The remaining durations are lower
    # bounds inside the scored interval, with no empirical skew allowance.
    for tid, row in sorted(record["threads"].items(), key=lambda item: int(item[0])):
        work_ns = max(0, raw_ns - row["idle_ns_delta"] - outside_ns)
        cpu_ns = max(0, min(raw_ns, row["cpu_ns_delta"]) - outside_ns)
        score = 100 * min(work_ns, cpu_ns) / central_ns if row["productive"] else 0.0
        threads[str(tid)] = dict(work_ns_lower_bound=work_ns, cpu_ns_lower_bound=cpu_ns, score_pct=score)
        roles[row["role"]]["threads"] += 1
        roles[row["role"]]["score_pct"] += score
    for row in roles.values():
        row["score_pct"] /= row["threads"]
    role = max(roles, key=lambda name: roles[name]["score_pct"])
    score = roles[role]["score_pct"]
    return dict(criterion="central-productive-role-v1", first_ns=first_ns, last_ns=last_ns,
        outside_before_ns=first_ns - record["stamp_before_ns"],
        outside_after_ns=record["stamp_after_ns"] - last_ns,
        score_pct=score, floor_met=score >= record["floor_pct"], highest_scoring_role=role,
        roles=roles, threads=threads)


def self_test():
    import copy
    import unittest

    class Controls(unittest.TestCase):
        def pair(self, rows):
            before = "lbver 1 stamp_ns 1000000000\n"
            after = "lbver 1 stamp_ns 21000000000\n"
            for tid, (role, clients, ops, busy, idle, cpu) in enumerate(rows):
                before += f"thread {tid} {role} 0 {clients} 1 0 0 0 0\n"
                after += (f"thread {tid} {role} 0 {clients} 2 {ops} "
                          f"{int(busy * 1e9)} {int(idle * 1e9)} {int(cpu * 1e9)}\n")
            return parse_snapshot(before.encode()), parse_snapshot(after.encode())

        def assess(self, rows):
            return productive_saturation(*self.pair(rows), floor_pct=95)

        def test_split_local_io_saturates_while_executors_are_idle(self):
            row = self.assess([("io", 32, 1000000, 10, 0, 20)] * 16 +
                              [("ex", 0, 0, 0, 20, 1)] * 16)
            self.assertEqual(row["score_pct"], 100)
            self.assertEqual(row["highest_scoring_role"], "io")
            self.assertEqual(row["roles"]["ex"]["score_pct"], 0)
            self.assertTrue(row["proposed_floor_met"])
            self.assertEqual(row["validation"], "UNVALIDATED")
            self.assertFalse(row["decision_input"])

        def test_executors_need_tasks_not_client_ownership(self):
            row = self.assess([("io", 32, 1000000, 5, 15, 6)] * 16 +
                              [("ex", 0, 7576000, 20, 0, 20)] * 16)
            self.assertEqual(row["highest_scoring_role"], "ex")
            self.assertEqual(row["score_pct"], 100)

        def test_fused_combined_counter_is_not_divided_between_roles(self):
            row = self.assess([("fused", 16, 2000000, 10, 0, 19.5)] * 32)
            self.assertAlmostEqual(row["score_pct"], 97.5)
            self.assertEqual(set(row["roles"]), {"fused"})

        def test_idle_polling_control_and_one_hot_thread_cannot_certify_role(self):
            shapes = [
                [("fused", 16, 0, 20, 0, 20)] * 32,  # control/spin without workload
                [("fused", 16, 100, 1, 19, 20)] * 32,  # CPU busy, mostly empty polls
                [("fused", 16, 100, 20, 0, 1)] * 32,  # wall time, not CPU capacity
                [("fused", 0, 100, 20, 0, 20)] * 32,  # no workload connections
                [("fused", 16, 100, 20, 0, 20)] + [("fused", 16, 0, 0, 20, 1)] * 31,
            ]
            for rows in shapes:
                with self.subTest(rows=rows[:2]):
                    self.assertFalse(self.assess(rows)["proposed_floor_met"])

        def test_bad_schema_and_duplicate_threads_are_rejected(self):
            for raw in (b"thread 0 io 0 1 1 1 1 1 1\n",
                        b"lbver 2 stamp_ns 1\nthread 0 io 0 1 1 1 1 1 1\n",
                        b"lbver 1 stamp_ns 1\nthread 0 ex 0 1 1 1 1 1 1\n",
                        b"lbver 1 stamp_ns 1\n" + b"thread 0 io 0 1 1 1 1 1 1\n" * 2):
                with self.subTest(raw=raw), self.assertRaises(RuntimeError):
                    parse_snapshot(raw)

        def test_counter_reset_role_change_and_no_clock_progress_are_invalid(self):
            before, after = self.pair([("fused", 16, 100, 20, 0, 20)])
            bads = [before, LbSnapshot(after.stamp_ns, {0: {**after.threads[0], "role": "io"}}),
                    LbSnapshot(after.stamp_ns, {0: {**after.threads[0], "cpu": -1}})]
            for bad in bads:
                with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                    productive_saturation(before, bad, floor_pct=95)

        def test_raw_saturation_replays_after_json_round_trip(self):
            before, after = self.pair([("io", 16, 10000, 10, 0, 20)] * 16 +
                                      [("ex", 0, 0, 0, 20, 1)] * 16)
            record = bottleneck_saturation(before, after, floor_pct=95)
            serialized = dict(record, threads={str(k): v for k, v in record["threads"].items()})
            replayed = replay_saturation(json.loads(json.dumps(serialized, sort_keys=True)), floor_pct=95,
                                          mode="2s", thread_count=32)
            self.assertEqual(replayed, record)
            self.assertEqual(replayed["score_pct"], 100)
            self.assertEqual(replayed["roles"]["ex"]["score_pct"], 0)

        def test_cached_saturation_cannot_hide_missing_or_unproductive_threads(self):
            before, after = self.pair([("fused", 16, 10000, 20, 0, 20)] * 32)
            record = bottleneck_saturation(before, after, floor_pct=95)
            mutations = [lambda value: value.update(score_pct=99),
                         lambda value: value.update(floor_pct=1),
                         lambda value: value.update(stamp_after_ns=value["stamp_before_ns"]),
                         lambda value: value["threads"].pop(31),
                         lambda value: value["threads"][0].update(ops_delta=0),
                         lambda value: value["threads"][0].update(cpu_ns_delta=-1),
                         lambda value: value["threads"][0].update(ops_delta=True),
                         lambda value: value["roles"]["fused"].update(score_pct=float("nan"))]
            for mutate in mutations:
                bad = copy.deepcopy(record)
                mutate(bad)
                with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                    replay_saturation(bad, floor_pct=95, mode="1s", thread_count=32)
            for bad in (None, {}, productive_saturation(before, after, floor_pct=95)):
                with self.subTest(record=bad), self.assertRaises(ValueError):
                    replay_saturation(bad, floor_pct=95, mode="1s", thread_count=32)
            with self.assertRaisesRegex(ValueError, "roles differ"):
                replay_saturation(record, floor_pct=95, mode="2s", thread_count=32)

        def test_saturation_is_bound_to_this_workload_interval(self):
            before, after = self.pair([("fused", 16, 10000, 20, 0, 20)] * 32)
            record = bottleneck_saturation(before, after, floor_pct=95)
            require_saturation_window(record, dict(midpoint_monotonic=11, window_seconds=20))
            for run in (dict(midpoint_monotonic=31, window_seconds=20),
                        dict(midpoint_monotonic=11, window_seconds=21),
                        dict(window_seconds=20),
                        dict(midpoint_monotonic=float("nan"), window_seconds=20)):
                with self.subTest(run=run), self.assertRaises(ValueError):
                    require_saturation_window(record, run)

        def test_wide_capture_cannot_borrow_outside_window_occupancy(self):
            # 980 occupied seconds surround an entirely idle central20 seconds.
            # The enclosing98% snapshot is valid arithmetic, but has no central
            # occupancy. Merely checking that it contains the window passes it.
            before, after = self.pair([("fused", 16, 10000, 980, 20, 980)] * 32)
            after = LbSnapshot(1_001_000_000_000, after.threads)
            record = bottleneck_saturation(before, after, floor_pct=95)
            self.assertTrue(record["floor_met"])
            central = require_saturation_window(record, dict(midpoint_monotonic=501, window_seconds=20))
            self.assertFalse(central["floor_met"])
            self.assertEqual(central["score_pct"], 0)

    return 0 if unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(Controls)).wasSuccessful() else 1


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--before", type=Path)
    parser.add_argument("--after", type=Path)
    args = parser.parse_args()
    if args.self_test:
        raise SystemExit(self_test())
    if args.before is None or args.after is None:
        parser.error("provide both raw snapshot paths, or --self-test")
    # Replaying raw artifacts is diagnostic only, even when the proposed floor is met.
    print(json.dumps(productive_saturation(parse_snapshot(args.before.read_bytes()),
        parse_snapshot(args.after.read_bytes()), floor_pct=95.0), indent=2))
