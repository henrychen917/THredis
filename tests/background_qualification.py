#!/usr/bin/env python3
"""On-demand, permanently untrusted qualification of an explicitly reviewed background.

First capture an inventory, review exact identities, and mark selected records reviewed=true
with classification desktop / interactive-frontend / waiting-supervisor. No name is approved
by default, unreadable identities stay unknown, and approval never includes descendants.
`run` captures 120 seconds before any boot, executes exactly one pinned ABBA block each for
h01/h09/h12 through abbagate.main(), then captures 60 seconds after the last measurement.
The normal generic CPU budget is only audited here; its crossings remain would-refuse events.
Known competing experiments, foreign server work, changed/unreviewed active identities and
observer errors still abort. No rate threshold, load pin or normal gate policy is relaxed.
Even twelve successful measurements cannot certify this operating environment: a separate
all-cell standing null is still required. These artifacts can never serve as that null.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import threading
import time

import abbagate as abba
import gate_quiet as quiet

CELLS = ("h01", "h09", "h12")
CLASSES = {"desktop", "interactive-frontend", "waiting-supervisor"}
PREFLIGHT = 120
POSTFLIGHT = 60
_DIGESTS = {}


def file_identity(path):
    before = path.stat()
    fields = {"device": before.st_dev, "inode": before.st_ino, "size": before.st_size,
              "mtime_ns": before.st_mtime_ns, "ctime_ns": before.st_ctime_ns}
    key = tuple(fields.values())
    if key not in _DIGESTS:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
            after = os.fstat(stream.fileno())
        if (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) != key:
            raise quiet.QuietViolation(f"identity file changed while hashing: {path}")
        _DIGESTS[key] = digest.hexdigest()
    return {**fields, "sha256": _DIGESTS[key]}


def script_identity(entry, executable, argv):
    name = Path(executable).name
    python = re.fullmatch(r"python[0-9.]*", name)
    shell = name in ("bash", "sh", "dash", "zsh", "ksh")
    if not python and not shell:
        return None
    arguments = [os.fsdecode(value) for value in argv.split(b"\0") if value][1:]
    while arguments:
        argument = arguments.pop(0)
        if argument == "--":
            break
        if argument == "-c" or shell and re.fullmatch(r"-[a-zA-Z]*c[a-zA-Z]*", argument):
            return None  # The complete inline body is already bound by the argv digest.
        if python and argument in ("-m", "-"):
            raise quiet.QuietViolation("reviewed interpreter module/stdin source cannot be resolved exactly")
        if python and argument in ("-W", "-X"):
            if not arguments:
                raise quiet.QuietViolation("incomplete interpreter option")
            arguments.pop(0)
            continue
        if not argument.startswith("-"):
            arguments.insert(0, argument)
            break
        allowed = (re.fullmatch(r"-[uBEIOPsSvq]+|-W.+|-X.+", argument) if python else
                   re.fullmatch(r"-[a-zA-Z]+|--noprofile|--norc", argument))
        if not allowed:
            raise quiet.QuietViolation(f"cannot resolve reviewed interpreter option {argument}")
    if not arguments:
        return None  # Interactive/waiting interpreter without a script argument.
    script = Path(arguments[0])
    path = entry / "root" / str(script).lstrip("/") if script.is_absolute() else entry / "cwd" / script
    return {"path": str(path.resolve()), **file_identity(path)}


def identity(pid, expected_start=None, proc_root=Path("/proc")):
    entry = proc_root / str(pid)
    def start():
        return int((entry / "stat").read_text().rsplit(")", 1)[1].split()[19])
    before = start()
    target = os.readlink(entry / "exe")
    executable = file_identity(entry / "exe")
    argv = (entry / "cmdline").read_bytes()
    script = script_identity(entry, target, argv)
    if start() != before or (expected_start is not None and before != expected_start):
        raise quiet.QuietViolation(f"PID {pid} changed while reading its identity")
    return {"pid": pid, "start_ticks": before, "uid": entry.stat().st_uid,
            "exe": {"path": target, **executable}, "script": script,
            "argv_sha256": hashlib.sha256(argv).hexdigest()}


def inventory():
    rows = []
    for row in sorted(quiet.snapshot().values(), key=lambda row: row.pid):
        if row.kernel_thread:
            continue  # PF_KTHREAD accounting remains separately visible in every run.
        record = {"pid": row.pid, "start_ticks": row.start, "parent_pid": row.parent,
                  "comm": row.name, "cpu_ticks": row.ticks, "affinity": sorted(row.affinity),
                  "reviewed": False, "classification": None, "identity": None}
        try:
            record["identity"] = identity(row.pid, row.start)
        except (OSError, quiet.QuietViolation) as error:
            record["identity_error"] = str(error)
        rows.append(record)
    return {"schema": 1, "captured_at": time.time(), "processes": rows}


def reviewed_inventory(document):
    if document.get("schema") != 1 or not isinstance(document.get("processes"), list):
        raise ValueError("invalid reviewed background inventory")
    reviewed, pids = {}, set()
    for row in document["processes"]:
        if row.get("reviewed") is not True:
            continue
        value = row.get("identity")
        if (row.get("classification") not in CLASSES or not isinstance(value, dict) or
                value.get("pid") != row.get("pid") or value.get("start_ticks") != row.get("start_ticks") or
                not isinstance(value.get("exe"), dict) or not value["exe"].get("path") or
                not isinstance(value["exe"].get("ctime_ns"), int) or
                not re.fullmatch(r"[0-9a-f]{64}", value["exe"].get("sha256", "")) or
                not re.fullmatch(r"[0-9a-f]{64}", value.get("argv_sha256", ""))):
            raise ValueError(f"reviewed PID {row.get('pid')} lacks an exact readable identity/classification")
        if row["pid"] in pids:
            raise ValueError(f"duplicate reviewed PID {row['pid']}")
        names = {row.get("comm"), Path(value["exe"]["path"]).name}
        if names & quiet.COMPETING:
            raise ValueError(f"PID {row['pid']} is a server/compiler/generator, not idle background")
        pids.add(row["pid"])
        reviewed[row["pid"], row["start_ticks"]] = value
    if not reviewed:
        raise ValueError("no exact background identities have been explicitly reviewed")
    return reviewed


def summary(document):
    for row in document["processes"]:
        value = row.get("identity")
        print(f"{'REVIEWED' if row.get('reviewed') else 'unreviewed':10} "
              f"{row['pid']}:{row['start_ticks']} {row['comm']} ticks={row['cpu_ticks']} "
              f"{value['exe']['path'] if value else 'UNKNOWN: ' + row.get('identity_error', 'unreadable')}")


class QualificationMonitor(quiet.QuietMonitor):
    def __init__(self, *args, reviewed, document, output, **kwargs):
        self.reviewed = reviewed
        self.document = document
        self.phase = "preflight"
        self.lock = threading.RLock()
        self.would_refuse = []
        self.preflight_complete = False
        self.postflight_seconds = None
        self.sample_path = output / "background-samples.jsonl"
        self.sample_path.touch(exist_ok=False)
        super().__init__(*args, **kwargs)

    def set_phase(self, phase):
        with self.lock:
            self.observe()  # Charge the complete interval before changing its label.
            self.check()
            self.phase = phase

    def sample(self):
        with self.lock:
            before, began = self.previous, self.previous_at
            super().sample()
            after = self.previous
            activity = quiet.interference(before, after, self.root, self.cpus, self.ancestors, self.helpers)
            kernel = quiet.interference(before, after, self.root, self.cpus,
                                        self.ancestors, self.helpers, kernel_only=True)
            # Only this diagnostic subclass may audit the generic numeric heuristic. The
            # normal observer's active-experiment and unknown-state failures stay hard.
            failure = self.failure
            generic = (failure and "error" not in failure and failure.get("processes") and
                       all(row["reason"] == "foreign CPU activity" for row in failure["processes"]))
            if generic:
                self.would_refuse.append({"phase": self.phase, **failure})
                self.failure = None
            owned = quiet.owned_processes(after, self.root) | self.ancestors | self.helpers.keys()
            active = {(row["pid"], row["start_ticks"]) for row in activity if row["cpu_ticks"]}
            try:
                for key, expected in self.reviewed.items():
                    current = after.get(key[0])
                    if current is None or current.identity != key:
                        raise quiet.QuietViolation(f"reviewed PID/start {key} exited or changed")
                    if current.identity not in owned and identity(current.pid, current.start) != expected:
                        raise quiet.QuietViolation(f"reviewed PID {current.pid} executable/argv identity changed")
                for row in after.values():
                    if row.kernel_thread or row.identity in owned or row.identity not in active:
                        continue
                    if row.name in quiet.COMPETING or row.experiment_driver:
                        raise quiet.QuietViolation(f"foreign server/experiment PID {row.pid} is CPU-active")
                    if row.identity not in self.reviewed:
                        known = next((item for item in self.document["processes"]
                                      if (item["pid"], item["start_ticks"]) == row.identity), {})
                        detail = known.get("identity_error", "identity not explicitly reviewed")
                        raise quiet.QuietViolation(f"unreviewed CPU-active PID/start {row.identity} ({row.name}): "
                                                   f"{detail}; background cannot be qualified")
            except (OSError, quiet.QuietViolation) as error:
                self.failure = self.failure or {"observed_at": time.time(), "error": str(error)}
            event = {"phase": self.phase, "started_monotonic": began, "ended_monotonic": self.previous_at,
                     "user_cpu_activity": activity, "kernel_cpu_activity": kernel,
                     "rolling_cpu_seconds": sum(row["cpu_ticks"] for _, _, rows in self.activity_windows
                                                for row in rows) * self.tick_seconds,
                     "cpu_budget_seconds": self.cpu_budget_seconds,
                     "would_refuse": failure if generic else None, "hard_failure": self.failure}
            with self.sample_path.open("a") as stream:
                stream.write(json.dumps(event, sort_keys=True) + "\n")
                stream.flush()
                os.fsync(stream.fileno())

    def capture(self, seconds):
        started = time.monotonic()
        self.observe()
        self.check()
        while time.monotonic() < started + seconds:
            time.sleep(max(0, min(self.interval, started + seconds - time.monotonic())))
            self.observe()
            self.check()
        return time.monotonic() - started

    def preflight(self):
        self.preflight_seconds = self.capture(PREFLIGHT)
        self.preflight_complete = True
        self.phase = "setup"
        return self.evidence()

    def close(self):
        if not self.closed:
            self.stop_event.set()
            if self.thread is not None:
                self.thread.join()
            if self.preflight_complete and self.failure is None:
                self.phase = "postflight"
                try:
                    self.postflight_seconds = self.capture(POSTFLIGHT)
                except quiet.QuietViolation:
                    pass  # Main's final check retains the hard failure and exits red.
            super().close()
        return self.evidence()

    def evidence(self):
        result = super().evidence()
        result.update(complete=False, diagnostic_complete=self.closed and self.failure is None,
                      scope="background-qualification; never eligible for a standing null or gate receipt",
                      postflight_seconds=self.postflight_seconds, would_refuse=self.would_refuse,
                      reviewed_inventory=self.document, sample_artifact=str(self.sample_path))
        return result


def measurement_windows(report, events):
    windows = []
    for row in report.get("cells", []):
        for block in row["rounds"]:
            for run in block["runs"]:
                if not run.get("complete") or "midpoint_monotonic" not in run:
                    continue
                begin = run["midpoint_monotonic"] - run["window_seconds"] / 2
                end = run["midpoint_monotonic"] + run["window_seconds"] / 2
                selected = [(index, event) for index, event in enumerate(events, 1)
                            if event["started_monotonic"] < end and event["ended_monotonic"] > begin]
                users = {}
                for _, event in selected:
                    for process in event["user_cpu_activity"]:
                        key = process["pid"], process["start_ticks"]
                        saved = users.setdefault(key, {**process, "cpu_ticks": 0})
                        saved["cpu_ticks"] += process["cpu_ticks"]
                windows.append({"cell": row["cell"]["id"], "artifacts": run["artifacts"],
                    "started_monotonic": begin, "ended_monotonic": end,
                    "sample_lines": [index for index, _ in selected],
                    "foreign_user_activity": list(users.values()),
                    "accounting": "full CPU delta for every partially overlapping sample; conservative edge bound"})
    return windows


def run(args):
    inventory_bytes = args.inventory.read_bytes()
    document = json.loads(inventory_bytes)
    reviewed = reviewed_inventory(document)
    # Use the real parser and real loop, but offer no arbitrary passthrough that could change
    # the experiment's cells, pins, window, arm identity, population, escalation or sequence.
    argv = ["abbagate.py", "--collect-null", "1", "--build-reference", "0", "--subset", "full",
            "--only", ",".join(CELLS), "--candidate", str(args.candidate), "--output", str(args.output),
            "--cells", str(args.cells), "--memtier", args.memtier]
    for key in ("server_cores", "load_cores", "server_smt", "load_smt", "ports", "port"):
        value = getattr(args, key)
        if value is not None:
            argv += ["--" + key.replace("_", "-"), str(value)]
    original = sys.argv
    try:
        sys.argv = argv
        options = abba.parse_args()
    finally:
        sys.argv = original
    cells = abba.selected_cells(abba.read_cells(options.cells), "full", options.only)
    if (tuple(cell.id for cell in cells) != CELLS or
            any(cell.depth <= 1 or not cell.instances for cell in cells)):
        raise ValueError("qualification requires h01/h09/h12 in source order, each with a pinned deep-pipeline floor")
    if abba.WINDOW != 20 or abba.ORDER != ("A", "B", "B", "A"):
        raise ValueError("qualification design requires the unchanged 20s ABBA measurement")
    def monitor(*positional, **keywords):
        output = options.output.resolve()
        (output / "reviewed-background.json").write_bytes(inventory_bytes)
        return QualificationMonitor(*positional, reviewed=reviewed, document=document, output=output, **keywords)
    rc = abba.main(options, diagnostic_monitor=monitor)
    report = json.loads((options.output / "results.json").read_text())
    sample_file = options.output / "background-samples.jsonl"
    events = [json.loads(line) for line in sample_file.read_text().splitlines()] if sample_file.exists() else []
    complete = (len(report.get("cells", [])) == 3 and
        [row["cell"]["id"] for row in report["cells"]] == list(CELLS) and
        all(len(row["rounds"]) == 1 and row["rounds"][0]["instances"] == row["cell"]["instances"] and
            [run["arm"] for run in row["rounds"][0]["runs"]] == list(abba.ORDER) and
            all(run.get("complete") is True for run in row["rounds"][0]["runs"]) for row in report["cells"]))
    evidence = {"run_kind": "background-qualification", "normal_gate_eligible": False,
        "driver_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "reviewed_inventory_sha256": hashlib.sha256(inventory_bytes).hexdigest(),
        "expected_complete_measurements": 12, "exact_pinned_measurements_complete": complete,
        "observed_complete_measurements": sum(run.get("complete") is True for row in report.get("cells", [])
                                               for block in row["rounds"] for run in block["runs"]),
        "windows": measurement_windows(report, events)}
    (options.output / "qualification-windows.json").write_text(json.dumps(evidence, indent=2) + "\n")
    if not complete:
        print("BACKGROUND QUALIFICATION INCOMPLETE: expected exactly 3 pinned ABBA blocks / 12 complete measurements", file=sys.stderr)
        return 1
    return rc


def self_test():
    import contextlib
    from dataclasses import replace
    import io
    import tempfile
    from types import SimpleNamespace
    import unittest
    from unittest import mock
    from abba_evidence import validate_null

    def metadata(pid=20, start=3):
        return {"pid": pid, "start_ticks": start, "uid": 1000,
                "exe": {"path": "/test/frontend", "device": 1, "inode": pid, "size": 10,
                        "mtime_ns": 1, "ctime_ns": 1, "sha256": "a" * 64}, "script": None,
                "argv_sha256": "a" * 64}

    def document():
        return {"schema": 1, "captured_at": 1, "processes": [
            {"pid": 20, "start_ticks": 3, "comm": "frontend", "cpu_ticks": 0,
             "reviewed": True, "classification": "interactive-frontend", "identity": metadata()}]}

    class Controls(unittest.TestCase):
        def test_script_content_change_is_detected_at_same_path_and_same_argv(self):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                entry = root / "99"
                entry.mkdir()
                (entry / "exe").symlink_to(Path(sys.executable).resolve())
                (entry / "root").symlink_to("/")
                (entry / "cwd").symlink_to(root)
                fields = ["0"] * 20
                fields[19] = "123"
                (entry / "stat").write_text("99 (python3) " + " ".join(fields))
                script = root / "supervisor.py"
                script.write_text("print(1)\n")
                (entry / "cmdline").write_bytes(os.fsencode(sys.executable) + b"\0" + os.fsencode(script) + b"\0")
                before = identity(99, 123, root)
                original = script.stat()
                script.write_text("print(2)\n")
                os.utime(script, ns=(original.st_atime_ns, original.st_mtime_ns))
                after = identity(99, 123, root)
                self.assertEqual(before["argv_sha256"], after["argv_sha256"])
                self.assertEqual(before["script"]["mtime_ns"], after["script"]["mtime_ns"])
                self.assertNotEqual(before["script"]["sha256"], after["script"]["sha256"])

        @contextlib.contextmanager
        def fixture(self):
            clock = [0.0]
            rows = {10: quiet.Process(10, 1, 1, "python3", 10, frozenset(range(64))),
                    20: quiet.Process(20, 3, 1, "frontend", 100, frozenset(range(64)))}
            with tempfile.TemporaryDirectory() as tmp, \
                 mock.patch.object(quiet, "snapshot", side_effect=lambda: dict(rows)), \
                 mock.patch.object(quiet, "read_topology", side_effect=lambda cpus: {c: frozenset([c]) for c in cpus}), \
                 mock.patch.object(time, "monotonic", side_effect=lambda: clock[0]), \
                 mock.patch(__name__ + ".identity", side_effect=lambda pid, start: metadata(pid, start)):
                monitor = QualificationMonitor(list(range(32)), list(range(32, 64)), own_root_pid=10,
                    reviewed=reviewed_inventory(document()), document=document(), output=Path(tmp))
                yield monitor, rows, clock

        def test_reviewed_ticks_cross_budget_but_never_certify_quiet(self):
            with self.fixture() as (monitor, rows, clock):
                clock[0] = 20
                rows[20] = replace(rows[20], ticks=201)
                monitor.sample()
                monitor.check()
                self.assertEqual(len(monitor.would_refuse), 1)
                self.assertAlmostEqual(monitor.would_refuse[0]["rolling_cpu_seconds"], 1.01)
                self.assertAlmostEqual(monitor.cpu_budget_seconds, .96)
                self.assertFalse(monitor.evidence()["complete"])
                event = json.loads(monitor.sample_path.read_text())
                self.assertEqual(event["user_cpu_activity"][0]["cpu_ticks"], 101)
                self.assertIsNotNone(event["would_refuse"])

        def test_unreviewed_child_does_not_inherit_approval(self):
            with self.fixture() as (monitor, rows, clock):
                rows[30] = quiet.Process(30, 4, 20, "worker", 1, frozenset([0]))
                clock[0] = 1
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "unreviewed CPU-active"):
                    monitor.check()

        def test_sleeping_compiler_and_active_servers_are_always_hard(self):
            for name, ticks in (("cc1plus", 0), ("GarnetServer", 1), ("memcached", 1)):
                with self.subTest(name=name), self.fixture() as (monitor, rows, clock):
                    rows[30] = quiet.Process(30, 4, 1, name, ticks, frozenset([0]))
                    clock[0] = 1
                    monitor.sample()
                    with self.assertRaises(quiet.QuietViolation):
                        monitor.check()

        def test_reused_pid_and_changed_executable_argv_are_hard(self):
            with self.fixture() as (monitor, rows, clock):
                rows[20] = replace(rows[20], start=99)
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "exited or changed"):
                    monitor.check()
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch(__name__ + ".identity", return_value={**metadata(), "argv_sha256": "b" * 64}):
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "executable/argv identity changed"):
                    monitor.check()

        def test_unreadable_reviewed_identity_and_observer_error_stay_hard(self):
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch(__name__ + ".identity", side_effect=PermissionError("unreadable exe")):
                monitor.sample()
                with self.assertRaisesRegex(quiet.QuietViolation, "unreadable exe"):
                    monitor.check()
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch.object(quiet, "snapshot", side_effect=PermissionError("unknown observer")):
                monitor.observe()
                with self.assertRaisesRegex(quiet.QuietViolation, "unknown observer"):
                    monitor.check()

        def test_inventory_retains_unknown_system_identity_without_approving_it(self):
            process = quiet.Process(33, 4, 1, "system-service", 1, frozenset([0]))
            with mock.patch.object(quiet, "snapshot", return_value={33: process}), \
                 mock.patch(__name__ + ".identity", side_effect=PermissionError("exe unreadable")):
                capture = inventory()
            self.assertEqual(len(capture["processes"]), 1)
            self.assertFalse(capture["processes"][0]["reviewed"])
            self.assertIsNone(capture["processes"][0]["identity"])
            capture["processes"][0].update(reviewed=True, classification="desktop")
            with self.assertRaises(ValueError):
                reviewed_inventory(capture)

        def test_server_cannot_be_reviewed_as_desktop(self):
            for name in ("GarnetServer", "memcached", "cc1plus"):
                value = document()
                value["processes"][0]["comm"] = name
                with self.subTest(name=name), self.assertRaises(ValueError):
                    reviewed_inventory(value)

        def test_preflight_and_postflight_capture_actual_fixed_lengths(self):
            with self.fixture() as (monitor, rows, clock), \
                 mock.patch.object(time, "sleep", side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
                monitor.preflight()
                self.assertEqual(monitor.preflight_seconds, 120)
                monitor.close()
                self.assertEqual(monitor.postflight_seconds, 60)
                self.assertTrue(monitor.evidence()["diagnostic_complete"])
                self.assertFalse(monitor.evidence()["complete"])
                events = [json.loads(line) for line in monitor.sample_path.read_text().splitlines()]
                self.assertEqual({e["phase"] for e in events}, {"preflight", "postflight"})

        def test_window_summary_charges_both_partial_edges_in_full(self):
            report = {"cells": [{"cell": {"id": "h01"}, "rounds": [{"runs": [{
                "complete": True, "midpoint_monotonic": 20, "window_seconds": 20, "artifacts": "h01/n4-1-A"}]}]}]}
            def event(a, b, ticks):
                return {"started_monotonic": a, "ended_monotonic": b, "user_cpu_activity": [
                    {"pid": 20, "start_ticks": 3, "cpu_ticks": ticks}]}
            result = measurement_windows(report, [event(8, 11, 2), event(11, 29, 3), event(29, 31, 4), event(31, 32, 99)])
            self.assertEqual(result[0]["sample_lines"], [1, 2, 3])
            self.assertEqual(result[0]["foreign_user_activity"][0]["cpu_ticks"], 9)

        def test_real_main_takes_exactly_twelve_measurements_and_never_writes_trusted_evidence(self):
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                candidate = root / "candidate"
                candidate.write_bytes(b"never executed")
                candidate.chmod(0o700)
                manifest = root / "inventory.json"
                manifest.write_text(json.dumps(document()))
                cells = root / "cells"
                cells.write_text("".join(f"{name} | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | - | - | 4\n" for name in CELLS))
                args = SimpleNamespace(inventory=manifest, candidate=candidate, output=root / "output", cells=cells,
                    memtier=sys.executable, server_cores="0-31", load_cores="32-63", server_smt="", load_smt="", ports=None, port="9079")
                phases, calls, writes = [], [], []
                fake = SimpleNamespace(start=lambda: None, check=lambda: None, set_phase=phases.append,
                    evidence=lambda: {"complete": False, "scope": "background-qualification"},
                    close=lambda: {"complete": False, "scope": "background-qualification"})
                def measure(runner, cell, arm, sequence, instances, knobs):
                    calls.append((cell.id, instances, sequence, arm))
                    return {"arm": arm, "rate": 100, "busy_pct": 99.9, "latency_ms": 1,
                            "complete": True, "midpoint_monotonic": 20 + len(calls) * 30,
                            "window_seconds": 20, "artifacts": f"{cell.id}/n{instances}-{sequence}-{arm}"}
                real_write = Path.write_text
                def write(path, value, *args, **kwargs):
                    if path.name == "results.json":
                        record = json.loads(value)
                        self.assertEqual(record["run_kind"], "background-qualification")
                        self.assertFalse(record["normal_gate_eligible"])
                        self.assertFalse(record["measurement_valid"])
                        self.assertFalse(record["comparison_trusted"])
                        self.assertNotEqual(record.get("null_control", {}).get("verdict"), "PASS")
                        writes.append(record)
                    return real_write(path, value, *args, **kwargs)
                with mock.patch(__name__ + ".QualificationMonitor", return_value=fake), \
                     mock.patch.object(abba.Runner, "measure", measure), \
                     mock.patch.object(abba, "accepted", return_value=True), \
                     mock.patch.object(abba, "check_placement"), \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.object(abba, "null_result", side_effect=AssertionError("diagnostic called null_result")), \
                     mock.patch.object(Path, "write_text", write), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), contextlib.redirect_stdout(io.StringIO()):
                    rc = run(args)
                self.assertEqual(rc, 3)
                self.assertEqual(calls, [(cell, 4, sequence, arm) for cell in CELLS for sequence, arm in enumerate(abba.ORDER, 1)])
                self.assertEqual(len([phase for phase in phases if phase.startswith("measurement:")]), 12)
                self.assertGreaterEqual(len(writes), 4)
                result = writes[-1]
                self.assertEqual(result["statistical_verdict"], "PASS")
                self.assertEqual(result["null_control"]["verdict"], "UNTRUSTED")
                counts = json.loads((args.output / "qualification-windows.json").read_text())
                self.assertTrue(counts["exact_pinned_measurements_complete"])
                self.assertEqual(counts["observed_complete_measurements"], 12)
                with self.assertRaises(ValueError):
                    validate_null(result, now=time.time())

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Controls)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("inventory", help="read-only identity capture; approves no processes")
    capture.add_argument("--output", required=True, type=Path)
    show = commands.add_parser("summary", help="print reviewed identities without exposing argv bodies")
    show.add_argument("inventory", type=Path)
    launch = commands.add_parser("run", help="diagnostic only: 120s + three ABBA blocks + 60s")
    launch.add_argument("--inventory", required=True, type=Path)
    launch.add_argument("--candidate", required=True, type=Path)
    launch.add_argument("--output", required=True, type=Path)
    launch.add_argument("--cells", type=Path, default=abba.ROOT / "tests/headline_cells.txt")
    launch.add_argument("--memtier", default="memtier_benchmark")
    for name in ("server-cores", "load-cores", "server-smt", "load-smt", "ports", "port"):
        launch.add_argument("--" + name)
    commands.add_parser("self-test", help="serverless identity and real-loop controls")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    if args.command == "inventory":
        document = inventory()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x") as stream:
            json.dump(document, stream, indent=2)
            stream.write("\n")
        summary(document)
    elif args.command == "summary":
        summary(json.loads(args.inventory.read_text()))
    elif args.command == "self-test":
        self_test()
    else:
        sys.exit(run(args))
