#!/usr/bin/env python3
"""Preregistered, on-demand ABBA experiments for gate measurement cost.

One selected, already pinned cell runs six preregistered blocks: fresh-wire
20s/10s nulls, a fresh/seeded-wire bridge, a seeded-wire 20s null, a matched-seed
wire/snapshot comparison, and a snapshot 20s null. Every measurement and unsuccessful block is
retained. No best repeat is selected, no default changes, and a single-cell
experiment cannot certify the rest of the regression matrix.

The comparison binary is byte-identical in both arms. Snapshot SAVE runs only on
two unscored priming boots: capture an empty snapshot, then boot from that image,
wire-populate and SAVE the full image. Scored seeded-wire arms also load the empty
image before wire population; ordinary fresh-wire arms remain a distinct method.
The bridge bundles seed and empty-loader effects; it is not a seed-only experiment.
The rate window is central WINDOW seconds; memtier latency histograms cover
WARMUP+WINDOW+TAIL (28/18 seconds here), which is reported separately.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
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


# Four original measurement purposes remain, plus two common-seed controls. The
# fresh baseline/10s pair comes first; the bridge connects that construction to the
# matched-seed pair. Every failed block remains evidence. Eight added measurements
# cost about four minutes at the owner's measured 30s/run, outside the normal gate.
BLOCKS = (("fresh20_null", 20, "fresh", "fresh"),
          ("fresh10_null", 10, "fresh", "fresh"),
          ("fresh_seeded20", 20, "fresh", "wire"),
          ("seeded20_null", 20, "wire", "wire"),
          ("seeded_snapshot20", 20, "wire", "snapshot"),
          ("snapshot20_null", 20, "snapshot", "snapshot"))
BASE_RUNNER = abba.Runner
GEOMETRY_KEYS = ("server_cores", "server_smt", "load_cores", "load_smt")


class SnapshotReady(Exception):
    """The unscored priming boot completed population/SAVE, before load generators."""


def snapshot_metadata(path):
    # Observe the server-written header; never manufacture a seed or snapshot.
    # Production snapshot_read_plan validates all frames/footer on the next boot.
    # Epoch/cut are retained but deliberately not compared: loading does not restore
    # SnapshotManager epoch/cut, and all benchmark keys have no expiration.
    with path.open("rb") as stream:
        header = stream.read(80)
    u32 = lambda offset: int.from_bytes(header[offset:offset + 4], "little")
    u64 = lambda offset: int.from_bytes(header[offset:offset + 8], "little")
    checksum = 1469598103934665603
    for byte in header[:64]:
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    if (len(header) != 80 or header[:8] != b"TOMOSNP\0" or u32(8) != 1 or
            u32(12) != 80 or checksum != u64(64) or not u32(16) or u32(20) > 1):
        raise RuntimeError(f"invalid/unsupported server-generated snapshot header: {path}")
    return {"path": str(path), "sha256": abba.sha256(path), "bytes": path.stat().st_size,
            "format": u32(8), "shards": u32(16), "hash_kind": u32(20),
            "hash_seed": u64(40), "sip_k0": u64(48), "sip_k1": u64(56),
            "epoch": u64(24), "cut_ms": u64(32), "raw_header_hex": header.hex()}


def seed_metadata(metadata):
    return {key: metadata[key] for key in
            ("format", "shards", "hash_kind", "hash_seed", "sip_k0", "sip_k1")}


@dataclass(frozen=True)
class SnapshotFixtures:
    empty: Path
    full: Path
    empty_metadata: dict
    full_metadata: dict

    def validate(self):
        for path, expected in ((self.empty, self.empty_metadata), (self.full, self.full_metadata)):
            if snapshot_metadata(path) != expected:
                raise RuntimeError("snapshot fixture changed after priming")
        if seed_metadata(self.empty_metadata) != seed_metadata(self.full_metadata):
            raise RuntimeError("empty/full snapshot hash seed metadata differ")


class PrimingRunner(BASE_RUNNER):
    def __init__(self, *args, empty_snapshot=None, capture_empty=False, **kwargs):
        super().__init__(*args, **kwargs)
        self.empty_snapshot, self.capture_empty = empty_snapshot, capture_empty
        self.snapshot = None

    def prepare_data(self, cell, arm, folder):
        if not self.capture_empty:
            if self.empty_snapshot is None:
                raise RuntimeError("full priming requires the captured empty snapshot")
            shutil.copyfile(self.empty_snapshot, folder / "dump.tomo")

    def populate(self, cell, arm, conn, folder):
        if conn.must("DBSIZE") != 0:
            raise RuntimeError("snapshot priming must start with exactly zero keys")
        if not self.capture_empty:
            super().populate(cell, arm, conn, folder)
        filename = conn.must("CONFIG", "GET", "dbfilename")[1].decode()
        if filename != "dump.tomo":
            raise RuntimeError("snapshot filename differs from the measured boot destination")
        if conn.must("SAVE") != b"OK":
            raise RuntimeError("unscored snapshot SAVE failed")
        self.snapshot = folder / filename
        snapshot_metadata(self.snapshot)
        raise SnapshotReady("unscored setup complete; no measurement window or load was started")


class ExperimentRunner(BASE_RUNNER):
    def __init__(self, *args, snapshot, population_by_arm, attempts, **kwargs):
        super().__init__(*args, **kwargs)
        self.snapshot = snapshot
        self.snapshot.validate()
        self.population_by_arm = population_by_arm
        self.attempts = attempts

    def population_environment(self):
        self.snapshot.validate()
        return {"population_by_arm": self.population_by_arm,
                "population_constructions": {"fresh": "fresh empty boot, then wire population; random boot seed",
                    "wire": "server-generated empty snapshot restore, then normal wire population",
                    "snapshot": "server-generated full snapshot restore; no wire repopulation"},
                "population_empty_snapshot_sha256": self.snapshot.empty_metadata["sha256"],
                "population_snapshot_sha256": self.snapshot.full_metadata["sha256"],
                "population_common_seed": seed_metadata(self.snapshot.empty_metadata),
                "population_seed_matched_arms": [arm for arm, method in self.population_by_arm.items() if method != "fresh"]}

    def prepare_data(self, cell, arm, folder):
        self.snapshot.validate()
        method = self.population_by_arm[arm]
        source = self.snapshot.full if method == "snapshot" else self.snapshot.empty if method == "wire" else None
        if method not in ("fresh", "wire", "snapshot"):
            raise RuntimeError(f"unknown population construction: {method}")
        if source is not None:
            # Both images are generated by SAVE on owned unscored boots. The empty
            # loader allocates no records, but is not claimed identical to a fresh
            # boot: main retains a load plan and owner clocks/size are initialized.
            # Full restore additionally retains its serialized shard sections for
            # the server lifetime and inserts records before IO-loop initialization.
            # These real startup/allocation effects belong in the measured method;
            # warmup is not evidence that either construction matches fresh wire.
            shutil.copyfile(source, folder / "dump.tomo")
            if abba.sha256(folder / "dump.tomo") != abba.sha256(source):
                raise RuntimeError("snapshot changed during the measurement's fixture copy")

    def populate(self, cell, arm, conn, folder):
        method = self.population_by_arm[arm]
        if method in ("fresh", "wire"):
            if conn.must("DBSIZE") != 0:
                raise RuntimeError(f"{method} population must start with exactly zero keys")
            original = super().populate(cell, arm, conn, folder)
            return {"method": method, "starting_construction": "empty-snapshot-then-wire" if method == "wire" else "fresh-then-wire",
                    "starting_keys": 0, "wire_population": original,
                    "common_seed": seed_metadata(self.snapshot.empty_metadata) if method == "wire" else None,
                    "empty_snapshot_sha256": self.snapshot.empty_metadata["sha256"] if method == "wire" else None}
        expected = abba.KEYS + (LONG_KEYS if cell.op == "REORDER" else 0)
        if conn.must("DBSIZE") != expected:
            raise RuntimeError(f"snapshot restored wrong key count; expected exactly {expected}")
        for number in (1, abba.KEYS):
            value = conn.must("GET", f"memtier-{number}")
            if not isinstance(value, bytes) or len(value) != 64:
                raise RuntimeError("snapshot did not retain the populated short-key namespace")
        result = {"method": "snapshot", "keys": expected,
                  "snapshot_sha256": self.snapshot.full_metadata["sha256"],
                  "common_seed": seed_metadata(self.snapshot.full_metadata)}
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
        escalate=False, max_instances=16, collect_null=0,
        null_result=fixture / "no-standing-experiment-control.json")


def prime_snapshot(args, fixture, output, cell):
    output.mkdir(parents=True)
    document = {"scored": False, "status": "INCOMPLETE", "boots": []}
    record_path = output / "priming.json"
    record_path.write_text(json.dumps(document, indent=2) + "\n")
    binary = fixture / "candidate"
    try:
        support = {arm: {name: abba.accepted(binary, name, value) for name, value in
                   (("thread-mode", "1s"), ("read-local", 0), ("overlap", 0), ("reorder", 0),
                    ("x-overlap", 0), ("x-ex-sched", 0))} for arm in ("A", "B")}
        plans, document["notes"] = abba.knob_plan(cell, support)
        snapshots, metadata = [], []
        for index, name in enumerate(("empty", "full")):
            row = {"kind": name, "status": "INCOMPLETE", "scored": False,
                   "starting_construction": "fresh-empty" if index == 0 else "captured-empty-restore-then-wire"}
            document["boots"].append(row)
            record_path.write_text(json.dumps(document, indent=2) + "\n")
            children = abba.Children()
            block_args = arguments_for_block(args, fixture, output / name)
            block_args.port, _ = abba.select_port(block_args.ports, block_args.port)
            runner = PrimingRunner(block_args, output / name,
                {"A": binary, "B": binary}, children,
                capture_empty=index == 0, empty_snapshot=snapshots[0] if snapshots else None)
            try:
                # SAVE doubles the live table's capacity, even on an empty store
                # (FlatStore::snapshot_prepare/mark). NEVER continue population on
                # the seed-capture boot. Restart from the empty image so full-fixture
                # priming follows exactly the same startup path as scored seeded wire.
                try:
                    runner.measure(cell, "A", 0, cell.instances if cell.depth > 1 else 1, plans["A"])
                except SnapshotReady:
                    pass
                else:
                    raise RuntimeError("snapshot priming reached a scored measurement unexpectedly")
                if runner.snapshot is None:
                    raise RuntimeError("priming did not produce a snapshot")
                source_metadata = snapshot_metadata(runner.snapshot)
                target = fixture / ("empty.tomo" if index == 0 else "dump.tomo")
                shutil.copyfile(runner.snapshot, target)
                captured = snapshot_metadata(target)
                if captured["sha256"] != source_metadata["sha256"]:
                    raise RuntimeError("snapshot changed while freezing priming output")
                snapshots.append(target)
                metadata.append(captured)
                row.update(snapshot=captured, source=str(runner.snapshot))
            except BaseException as error:
                row.update(status="FAIL", error=f"{type(error).__name__}: {error}")
                raise
            finally:
                # Finish the first owned process before constructing the second.
                children.close()
                record_path.write_text(json.dumps(document, indent=2) + "\n")
            row["status"] = "COMPLETE"
        images = SnapshotFixtures(*snapshots, *metadata)
        images.validate()
        document.update(status="COMPLETE", common_seed=seed_metadata(metadata[0]),
                        seed_metadata_equal=True)
        return images, document
    except BaseException as error:
        document.update(status="FAIL", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        record_path.write_text(json.dumps(document, indent=2) + "\n")


def run_block(args, fixture, output, cell, snapshot, specification):
    name, window, method_a, method_b = specification
    attempts = []
    original_runner, original_window = abba.Runner, abba.WINDOW
    abba.WINDOW = window
    abba.Runner = lambda *a, **kw: ExperimentRunner(*a, snapshot=snapshot,
        population_by_arm={"A": method_a, "B": method_b}, attempts=attempts, **kw)
    try:
        block_args = arguments_for_block(args, fixture, output / name)
        # Equal executable bytes are insufficient for wire-versus-snapshot: the store was built
        # differently. Only equal-method blocks collect null evidence; the method comparison
        # retains raw statistical facts and remains an explicitly untrusted gate diagnostic.
        block_args.collect_null = int(method_a == method_b)
        rc = abba.main(block_args)
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
    if report.get("measurement_valid") is False or report.get("quiet_box", {}).get("interference"):
        result["reasons"].append("quiet-box contention invalidated this comparison")
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
        if method_a == method_b:
            passed = report.get("null_control", {}).get("verdict") == "PASS"
        else:
            passed = report.get("statistical_verdict") == "PASS" and report.get("run_kind") == "comparison"
        result["status"] = "PASS" if passed else "FAIL"
    return result


def construction_checks(comparison, baseline, alternative):
    checks = {}
    for metric, pair in comparison["metrics"].items():
        null_error = max(abs(block["metrics"][metric]["delta_pct"]) for block in (baseline, alternative))
        resolution = max(null_error, baseline["metrics"][metric]["reference_spread_pct"],
                         alternative["metrics"][metric]["reference_spread_pct"])
        repeatable = all(alternative["metrics"][metric][f"{arm}_spread_pct"] <=
                         baseline["metrics"][metric][f"{arm}_spread_pct"] for arm in ("reference", "candidate"))
        # A later quiet null cannot erase a bad/noisy arm in its direct comparison.
        checks[metric] = {"delta_pct": pair["delta_pct"], "null_resolution_pct": resolution,
                          "within_measured_resolution": abs(pair["delta_pct"]) <= resolution,
                          "spread_did_not_degrade": repeatable,
                          "comparison_spread_did_not_degrade": pair["candidate_spread_pct"] <= pair["reference_spread_pct"]}
    okay = comparison["status"] == "PASS" and all(
        check["within_measured_resolution"] and check["spread_did_not_degrade"] and
        check["comparison_spread_did_not_degrade"] for check in checks.values())
    return {"status": "MEETS_SELECTED_CELL_CRITERIA" if okay else "REJECT", "metrics": checks}


def evaluate(blocks):
    by_name = {block["name"]: block for block in blocks}
    output = {"defaults_changed": False, "scope": "selected cell only; full-matrix null still required",
              "snapshot_scope": "requires fresh/seeded bridge and matched-seed construction comparison; no ordinary gate adoption"}
    null_names = ("fresh20_null", "fresh10_null", "seeded20_null", "snapshot20_null")
    output["observed_null_error_pct"] = {name: {
        metric: abs(pair["delta_pct"]) for metric, pair in by_name[name].get("metrics", {}).items()}
        for name in null_names if name in by_name}

    def unavailable(required, nulls):
        for name in required:
            if name not in by_name or by_name[name]["status"] == "UNTESTABLE":
                return {"status": "UNTESTABLE", "reason": f"required block unavailable: {name}"}
        for name in nulls:
            if by_name[name]["status"] != "PASS":
                return {"status": "UNTESTABLE", "reason": f"required byte-identical null failed: {name}"}
        return None

    # Each lever has its own observed construction/window. Keep every failed block
    # and the driver's nonzero exit, but never let an unrelated null erase usable
    # evidence or widen this lever's resolution. Only fresh20 is shared by both.
    window_required = ("fresh20_null", "fresh10_null")
    window_failure = unavailable(window_required, window_required)
    if window_failure:
        output["window10"] = window_failure
    else:
        fresh20, fresh10 = (by_name[name] for name in window_required)
        # Both arm spreads and every scored latency class must hold up on the
        # ordinary fresh-wire20/10 paths; seeded construction cannot substitute.
        checks = {metric: all(fresh10["metrics"][metric][f"{arm}_spread_pct"] <=
                              pair[f"{arm}_spread_pct"] for arm in ("reference", "candidate"))
                  for metric, pair in fresh20["metrics"].items()}
        output["window10"] = {"status": "MEETS_SELECTED_CELL_CRITERIA" if all(checks.values()) else "REJECT",
                              "construction": "ordinary fresh boot plus wire population",
                              "spread_did_not_degrade": checks}

    snapshot_nulls = ("fresh20_null", "seeded20_null", "snapshot20_null")
    snapshot_required = (*snapshot_nulls, "fresh_seeded20", "seeded_snapshot20")
    snapshot_failure = unavailable(snapshot_required, snapshot_nulls)
    if snapshot_failure:
        output["snapshot"] = snapshot_failure
    else:
        fresh20, seeded20, snapshot20 = (by_name[name] for name in snapshot_nulls)
        bridge = construction_checks(by_name["fresh_seeded20"], fresh20, seeded20)
        bridge["scope"] = "fresh/seeded bridge bundles random seed and empty-loader effects; not a seed-only control"
        matched = construction_checks(by_name["seeded_snapshot20"], seeded20, snapshot20)
        okay = all(check["status"] == "MEETS_SELECTED_CELL_CRITERIA" for check in (bridge, matched))
        output["snapshot"] = {**matched, "status": "MEETS_SELECTED_CELL_CRITERIA" if okay else "REJECT",
                              "fresh_seeded_bridge": bridge,
                              "matched_seed_comparison_status": matched["status"]}

    return output


def tables(report):
    print("\n| Block | Rate window | A/B Mops/s | Delta % | Rate spread A/B % | Boot+populate A/B s | Status |")
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
    from _abba_test_fixtures import quiet_record, saturation_record

    def write_snapshot(path, *, seed=7, epoch=1, cut=123, shards=256):
        # Synthetic header fixtures only; real priming always uses the server's SAVE.
        header = bytearray(80)
        header[:8] = b"TOMOSNP\0"
        for offset, value in ((8, 1), (12, 80), (16, shards), (20, 0)):
            header[offset:offset + 4] = value.to_bytes(4, "little")
        for offset, value in ((24, epoch), (32, cut), (40, seed), (48, seed + 1), (56, seed + 2)):
            header[offset:offset + 8] = value.to_bytes(8, "little")
        checksum = 1469598103934665603
        for byte in header[:64]:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        header[64:72] = checksum.to_bytes(8, "little")
        path.write_bytes(header)
        return snapshot_metadata(path)

    def image_fixture(directory):
        empty, full = directory / "empty.tomo", directory / "dump.tomo"
        return SnapshotFixtures(empty, full, write_snapshot(empty), write_snapshot(full, epoch=2, cut=456))

    class Experiments(unittest.TestCase):
        def test_failed_first_null_is_retained_and_all_later_blocks_still_run(self):
            self.test_complete_driver_calls_real_abba_loop_twenty_four_times(first_null_fail=True)

        def test_cross_lever_failure_keeps_real_driver_nonzero_and_every_block(self):
            for index in (1, 5):
                with self.subTest(index=index):
                    self.test_complete_driver_calls_real_abba_loop_twenty_four_times(first_null_fail=True, failed_index=index)

        def test_complete_driver_calls_real_abba_loop_twenty_four_times(self, first_null_fail=False, failed_index=0):
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
                epoch, ticks, quiet_started = int(time.time()) - 10000, [0.], [0.]
                original_gmtime = time.gmtime
                def measure(runner, cell, arm, sequence, instances, knobs):
                    calls.append((abba.WINDOW, arm, sequence, instances, runner.population_by_arm[arm]))
                    ticks[0] += abba.WINDOW + 8
                    return {"arm": arm, "rate": 90 if first_null_fail and len(calls) == (failed_index + 1) * 4 else 100,
                            "saturation": saturation_record(cell.mode, window_seconds=abba.WINDOW + .001),
                            "midpoint_monotonic": 1 + (abba.WINDOW + .001) / 2,
                            "latency_ms": 1, "busy_pct": 99.9,
                            "instances": instances,
                            "load_layout": abba.load_layout(runner.load_cpus, instances, cell.conns),
                            "complete": True, "window_seconds": abba.WINDOW + .001,
                            "populate_seconds": 1, "wall_seconds": abba.WINDOW + 8,
                            "commands": 2000, "pid": 123,
                            "artifacts": f"{cell.id}/n{instances}-{sequence}-{arm}"}
                def prime(a, fixture, out, cell):
                    snapshot = image_fixture(fixture)
                    return snapshot, {"scored": False, "seed_metadata_equal": True}
                def reference(a, out):
                    return a.reference_binary, {"source": "test", "commit": "unverified",
                                                "sha256": abba.sha256(a.reference_binary)}
                geometry = {"server_cores": "0-31", "server_smt": "",
                            "load_cores": "32-127", "load_smt": "160-255"}
                monitor = mock.Mock()
                monitor.start.side_effect = lambda: quiet_started.__setitem__(0, epoch + ticks[0])
                def evidence():
                    cpus = list(range(128)) + list(range(160, 256))
                    samples = max(2, int(epoch + ticks[0] - quiet_started[0]))
                    return quiet_record(cpus=cpus, samples=samples, started_at=quiet_started[0],
                        finished_at=epoch + ticks[0], sample_artifact=directory / "fake-quiet-samples.jsonl")
                monitor.evidence.side_effect = evidence
                monitor.close.side_effect = evidence
                original_window = abba.WINDOW
                with mock.patch.object(BASE_RUNNER, "measure", measure), \
                     mock.patch(__name__ + ".prime_snapshot", side_effect=prime), \
                     mock.patch(__name__ + ".default_geometry", return_value=geometry), \
                     mock.patch.object(abba, "check_placement"), \
                     mock.patch.object(abba, "accepted", return_value=True), \
                     mock.patch.object(abba, "resolve_reference", side_effect=reference), \
                     mock.patch.object(abba, "QuietMonitor", return_value=monitor) as quiet_factory, \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.object(time, "time", side_effect=lambda: epoch + ticks[0]), \
                     mock.patch.object(time, "monotonic", side_effect=lambda: ticks[0]), \
                     mock.patch.object(time, "gmtime", side_effect=lambda seconds=None:
                         original_gmtime(epoch + ticks[0] if seconds is None else seconds)), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main(args), int(first_null_fail))
                self.assertEqual(abba.WINDOW, original_window)
                expected = [(window, arm, sequence, 4, method_a if arm == "A" else method_b)
                            for _, window, method_a, method_b in BLOCKS
                            for sequence, arm in enumerate(abba.ORDER, 1)]
                self.assertEqual(calls, expected)
                self.assertEqual(quiet_factory.call_count, 7)  # Priming plus all six real blocks.
                self.assertTrue(all(call.kwargs["ports"] == (args.port,)
                                    for call in quiet_factory.call_args_list))
                report = json.loads((directory / "experiment/experiment.json").read_text())
                self.assertEqual(len(report["blocks"]), 6)
                self.assertEqual([len(block["attempts"]) for block in report["blocks"]], [4] * 6)
                self.assertFalse(report["evaluation"]["defaults_changed"])
                self.assertEqual(report["plan"]["scored_measurements"], 24)
                self.assertEqual(report["plan"]["unscored_snapshot_priming_boots"], 2)
                if first_null_fail and failed_index in (1, 5):
                    blocked, usable = ("window10", "snapshot") if failed_index == 1 else ("snapshot", "window10")
                    self.assertEqual(report["evaluation"][blocked]["status"], "UNTESTABLE")
                    self.assertEqual(report["evaluation"][usable]["status"], "MEETS_SELECTED_CELL_CRITERIA")
                for block in report["blocks"]:
                    if first_null_fail and block["name"] == BLOCKS[failed_index][0]:
                        self.assertNotEqual(block["status"], "PASS")
                        self.assertEqual(len(block["abba"]["cells"][0]["rounds"][0]["runs"]), 4)
                        continue
                    self.assertEqual(block["abba"]["reference"]["sha256"], block["abba"]["candidate"]["sha256"])
                    self.assertEqual(block["abba"]["verdict"], "PARTIAL")
                    self.assertFalse(block["abba"]["comparison_trusted"])
                    if block["null"]:
                        self.assertEqual(block["abba"]["null_control"]["verdict"], "PASS")
                    else:
                        self.assertEqual(block["abba"]["run_kind"], "comparison")
                        self.assertNotIn("null_control", block["abba"])

        def test_contended_box_refuses_before_snapshot_priming(self):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                source = directory / "cells"
                source.write_text("h12 | 1s | rl=1 | ov=0 | ro=1 | SET | p32 | 512 | - | - | 4\n")
                args = parse_args(["--cells", str(source), "--output", str(directory / "out"),
                                   "--server-cores", "0-31", "--server-smt", "",
                                   "--load-cores", "32-127", "--load-smt", "160-255"])
                quiet = mock.Mock()
                quiet.start.side_effect = RuntimeError("foreign CPU activity")
                quiet.evidence.return_value = {"complete": False, "interference": "foreign CPU activity"}
                with mock.patch.object(abba, "QuietMonitor", return_value=quiet) as quiet_factory, \
                     mock.patch.object(abba, "check_placement"), \
                     mock.patch(__name__ + ".default_geometry", side_effect=AssertionError("explicit geometry")), \
                     mock.patch(__name__ + ".prime_snapshot") as prime, \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                     contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(main(args), 1)
                prime.assert_not_called()
                quiet_factory.assert_called_once_with(list(range(32)), list(range(32, 128)) + list(range(160, 256)),
                    own_root_pid=os.getpid(), window_seconds=abba.WINDOW, ports=(args.port,),
                    sample_artifact=directory / "out/priming-quiet-samples.jsonl")
                quiet.close.assert_called_once()
                report = json.loads((directory / "out/experiment.json").read_text())
                self.assertIn("foreign CPU activity", report["error"])
                self.assertEqual(report["blocks"], [])

        def test_priming_cleanup_failure_prevents_every_scored_block(self):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                binary = directory / "binary"
                binary.write_bytes(b"fixture; never executed")
                binary.chmod(0o700)
                source = directory / "cells"
                source.write_text("h12 | 1s | rl=1 | ov=0 | ro=1 | SET | p32 | 512 | - | - | 4\n")
                args = parse_args(["--cells", str(source), "--output", str(directory / "out"),
                    "--candidate-binary", str(binary), "--memtier", sys.executable,
                    "--server-cores", "0-31", "--server-smt", "", "--load-cores", "32-127",
                    "--load-smt", "160-255"])
                quiet = mock.Mock()
                quiet.evidence.return_value = {"complete": False, "interference": "late foreign work"}
                quiet.check.side_effect = RuntimeError("late foreign work during owned priming cleanup")
                events = []
                def prime(*a):
                    events.append("prime-cleaned-up")
                    return directory / "dump.tomo", {"scored": False}
                quiet.close.side_effect = lambda: events.append("quiet-closed")
                with mock.patch.object(abba, "QuietMonitor", return_value=quiet) as quiet_factory, \
                     mock.patch.object(abba, "check_placement"), \
                     mock.patch(__name__ + ".prime_snapshot", side_effect=prime), \
                     mock.patch(__name__ + ".run_block") as block, \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                     contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(main(args), 1)
                block.assert_not_called()
                self.assertEqual(events[:2], ["prime-cleaned-up", "quiet-closed"])
                self.assertEqual(quiet_factory.call_args.kwargs["ports"], (args.port,))
                report = json.loads((directory / "out/experiment.json").read_text())
                self.assertIn("late foreign work", report["error"])
                self.assertEqual(report["quiet_priming"]["interference"], "late foreign work")

        def test_priming_saves_and_exits_before_measurement(self):
            for empty in (False, True):
                with self.subTest(empty=empty), tempfile.TemporaryDirectory() as tmp:
                    folder = Path(tmp)
                    conn = mock.Mock()
                    def command(*args):
                        if args == ("DBSIZE",):
                            return 0
                        if args[:2] == ("CONFIG", "GET"):
                            return [b"dbfilename", b"dump.tomo"]
                        if args == ("SAVE",):
                            write_snapshot(folder / "dump.tomo")
                            return b"OK"
                        raise AssertionError(args)
                    conn.must.side_effect = command
                    runner = object.__new__(PrimingRunner)
                    runner.capture_empty = empty
                    with mock.patch.object(BASE_RUNNER, "populate", return_value=None) as population:
                        with self.assertRaises(SnapshotReady):
                            runner.populate(None, "A", conn, folder)
                    self.assertEqual(population.call_count, int(not empty))
                    self.assertEqual(conn.must.call_args_list[0].args, ("DBSIZE",))
                    self.assertEqual(conn.must.call_args_list[-1].args, ("SAVE",))
                    self.assertEqual(snapshot_metadata(runner.snapshot)["hash_seed"], 7)
                    conn.must.side_effect = lambda *a: 1
                    with mock.patch.object(BASE_RUNNER, "populate") as population, \
                         self.assertRaisesRegex(RuntimeError, "exactly zero keys"):
                        runner.populate(None, "A", conn, folder)
                    population.assert_not_called()

        def test_two_real_priming_hooks_restart_before_full_population(self):
            for changed_seed in (False, True):
                with self.subTest(changed_seed=changed_seed), tempfile.TemporaryDirectory() as tmp:
                    directory = Path(tmp)
                    fixture = directory / "fixture"
                    fixture.mkdir()
                    args = parse_args(["--server-cores", "0-31", "--server-smt", "", "--load-cores", "32-127", "--load-smt", "160-255"])
                    cell = abba.Cell("h12", "1s", 1, 0, 1, "SET", 32, 512, instances=4)
                    events, active = [], []
                    class Children:
                        def __init__(self):
                            if active:
                                raise AssertionError("priming boots overlapped")
                            active.append(self)
                        def close(self):
                            events.append("stop")
                            active.remove(self)
                    def measure(runner, selected, arm, sequence, instances, knobs):
                        folder = runner.out / "actual-hook"
                        folder.mkdir(parents=True)
                        runner.prepare_data(selected, arm, folder)
                        events.append("empty-boot" if runner.capture_empty else "full-boot")
                        if not runner.capture_empty:
                            self.assertEqual(snapshot_metadata(folder / "dump.tomo")["hash_seed"], 7)
                        else:
                            self.assertFalse((folder / "dump.tomo").exists())
                        def command(*command):
                            if command == ("DBSIZE",): return 0
                            if command[:2] == ("CONFIG", "GET"): return [b"dbfilename", b"dump.tomo"]
                            if command == ("SAVE",):
                                events.append("empty-save" if runner.capture_empty else "full-save")
                                write_snapshot(folder / "dump.tomo", seed=8 if changed_seed and not runner.capture_empty else 7)
                                return b"OK"
                            raise AssertionError(command)
                        runner.populate(selected, arm, mock.Mock(must=command), folder)
                    with mock.patch.object(abba, "accepted", return_value=True), \
                         mock.patch.object(abba, "Children", Children), \
                         mock.patch.object(BASE_RUNNER, "measure", measure), \
                         mock.patch.object(BASE_RUNNER, "populate", side_effect=lambda *a: events.append("wire-population")):
                        if changed_seed:
                            with self.assertRaisesRegex(RuntimeError, "seed metadata differ"):
                                prime_snapshot(args, fixture, directory / "priming", cell)
                        else:
                            images, report = prime_snapshot(args, fixture, directory / "priming", cell)
                            images.validate()
                            self.assertEqual(report["status"], "COMPLETE")
                            self.assertTrue(report["seed_metadata_equal"])
                    self.assertEqual(events, ["empty-boot", "empty-save", "stop", "full-boot", "wire-population", "full-save", "stop"])
                    self.assertEqual(active, [])
                    artifact = json.loads((directory / "priming/priming.json").read_text())
                    self.assertEqual(artifact["status"], "FAIL" if changed_seed else "COMPLETE")
                    self.assertEqual(len(artifact["boots"]), 2)

        def test_snapshot_headers_retain_exact_seed_and_reject_corruption(self):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                images = image_fixture(directory)
                images.validate()  # Epoch/cut differ, while routing key material matches.
                self.assertEqual(seed_metadata(images.empty_metadata), seed_metadata(images.full_metadata))
                self.assertNotEqual(images.empty_metadata["epoch"], images.full_metadata["epoch"])
                for offset in (0, 8, 12, 16, 20, 40, 48, 56, 64):
                    data = bytearray(images.empty.read_bytes())
                    data[offset] ^= 1
                    bad = directory / "bad.tomo"
                    bad.write_bytes(data)
                    with self.subTest(offset=offset), self.assertRaisesRegex(RuntimeError, "snapshot header"):
                        snapshot_metadata(bad)
                images.empty.write_bytes(images.empty.read_bytes()[:79])
                with self.assertRaisesRegex(RuntimeError, "snapshot header"):
                    images.validate()
                write_snapshot(images.empty, seed=99)
                with self.assertRaisesRegex(RuntimeError, "fixture changed"):
                    images.validate()

        def test_snapshot_hook_never_repopulates_or_saves(self):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                images = image_fixture(directory)
                folder = directory / "measurement"
                folder.mkdir()
                runner = object.__new__(ExperimentRunner)
                runner.snapshot = images
                runner.population_by_arm = {"A": "wire", "B": "snapshot"}
                runner.prepare_data(None, "B", folder)
                self.assertEqual((folder / "dump.tomo").read_bytes(), images.full.read_bytes())
                conn = mock.Mock()
                conn.must.side_effect = lambda *a: abba.KEYS if a == ("DBSIZE",) else b"x" * 64
                cell = abba.Cell("h12", "1s", 1, 0, 1, "SET", 32, 512, instances=4)
                with mock.patch.object(BASE_RUNNER, "populate", side_effect=AssertionError("snapshot was repopulated")):
                    result = runner.populate(cell, "B", conn, folder)
                self.assertEqual(result["method"], "snapshot")
                self.assertEqual(result["common_seed"], seed_metadata(images.empty_metadata))
                self.assertTrue(all(call.args[0] in ("DBSIZE", "GET") for call in conn.must.call_args_list))
                conn.must.side_effect = lambda *a: abba.KEYS - 1
                with self.assertRaisesRegex(RuntimeError, "wrong key count"):
                    runner.populate(cell, "B", conn, folder)

        def test_seeded_wire_and_fresh_hooks_are_distinct_and_never_save(self):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                images = image_fixture(directory)
                runner = object.__new__(ExperimentRunner)
                runner.snapshot = images
                runner.population_by_arm = {"A": "fresh", "B": "wire"}
                cell = abba.Cell("h12", "1s", 1, 0, 1, "SET", 32, 512, instances=4)
                for arm in ("A", "B"):
                    folder = directory / arm
                    folder.mkdir()
                    runner.prepare_data(cell, arm, folder)
                    self.assertEqual((folder / "dump.tomo").exists(), arm == "B")
                    if arm == "B":
                        self.assertEqual((folder / "dump.tomo").read_bytes(), images.empty.read_bytes())
                    conn = mock.Mock()
                    conn.must.return_value = 0
                    with mock.patch.object(BASE_RUNNER, "populate", return_value={"original": "wire"}) as populate:
                        result = runner.populate(cell, arm, conn, folder)
                    populate.assert_called_once()
                    self.assertEqual(result["common_seed"], seed_metadata(images.empty_metadata) if arm == "B" else None)
                    conn.must.assert_called_once_with("DBSIZE")
                    conn.must.return_value = 1
                    with mock.patch.object(BASE_RUNNER, "populate") as populate, \
                         self.assertRaisesRegex(RuntimeError, "exactly zero keys"):
                        runner.populate(cell, arm, conn, folder)
                    populate.assert_not_called()
                environment = runner.population_environment()
                self.assertEqual(environment["population_seed_matched_arms"], ["B"])
                self.assertEqual(environment["population_empty_snapshot_sha256"], images.empty_metadata["sha256"])

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
            for run in runs:
                run["window_seconds"] = 20
            report["measurement_valid"] = False
            report["quiet_box"] = {"interference": {"processes": [{"pid": 123}]}}
            block = describe_block(cell, BLOCKS[0], report, attempts, 1)
            self.assertEqual(block["status"], "UNTESTABLE")
            self.assertTrue(any("quiet-box" in why for why in block["reasons"]))

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

        def test_bridge_failure_cannot_be_erased_by_matched_seed_results(self):
            pair = {"delta_pct": 0, "reference_spread_pct": .1, "candidate_spread_pct": .1}
            for poison in ("gain", "loss", "spread", "verdict"):
                with self.subTest(poison=poison):
                    blocks = [{"name": spec[0], "status": "PASS",
                               "metrics": {"rate": dict(pair), "p999_ms": dict(pair), "long_p999_ms": dict(pair)}} for spec in BLOCKS]
                    bridge = blocks[2]
                    if poison in ("gain", "loss"):
                        bridge["metrics"]["long_p999_ms"]["delta_pct"] = .2 if poison == "gain" else -.2
                    elif poison == "spread":
                        bridge["metrics"]["rate"]["candidate_spread_pct"] = .2
                    else:
                        bridge["status"] = "FAIL"
                    result = evaluate(blocks)
                    self.assertEqual(result["snapshot"]["status"], "REJECT")
                    self.assertEqual(result["snapshot"]["fresh_seeded_bridge"]["status"], "REJECT")
                    self.assertEqual(result["snapshot"]["matched_seed_comparison_status"], "MEETS_SELECTED_CELL_CRITERIA")
                    self.assertEqual(result["window10"]["status"], "MEETS_SELECTED_CELL_CRITERIA")
                    self.assertFalse(result["defaults_changed"])

        def test_each_null_only_blocks_its_own_lever_and_failed_blocks_stay_visible(self):
            pair = {"delta_pct": 0, "reference_spread_pct": .1, "candidate_spread_pct": .1}
            cases = (("fresh20_null", {"window10", "snapshot"}),
                     ("fresh10_null", {"window10"}),
                     ("seeded20_null", {"snapshot"}),
                     ("snapshot20_null", {"snapshot"}))
            for name, affected in cases:
                for status in ("FAIL", "UNTESTABLE", "MISSING"):
                    with self.subTest(name=name, status=status):
                        blocks = [{"name": spec[0], "status": "PASS", "metrics": {"rate": dict(pair)}} for spec in BLOCKS]
                        target = next(block for block in blocks if block["name"] == name)
                        if status == "MISSING":
                            blocks.remove(target)
                        else:
                            target["status"] = status
                            target["metrics"]["rate"]["delta_pct"] = 7
                        original = json.dumps(blocks, sort_keys=True)
                        result = evaluate(blocks)
                        self.assertEqual(json.dumps(blocks, sort_keys=True), original)
                        for lever in ("window10", "snapshot"):
                            self.assertEqual(result[lever]["status"],
                                "UNTESTABLE" if lever in affected else "MEETS_SELECTED_CELL_CRITERIA")
                            if lever in affected:
                                self.assertIn(name, result[lever]["reason"])
                        if status != "MISSING":
                            self.assertEqual(result["observed_null_error_pct"][name]["rate"], 7)
                        self.assertFalse(result["defaults_changed"])

        def test_snapshot_null_cannot_erase_a_bad_direct_comparison(self):
            pair = {"delta_pct": 0, "reference_spread_pct": .1, "candidate_spread_pct": .1}
            for poison in ("spread", "verdict"):
                with self.subTest(poison=poison):
                    blocks = [{"name": spec[0], "status": "PASS",
                               "metrics": {"rate": dict(pair)}} for spec in BLOCKS]
                    self.assertEqual(evaluate(blocks)["snapshot"]["status"], "MEETS_SELECTED_CELL_CRITERIA")
                    if poison == "spread":
                        blocks[4]["metrics"]["rate"]["candidate_spread_pct"] = .2
                    else:
                        blocks[4]["status"] = "FAIL"
                    self.assertEqual(evaluate(blocks)["snapshot"]["status"], "REJECT")

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
            "blocks": BLOCKS, "scored_measurements": 24, "unscored_snapshot_priming_boots": 2}
    if args.plan_only:
        print(json.dumps(plan, indent=2))
        return 0
    output = (args.output or abba.ROOT / "build" / f"abba-experiments-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}").resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "plan": plan, "blocks": [], "defaults_changed": False}
    started = time.monotonic()
    original_affinity = os.sched_getaffinity(0)
    priming_quiet = None
    old_handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
    def interrupted(signum, frame):
        raise InterruptedError(f"experiment interrupted by signal {signum}")
    for sig in old_handlers:
        signal.signal(sig, interrupted)
    try:
        # Support probes and snapshot preparation precede abbagate.main(). Observe
        # their entire lifetime, including owned cleanup, on the selected CPUs.
        # A final latched failure forbids scoring.
        priming_quiet = abba.QuietMonitor(
            abba.cpus(args.server_cores) + abba.cpus(args.server_smt),
            abba.cpus(args.load_cores) + abba.cpus(args.load_smt), own_root_pid=os.getpid(),
            window_seconds=abba.WINDOW, ports=(args.port,),
            sample_artifact=output / "priming-quiet-samples.jsonl")
        priming_quiet.start()
        report["quiet_before_priming"] = priming_quiet.evidence()
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
        priming_quiet.close()
        report["quiet_priming"] = priming_quiet.evidence()
        priming_quiet.check()
        priming_quiet = None
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
        if priming_quiet is not None:
            priming_quiet.close()
            report["quiet_priming"] = priming_quiet.evidence()
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
