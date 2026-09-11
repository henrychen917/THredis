#!/usr/bin/env python3
"""Diagnostic worker placement for exact owned generators; never normal gate evidence."""
import json
import os
from pathlib import Path
import signal
import time

from abba_profile import exe_identity, parse_cpus, parse_stat


STARTUP_SECONDS = 5


def require(condition, message):
    if not condition:
        raise RuntimeError("load worker affinity: " + message)


def physical_first(assigned):
    require(isinstance(assigned, list) and assigned and
            all(type(cpu) is int and cpu >= 0 for cpu in assigned) and
            len(set(assigned)) == len(assigned), "invalid assigned CPU list")
    groups = {}
    for cpu in sorted(assigned):
        siblings = parse_cpus(Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read_text().strip())
        require(cpu in siblings, "CPU topology omits its own CPU")
        key = tuple(sorted(siblings))
        groups.setdefault(key, []).append(cpu)
    ordered = sorted(groups.values(), key=lambda group: group[0])
    # One logical CPU from every assigned physical core before any sibling. The
    # process coordinator shares worker zero's CPU; no new load/core reservation.
    return [group[level] for level in range(max(map(len, ordered)))
            for group in ordered if level < len(group)]


def process_state(process):
    require(process.poll() is None, f"owned generator {process.pid} exited")
    path = Path("/proc") / str(process.pid)
    row = parse_stat((path / "stat").read_text())
    require(row["pid"] == process.pid and row["ppid"] == os.getpid() and row["state"] not in ("Z", "X", "x"),
            "target is not this driver's live direct child")
    tids = sorted(int(item.name) for item in (path / "task").iterdir() if item.name.isdecimal())
    require(process.pid in tids, "generator main TID disappeared")
    return row, tids


def task_snapshot(process):
    begin, tids = process_state(process)
    path = Path("/proc") / str(process.pid)
    executable = exe_identity(path / "exe")
    rows = []
    for tid in tids:
        entry = path / "task" / str(tid)
        raw_stat, raw_status = (entry / "stat").read_text(), (entry / "status").read_text()
        stat = parse_stat(raw_stat)
        status = dict(line.split(":", 1) for line in raw_status.splitlines() if ":" in line)
        affinity = sorted(os.sched_getaffinity(tid))
        require(stat["pid"] == tid and int(status["Pid"]) == tid and int(status["Tgid"]) == process.pid,
                "task PID/TGID changed")
        require(affinity == parse_cpus(status["Cpus_allowed_list"].strip()), "task affinity changed during observation")
        require(parse_stat((entry / "stat").read_text())["start_ticks"] == stat["start_ticks"],
                "task start identity changed during observation")
        rows.append(dict(tid=tid, tgid=process.pid, start_ticks=stat["start_ticks"], state=stat["state"],
                         affinity=affinity, raw_stat=raw_stat, raw_status=raw_status))
    end, final_tids = process_state(process)
    require(begin["start_ticks"] == end["start_ticks"] and tids == final_tids and
            exe_identity(path / "exe") == executable, "generator identity/task set changed during observation")
    return dict(pid=process.pid, start_ticks=begin["start_ticks"], executable=executable,
                observed_monotonic=time.monotonic(), threads=rows)


class WorkerAffinity:
    startup_seconds = STARTUP_SECONDS

    def __init__(self, folder):
        self.path = Path(folder) / "load-worker-affinity.json"
        self.processes, self.layout, self.paused = [], [], []
        self.pidfds = {}
        self.record = dict(schema=1, status="INCOMPLETE", normal_gate_eligible=False,
            policy="fixed-owned-worker-tids-physical-first-v1", startup_budget_seconds=self.startup_seconds,
            scope="diagnostic placement; no generator capacity or performance verdict",
            latency_limitation="full-run HDR includes startup SIGSTOP; central throughput improvement cannot justify p1/tail adoption",
            processes=[], checks=[])

    def save(self):
        self.path.write_text(json.dumps(self.record, indent=2) + "\n")

    def fail(self, error):
        self.record.update(status="INVALID", error=f"{type(error).__name__}: {error}")
        self.save()

    def before_deadline(self):
        require(time.monotonic() < self.deadline, "global generator startup/pinning deadline expired")

    def begin(self, processes, layout, launch_start, lifetime):
        self.processes, self.layout = processes, layout
        self.deadline, self.expiry = launch_start + self.startup_seconds, launch_start + lifetime
        self.record.update(first_launch_monotonic=launch_start, startup_deadline_monotonic=self.deadline,
                           requested_generator_lifetime_seconds=lifetime,
                           earliest_generator_expiry_monotonic=self.expiry)
        require(processes and len(processes) == len(layout), "missing generator placements")
        seen = {}
        try:
            for process, placement in zip(processes, layout):
                row, tids = process_state(process)
                if hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
                    self.pidfds[process.pid] = os.pidfd_open(process.pid)
                    require(process_state(process)[0]["start_ticks"] == row["start_ticks"],
                            "generator identity changed while opening pidfd")
                self.record["processes"].append(dict(pid=process.pid, start_ticks=row["start_ticks"],
                    expected_workers=placement["threads"], assigned_cpus=placement["cpus"], applied=[],
                    pidfd_bound=process.pid in self.pidfds))
                seen[process.pid] = set(tids)
            while True:
                self.before_deadline()
                ready = True
                for process, saved in zip(processes, self.record["processes"]):
                    row, tids = process_state(process)
                    require(row["start_ticks"] == saved["start_ticks"], "generator PID/start changed during startup")
                    require(seen[process.pid] <= set(tids), "generator task disappeared during startup")
                    seen[process.pid] = set(tids)
                    require(len(tids) <= saved["expected_workers"] + 1, "unexpected generator worker count")
                    ready &= len(tids) == saved["expected_workers"] + 1
                if ready:
                    break
                time.sleep(.01)
            # A bare sched_setaffinity(TID) cannot atomically compare a start time.
            # Freeze these owned child groups while reading and mutating TIDs, so
            # an exiting worker cannot donate its numeric TID to another process.
            for process in processes:
                self.paused.append(process)
                self.send_signal(process, signal.SIGSTOP)
            while True:
                self.before_deadline()
                snapshots = [task_snapshot(process) for process in processes]
                if all(all(row["state"] == "T" for row in value["threads"]) for value in snapshots):
                    break
                time.sleep(.01)
            for process, placement, saved, snapshot in zip(processes, layout, self.record["processes"], snapshots):
                self.before_deadline()
                require(snapshot["start_ticks"] == saved["start_ticks"] and
                        {row["tid"] for row in snapshot["threads"]} == seen[process.pid],
                        "generator task set changed before pinning")
                require(len(snapshot["threads"]) == placement["threads"] + 1, "worker count differs before pinning")
                order = physical_first(placement["cpus"])
                require(len(order) >= placement["threads"], "fewer assigned CPUs than workers")
                saved.update(original=snapshot, physical_first_cpus=order)
                workers = sorted(row["tid"] for row in snapshot["threads"] if row["tid"] != process.pid)
                mapping = {tid: cpu for tid, cpu in zip(workers, order)}
                mapping[process.pid] = order[0]
                saved["mapping"] = [dict(tid=row["tid"], tgid=process.pid, start_ticks=row["start_ticks"],
                    role="coordinator" if row["tid"] == process.pid else "worker",
                    requested_affinity=[mapping[row["tid"]]]) for row in snapshot["threads"]]
                for row in snapshot["threads"]:
                    self.before_deadline()
                    require(row["affinity"] == sorted(placement["cpus"]), "original task mask differs from declared load CPUs")
                    os.sched_setaffinity(row["tid"], {mapping[row["tid"]]})
                    applied = dict(tid=row["tid"], tgid=process.pid, start_ticks=row["start_ticks"],
                        role="coordinator" if row["tid"] == process.pid else "worker",
                        original_affinity=row["affinity"], requested_affinity=[mapping[row["tid"]]],
                        applied_affinity=sorted(os.sched_getaffinity(row["tid"])))
                    saved["applied"].append(applied)
                    require(applied["applied_affinity"] == applied["requested_affinity"],
                            "kernel did not apply requested task mask")
            self.verify("all-masks-applied-while-stopped")
        except BaseException as error:
            self.fail(error)
            raise
        finally:
            try:
                self.resume()
            finally:
                for fd in self.pidfds.values():
                    os.close(fd)
                self.pidfds.clear()
                self.save()
        self.before_deadline()
        self.record["all_resumed_monotonic"] = time.monotonic()
        self.verify("all-generators-resumed")

    def send_signal(self, process, signum):
        if process.pid in self.pidfds:
            signal.pidfd_send_signal(self.pidfds[process.pid], signum)
        else:
            # Popen retains this direct child's unreaped identity. Its signal
            # method polls before signaling, so an exited child is not reused.
            process.send_signal(signum)

    def resume(self):
        errors = []
        for process in self.paused:
            try:
                current, _ = process_state(process)
                saved = next(row for row in self.record["processes"] if row["pid"] == process.pid)
                require(current["start_ticks"] == saved["start_ticks"], "cannot resume changed generator PID/start")
                self.send_signal(process, signal.SIGCONT)
                saved["resumed_monotonic"] = time.monotonic()
            except BaseException as error:
                errors.append(f"PID {process.pid}: {error}")
        self.paused.clear()
        if errors:
            self.record["resume_errors"] = errors
            raise RuntimeError("load worker affinity resume failed: " + "; ".join(errors))

    def verify(self, stage):
        check = dict(stage=stage, started_monotonic=time.monotonic(), processes=[])
        self.record["checks"].append(check)
        for process, saved in zip(self.processes, self.record["processes"]):
            snapshot = task_snapshot(process)
            check["processes"].append(snapshot)
            require(snapshot["start_ticks"] == saved["start_ticks"] and
                    snapshot["executable"] == saved["original"]["executable"], "generator identity changed after pinning")
            expected = {row["tid"]: row for row in saved["applied"]}
            require({row["tid"] for row in snapshot["threads"]} == set(expected), "generator worker task set changed after pinning")
            for row in snapshot["threads"]:
                bound = expected[row["tid"]]
                require((row["start_ticks"], row["tgid"], row["affinity"]) ==
                        (bound["start_ticks"], bound["tgid"], bound["applied_affinity"]),
                        "bound task identity or actual affinity changed")
        check["finished_monotonic"] = time.monotonic()
        self.save()

    def finish(self):
        self.verify("after-central-window")
        self.record["status"] = "COMPLETE"
        self.save()


def runtime_seconds(runtime, requested):
    require(isinstance(runtime, dict) and runtime.get("Time unit") == "MILLISECONDS", "unsupported histogram runtime units")
    start, finish, duration = (runtime.get(key) for key in ("Start time", "Finish time", "Total duration"))
    require(all(type(value) is int and value > 0 for value in (start, finish, duration)) and
            finish - start == duration and duration >= requested * 1000,
            "invalid or shortened full histogram runtime")
    return duration / 1000


def histogram_runtime(folder, count, requested):
    rows = []
    for index in range(count):
        runtime = json.loads((Path(folder) / f"load-{index}.json").read_text())["ALL STATS"]["Runtime"]
        rows.append(dict(index=index, raw_runtime=runtime, duration_seconds=runtime_seconds(runtime, requested)))
    return dict(scope="each generator's entire HDR run, including startup/warmup/tail",
                requested_seconds=requested, processes=rows)


def validate_pair(floating, fixed):
    from abba_evidence import instrument

    for report, pin in ((floating, 0), (fixed, 1)):
        require(report.get("run_kind") == "background-qualification" and
                report.get("normal_gate_eligible") is False and report.get("comparison_trusted") is False,
                "placement comparisons accept only permanently untrusted diagnostics")
        require(report.get("pin_load_workers") == pin and report.get("load_startup_seconds") == STARTUP_SECONDS,
                "placement controls require explicit floating/fixed modes with matched 5s startup allowance")
        require(report.get("candidate", {}).get("sha256") == report.get("reference", {}).get("sha256") and
                report.get("candidate", {}).get("sha256"), "placement arms must be byte-identical")
        require(report.get("quiet_box", {}).get("diagnostic_complete") is True,
                "placement control's background observation did not complete")
        rows = report.get("cells", [])
        require(rows and all(row.get("rounds") for row in rows), "unreached placement cells")
        for row in rows:
            for block in row["rounds"]:
                require([run.get("arm") for run in block.get("runs", [])] == ["A", "B", "B", "A"],
                        "incomplete placement ABBA block")
                for run in block["runs"]:
                    require(run.get("complete") is True and
                            run.get("load_timing", {}).get("requested_lifetime_seconds") == 33 and
                            run.get("load_timing", {}).get("fresh_warmup_seconds") == 3 and
                            run.get("full_histogram_runtime", {}).get("requested_seconds") == 33,
                            "placement controls need complete matched 33s generators and fresh 3s warmup")
                    require((run.get("load_worker_affinity", {}).get("status") == "COMPLETE") if pin else
                            "load_worker_affinity" not in run, "placement mode was not actually applied")
                    histograms = run["full_histogram_runtime"].get("processes", [])
                    require([row.get("index") for row in histograms] == list(range(block["instances"])) and
                            all(row.get("duration_seconds") == runtime_seconds(row.get("raw_runtime"), 33)
                                for row in histograms), "missing actual full histogram durations")
    for field in ("instrument_fingerprint", "candidate", "cell_source", "window_seconds", "order"):
        require(floating.get(field) == fixed.get(field), "placement control differs: " + field)
    require(floating.get("window_seconds") == 20 and floating.get("order") == ["A", "B", "B", "A"],
            "placement experiment changed the 20s ABBA design")
    require(bool(floating.get("cpu_profile_requested")) == bool(fixed.get("cpu_profile_requested")),
            "placement controls used different profiling settings")
    require(instrument(floating["environment"]) == instrument(fixed["environment"]), "placement geometry/workload environment differs")
    def layout(report):
        return [(row["cell"], [(block["instances"], [run["load_layout"] for run in block["runs"]])
                              for block in row["rounds"]]) for row in report["cells"]]
    require(layout(floating) == layout(fixed), "placement controls changed cells, connections, workers, or assigned CPU lists")
    return dict(configuration="MATCHED", normal_gate_eligible=False, performance_verdict="UNASSESSED",
                limitation="sequential block comparison alone cannot establish placement causality",
                latency_comparison_eligible=False,
                latency_limitation="fixed full-run HDR includes startup SIGSTOP; p1/tail use requires excluding that pause or pinning before traffic",
                normal_unprofiled_operational_null_required=True,
                requested_generator_lifetime_seconds=33,
                statistical_verdicts=[report.get("statistical_verdict") for report in (floating, fixed)],
                cells=[row["cell"]["id"] for row in floating["cells"]])


def self_test():
    import copy
    import select
    import subprocess
    import sys
    import tempfile
    import unittest
    from unittest import mock

    class Controls(unittest.TestCase):
        def test_physical_cores_precede_siblings_and_no_cpu_is_added(self):
            siblings = {2: "2,130", 3: "3,131", 130: "2,130", 131: "3,131"}
            with mock.patch.object(Path, "read_text", lambda path: siblings[int(path.parts[-3][3:])]):
                self.assertEqual(physical_first([131, 130, 3, 2]), [2, 3, 130, 131])
                self.assertEqual(physical_first([3, 130, 131]), [3, 130, 131])
            with self.assertRaisesRegex(RuntimeError, "invalid assigned"):
                physical_first([2, 2])

        def child(self, cpus):
            program = """import sys, threading
events = [threading.Event(), threading.Event()]
threads = [threading.Thread(target=event.wait) for event in events]
for thread in threads: thread.start()
print('ready', flush=True)
for command in sys.stdin:
    if command.strip() == 'replace':
        events[0].set(); threads[0].join()
        events[0] = threading.Event()
        threads[0] = threading.Thread(target=events[0].wait)
        threads[0].start()
        print('replaced', flush=True)
"""
            process = subprocess.Popen(["taskset", "-c", ",".join(map(str, cpus)), sys.executable, "-u", "-c", program],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
            def cleanup():
                if process.poll() is None:
                    process.send_signal(signal.SIGCONT)
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
                for stream in (process.stdin, process.stdout, process.stderr):
                    stream.close()
            self.addCleanup(cleanup)
            self.assertTrue(select.select([process.stdout], [], [], 2)[0], "owned thread control never became ready")
            self.assertEqual(process.stdout.readline().strip(), "ready")
            return process

        def test_actual_owned_thread_masks_identity_and_churn(self):
            assigned = sorted(os.sched_getaffinity(0))[:3]
            self.assertGreaterEqual(len(assigned), 2, "control requires at least two allowed CPUs")
            process = self.child(assigned)
            with tempfile.TemporaryDirectory() as tmp:
                pin = WorkerAffinity(tmp)
                pin.begin([process], [dict(cpus=assigned, threads=2, clients=256)], time.monotonic(), 33)
                saved = pin.record["processes"][0]
                self.assertEqual(saved["pid"], process.pid)
                self.assertEqual(saved["pidfd_bound"], hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"))
                self.assertEqual(len(saved["applied"]), 3)
                worker_cpus = [row["applied_affinity"][0] for row in saved["applied"] if row["role"] == "worker"]
                self.assertEqual(worker_cpus, physical_first(assigned)[:2])
                self.assertEqual(len(set(worker_cpus)), 2)
                for row in saved["applied"]:
                    self.assertEqual(sorted(os.sched_getaffinity(row["tid"])), row["applied_affinity"])
                    self.assertEqual(row["original_affinity"], assigned)
                    self.assertEqual(row["tgid"], process.pid)
                self.assertEqual(next(row["applied_affinity"] for row in saved["applied"]
                                      if row["role"] == "coordinator"), [worker_cpus[0]])
                self.assertFalse(pin.pidfds)
                self.assertFalse(pin.paused)
                pin.verify("directed-control")
                process.stdin.write("replace\n")
                process.stdin.flush()
                self.assertTrue(select.select([process.stdout], [], [], 2)[0], "replacement thread never became ready")
                self.assertEqual(process.stdout.readline().strip(), "replaced")
                with self.assertRaisesRegex(RuntimeError, "task set changed"):
                    pin.verify("changed-worker")

        def test_actual_worker_count_and_partial_apply_fail_closed_and_resume(self):
            assigned = sorted(os.sched_getaffinity(0))[:3]
            self.assertGreaterEqual(len(assigned), 2)
            for failure in ("count", "apply"):
                with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                    process = self.child(assigned)
                    pin = WorkerAffinity(tmp)
                    setter = os.sched_setaffinity
                    calls = []
                    def apply(tid, mask):
                        calls.append(tid)
                        if len(calls) == 2:
                            raise OSError("injected affinity application failure")
                        setter(tid, mask)
                    with mock.patch.object(os, "sched_setaffinity", side_effect=apply):
                        with self.assertRaisesRegex((RuntimeError, OSError), "worker count|injected affinity"):
                            pin.begin([process], [dict(cpus=assigned, threads=1 if failure == "count" else 2, clients=1)],
                                      time.monotonic(), 33)
                    self.assertEqual(pin.record["status"], "INVALID")
                    self.assertFalse(pin.paused)
                    self.assertFalse(pin.pidfds)
                    self.assertEqual(len(calls), 0 if failure == "count" else 2)
                    if failure == "apply":
                        self.assertEqual(len(calls), 2)
                        self.assertEqual(len(pin.record["processes"][0]["applied"]), 1)
                        self.assertEqual(len(pin.record["processes"][0]["mapping"]), 3)
                    # A failed partial setup must not leave an owned child stopped.
                    process.stdin.write("replace\n")
                    process.stdin.flush()
                    self.assertTrue(select.select([process.stdout], [], [], 2)[0])
                    self.assertEqual(process.stdout.readline().strip(), "replaced")

        def test_startup_deadline_and_histogram_duration_are_hard_bounds(self):
            process = self.child(sorted(os.sched_getaffinity(0))[:2])
            with tempfile.TemporaryDirectory() as tmp:
                pin = WorkerAffinity(tmp)
                with self.assertRaisesRegex(RuntimeError, "deadline expired"):
                    pin.begin([process], [dict(cpus=sorted(os.sched_getaffinity(0))[:2], threads=2, clients=1)],
                              time.monotonic() - 6, 33)
                self.assertEqual(pin.record["status"], "INVALID")
                data = {"ALL STATS": {"Runtime": {"Time unit": "MILLISECONDS", "Start time": 1000,
                        "Finish time": 34001, "Total duration": 33001}}}
                path = Path(tmp) / "load-0.json"
                path.write_text(json.dumps(data))
                self.assertEqual(histogram_runtime(tmp, 1, 33)["processes"][0]["duration_seconds"], 33.001)
                data["ALL STATS"]["Runtime"].update({"Finish time": 29000, "Total duration": 28000})
                path.write_text(json.dumps(data))
                with self.assertRaisesRegex(RuntimeError, "shortened"):
                    histogram_runtime(tmp, 1, 33)

        def test_placement_pair_rejects_unmatched_lifetime_layout_or_profile(self):
            floating = dict(run_kind="background-qualification", normal_gate_eligible=False,
                comparison_trusted=False, pin_load_workers=0, load_startup_seconds=5,
                candidate={"sha256": "a" * 64}, reference={"sha256": "a" * 64},
                quiet_box=dict(diagnostic_complete=True), environment={},
                window_seconds=20, order=["A", "B", "B", "A"], statistical_verdict="FAIL", cells=[dict(
                    cell={"id": "control"}, rounds=[dict(instances=1, runs=[dict(arm=arm, complete=True,
                        load_layout=[dict(cpus=[2, 3], threads=2, clients=256)],
                        load_timing=dict(requested_lifetime_seconds=33, fresh_warmup_seconds=3),
                        full_histogram_runtime=dict(requested_seconds=33, processes=[dict(index=0, duration_seconds=33.,
                            raw_runtime={"Time unit": "MILLISECONDS", "Start time": 1000,
                                         "Finish time": 34000, "Total duration": 33000})])) for arm in ("A", "B", "B", "A")])])])
            fixed = copy.deepcopy(floating)
            fixed["pin_load_workers"] = 1
            for run in fixed["cells"][0]["rounds"][0]["runs"]:
                run["load_worker_affinity"] = {"status": "COMPLETE"}
            matched = validate_pair(floating, fixed)
            self.assertEqual(matched["configuration"], "MATCHED")
            self.assertEqual(matched["statistical_verdicts"], ["FAIL", "FAIL"])
            self.assertTrue(matched["normal_unprofiled_operational_null_required"])
            self.assertFalse(matched["latency_comparison_eligible"])
            for field in ("lifetime", "workers", "profile", "actual-duration"):
                changed = copy.deepcopy(fixed)
                run = changed["cells"][0]["rounds"][0]["runs"][0]
                if field == "lifetime":
                    run["load_timing"]["requested_lifetime_seconds"] = 28
                elif field == "workers":
                    run["load_layout"][0]["threads"] = 1
                elif field == "actual-duration":
                    run["full_histogram_runtime"]["processes"][0]["raw_runtime"].update(
                        {"Finish time": 29000, "Total duration": 28000})
                else:
                    changed["cpu_profile_requested"] = True
                with self.assertRaises(RuntimeError):
                    validate_pair(floating, changed)

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls))
    if not result.wasSuccessful():
        raise SystemExit(1)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("self-test", help="owned sleeping-thread and serverless negative controls")
    compare = commands.add_parser("compare", help="validate matched placement experiment settings; never score performance")
    compare.add_argument("floating", type=Path)
    compare.add_argument("fixed", type=Path)
    args = parser.parse_args()
    if args.command == "self-test":
        self_test()
    else:
        print(json.dumps(validate_pair(json.loads(args.floating.read_text()), json.loads(args.fixed.read_text())), indent=2))
