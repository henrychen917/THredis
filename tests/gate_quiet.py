#!/usr/bin/env python3
"""Refuse foreign work before and during a performance measurement.

This observer never stops a process. /proc identities, not argv patterns, distinguish
the driver's children from foreign work. Ordinary sleeping services are harmless;
known compilers, load generators and ABBA drivers are competing experiments even
while temporarily asleep between phases. An idle unrelated server is recorded,
then subject to the same bounded CPU screening as other generic foreign processes.
Every observed foreign CPU tick is recorded, including activity below the budget.
PF_KTHREAD identifies kernel threads; their CPU activity is recorded separately
and cannot be mistaken for a foreign user workload because exe access is denied.
The gate's declared row watchdog is controller housekeeping only after its exact
PID/start identity, script and captured controller parent are independently verified.
The screening budget is not a regression tolerance or a bound on cache/tail effects;
the standing identical-binary null must still validate the comparison instrument.
The sample cannot see a process born and reaped entirely between observations; the
exclusive-box rule remains necessary, and the evidence records this limitation.
"""
from __future__ import annotations

from dataclasses import dataclass
from collections import deque
import math
import os
from pathlib import Path
import re
import threading
import time

from gateplan import read_topology


ACTIVE_EXPERIMENTS = frozenset(("make", "gmake", "ninja", "cc1", "cc1plus", "clang", "clang++",
    "gcc", "g++", "ld", "ld.lld", "lto1", "rustc", "cargo", "memtier_benchma",
    "memtier_benchmar", "memtier_benchmark", "binary-A", "binary-B", "redis-benchmark"))
SERVERS = frozenset(("redis-server", "tomokv", "dragonfly", "keydb-server", "memcached", "GarnetServer"))
COMPETING = ACTIVE_EXPERIMENTS | SERVERS

# The first live preflight rejected two isolated 10ms desktop ticks. A subsequent
# idle-box capture (2026-09-10, quiet-background-60s.json) recorded ~2.6 CPU seconds
# in 68.77 wall seconds, with 0.16-second bursts but every rolling20s below 0.96s.
# This screening heuristic uses the owner's recorded 0.15% quiet benchmark
# resolution as its capacity scale: 0.0015 * SERVER physical cores * measurement
# seconds. The owner did not specify a CPU-time bound.
# Load cores and SMT threads never enlarge it. This is not evidence that CPU time
# bounds cache displacement or tail latency. The all-cell null remains mandatory.
# Test the full rolling budget at every sample; a strict per-second rate cap would
# reject ten samples of that same idle capture. No measured burst factor is added.
# Replaying that capture gives 0.92s/0.96s at20s (PASS), 0.53s/0.48s at10s (FAIL).
# Shortening the measurement does not preserve this precondition automatically.
GENERIC_CPU_FRACTION = 0.0015

# Verified against the installed Linux 7.0.0-31 include/linux/sched.h:1781.
# /proc/PID/stat field9 exports task flags. Neither comm nor an unreadable exe
# distinguishes kernel workers from another user's process: both can give EPERM.
PF_KTHREAD = 0x00200000


class QuietViolation(RuntimeError):
    """An invalid instrument session; callers must stop the whole tier, not retry."""


@dataclass(frozen=True)
class Process:
    pid: int
    start: int
    parent: int
    name: str
    ticks: int
    affinity: frozenset[int]
    experiment_driver: bool = False
    kernel_thread: bool = False

    @property
    def identity(self):
        return self.pid, self.start


def thread_affinity(entry: Path) -> frozenset[int]:
    affinity = set()
    for task in (entry / "task").iterdir():
        try:
            affinity.update(os.sched_getaffinity(int(task.name)))
        except ProcessLookupError:
            continue
    return frozenset(affinity)


def experiment_driver(argv: list[bytes]) -> bool:
    # Inspect the actual script argument, never a substring of shell/source text.
    # A foreign ABBA driver remains an active experiment between child boots.
    if not argv or not Path(os.fsdecode(argv[0])).name.startswith("python"):
        return False
    for argument in argv[1:]:
        if argument in (b"-c", b"-m"):
            return False
        if argument.startswith(b"-"):
            continue
        return Path(os.fsdecode(argument)).name in ("abbagate.py", "abba_experiments.py", "legacy_reorder_witness.py",
                                                    "background_qualification.py", "abba_saturation_controls.py")
    return False


def snapshot(proc_root=Path("/proc"), *, cmdline_reader=None) -> dict[int, Process]:
    result = {}
    read_cmdline = cmdline_reader or (lambda entry, start: (entry / "cmdline").read_bytes())
    for entry in proc_root.iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            raw = (entry / "stat").read_text()
            name, rest = raw.split("(", 1)[1].rsplit(")", 1)
            fields = rest.split()
            kernel = bool(int(fields[6]) & PF_KTHREAD)
            pid = int(entry.name)
            # Linux permits each thread to have its own affinity. The leader can
            # sleep on a reserved CPU while workers contend with the measured
            # server. Process stat ticks include those workers; its mask must too.
            affinity = thread_affinity(entry)
            if not affinity:
                continue  # The complete thread group exited while being read.
            result[pid] = Process(pid, int(fields[19]), int(fields[1]), name,
                                  int(fields[11]) + int(fields[12]),
                                  affinity, False if kernel else experiment_driver(
                                      read_cmdline(entry, int(fields[19])).split(b"\0")), kernel)
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


def controller_ancestors(rows: dict[int, Process], root: tuple[int, int]) -> set[tuple[int, int]]:
    current = rows.get(root[0])
    if current is None or current.identity != root:
        raise RuntimeError("quiet observer lost its owning process identity")
    result = set()
    while current.parent in rows:
        current = rows[current.parent]
        if current.identity == root or current.identity in result:
            raise RuntimeError("cycle in observer ownership ancestry")
        result.add(current.identity)
    return result


def read_watchdog(pid):
    entry = Path("/proc") / str(pid)
    argv = [os.fsdecode(value) for value in (entry / "cmdline").read_bytes().split(b"\0") if value]
    script = ((entry / "cwd").resolve() / argv[1]).resolve() if len(argv) > 1 else None
    # Read stat after argv/cwd so PID reuse cannot turn the earlier snapshot's identity into an
    # exemption for another process. The caller also checks the captured parent identity.
    fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
    return argv, script, (pid, int(fields[19])), int(fields[1])


def controller_watchdog(rows, ancestors, spec, *, reader=None):
    if not spec:
        return {}
    match = re.fullmatch(r"([1-9][0-9]*):([1-9][0-9]*)", spec)
    if not match:
        raise QuietViolation("invalid exact watchdog PID:start declaration")
    identity = tuple(map(int, match.groups()))
    row = rows.get(identity[0])
    if row is None or row.identity != identity:
        raise QuietViolation("declared ABBA watchdog exited or its PID identity changed")
    parent = rows.get(row.parent)
    if parent is None or parent.identity not in ancestors:
        raise QuietViolation("declared watchdog does not belong to a captured controller ancestor")
    try:
        argv, script, live_identity, live_parent = (reader or read_watchdog)(row.pid)
        expected_script = Path(__file__).resolve().with_name("gate_history.py")
        if (live_identity != identity or live_parent != parent.pid or len(argv) < 3 or
                not re.fullmatch(r"python[0-9.]*", Path(argv[0]).name) or
                script != expected_script or argv[2] != "watch" or len(argv[3:]) % 2):
            raise ValueError("watchdog executable/script/parent differs")
        options = dict(zip(argv[3::2], argv[4::2]))
        cancellation = {"--generation", "--cancel-request", "--cancel-receipt"}
        if (len(options) * 2 != len(argv[3:]) or
                set(options) - {"--pid", "--parent-start", "--seconds", "--marker", "--grace"} - cancellation or
                options.get("--pid") != str(parent.pid) or options.get("--parent-start") != str(parent.start) or
                "--seconds" not in options or "--marker" not in options):
            raise ValueError("watchdog arguments do not name its exact controller parent")
        if set(options) & cancellation:
            if not cancellation <= set(options) or not re.fullmatch(
                    rf"{parent.pid}\.{parent.start}\.[1-9][0-9]*", options["--generation"]):
                raise ValueError("watchdog cancellation generation does not name its exact parent")
            prefix = options["--marker"].removesuffix(".json") + "." + options["--generation"]
            if (options["--cancel-request"] != prefix + ".cancel" or
                    options["--cancel-receipt"] != prefix + ".cancelled"):
                raise ValueError("watchdog cancellation paths do not name this generation")
    except (OSError, ValueError, IndexError) as error:
        raise QuietViolation("cannot validate declared ABBA watchdog: " + str(error)) from error
    return {identity: {"pid": row.pid, "start_ticks": row.start, "parent_pid": parent.pid,
                       "parent_start_ticks": parent.start, "script": str(script), "argv": argv,
                       "cpu_ticks": 0, "classification": "exact declared row-watchdog housekeeping"}}


def interference(before: dict[int, Process], after: dict[int, Process],
                 root: tuple[int, int], cpus: set[int], excluded_ancestors=(), excluded_helpers=(),
                 *, kernel_only=False) -> list[dict]:
    owned = owned_processes(after, root) | set(excluded_ancestors) | set(excluded_helpers)
    offenders = []
    for row in after.values():
        if row.kernel_thread != kernel_only:
            continue
        prior = before.get(row.pid)
        same_process = prior and prior.identity == row.identity
        affinity = row.affinity | (prior.affinity if same_process else frozenset())
        if row.identity in owned or not cpus.intersection(affinity):
            continue
        # A process first observed during the session has no earlier baseline.
        # Its own accumulated ticks still witness work; treating absence as zero
        # delta let a newly started Python workload evade the first observation.
        delta = max(0, row.ticks - prior.ticks) if same_process else row.ticks
        active = not row.kernel_thread and (row.name in ACTIVE_EXPERIMENTS or row.experiment_driver)
        if active or delta:
            offenders.append({"pid": row.pid, "start_ticks": row.start, "comm": row.name,
                              "cpu_ticks": delta, "reason": "active foreign experiment" if active
                              else "kernel CPU activity (PF_KTHREAD)" if row.kernel_thread else "foreign CPU activity",
                              "kernel_thread": row.kernel_thread,
                              "overlapping_cpus": sorted(cpus.intersection(affinity))})
    return offenders


class QuietMonitor:
    """Latch the first interference; checking never retries a bad sample into green."""
    def _snapshot(self):
        # The normal observer always requires its original complete /proc view.
        # A permanently untrusted diagnostic subclass may supply its reviewed observation policy.
        return snapshot()

    def __init__(self, server_cpus, load_cpus, *, own_root_pid=None, interval=1.0,
                 window_seconds=20):
        requested = set(server_cpus) | set(load_cpus)
        if (not requested or not server_cpus or not math.isfinite(interval) or interval <= 0 or
                not math.isfinite(window_seconds) or window_seconds <= 0):
            raise ValueError("quiet monitor requires server CPUs and positive finite intervals")
        topology = read_topology(sorted(requested))
        # An unused SMT sibling still shares the measured physical core. Monitor
        # it even when the driver's cgroup cannot schedule a task on that sibling.
        self.cpus = set().union(*(topology[cpu] for cpu in requested))
        self.requested_cpus = requested
        self.server_physical_cores = len({topology[cpu] for cpu in server_cpus})
        self.core_of_cpu = {sibling: min(topology[cpu]) for cpu in requested
                            for sibling in topology[cpu]}
        self.window_seconds = window_seconds
        self.cpu_budget_seconds = GENERIC_CPU_FRACTION * self.server_physical_cores * window_seconds
        self.tick_seconds = 1 / os.sysconf("SC_CLK_TCK")
        self.previous = self._snapshot()
        self.previous_at = time.monotonic()
        self.started_monotonic = self.previous_at
        pid = os.getpid() if own_root_pid is None else own_root_pid
        self.root = self.previous[pid].identity
        # Log-reading/control wakes the invoking shell/Codex as well as this
        # driver. Exclude only their captured identities, never all descendants
        # of an ancestor: a sibling compiler/test is still competing CPU work.
        self.ancestors = controller_ancestors(self.previous, self.root)
        self.watchdog_spec = os.getenv("GATE_QUIET_WATCHDOG", "")
        self.helpers = controller_watchdog(self.previous, self.ancestors, self.watchdog_spec)
        self.excluded_activity = {identity: {"pid": identity[0], "start_ticks": identity[1],
            "comm": self.previous[identity[0]].name, "cpu_ticks": 0}
            for identity in self.ancestors}
        self.known_programs = {}
        self.foreign_activity = {}
        self.kernel_activity = {}
        self.activity_windows = deque()
        self.peak_rolling = None
        self.peak_core_concentration = None
        self.max_sample_interval = 0.0
        self.samples_longer_than_window = 0
        self.preflight_seconds = None
        self.interval = interval
        self.started = time.time()
        self.samples = 0
        self.failure = None
        self.stop_event = threading.Event()
        self.thread = None
        self.closed = False
        self.finished = None

    def sample(self):
        current = self._snapshot()
        now = time.monotonic()
        elapsed = now - self.previous_at
        if elapsed < 0:
            raise QuietViolation("quiet observer monotonic clock moved backwards")
        # Revalidate every sample, including the final one. An exited/reparented watcher or a
        # process execing another program cannot retain the original helper exemption.
        controller_watchdog(current, self.ancestors, self.watchdog_spec)
        activity = interference(self.previous, current, self.root, self.cpus, self.ancestors, self.helpers)
        kernel_activity = interference(self.previous, current, self.root, self.cpus,
                                       self.ancestors, self.helpers, kernel_only=True)
        offenders = [row for row in activity if row["reason"] == "active foreign experiment"]
        # Every foreign tick contributes to one aggregate ceiling. Charging the
        # complete overlapping process also avoids hiding workers behind a leader
        # pinned elsewhere; no load/whole-machine denominator dilutes a hot core.
        self.activity_windows.append((self.previous_at, now, activity))
        while self.activity_windows and self.activity_windows[0][1] <= now - self.window_seconds:
            self.activity_windows.popleft()
        self.max_sample_interval = max(self.max_sample_interval, elapsed)
        self.samples_longer_than_window += elapsed > self.window_seconds
        # Networking can run the measured server's own work in ksoftirqd/workqueues.
        # Retain those ticks separately; they are not evidence of a foreign USER
        # workload. Do not attribute them to this benchmark or claim they are free.
        for row in activity + kernel_activity:
            identity = row["pid"], row["start_ticks"]
            bucket = self.kernel_activity if row["kernel_thread"] else self.foreign_activity
            saved = bucket.setdefault(identity, {
                "pid": row["pid"], "start_ticks": row["start_ticks"], "comm": row["comm"],
                "cpu_ticks": 0, "first_observed_monotonic": now, "last_observed_monotonic": now,
                "possible_physical_cores": []})
            saved["cpu_ticks"] += row["cpu_ticks"]
            saved["last_observed_monotonic"] = now
            saved["possible_physical_cores"] = sorted(set(saved["possible_physical_cores"]) |
                {self.core_of_cpu[cpu] for cpu in row["overlapping_cpus"]})
        rolling = [row for _, _, rows in self.activity_windows for row in rows]
        ticks = sum(row["cpu_ticks"] for row in rolling)
        possible_core_ticks = {}
        for row in rolling:
            # Affinity is permission, not actual placement. Count the FULL delta
            # against every possible physical core to report a concentration upper
            # bound; never attribute interval runtime to /proc's last-CPU field.
            for core in {self.core_of_cpu[cpu] for cpu in row["overlapping_cpus"]}:
                possible_core_ticks[core] = possible_core_ticks.get(core, 0) + row["cpu_ticks"]
        peak_core_ticks = max(possible_core_ticks.values(), default=0)
        if (self.peak_core_concentration is None or
                peak_core_ticks * self.tick_seconds > self.peak_core_concentration["cpu_seconds"]):
            self.peak_core_concentration = {"cpu_seconds": peak_core_ticks * self.tick_seconds,
                "ended_monotonic": now, "possible_physical_cores": sorted(
                    core for core, count in possible_core_ticks.items() if count == peak_core_ticks)}
        if self.peak_rolling is None or ticks > self.peak_rolling["cpu_ticks"]:
            self.peak_rolling = {"cpu_ticks": ticks, "cpu_seconds": ticks * self.tick_seconds,
                "ended_monotonic": now, "oldest_sample_started_monotonic": self.activity_windows[0][0],
                "busiest_possible_core_cpu_seconds": peak_core_ticks * self.tick_seconds,
                "busiest_possible_physical_cores": sorted(core for core, count in possible_core_ticks.items()
                                                         if count == peak_core_ticks)}
        # Retain the whole oldest sample when it partially overlaps the window.
        # The ceiling also applies to a single sample, even if sampling was delayed
        # for longer than WINDOW: an observation gap cannot purchase extra budget.
        sample_ticks = sum(row["cpu_ticks"] for row in activity)
        if max(ticks, sample_ticks) * self.tick_seconds > self.cpu_budget_seconds:
            offenders = rolling
        owned = owned_processes(current, self.root) | self.ancestors | self.helpers.keys()
        for row in current.values():
            prior = self.previous.get(row.pid)
            if row.identity in self.ancestors and prior and prior.identity == row.identity:
                self.excluded_activity[row.identity]["cpu_ticks"] += max(0, row.ticks - prior.ticks)
            if row.identity in self.helpers and prior and prior.identity == row.identity:
                self.helpers[row.identity]["cpu_ticks"] += max(0, row.ticks - prior.ticks)
            if row.kernel_thread or row.identity in owned or not self.cpus.intersection(row.affinity):
                continue
            if row.name in COMPETING or row.experiment_driver:
                self.known_programs[row.identity] = {"pid": row.pid, "start_ticks": row.start,
                    "comm": row.name, "observed_at": time.time(), "total_cpu_ticks": row.ticks,
                    "classification": "active experiment" if row.name in ACTIVE_EXPERIMENTS or
                    row.experiment_driver else "server presence; activity checked separately"}
        self.previous = current
        self.previous_at = now
        self.samples += 1
        if offenders and self.failure is None:
            self.failure = {"observed_at": time.time(), "processes": offenders,
                "rolling_cpu_seconds": ticks * self.tick_seconds,
                "sample_cpu_seconds": sample_ticks * self.tick_seconds,
                "cpu_budget_seconds": self.cpu_budget_seconds}

    def evidence(self):
        return {"started_at": self.started, "sample_interval_seconds": self.interval,
                "finished_at": self.finished, "complete": self.closed and self.failure is None,
                "samples": self.samples, "tick_seconds": self.tick_seconds,
                "cpus": sorted(self.cpus), "requested_cpus": sorted(self.requested_cpus),
                "interference": self.failure,
                "known_programs": list(self.known_programs.values()),
                "foreign_cpu_activity": list(self.foreign_activity.values()),
                "kernel_cpu_activity": list(self.kernel_activity.values()),
                "generic_cpu_screening": {"server_physical_cores": self.server_physical_cores,
                    "capacity_fraction": GENERIC_CPU_FRACTION, "window_seconds": self.window_seconds,
                    "cpu_budget_seconds": self.cpu_budget_seconds, "peak_rolling": self.peak_rolling,
                    "peak_possible_core_concentration": self.peak_core_concentration,
                    "max_sample_interval_seconds": self.max_sample_interval,
                    "samples_longer_than_window": self.samples_longer_than_window,
                    "preflight_seconds": self.preflight_seconds,
                    "accounting": "process CPU ticks; partial oldest samples charged in full",
                    "kernel_classification": "PF_KTHREAD in /proc/PID/stat field9; excluded only from foreign-user CPU budget",
                    "limitation": "screening only; CPU fraction does not bound cache or tail effects"},
                "excluded_controller_ancestors": list(self.excluded_activity.values()),
                "excluded_controller_helpers": list(self.helpers.values()),
                "limitation": "processes born and reaped between samples may be missed"}

    def check(self):
        if self.failure:
            details = self.failure.get("processes", [])
            reason = "; ".join(f"PID {p['pid']} ({p['comm']}): {p['reason']}, "
                               f"{p['cpu_ticks']} CPU ticks" for p in details)
            if self.failure.get("rolling_cpu_seconds", 0) > self.cpu_budget_seconds:
                reason = (f"foreign CPU screening budget exceeded: "
                          f"{self.failure['rolling_cpu_seconds']:.6f}s > {self.cpu_budget_seconds:.6f}s "
                          f"per {self.window_seconds:g}s on {self.server_physical_cores} physical server cores; " + reason)
            raise QuietViolation("QUIET-BOX PRECONDITION FAILED: " + (reason or self.failure["error"]))

    def preflight(self):
        self.sample()  # Refuse known competing programs without waiting first.
        self.check()
        # Observe a full measurement window before any boot. Otherwise generic
        # sustained work could borrow an unobserved past and pass a short preflight.
        deadline = self.started_monotonic + self.window_seconds
        while self.previous_at < deadline:
            time.sleep(min(self.interval, deadline - self.previous_at))
            self.sample()
            self.check()
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
        except Exception as exc:
            # Unknown observer state also invalidates the instrument; a dead
            # monitor thread must not silently certify the remaining measurements.
            self.failure = self.failure or {"observed_at": time.time(), "error": str(exc)}

    def close(self):
        if self.closed:
            return self.evidence()
        self.stop_event.set()
        if self.thread is not None:
            self.thread.join()
        self.observe()
        self.closed = True
        self.finished = time.time()
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

        def test_one_foreign_tick_is_recorded(self):
            from dataclasses import replace
            bad = {**self.before, 20: replace(self.other, ticks=101)}
            self.assertEqual(self.check_rows(bad)[0]["pid"], 20)

        def test_foreign_workload_refused_even_while_sleeping(self):
            from dataclasses import replace
            bad = {**self.before, 20: replace(self.other, name="cc1plus")}
            self.assertEqual(self.check_rows(bad)[0]["cpu_ticks"], 0)

        def test_idle_unrelated_server_is_recorded_and_only_activity_refuses(self):
            from dataclasses import replace
            idle = {**self.before, 20: replace(self.other, name="memcached")}
            active = {**idle, 20: replace(idle[20], ticks=101)}
            with mock.patch(__name__ + ".snapshot", side_effect=[idle, idle, active]):
                monitor = QuietMonitor([0], [1], own_root_pid=10, window_seconds=1)
                monitor.sample()
                monitor.check()
                self.assertEqual(monitor.evidence()["known_programs"][0]["comm"], "memcached")
                self.assertIsNone(monitor.evidence()["interference"])
                monitor.sample()
                with self.assertRaisesRegex(QuietViolation, "PID 20.*1 CPU ticks"):
                    monitor.check()

        def test_only_exact_controller_ancestors_are_excluded_not_their_siblings(self):
            from dataclasses import replace
            controller = Process(1, 77, 0, "codex", 5, frozenset((0,)))
            before = {**self.before, 1: controller}
            after = {**before, 1: replace(controller, ticks=6)}
            sibling = Process(40, 88, 1, "cc1plus", 0, frozenset((0,)))
            with mock.patch(__name__ + ".snapshot", side_effect=[before, after, {**after, 40: sibling}]):
                monitor = QuietMonitor([0], [1], own_root_pid=10)
                monitor.sample()
                monitor.check()
                self.assertEqual(monitor.evidence()["excluded_controller_ancestors"], [
                    {"pid": 1, "start_ticks": 77, "comm": "codex", "cpu_ticks": 1}])
                monitor.sample()
                with self.assertRaisesRegex(QuietViolation, "PID 40"):
                    monitor.check()

        def test_python_driver_detection_uses_script_argument_not_source_substrings(self):
            self.assertTrue(experiment_driver([b"python3", b"-u", b"/work/tests/abbagate.py", b"--only", b"h01"]))
            self.assertTrue(experiment_driver([b"python3", b"/work/tests/legacy_reorder_witness.py"]))
            self.assertFalse(experiment_driver([b"python3", b"-c", b"source mentions /work/tests/abbagate.py"]))
            self.assertFalse(experiment_driver([b"bash", b"-c", b"cat tests/abbagate.py"]))
            from dataclasses import replace
            sleeping = {**self.before, 20: replace(self.other, experiment_driver=True)}
            self.assertEqual(self.check_rows(sleeping)[0]["reason"], "active foreign experiment")

        def watchdog_fixture(self):
            controller = Process(1, 77, 0, "bash", 5, frozenset((0,)))
            watcher = Process(30, 42, 1, "python3", 0, frozenset((0,)))
            rows = {**self.before, 1: controller, 30: watcher}
            script = Path(__file__).resolve().with_name("gate_history.py")
            argv = ["python3", "tests/gate_history.py", "watch", "--pid", "1", "--parent-start", "77",
                    "--seconds", "30", "--marker", "/tmp/test-row-marker"]
            return rows, (argv, script, watcher.identity, controller.pid)

        def test_exact_declared_watcher_is_housekeeping_but_compiler_and_server_siblings_fail(self):
            from dataclasses import replace
            before, metadata = self.watchdog_fixture()
            before[50] = Process(50, 99, 1, "tomokv", 0, frozenset((0,)))
            after = {**before, 30: replace(before[30], ticks=2)}
            busy = {**after, 50: replace(before[50], ticks=1),
                    40: Process(40, 88, 1, "cc1plus", 0, frozenset((0,)))}
            with mock.patch.dict(os.environ, {"GATE_QUIET_WATCHDOG": "30:42"}), \
                 mock.patch(__name__ + ".read_watchdog", return_value=metadata), \
                 mock.patch(__name__ + ".snapshot", side_effect=[before, after, busy]):
                monitor = QuietMonitor([0], [1], own_root_pid=10, window_seconds=1)
                monitor.sample()
                monitor.check()
                helper = monitor.evidence()["excluded_controller_helpers"][0]
                self.assertEqual((helper["pid"], helper["start_ticks"], helper["cpu_ticks"]), (30, 42, 2))
                self.assertEqual((helper["parent_pid"], helper["parent_start_ticks"]), (1, 77))
                self.assertEqual(helper["argv"], metadata[0])
                monitor.sample()
                with self.assertRaises(QuietViolation):
                    monitor.check()
                self.assertEqual({row["pid"] for row in monitor.failure["processes"]}, {40, 50})
            # Without the explicit declaration the identical sibling Python workload is foreign.
            self.assertEqual(interference(before, after, self.root.identity, {0}, {(1, 77)})[0]["pid"], 30)

        def test_cancellation_options_are_bound_to_exact_watch_generation(self):
            rows, metadata = self.watchdog_fixture()
            argv, script, identity, parent = metadata
            extra = ["--generation", "1.77.2", "--cancel-request", "/tmp/test-row-marker.1.77.2.cancel",
                     "--cancel-receipt", "/tmp/test-row-marker.1.77.2.cancelled"]
            reader = lambda _: (argv + extra, script, identity, parent)
            self.assertEqual(len(controller_watchdog(rows, {(1, 77)}, "30:42", reader=reader)), 1)
            for bad in (extra[:2], [*extra[:1], "1.99.2", *extra[2:]],
                        [*extra[:-1], "/tmp/another-generation.cancelled"]):
                with self.subTest(options=bad), self.assertRaises(QuietViolation):
                    controller_watchdog(rows, {(1, 77)}, "30:42",
                                        reader=lambda _: (argv + bad, script, identity, parent))

        def test_watchdog_identity_parent_and_script_cannot_be_spoofed(self):
            from dataclasses import replace
            rows, metadata = self.watchdog_fixture()
            argv, script, identity, parent = metadata
            cases = [
                (rows, "30:43", metadata),
                ({**rows, 1: replace(rows[1], start=78)}, "30:42", metadata),
                (rows, "30:42", (argv, script, (30, 43), parent)),
                (rows, "30:42", (argv, script.with_name("abbagate.py"), identity, parent)),
                (rows, "30:42", ([*argv[:4], "2", *argv[5:]], script, identity, parent)),
                (rows, "30:42", ([*argv, "--pid", "1"], script, identity, parent)),
            ]
            for snapshot_rows, spec, data in cases:
                with self.subTest(spec=spec, argv=data[0]), self.assertRaises(QuietViolation):
                    controller_watchdog(snapshot_rows, {(1, 77)}, spec, reader=lambda pid: data)

        def test_watchdog_revalidation_latches_exec_or_exit_during_session(self):
            rows, metadata = self.watchdog_fixture()
            bad = (["python3", "tests/abbagate.py"], metadata[1].with_name("abbagate.py"), metadata[2], metadata[3])
            with mock.patch.dict(os.environ, {"GATE_QUIET_WATCHDOG": "30:42"}), \
                 mock.patch(__name__ + ".read_watchdog", side_effect=[metadata, bad]), \
                 mock.patch(__name__ + ".snapshot", side_effect=[rows, rows]):
                monitor = QuietMonitor([0], [1], own_root_pid=10)
                monitor.observe()
                with self.assertRaisesRegex(QuietViolation, "cannot validate declared ABBA watchdog"):
                    monitor.check()

        def test_actual_gate_watchdog_token_and_proc_argv_are_accepted(self):
            import subprocess
            import tempfile
            root = Path(__file__).resolve().parents[1]
            (root / "build").mkdir(exist_ok=True)
            gate = (root / "tests/gate.sh").read_text()
            watch = gate[gate.index("row_clock(){"):gate.index("\nrow_unwatch(){")]
            token = gate[gate.index("ABBA_WATCH_START=missing\n"):gate.index('GATE_QUIET_WATCHDOG="$ROW_WATCHDOG:$ABBA_WATCH_START"')]
            with tempfile.TemporaryDirectory(prefix="quiet-watchdog-", dir=root / "build") as temporary:
                # Run the real row watcher and the gate's actual token-producing shell code.
                # The child only validates /proc metadata; no server, workload, or quiet sampling
                # of the contended box is started. Cleanup addresses exactly the watcher we own.
                script = '''set -eu
ROW_TIMEOUT=30; ROW_PAUSED=0; ROW_MARKER=$TEST_MARKER
''' + watch + '''
row_clock; ROW_START=$ROW_NOW
row_watch
trap 'kill -TERM "$ROW_WATCHDOG" 2>/dev/null || :; wait "$ROW_WATCHDOG" 2>/dev/null || :' EXIT
''' + token + '''
GATE_QUIET_WATCHDOG="$ROW_WATCHDOG:$ABBA_WATCH_START" python3 - <<'PY'
import os,sys
sys.path.insert(0, 'tests')
from gate_quiet import snapshot,controller_ancestors,controller_watchdog
rows=snapshot(); root=rows[os.getpid()].identity
helpers=controller_watchdog(rows, controller_ancestors(rows, root), os.environ['GATE_QUIET_WATCHDOG'])
assert len(helpers)==1
print('real gate watchdog metadata accepted')
PY
'''
                result = subprocess.run(["bash"], cwd=root, input=script, text=True, capture_output=True,
                    timeout=10, env={**os.environ, "TEST_MARKER": str(Path(temporary) / "marker.json")})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("real gate watchdog metadata accepted", result.stdout)

        def test_pid_reuse_does_not_inherit_ticks_or_ownership(self):
            from dataclasses import replace
            bad = {**self.before, 11: replace(self.own, start=20, parent=1)}
            self.assertEqual(self.check_rows(bad)[0]["pid"], 11)
            reused = {**self.before, 20: replace(self.other, start=30, ticks=200)}
            self.assertEqual(self.check_rows(reused)[0]["cpu_ticks"], 200)

        def test_new_generic_process_counts_its_own_cpu_time(self):
            new = Process(30, 40, 1, "python3", 1, frozenset((0,)))
            bad = {**self.before, new.pid: new}
            self.assertEqual(self.check_rows(bad)[0]["cpu_ticks"], 1)

        def test_worker_affinity_is_not_hidden_by_its_leader(self):
            import tempfile
            with tempfile.TemporaryDirectory() as tmp:
                entry = Path(tmp)
                (entry / "task/100").mkdir(parents=True)
                (entry / "task/101").mkdir()
                with mock.patch.object(os, "sched_getaffinity", side_effect=lambda pid: {8} if pid == 100 else {0}):
                    self.assertEqual(thread_affinity(entry), frozenset((0, 8)))

        def test_unreadable_executable_does_not_hide_other_users_cpu_activity(self):
            import tempfile
            with tempfile.TemporaryDirectory() as tmp:
                proc = Path(tmp)
                entry = proc / "100"
                (entry / "task/100").mkdir(parents=True)
                (entry / "task/101").mkdir()
                fields = ["0"] * 20
                fields[0], fields[1], fields[11], fields[12], fields[19] = "S", "1", "7", "3", "123"
                (entry / "stat").write_text("100 (foreign service) " + " ".join(fields))
                (entry / "cmdline").write_bytes(b"service\0")
                with mock.patch.object(Path, "readlink", side_effect=PermissionError("hidden executable")) as exe, \
                     mock.patch.object(os, "sched_getaffinity", side_effect=lambda pid: {8} if pid == 100 else {0}):
                    rows = snapshot(proc)
                exe.assert_not_called()  # No permission-based exemption exists.
                self.assertEqual(rows[100].ticks, 10)
                self.assertEqual(rows[100].affinity, frozenset((0, 8)))
                self.assertFalse(rows[100].kernel_thread)

        def test_kernel_flag_uses_actual_stat_field_not_comm_or_exe_permissions(self):
            import tempfile
            with tempfile.TemporaryDirectory() as tmp:
                proc = Path(tmp)
                # 0x4208040 was read from this box's /proc/31/stat (ksoftirqd/2).
                # Its other flags must not confuse the PF_KTHREAD mask. Deliberate
                # misleading comm values prove that names do not classify a task.
                for pid, flags, name in ((100, 0x4208040, "editor"), (101, 0x400040, "ksoftirqd/2")):
                    entry = proc / str(pid)
                    (entry / "task" / str(pid)).mkdir(parents=True)
                    fields = ["0"] * 20
                    fields[0], fields[1], fields[6] = "S", "1", str(flags)
                    fields[11], fields[12], fields[19] = "7", "3", "123"
                    (entry / "stat").write_text(f"{pid} ({name}) " + " ".join(fields))
                    if pid == 101:
                        (entry / "cmdline").write_bytes(b"user-service\0")
                with mock.patch.object(Path, "readlink", side_effect=PermissionError("hidden executable")), \
                     mock.patch.object(os, "sched_getaffinity", return_value={0}):
                    rows = snapshot(proc)
                self.assertTrue(rows[100].kernel_thread)
                self.assertFalse(rows[101].kernel_thread)

        def test_kernel_cpu_is_recorded_without_spending_foreign_user_budget(self):
            from dataclasses import replace
            monitor = self.budget_fixture()
            kernel = Process(100, 55, 2, "ksoftirqd/2", 1000, frozenset((0,)), kernel_thread=True)
            rows = {**self.before, 20: replace(self.other, ticks=102), 100: kernel}
            self.budget_sample(monitor, 1, rows)
            monitor.check()
            evidence = monitor.evidence()
            self.assertEqual(evidence["kernel_cpu_activity"][0]["cpu_ticks"], 1000)
            self.assertEqual(sum(row["cpu_ticks"] for row in evidence["foreign_cpu_activity"]), 2)
            self.assertAlmostEqual(evidence["generic_cpu_screening"]["peak_rolling"]["cpu_seconds"], .02)
            self.budget_sample(monitor, 2, {**rows, 20: replace(self.other, ticks=202, name="kworker/0:1")})
            with self.assertRaisesRegex(QuietViolation, "foreign CPU screening budget exceeded"):
                monitor.check()  # A kernel-looking user comm is still foreign work.

        def test_reserved_smt_sibling_is_still_monitored(self):
            topology = {0: frozenset((0, 128)), 1: frozenset((1, 129))}
            with mock.patch(__name__ + ".read_topology", return_value=topology), \
                 mock.patch(__name__ + ".snapshot", return_value=self.before):
                monitor = QuietMonitor([0], [1], own_root_pid=10)
            self.assertEqual(monitor.cpus, {0, 1, 128, 129})

        def test_affinity_change_does_not_erase_previous_overlap(self):
            from dataclasses import replace
            after = {**self.before, 20: replace(self.other, ticks=101, affinity=frozenset((2,)))}
            self.assertEqual(self.check_rows(after)[0]["overlapping_cpus"], [0])

        def test_nonoverlapping_activity_is_irrelevant(self):
            from dataclasses import replace
            after = {**self.before, 20: replace(self.other, name="make", affinity=frozenset((2,)))}
            self.assertEqual(interference(after, after, self.root.identity, {0}), [])

        def test_interference_is_latched(self):
            from dataclasses import replace
            bad = {**self.before, 20: replace(self.other, ticks=101)}
            with mock.patch(__name__ + ".snapshot", side_effect=[self.before, bad, bad]):
                monitor = QuietMonitor([0], [1], own_root_pid=10, window_seconds=1)
                monitor.sample()
                monitor.sample()
                with self.assertRaisesRegex(RuntimeError, "PID 20"):
                    monitor.check()

        def budget_fixture(self, server_count=32, window=20):
            topology = {cpu: frozenset((cpu % 128, cpu % 128 + 128)) for cpu in range(256)}
            with mock.patch(__name__ + ".read_topology", return_value=topology), \
                 mock.patch(__name__ + ".snapshot", return_value=self.before), \
                 mock.patch.object(time, "monotonic", return_value=0):
                return QuietMonitor(list(range(server_count)), list(range(server_count, 256)),
                                    own_root_pid=10, window_seconds=window)

        def budget_sample(self, monitor, second, rows):
            with mock.patch(__name__ + ".snapshot", return_value=rows), \
                 mock.patch.object(time, "monotonic", return_value=second):
                monitor.sample()

        def test_background_ticks_pass_and_are_recorded_with_concentration_bound(self):
            from dataclasses import replace
            monitor = self.budget_fixture(window=1)
            self.budget_sample(monitor, 1, {**self.before, 20: replace(self.other, ticks=102)})
            monitor.check()
            evidence = monitor.evidence()
            self.assertEqual(evidence["foreign_cpu_activity"][0]["cpu_ticks"], 2)
            budget = evidence["generic_cpu_screening"]
            self.assertEqual(budget["server_physical_cores"], 32)
            self.assertAlmostEqual(budget["cpu_budget_seconds"], .048)
            self.assertAlmostEqual(budget["peak_rolling"]["busiest_possible_core_cpu_seconds"], .02)
            small = self.budget_fixture(server_count=1, window=1)
            self.budget_sample(small, 1, {**self.before, 20: replace(self.other, ticks=102)})
            with self.assertRaisesRegex(QuietViolation, "1 physical server cores"):
                small.check()

        def test_hot_core_and_many_small_tasks_cannot_hide_in_256_cpu_geometry(self):
            from dataclasses import replace
            hot = self.budget_fixture()
            self.budget_sample(hot, 1, {**self.before, 20: replace(self.other, ticks=200)})
            with self.assertRaisesRegex(QuietViolation, "budget exceeded"):
                hot.check()
            many = self.budget_fixture()
            rows = {**self.before, **{pid: Process(pid, pid, 1, "worker", 1, frozenset((0,)))
                                     for pid in range(100, 200)}}
            self.budget_sample(many, 1, rows)
            with self.assertRaises(QuietViolation):
                many.check()
            self.assertEqual(sum(row["cpu_ticks"] for row in many.evidence()["foreign_cpu_activity"]), 100)

        def test_sleeping_compiler_still_refused_without_spending_cpu_budget(self):
            from dataclasses import replace
            monitor = self.budget_fixture()
            self.budget_sample(monitor, 0, {**self.before, 20: replace(self.other, name="cc1plus")})
            with self.assertRaisesRegex(QuietViolation, "active foreign experiment"):
                monitor.check()

        def test_rolling_edge_and_long_sampling_gap_charge_complete_sample(self):
            from dataclasses import replace
            monitor = self.budget_fixture()
            self.budget_sample(monitor, 19, {**self.before, 20: replace(self.other, ticks=190)})
            monitor.check()
            self.budget_sample(monitor, 21, {**self.before, 20: replace(self.other, ticks=200)})
            with self.assertRaises(QuietViolation):
                monitor.check()  # Full 0..19 sample overlaps the 1..21 window.
            gap = self.budget_fixture()
            self.budget_sample(gap, 100, {**self.before, 20: replace(self.other, ticks=200)})
            with self.assertRaises(QuietViolation):
                gap.check()  # 100 seconds of silence cannot buy 4.8 CPU seconds.
            self.assertEqual(gap.evidence()["generic_cpu_screening"]["samples_longer_than_window"], 1)

        def test_preflight_observes_full_window_without_real_sleep(self):
            monitor = self.budget_fixture()
            clock = [0.0]
            def sleep(seconds):
                clock[0] += seconds
            with mock.patch(__name__ + ".snapshot", return_value=self.before), \
                 mock.patch.object(time, "monotonic", side_effect=lambda: clock[0]), \
                 mock.patch.object(time, "sleep", side_effect=sleep):
                evidence = monitor.preflight()
            self.assertEqual(clock[0], 20)
            self.assertEqual(evidence["generic_cpu_screening"]["preflight_seconds"], 20)

        def test_observer_error_latches_and_close_is_idempotent(self):
            with mock.patch(__name__ + ".snapshot", side_effect=[self.before, RuntimeError("lost /proc access")]):
                monitor = QuietMonitor([0], [1], own_root_pid=10)
                monitor.close()
                self.assertEqual(monitor.close()["interference"]["error"], "lost /proc access")
                with self.assertRaisesRegex(QuietViolation, "lost /proc access"):
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
