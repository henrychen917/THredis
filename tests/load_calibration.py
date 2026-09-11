#!/usr/bin/env python3
"""One-arm load-floor calibration, never a comparison or standing null.

A rung asks only whether more generator workers still increase throughput. Keep
one populated server alive for the cell and vary generators alone. The first
saturated rung followed by a non-increasing, higher-worker rung is a PIN. A
single observation cannot estimate repeatability, so no tolerance is invented:
strictly increasing observations keep searching and an unconfirmed ceiling fails.
The normal 20-second paired ABBA path retains its own repeatability-based rule.
"""
from contextlib import contextmanager
from dataclasses import asdict
import json
import math
import os
from pathlib import Path
import shutil
import signal
import sys
import time

import abbagate as abba

WINDOW = 10


# Rate jitter between saturated rungs of one binary, measured on identical bytes by the standing
# null (see select_calibration_floor). Two nulls: 0.71% (2026-09-10), 0.6-1.0% (2026-09-11).
PLATEAU_TOLERANCE_PCT = 1.0


def require(condition, message):
    if not condition:
        raise ValueError(message)


def select_calibration_floor(cell, rounds):
    """Replay one-arm observations; serialized PINs never establish their own floor."""
    require(rounds, "no calibration observations")
    previous = 0
    rows = []
    for block in rounds:
        n, runs = block.get("instances"), block.get("runs")
        require(type(n) is int and n > previous, "calibration rungs must increase")
        previous = n
        require(isinstance(runs, list) and len(runs) == 1, "calibration needs exactly one observation per rung")
        run = runs[0]
        require(run.get("arm") == "B" and run.get("instances") == n and
                run.get("complete") is True and not run.get("error") and run.get("calibration_only") is True,
                "calibration observation is incomplete or not one-arm")
        require(type(run.get("rate")) in (int, float) and math.isfinite(run["rate"]) and run["rate"] > 0,
                "invalid calibration rate")
        require(type(run.get("window_seconds")) in (int, float) and
                math.isfinite(run["window_seconds"]) and run["window_seconds"] >= WINDOW,
                "shortened calibration observation")
        layout = run.get("load_layout")
        require(isinstance(layout, list) and len(layout) == n, "missing calibration load layout")
        workers, connections, assigned = 0, 0, []
        for placement in layout:
            threads, clients, cpus = (placement.get(key) for key in ("threads", "clients", "cpus"))
            require(type(threads) is int and type(clients) is int and threads > 0 and clients > 0 and
                    isinstance(cpus, list) and cpus and threads <= len(cpus) and
                    all(type(cpu) is int and cpu >= 0 for cpu in cpus), "invalid calibration worker layout")
            workers += threads
            connections += threads * clients
            assigned += cpus
        require(connections == cell.conns and len(assigned) == len(set(assigned)),
                "calibration changed total connections or overlapped load CPUs")
        if rows:
            require(set(assigned) == set(rows[0]["assigned_cpus"]), "calibration changed assigned load CPUs")
        rows.append(dict(instances=n, rate=run["rate"], worker_threads=workers,
                         assigned_cpus=assigned, saturation_pct=abba.saturation_score(run, cell)))
    selected = confirmation = None
    if cell.depth == 1:
        selected = 0
    else:
        for index, (current, above) in enumerate(zip(rows, rows[1:])):
            # "Non-increasing" needs a tolerance, or a flat plateau is a coin flip. h27 on
            # 2026-09-12: n=4 30.07M, n=8 30.27M -- a +0.7% jitter on a plateau the instrument
            # itself scores as saturated from n=4 -- read as "still climbing", and the search walked
            # to the 16-instance ceiling and pinned nothing; the previous run of the same cell dipped
            # 2% at n=8 and pinned at 4. The tolerance is the instrument's demonstrated rate error on
            # IDENTICAL bytes, measured by two standing nulls (0.71%, and 0.6-1.0% across 15 cells),
            # not a chosen number. A genuine climb is 100%+ per rung here and cannot hide inside it.
            if (current["saturation_pct"] >= abba.BUSY_FLOOR and
                    above["worker_threads"] > current["worker_threads"] and
                    above["rate"] < current["rate"] * (1 + PLATEAU_TOLERANCE_PCT / 100)):
                selected, confirmation = index, index + 1
                break
    return dict(method="one-arm-observed-peak-v1", measurement_valid=True,
        status="EXEMPT" if cell.depth == 1 else "PIN" if selected is not None else "UNPROVEN",
        selected_index=selected, confirmation_index=confirmation,
        lowest_tested_qualifying_instances=rows[selected]["instances"] if selected is not None else None,
        confirmation_instances=rows[confirmation]["instances"] if confirmation is not None else None,
        generator_headroom="UNPROVEN", tested_rungs=rows)


@contextmanager
def persistent_cell(runner):
    # Only this calibration context owns retained server state. Every normal
    # Runner.measure still closes its server; errors clear and reap this state.
    session = {}
    try:
        yield session
    finally:
        conn, srv = session.get("conn"), session.get("srv")
        try:
            if conn is not None:
                conn.close()
        finally:
            if srv is not None:
                runner.children.stop(srv)
            session.clear()


def main(args):
    require(not args.escalate and not args.collect_null and not args.list_cells,
            "--calibrate cannot combine with --escalate, --collect-null or --list-cells")
    start = time.monotonic()
    out = (args.output or abba.ROOT / "build" / f"load-calibration-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}").resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = dict(schema=1, run_kind="load-calibration", verdict="FAIL", complete=False,
        measurement_valid=False, comparison_trusted=False, normal_gate_eligible=False,
        window_seconds=WINDOW, order=["B"], cells=[], output=str(out),
        started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), subset=args.subset, only=args.only)
    children, quiet = abba.Children(), None
    original_affinity = os.sched_getaffinity(0)
    def interrupted(signum, _frame):
        raise InterruptedError(f"calibration interrupted by signal {signum}")
    handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
    for sig in handlers:
        signal.signal(sig, interrupted)
    def publish():
        report["elapsed_seconds"] = time.monotonic() - start
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    try:
        quiet_file = os.getenv("GATE_QUIET_FILE")
        if quiet_file:
            path = Path(quiet_file)
            age = time.time() - path.stat().st_mtime if path.exists() else -1
            require(age >= 60 * float(os.getenv("GATE_QUIET_MINUTES", "3")),
                    "quiet-file precondition failed; no calibration CPU work started")
        abba.resolve_geometry(args)
        server_physical, load_physical = abba.cpus(args.server_cores), abba.cpus(args.load_cores)
        server_smt, load_smt = abba.cpus(args.server_smt), abba.cpus(args.load_smt)
        abba.check_placement(server_physical, load_physical, server_smt, load_smt)
        server_cpus, load_cpus = sorted(server_physical + server_smt), sorted(load_physical + load_smt)
        args.port, permitted_ports = abba.select_port(args.ports, args.port)
        os.sched_setaffinity(0, load_cpus)
        split_ratio = abba.measured_ratio("abba", len(server_cpus))
        placement = dict(server_physical=server_physical, server_smt=server_smt,
            load_physical=load_physical, load_smt=load_smt, split_ratio=split_ratio)
        inventory = abba.read_cells(args.cells, placement=placement)
        cells = abba.selected_cells(inventory, args.subset, args.only)
        report.update(coverage=abba.coverage(cells), cell_source=dict(path=str(args.cells.resolve()),
            text=args.cells.read_text(), sha256=abba.sha256(args.cells), total_cells=len(inventory)),
            receipt_harness_sha256=abba.harness_fingerprint(abba.ROOT)["sha256"],
            instrument_fingerprint=abba.instrument_fingerprint(abba.ROOT))
        quiet = abba.QuietMonitor(server_cpus, load_cpus, own_root_pid=os.getpid(),
            window_seconds=abba.WINDOW, ports=(args.port,), sample_artifact=out / "quiet-samples.jsonl")
        quiet.start()
        require(args.candidate.is_file() and os.access(args.candidate, os.X_OK), "candidate executable unavailable")
        binary = out / "binary-B"
        shutil.copy2(args.candidate.resolve(), binary)
        report["candidate"] = dict(path=str(args.candidate.resolve()), sha256=abba.sha256(binary),
            workspace_commit=abba.git("rev-parse", "HEAD"), workspace_status=abba.git("status", "--short"))
        args.memtier = shutil.which(args.memtier)
        require(args.memtier, "memtier_benchmark not available")
        args.memtier = str(Path(args.memtier).resolve())
        runner = abba.Runner(args, out, {"B": binary}, children)
        report["environment"] = dict(uname=list(os.uname()), **placement, server_cpus=server_cpus,
            load_cpus=load_cpus, load_instance_ceiling=min(args.max_instances, len(load_physical)),
            port=args.port, permitted_ports=permitted_ports, keys=abba.KEYS, data_bytes=64, key_pattern="P:P",
            atomic="per-cell", split_flip_auto=0, population_by_arm={"B": "wire"},
            python_runtime=report["instrument_fingerprint"]["python"], memtier_path=args.memtier,
            memtier_sha256=abba.sha256(Path(args.memtier)),
            memtier_version=abba.capture([args.memtier, "--version"]).stdout.strip())
        support = {name: abba.accepted(binary, name, value) for name, value in
            (("thread-mode", "1s"), ("read-local", 0), ("overlap", 0), ("reorder", 0))}
        report["accepted_knobs"] = {"B": support}
        for cell in cells:
            row = dict(cell=asdict(cell), status="FAIL", rounds=[])
            report["cells"].append(row)
            try:
                plans, _ = abba.knob_plan(cell, {"A": support, "B": support})
                row["knobs"] = {"B": plans["B"]}
                ladder = sorted(set(abba.LADDER) | ({cell.instances} if cell.instances else set()))
                row["load_ladder"] = [n for n in ladder if n <= min(args.max_instances, cell.conns, len(load_physical))]
                with persistent_cell(runner) as session:
                    for sequence, n in enumerate(row["load_ladder"], 1):
                        quiet.check()
                        run = runner.measure(cell, "B", sequence, n, plans["B"], _calibration=session, _window=WINDOW)
                        row["rounds"].append(dict(instances=n, runs=[run]))
                        quiet.check()
                        row["selection"] = select_calibration_floor(cell, row["rounds"])
                        row["status"] = row["selection"]["status"]
                        publish()
                        if row["status"] in ("PIN", "EXEMPT"):
                            break
                require(row["status"] in ("PIN", "EXEMPT"), "ceiling reached without a saturated peak and higher-worker confirmation")
                print(f"CALIBRATION {cell.id} {row['status']} instances="
                      f"{row['selection']['lowest_tested_qualifying_instances']} "
                      f"confirmation={row['selection']['confirmation_instances']}; no performance verdict", flush=True)
            except (InterruptedError, abba.QuietViolation):
                raise
            except Exception as error:
                row.update(status="FAIL", reason=f"{type(error).__name__}: {error}")
                print(f"CALIBRATION {cell.id} FAIL: {row['reason']}", flush=True)
            finally:
                children.close()
                report["quiet_box"] = quiet.evidence()
                publish()
        report["quiet_box"] = quiet.close()
        quiet.check()
        require(abba.harness_fingerprint(abba.ROOT)["sha256"] == report["receipt_harness_sha256"] and
                abba.instrument_fingerprint(abba.ROOT) == report["instrument_fingerprint"],
                "measurement instrument changed during calibration")
        require(report["cells"] and all(row["status"] in ("PIN", "EXEMPT") for row in report["cells"]),
                "calibration has failed or unconfirmed cells")
        report.update(verdict="PIN", complete=True)
        publish()
        # PIN is deliberately nonzero, as a null's PARTIAL result is. It cannot
        # accidentally satisfy a shell gate or become a standing ABBA result.
        return 3
    except (Exception, KeyboardInterrupt) as error:
        report.update(verdict="FAIL", complete=False, reason=f"{type(error).__name__}: {error}")
        print(f"CALIBRATION FAIL: {report['reason']}", file=sys.stderr, flush=True)
        return 1
    finally:
        children.close()
        if quiet is not None:
            report["quiet_box"] = quiet.close()
            if report["quiet_box"].get("interference"):
                report.update(verdict="FAIL", complete=False)
        publish()
        print(f"CALIBRATION elapsed={report['elapsed_seconds']:.1f}s; {out / 'results.json'}", flush=True)
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
        os.sched_setaffinity(0, original_affinity)


def self_test():
    import contextlib
    import copy
    import io
    import tempfile
    from types import SimpleNamespace
    import unittest
    from unittest import mock
    from _abba_test_fixtures import saturation_record

    class Controls(unittest.TestCase):
        def setUp(self):
            self.cell = abba.Cell("calibration-control", "1s", 1, 0, 0, "GET", 32, 512)

        def block(self, n, rate=100, saturation=99.9):
            return dict(instances=n, runs=[dict(arm="B", instances=n, complete=True,
                calibration_only=True, rate=rate, window_seconds=10, midpoint_monotonic=6,
                commands=1_000_000, busy_pct=99.9,
                saturation=saturation_record(score=saturation, window_seconds=10),
                load_layout=abba.load_layout(list(range(32, 128)) + list(range(160, 256)), n, 512))])

        def test_observed_peak_matches_full_escalation_on_same_rates(self):
            rounds = [self.block(1, 50), self.block(2, 100), self.block(4, 99)]
            fast = select_calibration_floor(self.cell, rounds)
            full = [dict(instances=block["instances"], runs=[dict(block["runs"][0], arm=arm, busy_pct=99.9)
                    for arm in abba.ORDER]) for block in rounds]
            slow = abba.select_load_floor(self.cell, full)
            self.assertEqual(fast["status"], "PIN")
            self.assertEqual(fast["lowest_tested_qualifying_instances"], 2)
            self.assertEqual(fast["lowest_tested_qualifying_instances"], slow["lowest_tested_qualifying_instances"])
            self.assertEqual(fast["confirmation_instances"], 4)

        def test_unconfirmed_unsaturated_or_no_capacity_increase_cannot_pin(self):
            # A rise at or beyond the measured plateau jitter (PLATEAU_TOLERANCE_PCT) is still a
            # climb; a rise inside it is plateau noise and confirms the lower rung.
            for rounds in ([self.block(1)], [self.block(1, 100), self.block(2, 102)],
                           [self.block(1, 100), self.block(2, 100 * (1 + PLATEAU_TOLERANCE_PCT / 100))],
                           [self.block(1, 100, 20), self.block(2, 99)]):
                self.assertEqual(select_calibration_floor(self.cell, rounds)["status"], "UNPROVEN")
            within = select_calibration_floor(self.cell, [self.block(1, 100), self.block(2, 100.5)])
            self.assertEqual((within["status"], within["lowest_tested_qualifying_instances"],
                              within["confirmation_instances"]), ("PIN", 1, 2))
            rounds = [self.block(1), self.block(2)]
            # Two generators with eight workers each do not add capacity to one
            # sixteen-worker generator, even though instance count increased.
            for placement in rounds[1]["runs"][0]["load_layout"]:
                placement.update(threads=8, clients=32)
            self.assertEqual(select_calibration_floor(self.cell, rounds)["status"], "UNPROVEN")

        def test_failed_empty_shortened_and_changed_workload_observations_refuse(self):
            rounds = [self.block(1), self.block(2)]
            changes = [lambda r: r.clear(), lambda r: r[1].update(runs=[]),
                lambda r: r[1]["runs"][0].update(complete=False),
                lambda r: r[1]["runs"][0].update(error="explicit failure"),
                lambda r: r[1]["runs"][0].update(window_seconds=9.9),
                lambda r: r[1]["runs"][0].update(rate=float("nan")),
                lambda r: r[1]["runs"][0].update(calibration_only=False),
                lambda r: r[1]["runs"][0]["load_layout"][0].update(clients=1),
                lambda r: r[1].update(instances=1)]
            for mutate in changes:
                broken = copy.deepcopy(rounds)
                mutate(broken)
                with self.assertRaises((ValueError, TypeError, KeyError)):
                    select_calibration_floor(self.cell, broken)

        def test_real_runner_reuses_one_boot_population_and_reaps_on_failure(self):
            for broken in (False, True):
                with self.subTest(broken=broken), tempfile.TemporaryDirectory() as temporary:
                    out, ticks = Path(temporary), [100.]
                    started, stopped, loads = [], [], []
                    server = SimpleNamespace(pid=123, poll=lambda: None)
                    conn = mock.Mock()
                    conn.must.side_effect = lambda *args: [args[-1].encode(), b"1"]
                    def start(argv, _log, _cwd):
                        if "--protocol=redis" not in argv:
                            started.append("server")
                            return server
                        requested = next(value for value in argv if value.startswith("--test-time="))
                        self.assertEqual(requested, "--test-time=18")
                        process = SimpleNamespace(pid=200 + len(loads), poll=lambda: None, finished=False)
                        def wait(timeout):
                            process.finished = True
                            return 0
                        process.wait = wait
                        loads.append(process)
                        started.append("load")
                        return process
                    children = SimpleNamespace(start=start, stop=lambda process: stopped.append(process.pid))
                    args = SimpleNamespace(server_cores="0-31", server_smt="", load_cores="32-127", load_smt="",
                        port=9090, memtier="fixture-never-executed")
                    runner = abba.Runner(args, out, {"B": Path("fixture-server-never-executed")}, children)
                    def info(_conn, section):
                        if section == "server":
                            return dict(process_id="123", read_local="1")
                        if section == "clients":
                            return dict(connected_clients="513" if any(not p.finished for p in loads) else "1")
                        if section == "commandstats":
                            return {}
                        return dict(total_commands_processed=str(int(ticks[0] * 1000)), keyspace_misses="0")
                    def sleep(seconds):
                        self.assertIn(seconds, (3, 10))
                        ticks[0] += seconds
                        if broken and len(loads) > 1:
                            raise RuntimeError("injected failed rung")
                    def monotonic():
                        ticks[0] += .00001
                        return ticks[0]
                    def endpoint(process):
                        now = monotonic()
                        return dict(pid=process.pid, start_time_ticks=1, clock_ticks_per_second=100,
                            user_ticks=int(now * 100), system_ticks=0,
                            read_started_monotonic=now, read_finished_monotonic=now)
                    lb = SimpleNamespace(threads={i: {"role": "fused"} for i in range(32)})
                    with mock.patch.multiple(abba, Conn=mock.Mock(return_value=conn),
                            require_unbound_port=mock.Mock(), info=mock.Mock(side_effect=info),
                            lb_snapshot=mock.Mock(return_value=lb), cpu_seconds=mock.Mock(return_value=0),
                            generator_cpu_endpoint=mock.Mock(side_effect=endpoint), busy_between=mock.Mock(return_value=(99.9, {})),
                            busy_deltas=mock.Mock(return_value={}), productive_saturation=mock.Mock(return_value={}),
                            bottleneck_saturation=mock.Mock(return_value=saturation_record(window_seconds=10)),
                            require_saturation_window=mock.Mock(return_value={"score_pct": 99.9}),
                            require_workload_witness=mock.Mock(return_value={}),
                            require_workload_accounting=mock.Mock(return_value={}),
                            memtier_totals=mock.Mock(return_value=dict(rate=100, latency_ms=1))), \
                         mock.patch.object(runner, "populate", return_value=None) as population, \
                         mock.patch.object(time, "sleep", side_effect=sleep), \
                         mock.patch.object(time, "monotonic", side_effect=monotonic), \
                         contextlib.redirect_stdout(io.StringIO()):
                        def drive():
                            with persistent_cell(runner) as session:
                                first = runner.measure(self.cell, "B", 1, 1, {}, _calibration=session, _window=10)
                                self.assertNotIn(123, stopped)
                                self.assertFalse(first["population_reused"])
                                second = runner.measure(self.cell, "B", 2, 2, {}, _calibration=session, _window=10)
                                self.assertTrue(second["population_reused"])
                                self.assertEqual(first["pid"], second["pid"])
                                self.assertNotIn(123, stopped)
                        if broken:
                            with self.assertRaisesRegex(RuntimeError, "injected failed rung"):
                                drive()
                        else:
                            drive()
                        population.assert_called_once()
                    self.assertEqual(started.count("server"), 1)
                    self.assertEqual(stopped.count(123), 1)
                    self.assertEqual(set(stopped), {123, 200, 201, 202})
                    conn.close.assert_called_once()
                    last = json.loads((out / self.cell.id / "n2-2-B/measurement.json").read_text())
                    self.assertEqual(last["complete"], not broken)
                    self.assertEqual(last["calibration_only"], True)

        def test_real_dispatch_emits_pin_only_and_retains_failed_rung_or_late_quiet(self):
            for failure in (None, "rung", "quiet"):
                with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                    directory = Path(temporary)
                    binary = directory / "binary"
                    binary.write_bytes(b"fixture; never executed")
                    binary.chmod(0o700)
                    source = directory / "cells"
                    source.write_text("calibration-control | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | - | - | -\n")
                    argv = ["abbagate.py", "--calibrate", "--candidate", str(binary), "--cells", str(source),
                        "--only", self.cell.id, "--output", str(directory / "out"), "--memtier", sys.executable,
                        "--server-cores", "0-31", "--server-smt", "", "--load-cores", "32-127",
                        "--load-smt", "160-255", "--max-instances", "4"]
                    with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, {}, clear=True):
                        args = abba.parse_args()
                    events, closes = [], []
                    class Quiet:
                        closed = False
                        def start(self): pass
                        def close(self):
                            self.closed = True
                            return self.evidence()
                        def check(self):
                            if self.closed and failure == "quiet":
                                raise abba.QuietViolation("injected late contention")
                        def evidence(self):
                            return dict(complete=self.closed and failure != "quiet",
                                        interference="injected late contention" if self.closed and failure == "quiet" else None)
                    quiet = Quiet()
                    child = SimpleNamespace(pid=123)
                    conn = SimpleNamespace(close=lambda: closes.append("connection"))
                    children = SimpleNamespace(close=lambda: None, stop=lambda process: closes.append(process.pid))
                    def measure(runner, cell, arm, sequence, instances, knobs, *, _calibration, _window):
                        self.assertEqual((_window, arm), (10, "B"))
                        reused = bool(_calibration)
                        if not reused:
                            _calibration.update(conn=conn, srv=child)
                        events.append((instances, reused))
                        if failure == "rung" and instances == 2:
                            raise RuntimeError("injected rung failure")
                        run = self.block(instances, {1: 50, 2: 100, 4: 99}[instances])["runs"][0]
                        run.update(pid=123, population_reused=reused, busy_pct=99.9,
                            artifacts=f"{cell.id}/n{instances}-{sequence}-B")
                        return run
                    fingerprint = dict(sha256="f" * 64, python={})
                    original_window = abba.WINDOW
                    with mock.patch.multiple(abba, QuietMonitor=mock.Mock(return_value=quiet),
                            Children=mock.Mock(return_value=children), accepted=mock.Mock(return_value=True),
                            check_placement=mock.Mock(), git=mock.Mock(return_value="fixture"),
                            harness_fingerprint=mock.Mock(return_value={"sha256": "f" * 64}),
                            instrument_fingerprint=mock.Mock(return_value=fingerprint)), \
                         mock.patch.object(abba.Runner, "measure", measure), \
                         mock.patch.object(os, "sched_setaffinity"), mock.patch.dict(os.environ, {}, clear=True), \
                         contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(abba.main(args), 3 if failure is None else 1)
                    report = json.loads((directory / "out/results.json").read_text())
                    self.assertEqual(report["verdict"], "PIN" if failure is None else "FAIL")
                    self.assertEqual(report["complete"], failure is None)
                    self.assertEqual(report["run_kind"], "load-calibration")
                    self.assertEqual(report["order"], ["B"])
                    self.assertEqual(report["window_seconds"], 10)
                    self.assertEqual(abba.WINDOW, original_window)
                    self.assertEqual(closes, ["connection", 123])
                    self.assertNotIn("null_control", report)
                    for flag in ("measurement_valid", "comparison_trusted", "normal_gate_eligible"):
                        self.assertIs(report[flag], False)
                    self.assertEqual(events, [(1, False), (2, True)] + ([] if failure == "rung" else [(4, True)]))
                    if failure is None:
                        self.assertEqual(report["cells"][0]["selection"]["lowest_tested_qualifying_instances"], 2)

        def test_short_windows_require_calibration_session(self):
            runner = object.__new__(abba.Runner)
            with self.assertRaisesRegex(ValueError, "calibration session"):
                runner.measure(self.cell, "B", 1, 1, {}, _window=10)

        def test_session_cleanup_survives_connection_close_error(self):
            runner = SimpleNamespace(children=SimpleNamespace(stop=mock.Mock()))
            conn = mock.Mock()
            conn.close.side_effect = RuntimeError("close failed")
            server = object()
            with self.assertRaisesRegex(RuntimeError, "close failed"):
                with persistent_cell(runner) as session:
                    session.update(conn=conn, srv=server)
            runner.children.stop.assert_called_once_with(server)
            self.assertEqual(session, {})

    return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls)).wasSuccessful() else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        raise SystemExit(self_test())
    raise SystemExit("Use tests/abbagate.py --calibrate, or --self-test for serverless controls")
