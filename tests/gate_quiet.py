#!/usr/bin/env python3
"""Refuse foreign work before and during a performance measurement.

This observer never stops a process. /proc identities, not argv patterns, distinguish
the driver's children from foreign work. Ordinary sleeping services are harmless;
known compilers, load generators and ABBA drivers are competing experiments even
while temporarily asleep between phases. An idle unrelated server is recorded,
then refused if traffic produces CPU activity. Other processes are reported if their sampled
CPU time advances by at least one accounting tick on an overlapping affinity mask.
The gate's declared row watchdog is controller housekeeping only after its exact
PID/start identity, script and captured controller parent are independently verified.
That is an interference witness, not a chosen regression tolerance or noise floor.
The sample cannot see a process born and reaped entirely between observations; the
exclusive-box rule remains necessary, and the evidence records this limitation.
"""
from __future__ import annotations

from dataclasses import dataclass
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
SERVERS = frozenset(("redis-server", "tomokv", "dragonfly", "keydb-server", "memcached"))
COMPETING = ACTIVE_EXPERIMENTS | SERVERS


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
        return Path(os.fsdecode(argument)).name in ("abbagate.py", "abba_experiments.py", "legacy_reorder_witness.py")
    return False


def snapshot(proc_root=Path("/proc")) -> dict[int, Process]:
    result = {}
    for entry in proc_root.iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            raw = (entry / "stat").read_text()
            name, rest = raw.split("(", 1)[1].rsplit(")", 1)
            fields = rest.split()
            # Kernel threads have no executable and cannot be a user workload. Do
            # not suppress a process merely because it belongs to another user.
            try:
                (entry / "exe").readlink()
            except PermissionError:
                # Other users' executable links are commonly hidden even though
                # stat and affinity are readable (including PID 1 on this box).
                # Keep such processes in the observer as user workloads. Missing
                # links still identify kernel threads through FileNotFoundError.
                pass
            pid = int(entry.name)
            # Linux permits each thread to have its own affinity. The leader can
            # sleep on a reserved CPU while workers contend with the measured
            # server. Process stat ticks include those workers; its mask must too.
            affinity = thread_affinity(entry)
            if not affinity:
                continue  # The complete thread group exited while being read.
            result[pid] = Process(pid, int(fields[19]), int(fields[1]), name,
                                  int(fields[11]) + int(fields[12]),
                                  affinity, experiment_driver((entry / "cmdline").read_bytes().split(b"\0")))
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
        if (len(options) * 2 != len(argv[3:]) or
                set(options) - {"--pid", "--parent-start", "--seconds", "--marker", "--grace"} or
                options.get("--pid") != str(parent.pid) or options.get("--parent-start") != str(parent.start) or
                "--seconds" not in options or "--marker" not in options):
            raise ValueError("watchdog arguments do not name its exact controller parent")
    except (OSError, ValueError, IndexError) as error:
        raise QuietViolation("cannot validate declared ABBA watchdog: " + str(error)) from error
    return {identity: {"pid": row.pid, "start_ticks": row.start, "parent_pid": parent.pid,
                       "parent_start_ticks": parent.start, "script": str(script), "argv": argv,
                       "cpu_ticks": 0, "classification": "exact declared row-watchdog housekeeping"}}


def interference(before: dict[int, Process], after: dict[int, Process],
                 root: tuple[int, int], cpus: set[int], excluded_ancestors=(), excluded_helpers=()) -> list[dict]:
    owned = owned_processes(after, root) | set(excluded_ancestors) | set(excluded_helpers)
    offenders = []
    for row in after.values():
        prior = before.get(row.pid)
        same_process = prior and prior.identity == row.identity
        affinity = row.affinity | (prior.affinity if same_process else frozenset())
        if row.identity in owned or not cpus.intersection(affinity):
            continue
        # A process first observed during the session has no earlier baseline.
        # Its own accumulated ticks still witness work; treating absence as zero
        # delta let a newly started Python workload evade the first observation.
        delta = max(0, row.ticks - prior.ticks) if same_process else row.ticks
        active = row.name in ACTIVE_EXPERIMENTS or row.experiment_driver
        if active or delta:
            offenders.append({"pid": row.pid, "start_ticks": row.start, "comm": row.name,
                              "cpu_ticks": delta, "reason": "active foreign experiment" if active
                              else "foreign CPU activity",
                              "overlapping_cpus": sorted(cpus.intersection(affinity))})
    return offenders


class QuietMonitor:
    """Latch the first interference; checking never retries a bad sample into green."""
    def __init__(self, server_cpus, load_cpus, *, own_root_pid=None, interval=1.0):
        requested = set(server_cpus) | set(load_cpus)
        if not requested or not math.isfinite(interval) or interval <= 0:
            raise ValueError("quiet monitor requires CPUs and a positive finite sample interval")
        topology = read_topology(sorted(requested))
        # An unused SMT sibling still shares the measured physical core. Monitor
        # it even when the driver's cgroup cannot schedule a task on that sibling.
        self.cpus = set().union(*(topology[cpu] for cpu in requested))
        self.requested_cpus = requested
        self.previous = snapshot()
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
        self.interval = interval
        self.started = time.time()
        self.samples = 0
        self.failure = None
        self.stop_event = threading.Event()
        self.thread = None
        self.closed = False
        self.finished = None

    def sample(self):
        current = snapshot()
        # Revalidate every sample, including the final one. An exited/reparented watcher or a
        # process execing another program cannot retain the original helper exemption.
        controller_watchdog(current, self.ancestors, self.watchdog_spec)
        offenders = interference(self.previous, current, self.root, self.cpus, self.ancestors, self.helpers)
        owned = owned_processes(current, self.root) | self.ancestors | self.helpers.keys()
        for row in current.values():
            prior = self.previous.get(row.pid)
            if row.identity in self.ancestors and prior and prior.identity == row.identity:
                self.excluded_activity[row.identity]["cpu_ticks"] += max(0, row.ticks - prior.ticks)
            if row.identity in self.helpers and prior and prior.identity == row.identity:
                self.helpers[row.identity]["cpu_ticks"] += max(0, row.ticks - prior.ticks)
            if row.identity in owned or not self.cpus.intersection(row.affinity):
                continue
            if row.name in COMPETING or row.experiment_driver:
                self.known_programs[row.identity] = {"pid": row.pid, "start_ticks": row.start,
                    "comm": row.name, "observed_at": time.time(), "total_cpu_ticks": row.ticks,
                    "classification": "active experiment" if row.name in ACTIVE_EXPERIMENTS or
                    row.experiment_driver else "server presence; activity checked separately"}
        self.previous = current
        self.samples += 1
        if offenders and self.failure is None:
            self.failure = {"observed_at": time.time(), "processes": offenders}

    def evidence(self):
        return {"started_at": self.started, "sample_interval_seconds": self.interval,
                "finished_at": self.finished, "complete": self.closed and self.failure is None,
                "samples": self.samples, "tick_seconds": 1 / os.sysconf("SC_CLK_TCK"),
                "cpus": sorted(self.cpus), "requested_cpus": sorted(self.requested_cpus),
                "interference": self.failure,
                "known_programs": list(self.known_programs.values()),
                "excluded_controller_ancestors": list(self.excluded_activity.values()),
                "excluded_controller_helpers": list(self.helpers.values()),
                "limitation": "processes born and reaped between samples may be missed"}

    def check(self):
        if self.failure:
            details = self.failure.get("processes", [])
            reason = "; ".join(f"PID {p['pid']} ({p['comm']}): {p['reason']}, "
                               f"{p['cpu_ticks']} CPU ticks" for p in details)
            raise QuietViolation("QUIET-BOX PRECONDITION FAILED: " + (reason or self.failure["error"]))

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

        def test_one_foreign_tick_is_contamination(self):
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
                monitor = QuietMonitor([0], [1], own_root_pid=10)
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
                monitor = QuietMonitor([0], [1], own_root_pid=10)
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
                with mock.patch.object(Path, "readlink", side_effect=PermissionError("hidden executable")), \
                     mock.patch.object(os, "sched_getaffinity", side_effect=lambda pid: {8} if pid == 100 else {0}):
                    rows = snapshot(proc)
                self.assertEqual(rows[100].ticks, 10)
                self.assertEqual(rows[100].affinity, frozenset((0, 8)))

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
                monitor = QuietMonitor([0], [1], own_root_pid=10)
                monitor.sample()
                monitor.sample()
                with self.assertRaisesRegex(RuntimeError, "PID 20"):
                    monitor.check()

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
