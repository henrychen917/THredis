#!/usr/bin/env python3
"""Refuse foreign work before and during a performance measurement.

This observer never stops a process. /proc identities, not argv patterns, distinguish
the driver's children from foreign work. Ordinary sleeping services are harmless;
known compilers, servers and load generators are competing experiments even while
temporarily asleep between phases. Other processes are reported if their sampled
CPU time advances by at least one accounting tick on an overlapping affinity mask.
That is an interference witness, not a chosen regression tolerance or noise floor.
The sample cannot see a process born and reaped entirely between observations; the
exclusive-box rule remains necessary, and the evidence records this limitation.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import threading
import time


COMPETING = frozenset(("make", "gmake", "ninja", "cc1", "cc1plus", "clang", "clang++",
    "gcc", "g++", "ld", "ld.lld", "lto1", "rustc", "cargo", "memtier_benchma",
    "memtier_benchmar", "memtier_benchmark", "redis-server", "tomokv", "binary-A", "binary-B",
    "dragonfly", "keydb-server", "memcached", "redis-benchmark"))


@dataclass(frozen=True)
class Process:
    pid: int
    start: int
    parent: int
    name: str
    ticks: int
    affinity: frozenset[int]

    @property
    def identity(self):
        return self.pid, self.start


def snapshot() -> dict[int, Process]:
    result = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            raw = (entry / "stat").read_text()
            name, rest = raw.split("(", 1)[1].rsplit(")", 1)
            fields = rest.split()
            # Kernel threads have no executable and cannot be a user workload. Do
            # not suppress a process merely because it belongs to another user.
            (entry / "exe").readlink()
            pid = int(entry.name)
            result[pid] = Process(pid, int(fields[19]), int(fields[1]), name,
                                  int(fields[11]) + int(fields[12]),
                                  frozenset(os.sched_getaffinity(pid)))
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as exc:
            raise RuntimeError(f"cannot inspect PID {entry.name}; quiet-box status is unknown") from exc
    return result


def owned_processes(rows: dict[int, Process], root: tuple[int, int]) -> set[tuple[int, int]]:
    current = rows.get(root[0])
    if current is None or current.identity != root:
        raise RuntimeError("quiet observer lost its owning process identity")
    owned = {root}
    while True:
        parent_pids = {pid for pid, _ in owned}
        added = {row.identity for row in rows.values() if row.parent in parent_pids} - owned
        if not added:
            return owned
        owned.update(added)


def interference(before: dict[int, Process], after: dict[int, Process],
                 root: tuple[int, int], cpus: set[int]) -> list[dict]:
    owned = owned_processes(after, root)
    offenders = []
    for row in after.values():
        if row.identity in owned or not cpus.intersection(row.affinity):
            continue
        prior = before.get(row.pid)
        delta = max(0, row.ticks - prior.ticks) if prior and prior.identity == row.identity else 0
        if row.name in COMPETING or delta:
            offenders.append({"pid": row.pid, "start_ticks": row.start, "comm": row.name,
                              "cpu_ticks": delta, "reason": "competing workload" if row.name in
                              COMPETING else "foreign CPU activity",
                              "overlapping_cpus": sorted(cpus.intersection(row.affinity))})
    return offenders


class QuietMonitor:
    """Latch the first interference; checking never retries a bad sample into green."""
    def __init__(self, server_cpus, load_cpus, *, own_root_pid=None, interval=1.0):
        self.cpus = set(server_cpus) | set(load_cpus)
        self.previous = snapshot()
        pid = os.getpid() if own_root_pid is None else own_root_pid
        self.root = self.previous[pid].identity
        self.interval = interval
        self.started = time.time()
        self.samples = 0
        self.failure = None
        self.stop_event = threading.Event()
        self.thread = None

    def sample(self):
        current = snapshot()
        offenders = interference(self.previous, current, self.root, self.cpus)
        self.previous = current
        self.samples += 1
        if offenders and self.failure is None:
            self.failure = {"observed_at": time.time(), "processes": offenders}

    def evidence(self):
        return {"started_at": self.started, "sample_interval_seconds": self.interval,
                "samples": self.samples, "tick_seconds": 1 / os.sysconf("SC_CLK_TCK"),
                "cpus": sorted(self.cpus), "interference": self.failure,
                "limitation": "processes born and reaped between samples may be missed"}

    def check(self):
        if self.failure:
            details = self.failure.get("processes", [])
            reason = "; ".join(f"PID {p['pid']} ({p['comm']}): {p['reason']}, "
                               f"{p['cpu_ticks']} CPU ticks" for p in details)
            raise RuntimeError("QUIET-BOX PRECONDITION FAILED: " + (reason or self.failure["error"]))

    def preflight(self):
        self.sample()  # Refuse known competing programs without waiting first.
        self.check()
        time.sleep(self.interval)
        self.sample()
        self.check()
        return self.evidence()

    def start(self):
        self.preflight()
        def observe():
            while not self.stop_event.wait(self.interval):
                try:
                    self.sample()
                except Exception as exc:
                    self.failure = self.failure or {"observed_at": time.time(), "error": str(exc)}
        self.thread = threading.Thread(target=observe, name="gate-quiet", daemon=True)
        self.thread.start()
        return self

    def close(self):
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join()
        self.sample()
        return self.evidence()


def assert_quiet(server_cpus, load_cpus, *, own_root_pid=None) -> dict:
    return QuietMonitor(server_cpus, load_cpus, own_root_pid=own_root_pid).preflight()


def self_test():
    import unittest
    from unittest import mock
    class Controls(unittest.TestCase):
        def setUp(self):
            self.root = Process(10, 1, 1, "python3", 1, frozenset((0, 1)))
            self.own = Process(11, 2, 10, "tomokv", 1000, frozenset((0,)))
            self.other = Process(20, 3, 1, "editor", 100, frozenset((0, 1)))
            self.before = {p.pid: p for p in (self.root, self.own, self.other)}

        def check_rows(self, after):
            return interference(self.before, after, self.root.identity, {0})

        def test_own_server_and_sleeping_services_are_allowed(self):
            self.assertEqual(self.check_rows(self.before), [])

        def test_one_foreign_tick_is_contamination(self):
            from dataclasses import replace
            bad = {**self.before, 20: replace(self.other, ticks=101)}
            self.assertEqual(self.check_rows(bad)[0]["pid"], 20)

        def test_foreign_workload_refused_even_while_sleeping(self):
            from dataclasses import replace
            bad = {**self.before, 20: replace(self.other, name="cc1plus")}
            self.assertEqual(self.check_rows(bad)[0]["cpu_ticks"], 0)

        def test_pid_reuse_does_not_inherit_ticks_or_ownership(self):
            from dataclasses import replace
            bad = {**self.before, 11: replace(self.own, start=20, parent=1)}
            self.assertEqual(self.check_rows(bad)[0]["pid"], 11)
            clean = {**self.before, 20: replace(self.other, start=30, ticks=200)}
            self.assertEqual(self.check_rows(clean), [])

        def test_nonoverlapping_activity_is_irrelevant(self):
            from dataclasses import replace
            after = {**self.before, 20: replace(self.other, name="make", affinity=frozenset((2,)))}
            self.assertEqual(self.check_rows(after), [])

        def test_interference_is_latched(self):
            from dataclasses import replace
            bad = {**self.before, 20: replace(self.other, ticks=101)}
            with mock.patch(__name__ + ".snapshot", side_effect=[self.before, bad, bad]):
                monitor = QuietMonitor([0], [1], own_root_pid=10)
                monitor.sample()
                monitor.sample()
                with self.assertRaisesRegex(RuntimeError, "PID 20"):
                    monitor.check()
    return 0 if unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(Controls)).wasSuccessful() else 1


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not args.self_test:
        parser.error("use the Python API from the performance runner, or --self-test")
    raise SystemExit(self_test())
