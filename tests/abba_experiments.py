#!/usr/bin/env python3
"""Preregistered, on-demand ABBA experiments for gate measurement cost.

One selected, already pinned cell runs four blocks in this fixed order: 20-second
wire/wire null, 10-second wire/wire null, 20-second wire/snapshot comparison,
20-second snapshot/snapshot null. Every measurement and unsuccessful block is
retained. No best repeat is selected, no default changes, and a single-cell
experiment cannot certify the rest of the regression matrix.

The comparison binary is byte-identical in both arms. Snapshot SAVE runs only on
a separate unscored priming boot; it never changes a scored wire arm's layout.
The rate window is central WINDOW seconds; memtier latency histograms cover
WARMUP+WINDOW+TAIL (28/18 seconds here), which is reported separately.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import json
import math
import os
from pathlib import Path
import shutil
import signal
import statistics
import sys
import tempfile
import time

import abbagate as abba
from abba_workloads import LONG_BYTES, LONG_KEYS, sample_long_cost
from gateplan import cpu_string, default_physical, permitted_cpus, read_topology


BLOCKS = (("wire20_null", 20, "wire", "wire"),
          ("wire10_null", 10, "wire", "wire"),
          ("wire_snapshot20", 20, "wire", "snapshot"),
          ("snapshot20_null", 20, "snapshot", "snapshot"))
BASE_RUNNER = abba.Runner
GEOMETRY_KEYS = ("server_cores", "server_smt", "load_cores", "load_smt")


class SnapshotReady(Exception):
    """The unscored priming boot completed population/SAVE, before load generators."""


class PrimingRunner(BASE_RUNNER):
    snapshot = None

    def populate(self, cell, arm, conn, folder):
        super().populate(cell, arm, conn, folder)
        filename = conn.must("CONFIG", "GET", "dbfilename")[1].decode()
        if Path(filename).name != filename:
            raise RuntimeError("snapshot filename is not confined to our data directory")
        if conn.must("SAVE") != b"OK":
            raise RuntimeError("unscored snapshot SAVE failed")
        self.snapshot = folder / filename
        if not self.snapshot.is_file() or not self.snapshot.stat().st_size:
            raise RuntimeError("SAVE did not produce a snapshot")
        raise SnapshotReady("unscored setup complete; no measurement window or load was started")


class ExperimentRunner(BASE_RUNNER):
    def __init__(self, *args, snapshot, population_by_arm, attempts, **kwargs):
        super().__init__(*args, **kwargs)
        self.snapshot = snapshot
        self.population_by_arm = population_by_arm
        self.attempts = attempts

    def prepare_data(self, cell, arm, folder):
        if self.population_by_arm[arm] == "snapshot":
            # Default dbfilename is explicitly checked on the priming boot. Copy
            # into this measurement's owned directory before its server starts.
            shutil.copyfile(self.snapshot, folder / self.snapshot.name)

    def populate(self, cell, arm, conn, folder):
        if self.population_by_arm[arm] == "wire":
            return super().populate(cell, arm, conn, folder)
        expected = abba.KEYS + (LONG_KEYS if cell.op == "REORDER" else 0)
        if conn.must("DBSIZE") != expected:
            raise RuntimeError(f"snapshot restored wrong key count; expected exactly {expected}")
        for number in (1, abba.KEYS):
            value = conn.must("GET", f"memtier-{number}")
            if not isinstance(value, bytes) or len(value) != 64:
                raise RuntimeError("snapshot did not retain the populated short-key namespace")
        result = {"method": "snapshot", "keys": expected,
                  "snapshot_sha256": abba.sha256(self.snapshot)}
        if cell.op == "REORDER":
            for number in (1, LONG_KEYS):
                if conn.must("BITCOUNT", f"blocker:memtier-{number}") != LONG_BYTES * 8:
                    raise RuntimeError("snapshot did not retain the long-blocker namespace")
            # Probe service cost without rewriting restored values. Re-populating
            # the long strings here would erase the layout difference being tested.
            result["sampled_handler_usec"] = sample_long_cost(conn)
        return result

    def measure(self, cell, arm, sequence, instances, knobs):
        self.attempts.append({"arm": arm, "sequence": sequence, "instances": instances,
                              "window_seconds": abba.WINDOW,
                              "population": self.population_by_arm[arm]})
        return super().measure(cell, arm, sequence, instances, knobs)


def default_geometry():
    topology = read_topology()
    available = permitted_cpus(topology)
    physical = default_physical(available, topology)
    if len(physical) <= 32:
        raise ValueError("experiment default needs 32 physical server cores plus separate load cores")
    server, load = physical[:32], physical[32:]
    load_smt = sorted({sibling for core in load for sibling in topology[core]
                       if sibling in available and sibling not in load})
    return {"server_cores": cpu_string(server), "server_smt": "",
            "load_cores": cpu_string(load), "load_smt": cpu_string(load_smt)}


def arguments_for_block(args, fixture, output):
    return argparse.Namespace(self_test=False, list_cells=False, subset="full", only="",
        candidate=fixture / "candidate", reference_binary=fixture / "reference",
        cells=fixture / "cell.txt", bench_bins=fixture, build_reference=0,
        server_cores=args.server_cores, server_smt=args.server_smt,
        load_cores=args.load_cores, load_smt=args.load_smt,
        ports=args.ports, port=args.port, memtier=args.memtier, output=output,
        escalate=False, max_instances=16)


def prime_snapshot(args, fixture, output, cell):
    children = abba.Children()
    block_args = arguments_for_block(args, fixture, output)
    block_args.port, _ = abba.select_port(block_args.ports, block_args.port)
    runner = PrimingRunner(block_args, output, {"A": fixture / "reference", "B": fixture / "candidate"}, children)
    try:
        binary = fixture / "candidate"
        support = {arm: {name: abba.accepted(binary, name, value) for name, value in
                   (("thread-mode", "1s"), ("read-local", 0), ("overlap", 0), ("reorder", 0),
                    ("x-overlap", 0), ("x-ex-sched", 0))} for arm in ("A", "B")}
        plans, notes = abba.knob_plan(cell, support)
        try:
            # Reuse the real boot/config/population code. SnapshotReady exits before
            # the generator loop; the priming measurement.json remains unscored.
            runner.measure(cell, "A", 0, cell.instances if cell.depth > 1 else 1, plans["A"])
        except SnapshotReady:
            pass
        else:
            raise RuntimeError("snapshot priming reached a scored measurement unexpectedly")
        if runner.snapshot is None or runner.snapshot.name != "dump.tomo":
            raise RuntimeError("priming snapshot does not match the normal boot's default dbfilename")
        snapshot = fixture / "dump.tomo"
        shutil.copyfile(runner.snapshot, snapshot)
        return snapshot, {"path": str(snapshot), "sha256": abba.sha256(snapshot),
                          "bytes": snapshot.stat().st_size, "scored": False,
                          "source": str(runner.snapshot), "notes": notes}
    finally:
        children.close()


def run_block(args, fixture, output, cell, snapshot, specification):
    name, window, method_a, method_b = specification
    attempts = []
    original_runner, original_window = abba.Runner, abba.WINDOW
    abba.WINDOW = window
    abba.Runner = lambda *a, **kw: ExperimentRunner(*a, snapshot=snapshot,
        population_by_arm={"A": method_a, "B": method_b}, attempts=attempts, **kw)
    try:
        rc = abba.main(arguments_for_block(args, fixture, output / name))
    finally:
        abba.Runner, abba.WINDOW = original_runner, original_window
    report = json.loads((output / name / "results.json").read_text())
    if "InterruptedError" in report.get("reason", "") or "KeyboardInterrupt" in report.get("reason", ""):
        raise InterruptedError(report["reason"])
    return describe_block(cell, specification, report, attempts, rc)


def describe_block(cell, specification, report, attempts, rc):
    name, window, method_a, method_b = specification
    result = {"name": name, "window_seconds": window, "population_A": method_a,
              "population_B": method_b, "null": method_a == method_b,
              "attempts": attempts, "exit_code": rc, "abba": report,
              "status": "UNTESTABLE", "reasons": []}
    rows = report.get("cells", [])
    rounds = rows[0].get("rounds", []) if len(rows) == 1 else []
    runs = rounds[0].get("runs", []) if len(rounds) == 1 else []
    result["attempted_measurements"] = len(attempts)
    result["completed_measurements"] = len(runs)
    expected_n = cell.instances if cell.depth > 1 else 1
    if len(attempts) != 4 or [a["arm"] for a in attempts] != list(abba.ORDER):
        result["reasons"].append("real measurement loop did not attempt exactly one ABBA block")
    if len(runs) != 4 or any(a["instances"] != expected_n for a in attempts):
        result["reasons"].append("missing measurements or changed pinned load")
    if not runs or len(runs) != 4:
        result["reasons"].append(report.get("reason") or (rows[0].get("reason", "incomplete block") if rows else "no cell ran"))
        return result
    if any(not run.get("complete") or not math.isfinite(run.get("window_seconds", float("nan")))
           or run["window_seconds"] < window for run in runs):
        result["reasons"].append("measurement window was missing, incomplete, or shorter than requested")
    if cell.depth > 1 and any(run["busy_pct"] < abba.BUSY_FLOOR for run in runs):
        result["reasons"].append("unsaturated; re-pin the cell with --escalate before comparing methods")
    if cell.metric == "p999_ms" and any(
            run.get("histogram_window_seconds") != abba.WARMUP + window + abba.TAIL
            or not all(math.isfinite(run.get(key, float("nan"))) and run[key] > 0
                       for key in ("p999_ms", "long_p999_ms")) for run in runs):
        result["reasons"].append("missing tail histogram or wrong histogram window")
    metrics = ["rate"] + ([cell.metric] if cell.metric != "rate" else [])
    if cell.metric == "p999_ms":
        metrics.append("long_p999_ms")
    try:
        result["metrics"] = {metric: abba.paired(runs, metric) for metric in metrics}
    except (ValueError, KeyError) as exc:
        result["reasons"].append(f"invalid measurement metric: {exc}")
        return result
    if any(pair[f"{arm}_spread_pct"] > abba.MAX_SPREAD for pair in result["metrics"].values()
           for arm in ("reference", "candidate")):
        result["reasons"].append("unstable measurement exceeds the instrument validity boundary")
    result["populate_seconds"] = {arm: statistics.mean(run["populate_seconds"] for run in runs if run["arm"] == arm)
                                  for arm in ("A", "B")}
    result["minimum_busy_pct"] = min(run["busy_pct"] for run in runs)
    result["actual_window_seconds"] = [run["window_seconds"] for run in runs]
    result["elapsed_seconds"] = report["elapsed_seconds"]
    result["latency_window_seconds"] = abba.WARMUP + window + abba.TAIL
    if not result["reasons"]:
        result["status"] = "PASS" if report["verdict"] == "PASS" else "FAIL"
    return result


def evaluate(blocks):
    by_name = {block["name"]: block for block in blocks}
    output = {"defaults_changed": False, "scope": "selected cell only; full-matrix null still required"}
    required = [spec[0] for spec in BLOCKS]
    if any(name not in by_name for name in required) or any(block["status"] == "UNTESTABLE" for block in blocks):
        return {**output, "window10": {"status": "UNTESTABLE"}, "snapshot": {"status": "UNTESTABLE"}}
    wire20, wire10, comparison, snapshot20 = [by_name[name] for name in required]
    nulls = (wire20, wire10, snapshot20)
    output["observed_null_error_pct"] = {block["name"]: {
        metric: abs(pair["delta_pct"]) for metric, pair in block["metrics"].items()} for block in nulls}
    if any(block["status"] != "PASS" for block in nulls):
        reason = "the comparison instrument failed at least one byte-identical null"
        return {**output, "window10": {"status": "UNTESTABLE", "reason": reason},
                "snapshot": {"status": "UNTESTABLE", "reason": reason}}
    # Both arm spreads and every scored latency class must hold up. Rate stability
    # alone cannot authorize shortening a latency histogram from 28 to 18 seconds.
    window_checks = {metric: all(wire10["metrics"][metric][f"{arm}_spread_pct"] <=
                                pair[f"{arm}_spread_pct"] for arm in ("reference", "candidate"))
                     for metric, pair in wire20["metrics"].items()}
    output["window10"] = {"status": "MEETS_SELECTED_CELL_CRITERIA" if all(window_checks.values()) else "REJECT",
                           "spread_did_not_degrade": window_checks}
    snapshot_checks = {}
    for metric, pair in comparison["metrics"].items():
        null_error = max(abs(block["metrics"][metric]["delta_pct"]) for block in (wire20, snapshot20))
        resolution = max(null_error, wire20["metrics"][metric]["reference_spread_pct"],
                         snapshot20["metrics"][metric]["reference_spread_pct"])
        repeatable = all(snapshot20["metrics"][metric][f"{arm}_spread_pct"] <=
                         wire20["metrics"][metric][f"{arm}_spread_pct"] for arm in ("reference", "candidate"))
        snapshot_checks[metric] = {"delta_pct": pair["delta_pct"], "null_resolution_pct": resolution,
                                   "within_measured_resolution": abs(pair["delta_pct"]) <= resolution,
                                   "spread_did_not_degrade": repeatable}
    okay = all(check["within_measured_resolution"] and check["spread_did_not_degrade"]
               for check in snapshot_checks.values())
    output["snapshot"] = {"status": "MEETS_SELECTED_CELL_CRITERIA" if okay else "REJECT",
                           "metrics": snapshot_checks}
    return output


def tables(report):
    print("\n| Block | Rate window | A/B Mops/s | Delta % | Rate spread A/B % | Populate A/B s | Status |")
    print("|---|---:|---:|---:|---:|---:|---|")
    for block in report.get("blocks", []):
        if "metrics" not in block:
            print(f"| {block['name']} | {block['window_seconds']}s | — | — | — | — | {block['status']} |")
            continue
        p = block["metrics"]["rate"]
        pop = block.get("populate_seconds", {"A": float("nan"), "B": float("nan")})
        print(f"| {block['name']} | {block['window_seconds']}s | {p['reference_mean']/1e6:.4f}/{p['candidate_mean']/1e6:.4f} | "
              f"{p['delta_pct']:+.4f} | {p['reference_spread_pct']:.4f}/{p['candidate_spread_pct']:.4f} | "
              f"{pop['A']:.3f}/{pop['B']:.3f} | {block['status']} |")
    latency = [(block, metric, pair) for block in report.get("blocks", [])
               for metric, pair in block.get("metrics", {}).items() if metric != "rate"]
    if latency:
        print("\n| Block | Latency metric | Histogram/run window | A/B ms | Delta % | Spread A/B % |")
        print("|---|---|---:|---:|---:|---:|")
        for block, metric, p in latency:
            print(f"| {block['name']} | {metric} | {block['latency_window_seconds']}s | "
                  f"{p['reference_mean']:.5f}/{p['candidate_mean']:.5f} | {p['delta_pct']:+.4f} | "
                  f"{p['reference_spread_pct']:.4f}/{p['candidate_spread_pct']:.4f} |")
    print("\n" + json.dumps(report.get("evaluation", {}), indent=2))
    for block in report.get("blocks", []):
        for reason in block.get("reasons", []):
            print(f"UNTESTABLE {block['name']}: {reason}")
    print("No defaults changed. All blocks and their measurements are retained.")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-binary", type=Path, default=abba.ROOT / "build/tomokv")
    parser.add_argument("--cells", type=Path, default=abba.ROOT / "tests/headline_cells.txt")
    parser.add_argument("--cell", default="h12")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--memtier", default="memtier_benchmark")
    for name in ("server-cores", "server-smt", "load-cores", "load-smt"):
        parser.add_argument("--" + name, default=None)
    parser.add_argument("--ports", default="8700-8700")
    parser.add_argument("--port", type=int)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--plan-only", action="store_true", help="show geometry/block plan without starting processes")
    return parser.parse_args(argv)


def self_test():
    import contextlib
    import io
    import unittest
    from unittest import mock
    from types import SimpleNamespace

    class Experiments(unittest.TestCase):
        def test_complete_driver_calls_real_abba_loop_sixteen_times(self):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                binary = directory / "input-binary"
                binary.write_bytes(b"fixture binary identity; never executed")
                binary.chmod(0o700)
                source = directory / "cells"
                source.write_text("h12 | 1s | rl=1 | ov=0 | ro=1 | SET | p32 | 512 | - | - | 4\n")
                args = parse_args(["--candidate-binary", str(binary), "--cells", str(source),
                                   "--output", str(directory / "experiment"), "--memtier", sys.executable])
                calls = []
                def measure(runner, cell, arm, sequence, instances, knobs):
                    calls.append((abba.WINDOW, arm, sequence, instances, runner.population_by_arm[arm]))
                    return {"arm": arm, "rate": 100, "latency_ms": 1, "busy_pct": 99.9,
                            "complete": True, "window_seconds": abba.WINDOW + .001,
                            "populate_seconds": 1, "wall_seconds": abba.WINDOW + 8}
                def prime(a, fixture, out, cell):
                    snapshot = fixture / "dump.tomo"
                    snapshot.write_bytes(b"snapshot fixture")
                    return snapshot, {"scored": False, "sha256": abba.sha256(snapshot)}
                def reference(a, out):
                    return a.reference_binary, {"source": "test", "commit": "unverified",
                                                "sha256": abba.sha256(a.reference_binary)}
                geometry = {"server_cores": "0-31", "server_smt": "",
                            "load_cores": "32-127", "load_smt": "160-255"}
                original_window = abba.WINDOW
                with mock.patch.object(BASE_RUNNER, "measure", measure), \
                     mock.patch(__name__ + ".prime_snapshot", side_effect=prime), \
                     mock.patch(__name__ + ".default_geometry", return_value=geometry), \
                     mock.patch.object(abba, "check_placement"), \
                     mock.patch.object(abba, "accepted", return_value=True), \
                     mock.patch.object(abba, "resolve_reference", side_effect=reference), \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.dict(sys.modules, {"gate_quiet": SimpleNamespace(
                         assert_quiet=mock.Mock(return_value={"status": "quiet"}))}), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main(args), 0)
                self.assertEqual(abba.WINDOW, original_window)
                expected = [(window, arm, sequence, 4, method_a if arm == "A" else method_b)
                            for _, window, method_a, method_b in BLOCKS
                            for sequence, arm in enumerate(abba.ORDER, 1)]
                self.assertEqual(calls, expected)
                report = json.loads((directory / "experiment/experiment.json").read_text())
                self.assertEqual(len(report["blocks"]), 4)
                self.assertEqual([len(block["attempts"]) for block in report["blocks"]], [4] * 4)
                self.assertFalse(report["evaluation"]["defaults_changed"])
                for block in report["blocks"]:
                    self.assertEqual(block["abba"]["reference"]["sha256"], block["abba"]["candidate"]["sha256"])

        def test_contended_box_refuses_before_snapshot_priming(self):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                source = directory / "cells"
                source.write_text("h12 | 1s | rl=1 | ov=0 | ro=1 | SET | p32 | 512 | - | - | 4\n")
                args = parse_args(["--cells", str(source), "--output", str(directory / "out"),
                                   "--server-cores", "0-31", "--server-smt", "",
                                   "--load-cores", "32-127", "--load-smt", "160-255"])
                quiet = mock.Mock(side_effect=RuntimeError("foreign CPU activity"))
                with mock.patch.dict(sys.modules, {"gate_quiet": SimpleNamespace(assert_quiet=quiet)}), \
                     mock.patch.object(abba, "check_placement"), \
                     mock.patch(__name__ + ".default_geometry", side_effect=AssertionError("explicit geometry")), \
                     mock.patch(__name__ + ".prime_snapshot") as prime, \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                     contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(main(args), 1)
                prime.assert_not_called()
                quiet.assert_called_once_with(list(range(32)), list(range(32, 128)) + list(range(160, 256)),
                                              own_root_pid=os.getpid())
                report = json.loads((directory / "out/experiment.json").read_text())
                self.assertIn("foreign CPU activity", report["error"])
                self.assertEqual(report["blocks"], [])

        def test_priming_saves_and_exits_before_measurement(self):
            with tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                conn = mock.Mock()
                def command(*args):
                    if args[:2] == ("CONFIG", "GET"):
                        return [b"dbfilename", b"dump.tomo"]
                    if args == ("SAVE",):
                        (folder / "dump.tomo").write_bytes(b"saved")
                        return b"OK"
                    raise AssertionError(args)
                conn.must.side_effect = command
                runner = object.__new__(PrimingRunner)
                with mock.patch.object(BASE_RUNNER, "populate", return_value=None) as population:
                    with self.assertRaises(SnapshotReady):
                        runner.populate(None, "A", conn, folder)
                population.assert_called_once()
                self.assertEqual(conn.must.call_args_list[-1].args, ("SAVE",))
                self.assertEqual(runner.snapshot.read_bytes(), b"saved")

        def test_snapshot_hook_never_repopulates_or_saves(self):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                snapshot = directory / "dump.tomo"
                snapshot.write_bytes(b"saved state")
                folder = directory / "measurement"
                folder.mkdir()
                runner = object.__new__(ExperimentRunner)
                runner.snapshot = snapshot
                runner.population_by_arm = {"A": "wire", "B": "snapshot"}
                runner.prepare_data(None, "B", folder)
                self.assertEqual((folder / "dump.tomo").read_bytes(), snapshot.read_bytes())
                conn = mock.Mock()
                conn.must.side_effect = lambda *a: abba.KEYS if a == ("DBSIZE",) else b"x" * 64
                cell = abba.Cell("h12", "1s", 1, 0, 1, "SET", 32, 512, instances=4)
                with mock.patch.object(BASE_RUNNER, "populate", side_effect=AssertionError("snapshot was repopulated")):
                    result = runner.populate(cell, "B", conn, folder)
                self.assertEqual(result["method"], "snapshot")
                self.assertTrue(all(call.args[0] in ("DBSIZE", "GET") for call in conn.must.call_args_list))
                conn.must.side_effect = lambda *a: abba.KEYS - 1
                with self.assertRaisesRegex(RuntimeError, "wrong key count"):
                    runner.populate(cell, "B", conn, folder)

        def test_default_geometry_reserves_server_siblings(self):
            topology = {cpu: frozenset((cpu % 128, cpu % 128 + 128)) for cpu in range(256)}
            with mock.patch(__name__ + ".read_topology", return_value=topology), \
                 mock.patch(__name__ + ".permitted_cpus", return_value=set(range(256))):
                geometry = default_geometry()
            self.assertEqual(geometry, {"server_cores": "0-31", "server_smt": "",
                                       "load_cores": "32-127", "load_smt": "160-255"})

        def test_unsaturated_and_missing_windows_are_untestable(self):
            cell = abba.Cell("h", "1s", 0, 0, 0, "GET", 32, 512, instances=4)
            attempts = [{"arm": arm, "instances": 4} for arm in abba.ORDER]
            runs = [{"arm": arm, "rate": 100, "complete": True, "window_seconds": 20,
                     "busy_pct": 90, "populate_seconds": 1} for arm in abba.ORDER]
            report = {"cells": [{"rounds": [{"runs": runs}]}], "elapsed_seconds": 120, "verdict": "PASS"}
            block = describe_block(cell, BLOCKS[0], report, attempts, 0)
            self.assertEqual(block["status"], "UNTESTABLE")
            self.assertTrue(any("unsaturated" in why for why in block["reasons"]))
            for run in runs:
                run["busy_pct"] = 99.9
                run["window_seconds"] = 10
            block = describe_block(cell, BLOCKS[0], report, attempts, 0)
            self.assertEqual(block["status"], "UNTESTABLE")
            self.assertTrue(any("window" in why for why in block["reasons"]))

        def test_failed_null_prevents_adoption(self):
            blocks = [{"name": spec[0], "status": "FAIL" if index == 0 else "PASS",
                       "metrics": {"rate": {"delta_pct": -.7}}} for index, spec in enumerate(BLOCKS)]
            result = evaluate(blocks)
            self.assertEqual(result["window10"]["status"], "UNTESTABLE")
            self.assertEqual(result["snapshot"]["status"], "UNTESTABLE")
            self.assertFalse(result["defaults_changed"])

        def test_noisier_latency_cannot_hide_behind_stable_rate(self):
            pair = {"delta_pct": 0, "reference_spread_pct": .1, "candidate_spread_pct": .1}
            blocks = [{"name": spec[0], "status": "PASS",
                       "metrics": {"rate": dict(pair), "p999_ms": dict(pair)}} for spec in BLOCKS]
            blocks[1]["metrics"]["p999_ms"]["reference_spread_pct"] = .2
            result = evaluate(blocks)
            self.assertEqual(result["window10"]["status"], "REJECT")
            self.assertTrue(result["window10"]["spread_did_not_degrade"]["rate"])
            self.assertFalse(result["window10"]["spread_did_not_degrade"]["p999_ms"])

    return 0 if unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(Experiments)).wasSuccessful() else 1


def main(args):
    if any(getattr(args, key) is None for key in GEOMETRY_KEYS):
        for key, value in default_geometry().items():
            if getattr(args, key) is None:
                setattr(args, key, value)
    abba.check_placement(abba.cpus(args.server_cores), abba.cpus(args.load_cores),
                         abba.cpus(args.server_smt), abba.cpus(args.load_smt))
    args.port, _ = abba.select_port(args.ports, args.port)
    matches = [cell for cell in abba.read_cells(args.cells) if cell.id == args.cell]
    if len(matches) != 1:
        raise ValueError("--cell must select exactly one cell from the source")
    cell = matches[0]
    if cell.depth > 1 and not cell.instances:
        raise ValueError("selected cell has no measured pin; calibrate with abbagate --escalate first")
    plan = {"cell": asdict(cell), "geometry": {key: getattr(args, key) for key in GEOMETRY_KEYS},
            "blocks": BLOCKS, "scored_measurements": 16, "unscored_snapshot_priming_boots": 1}
    if args.plan_only:
        print(json.dumps(plan, indent=2))
        return 0
    output = (args.output or abba.ROOT / "build" / f"abba-experiments-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}").resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "plan": plan, "blocks": [], "defaults_changed": False}
    started = time.monotonic()
    original_affinity = os.sched_getaffinity(0)
    old_handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
    def interrupted(signum, frame):
        raise InterruptedError(f"experiment interrupted by signal {signum}")
    for sig in old_handlers:
        signal.signal(sig, interrupted)
    try:
        # The priming boot precedes abbagate.main(), so it needs the same loud
        # quiet-box precondition. The ABBA runner monitors each scored block too.
        # Import lazily so --plan-only and serverless tests need no live monitor.
        from gate_quiet import assert_quiet
        report["quiet_before_priming"] = assert_quiet(
            abba.cpus(args.server_cores) + abba.cpus(args.server_smt),
            abba.cpus(args.load_cores) + abba.cpus(args.load_smt), own_root_pid=os.getpid())
        quiet_file = os.getenv("GATE_QUIET_FILE")
        if quiet_file:
            quiet = Path(quiet_file)
            age = time.time() - quiet.stat().st_mtime if quiet.exists() else -1
            if age < 60 * float(os.getenv("GATE_QUIET_MINUTES", "3")):
                raise RuntimeError("quiet-file precondition failed before unscored priming")
        os.sched_setaffinity(0, abba.cpus(args.load_cores) + abba.cpus(args.load_smt))
        args.memtier = shutil.which(args.memtier)
        if not args.memtier:
            raise ValueError("memtier executable not available")
        fixture = output / "fixture"
        fixture.mkdir()
        source = args.candidate_binary.resolve()
        if not source.is_file() or not os.access(source, os.X_OK):
            raise ValueError("candidate binary is not executable")
        digest = abba.sha256(source)
        for name in ("candidate", "reference"):
            shutil.copy2(source, fixture / name)
            if abba.sha256(fixture / name) != digest:
                raise RuntimeError("candidate changed while copying the byte-identical fixture")
        report["binary_fixture"] = {"source": str(source), "sha256": digest,
                                     "reference": str(fixture / "reference"), "candidate": str(fixture / "candidate")}
        source_row = next(line for line in args.cells.read_text().splitlines()
                          if line.split("|", 1)[0].strip() == cell.id)
        (fixture / "cell.txt").write_text(source_row + "\n")
        (output / "experiment.json").write_text(json.dumps(report, indent=2) + "\n")
        snapshot, report["snapshot"] = prime_snapshot(args, fixture, output / "unscored-prime", cell)
        for specification in BLOCKS:
            report["blocks"].append(run_block(args, fixture, output, cell, snapshot, specification))
            report["elapsed_seconds"] = time.monotonic() - started
            (output / "experiment.json").write_text(json.dumps(report, indent=2) + "\n")
        report["evaluation"] = evaluate(report["blocks"])
        return 0 if all(block["status"] == "PASS" for block in report["blocks"]) else 1
    except (Exception, KeyboardInterrupt) as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        print(f"ABBA EXPERIMENT UNTESTABLE: {report['error']}", file=sys.stderr)
        report["evaluation"] = {"window10": {"status": "UNTESTABLE"}, "snapshot": {"status": "UNTESTABLE"},
                                "defaults_changed": False}
        return 1
    finally:
        report["elapsed_seconds"] = time.monotonic() - started
        (output / "experiment.json").write_text(json.dumps(report, indent=2) + "\n")
        os.sched_setaffinity(0, original_affinity)
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
        tables(report)
        print(f"Artifacts: {output / 'experiment.json'}")


if __name__ == "__main__":
    args = parse_args()
    try:
        raise SystemExit(self_test() if args.self_test else main(args))
    except (ValueError, OSError) as exc:
        print(f"ABBA EXPERIMENT REFUSED: {exc}", file=sys.stderr)
        raise SystemExit(2)
