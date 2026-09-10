#!/usr/bin/env python3
"""UNVALIDATED productive-role saturation diagnostics; never a gate verdict.

The original all-thread busy ratio remains the gate's decision input. This module
prepares a competing criterion for controlled live validation, without changing
the 95% floor or certifying saturation from contaminated saved measurements.
"""
from dataclasses import dataclass
import json
import math
from pathlib import Path


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


def productive_saturation(start: LbSnapshot, end: LbSnapshot, *, floor_pct: float) -> dict:
    """Return an UNVALIDATED hypothesis, preserving the exact counters behind it.

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

    This necessary evidence does not prove a throughput plateau. Adoption needs
    quiet live loaded/underloaded/polling controls in both modes and every relevant
    path, then the standing null. No validation switch or inferred approval lives
    here; the existing gate decisions continue to use legacy busy_pct.
    """
    if not math.isfinite(floor_pct) or not 0 < floor_pct <= 100:
        raise ValueError("invalid diagnostic saturation floor")
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
    for tid, before in start.threads.items():
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
    return dict(criterion="productive-role-v1", validation="UNVALIDATED", decision_input=False,
        stamp_before_ns=start.stamp_ns, stamp_after_ns=end.stamp_ns, window_seconds=wall / 1e9,
        floor_pct=floor_pct, score_pct=score, proposed_floor_met=score >= floor_pct,
        highest_scoring_role=role, roles=role_rows, threads=thread_rows)


def self_test():
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
