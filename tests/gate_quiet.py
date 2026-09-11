#!/usr/bin/env python3
"""Quiet preflight: selected-core CPU activity and intended TCP ports.

Affinity is permission, not placement. /proc/stat supplies actual activity on the
CPUs granted to this run; no process inventory, identity or unrequested SMT sibling
is inspected. During the benchmark these same counters include our own workload,
so runtime samples are observations, not claims about foreign CPU consumption.
The preflight budget and the independent identical-binary null remain mandatory.
"""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import socket
import threading
import time

from gateplan import read_topology

BACKGROUND_CPU_FRACTION = 0.0015
POLICY = "selected-core-port-budget-v1"


class QuietViolation(RuntimeError):
    """A failed precondition is latched; retrying samples cannot turn it green."""


def cpu_snapshot(cpus, path=Path("/proc/stat")):
    selected = {f"cpu{cpu}": cpu for cpu in cpus}
    result = {}
    with path.open() as source:
        for line in source:
            fields = line.split()
            if not fields or fields[0] not in selected:
                continue
            # guest/guest_nice are already included in user/nice. Idle and iowait
            # consume no CPU; IRQ, softirq and steal must count against quietness.
            ticks = [int(value) for value in fields[1:9]]
            if len(ticks) != 8:
                raise QuietViolation("incomplete selected CPU counters")
            result[selected[fields[0]]] = (sum(ticks), sum(ticks) - ticks[3] - ticks[4])
    if result.keys() != set(cpus):
        raise QuietViolation("selected CPUs missing from /proc/stat")
    return result


def occupied_ports(ports, net_root=Path("/proc/net")):
    """Read sockets, not their owners: even an idle listener occupies our port."""
    selected = set(ports)
    occupied = set()
    for name in ("tcp", "tcp6"):
        path = net_root / name
        if name == "tcp6" and not path.exists():
            continue  # IPv6 may be disabled in this network namespace.
        for line in path.read_text().splitlines()[1:]:
            fields = line.split()
            if len(fields) > 3 and fields[3] == "0A":
                port = int(fields[1].rsplit(":", 1)[1], 16)
                if port in selected:
                    occupied.add(port)
    return sorted(occupied)


class QuietMonitor:
    def __init__(self, server_cpus, load_cpus, *, own_root_pid=None, interval=1.0,
                 window_seconds=20, sample_artifact=None, ports=()):
        self.cpus = set(server_cpus) | set(load_cpus)
        self.requested_cpus = self.cpus.copy()
        if (not server_cpus or not self.cpus or not math.isfinite(interval) or interval <= 0 or
                not math.isfinite(window_seconds) or window_seconds <= 0):
            raise ValueError("quiet monitor requires server CPUs and positive finite intervals")
        self.ports = sorted(set(ports))
        if any(type(port) is not int or not 1 <= port <= 65535 for port in self.ports):
            raise ValueError("quiet monitor ports must be integers in 1..65535")
        # Topology determines only the budget denominator, never expands the
        # observed CPU set beyond the explicit server/load axes.
        topology = read_topology(sorted(server_cpus))
        self.server_physical_cores = len({topology[cpu] for cpu in server_cpus})
        self.window_seconds = window_seconds
        self.cpu_budget_seconds = BACKGROUND_CPU_FRACTION * self.server_physical_cores * window_seconds
        self.interval = interval
        self.tick_seconds = 1 / os.sysconf("SC_CLK_TCK")
        self.previous = cpu_snapshot(self.cpus)
        self.previous_at = self.started_monotonic = time.monotonic()
        self.started = time.time()
        self.finished = None
        self.preflight_seconds = None
        self.preflight_cpu_ticks = 0
        self.samples = 0
        self.listener_checks = 0
        self.failure = None
        self.closed = False
        self.thread = None
        self.stop_event = threading.Event()
        self.sample_artifact = Path(sample_artifact) if sample_artifact else None
        self.sample_events = []
        if self.sample_artifact:
            self.sample_artifact.touch(exist_ok=False)

    def sample(self, *, enforce=False):
        current = cpu_snapshot(self.cpus)
        now = time.monotonic()
        if now < self.previous_at:
            raise QuietViolation("quiet observer monotonic clock moved backwards")
        activity = []
        for cpu in sorted(self.cpus):
            total = current[cpu][0] - self.previous[cpu][0]
            busy = current[cpu][1] - self.previous[cpu][1]
            if total < 0 or busy < 0 or busy > total:
                raise QuietViolation(f"selected CPU {cpu} counters moved backwards")
            activity.append(dict(cpu=cpu, total_ticks=total, busy_ticks=busy))
        if enforce:
            self.preflight_cpu_ticks += sum(row["busy_ticks"] for row in activity)
            seconds = self.preflight_cpu_ticks * self.tick_seconds
            if seconds > self.cpu_budget_seconds and self.failure is None:
                self.failure = dict(observed_at=time.time(), cpu_seconds=seconds,
                    cpu_budget_seconds=self.cpu_budget_seconds,
                    error=f"selected-core CPU screening budget exceeded: {seconds:.6f}s > "
                          f"{self.cpu_budget_seconds:.6f}s per {self.window_seconds:g}s",
                    cpus=[row for row in activity if row["busy_ticks"]])
        event = dict(started_monotonic=self.previous_at, ended_monotonic=now,
                     phase="preflight" if enforce else "owned-workload",
                     cpus=activity)
        if self.sample_artifact:
            with self.sample_artifact.open("a") as stream:
                stream.write(json.dumps(event, sort_keys=True) + "\n")
        else:
            self.sample_events.append(event)
        self.previous, self.previous_at = current, now
        self.samples += 1

    def check_ports(self):
        occupied = occupied_ports(self.ports)
        self.listener_checks += 1
        if occupied and self.failure is None:
            self.failure = dict(observed_at=time.time(), ports=occupied,
                                error=f"intended TCP ports already listening: {occupied}")
        self.check()

    def preflight(self):
        self.check_ports()
        deadline = self.started_monotonic + self.window_seconds
        while self.previous_at < deadline:
            time.sleep(min(self.interval, deadline - self.previous_at))
            self.sample(enforce=True)
            self.check()
        self.check_ports()
        self.preflight_seconds = self.previous_at - self.started_monotonic
        return self.evidence()

    def start(self):
        self.preflight()
        def observe():
            while not self.stop_event.wait(self.interval):
                self.observe()
        self.thread = threading.Thread(target=observe, name="gate-quiet", daemon=True)
        self.thread.start()
        return self

    def observe(self):
        try:
            self.sample()
        except Exception as error:
            self.failure = self.failure or dict(observed_at=time.time(), error=str(error))

    def check(self):
        if self.failure:
            raise QuietViolation("QUIET-BOX PRECONDITION FAILED: " + self.failure["error"])

    def close(self):
        if not self.closed:
            self.stop_event.set()
            if self.thread is not None:
                self.thread.join()
            self.observe()
            self.closed = True
            self.finished = time.time()
        return self.evidence()

    def evidence(self):
        return dict(policy=POLICY, started_at=self.started, finished_at=self.finished,
            complete=self.closed and self.failure is None and self.preflight_seconds is not None,
            samples=self.samples, cpu_samples=self.samples, sample_interval_seconds=self.interval,
            tick_seconds=self.tick_seconds, cpus=sorted(self.cpus), requested_cpus=sorted(self.cpus),
            ports=self.ports, listener_checks=self.listener_checks, interference=self.failure,
            sample_artifact=str(self.sample_artifact) if self.sample_artifact else None,
            **({"cpu_activity": self.sample_events} if not self.sample_artifact else {}),
            generic_cpu_screening=dict(scope="preflight", server_physical_cores=self.server_physical_cores,
                capacity_fraction=BACKGROUND_CPU_FRACTION, window_seconds=self.window_seconds,
                cpu_budget_seconds=self.cpu_budget_seconds, preflight_seconds=self.preflight_seconds,
                peak_rolling=dict(cpu_ticks=self.preflight_cpu_ticks,
                                  cpu_seconds=self.preflight_cpu_ticks * self.tick_seconds),
                accounting="selected /proc/stat CPUs only; runtime samples include owned workload"),
            limitation="preflight detects existing contention; runtime CPU counters cannot distinguish owned work")


def assert_quiet(server_cpus, load_cpus, *, own_root_pid=None, ports=()):
    monitor = QuietMonitor(server_cpus, load_cpus, ports=ports)
    monitor.preflight()
    return monitor.close()


def self_test():
    import tempfile
    import unittest
    from unittest import mock

    class Controls(unittest.TestCase):
        def monitor(self, samples):
            topology = {0: frozenset((0, 128)), 1: frozenset((1, 129))}
            with mock.patch(__name__ + ".read_topology", return_value=topology), \
                 mock.patch(__name__ + ".cpu_snapshot", return_value=samples):
                return QuietMonitor([0], [1], window_seconds=20)

        def test_only_selected_counters_are_read(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "stat"
                path.write_text("cpu nonsense ignored\ncpu0 1 0 2 97 0 0 0 0 0 0\n"
                                "cpu128 unrequested and deliberately malformed\n")
                self.assertEqual(cpu_snapshot({0}, path), {0: (100, 3)})
                with self.assertRaises(QuietViolation):
                    cpu_snapshot({1}, path)

        def test_desktop_outside_selected_cpus_does_not_block(self):
            monitor = self.monitor({0: (100, 0), 1: (100, 0)})
            with mock.patch(__name__ + ".cpu_snapshot", return_value={0: (2100, 1), 1: (2100, 0)}):
                monitor.sample(enforce=True)
            monitor.check()
            self.assertEqual(monitor.cpus, {0, 1})
            self.assertEqual(monitor.evidence()["generic_cpu_screening"]["peak_rolling"]["cpu_ticks"], 1)

        def test_competing_benchmark_on_selected_core_refused_and_latched(self):
            monitor = self.monitor({0: (100, 0), 1: (100, 0)})
            with mock.patch(__name__ + ".cpu_snapshot", return_value={0: (2100, 2000), 1: (2100, 0)}):
                monitor.sample(enforce=True)
                monitor.sample()
            with self.assertRaisesRegex(QuietViolation, "selected-core CPU screening budget exceeded"):
                monitor.check()

        def test_own_workload_recorded_without_becoming_foreign_activity(self):
            monitor = self.monitor({0: (100, 0), 1: (100, 0)})
            with mock.patch(__name__ + ".cpu_snapshot", return_value={0: (2100, 2000), 1: (2100, 2000)}):
                monitor.sample()
            monitor.check()
            self.assertEqual(monitor.evidence()["cpu_activity"][0]["phase"], "owned-workload")

        def test_idle_listener_blocks_only_intended_port(self):
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                port = listener.getsockname()[1]
                self.assertEqual(occupied_ports([port]), [port])
                self.assertEqual(occupied_ports([]), [])
                monitor = self.monitor({0: (100, 0), 1: (100, 0)})
                monitor.ports = [port]
                with self.assertRaisesRegex(QuietViolation, "already listening"):
                    monitor.check_ports()

        def test_missing_or_backwards_counters_fail(self):
            monitor = self.monitor({0: (100, 0), 1: (100, 0)})
            with mock.patch(__name__ + ".cpu_snapshot", return_value={0: (99, 0), 1: (100, 0)}):
                monitor.observe()
            with self.assertRaisesRegex(QuietViolation, "counters moved backwards"):
                monitor.check()

        def test_closing_without_preflight_cannot_certify_quietness(self):
            monitor = self.monitor({0: (100, 0), 1: (100, 0)})
            with mock.patch(__name__ + ".cpu_snapshot", return_value={0: (100, 0), 1: (100, 0)}):
                self.assertFalse(monitor.close()["complete"])

        def test_unreadable_cpu_file_invalidates_observer(self):
            monitor = self.monitor({0: (100, 0), 1: (100, 0)})
            with mock.patch(__name__ + ".cpu_snapshot", side_effect=PermissionError("lost /proc access")):
                monitor.observe()
            with self.assertRaisesRegex(QuietViolation, "lost /proc access"):
                monitor.check()

    result = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(self_test())
