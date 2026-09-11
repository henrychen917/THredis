#!/usr/bin/env python3
"""Measured gate inputs, never tolerances or experiment definitions.

The config cannot waive a live quiet, stability, saturation or workload check.
Floors bind the cell parameters AND placement that produced them: a different
shape becomes unpinned and must be calibrated again. Historical observations
without enough provenance remain visible but cannot certify a current floor.
"""
import argparse
from dataclasses import asdict, is_dataclass, replace
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import time

DEFAULT = Path(__file__).with_name("gate_measurements.json")
SHAPE = ("mode", "read_local", "overlap", "reorder", "op", "depth", "conns", "atomic", "score", "mix")
AXES = ("server_physical", "server_smt", "load_physical", "load_smt", "split_ratio")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def provenance(value, label):
    require(isinstance(value, dict) and set(value) == {"when", "how"} and
            all(isinstance(v, str) and v.strip() for v in value.values()),
            f"{label}: measurement requires when/how provenance")


def load(path=DEFAULT):
    value = json.loads(Path(path).read_text())
    require(set(value) == {"schema", "box", "geometries", "reference_binary", "load_floors"}
            and value["schema"] == 1, "unknown measured config fields/schema")
    box = value["box"]
    require(set(box) == {"physical_cores", "smt_siblings", "memtier_instance_ceiling_ops_per_second"},
            "unknown box measurement; tolerances do not belong in config")
    for name, record in box.items():
        require(set(record) == {"value", "provenance"}, f"unknown box field: {name}")
        provenance(record["provenance"], name)
    count, groups = box["physical_cores"]["value"], box["smt_siblings"]["value"]
    require(type(count) is int and count > 0 and isinstance(groups, list) and len(groups) == count,
            "invalid measured physical-core count/SMT map")
    flattened = [cpu for group in groups for cpu in group]
    require(all(group and group == sorted(set(group)) for group in groups) and
            all(type(cpu) is int and cpu >= 0 for cpu in flattened) and
            len(flattened) == len(set(flattened)), "invalid measured SMT sibling map")
    ceiling = box["memtier_instance_ceiling_ops_per_second"]["value"]
    require(isinstance(ceiling, list) and len(ceiling) == 2 and
            all(type(v) is int and v > 0 for v in ceiling) and ceiling[0] <= ceiling[1],
            "invalid observed memtier instance ceiling")
    require(set(value["geometries"]) == {"correctness", "abba"}, "unknown geometry family")
    for family, budgets in value["geometries"].items():
        for budget, record in budgets.items():
            require(re.fullmatch(r"[1-9][0-9]*", budget) and
                    set(record) == {"io", "ex", "provenance"} and
                    all(type(record[k]) is int and record[k] > 0 for k in ("io", "ex")) and
                    record["io"] + record["ex"] == int(budget), f"invalid {family} ratio for {budget} threads")
            provenance(record["provenance"], f"{family}/{budget}")
    reference = value["reference_binary"]
    require(set(reference) == {"path", "commit", "sha256", "provenance"} and
            isinstance(reference["path"], str) and reference["path"] and
            re.fullmatch("[0-9a-f]{40}", reference["commit"]) and
            re.fullmatch("[0-9a-f]{64}", reference["sha256"]), "invalid reference binary identity")
    provenance(reference["provenance"], "reference binary")
    for ident, floor in value["load_floors"].items():
        require(set(floor) == {"instances", "shape", "geometry", "instrument_sha256", "status", "observed_rate", "observed_busy", "provenance"}
                and type(floor["instances"]) is int and floor["instances"] > 0 and
                set(floor["shape"]) == set(SHAPE) and floor["status"] in ("calibrated", "historical-unverified"),
                f"{ident}: invalid load-floor fields")
        provenance(floor["provenance"], ident)
        require(floor["status"] != "calibrated" or isinstance(floor["geometry"], dict) and
                set(floor["geometry"]) == set(AXES) and isinstance(floor["instrument_sha256"], str) and
                re.fullmatch("[0-9a-f]{64}", floor["instrument_sha256"]),
                f"{ident}: calibrated floor lacks placement/instrument identity")
        if floor["status"] == "calibrated":
            for name, unit in (("observed_rate", "ops_per_second"), ("observed_busy", "percent")):
                observation = floor[name]
                require(isinstance(observation, dict) and set(observation) == {"unit", "order", "values"} and
                        observation["unit"] == unit and observation["order"] == ["A", "B", "B", "A"] and
                        isinstance(observation["values"], list) and len(observation["values"]) == 4 and
                        all(type(v) in (int, float) and math.isfinite(v) and v >= 0
                            for v in observation["values"]),
                        f"{ident}: calibrated floor lacks its four measured {name} samples")
    return value


def ratio(family, threads, measurements=None):
    measurements = load() if measurements is None else measurements
    record = measurements["geometries"][family].get(str(threads))
    require(record is not None, f"no reviewed {family} io:ex ratio for {threads} server threads; "
            "record the measured geometry in gate_measurements.json, never derive it from a core range")
    return f"{record['io']}:{record['ex']}"


def shape(cell):
    cell = asdict(cell) if is_dataclass(cell) else cell
    return {key: cell[key] for key in SHAPE}


def geometry(environment):
    return {key: environment[key] for key in AXES}


def instrument_digest(root=DEFAULT.parent.parent):
    from abba_instrument import instrument_fingerprint
    return instrument_fingerprint(root)["sha256"]


def apply_floor(cell, measurements=None, placement=None, instrument_sha256=None):
    measurements = load() if measurements is None else measurements
    fields = asdict(cell) if is_dataclass(cell) else cell
    floor = measurements["load_floors"].get(fields["id"])
    valid = (floor is not None and floor["status"] == "calibrated" and
             floor["shape"] == shape(fields) and (placement is None or floor["geometry"] == placement))
    if valid:
        # The cell's flags do not describe MGET key count, value bytes, keyspace,
        # generation pattern or service-blocker size. Bind the full instrument's
        # transitive code as well. Changing any of it conservatively recalibrates;
        # importing config DATA does not alter this code-only fingerprint.
        valid = floor["instrument_sha256"] == (instrument_sha256 or instrument_digest())
    count = floor["instances"] if valid else 0
    return replace(cell, instances=count) if is_dataclass(cell) else {**cell, "instances": count}


def configured_reference(commit, measurements=None):
    measurements = load() if measurements is None else measurements
    record = measurements["reference_binary"]
    require(record["commit"] == commit,
            f"configured reference {record['commit']} differs from last push {commit}; review the reference identity")
    path = Path(record["path"])
    require(path.is_file() and sha256(path) == record["sha256"],
            f"configured reference digest mismatch/unavailable: {path}")
    return path, {**record, "source": "gate_measurements.json", "ref": "origin/cpp"}


def import_calibration(path, measurements, cells):
    # Recompute the mathematical selection; trusting a serialized PASS would let an
    # edited summary promote an unsaturated or incomplete measurement to a floor.
    from abbagate import Cell, ORDER, select_load_floor
    from abba_evidence import validate_measurements, validate_null
    from abba_instrument import instrument_fingerprint
    path = Path(path).resolve()
    content = path.read_bytes()
    report = json.loads(content)
    fingerprint = instrument_fingerprint(DEFAULT.parent.parent)
    # Load selection alone cannot certify an instrument: an explicit failed null,
    # a failed depth-1 row, a shortened measurement, or incomplete quiet coverage
    # must reject the entire import too. Reuse the normal report validator.
    validate_measurements(report, now=time.time(), expected_instrument=fingerprint)
    if report["run_kind"] == "null-control":
        validate_null(report, now=time.time())
    require(report.get("escalate") is True, "floor import requires an explicit --escalate campaign")
    datetime.fromisoformat(report["started_utc"].replace("Z", "+00:00"))
    placement = geometry(report["environment"])
    instrument = fingerprint["sha256"]
    report_digest = hashlib.sha256(content).hexdigest()
    require(placement["split_ratio"] == ratio("abba", len(report["environment"]["server_cpus"]), measurements),
            "calibration ratio differs from reviewed config")
    current = {cell.id: cell for cell in cells}
    require(report.get("cells"), "empty calibration campaign")
    require([row["cell"]["id"] for row in report["cells"]] == report["coverage"]["ids"],
            "calibration is incomplete: observed cell IDs differ from requested coverage")
    updates = {}
    for row in report["cells"]:
        cell = Cell(**row["cell"])
        require(cell.id in current and shape(current[cell.id]) == shape(cell), f"{cell.id}: cell shape changed since calibration")
        if cell.depth == 1:
            continue
        require(row.get("instrument_valid", True) and not row.get("error"), f"{cell.id}: invalid measurement row")
        selection = select_load_floor(replace(cell, instances=0), row["rounds"])
        require(selection["measurement_valid"] and selection["status"] == "CONFIRMED",
                f"{cell.id}: no measured saturation plateau with higher-capacity confirmation")
        count = selection["lowest_tested_qualifying_instances"]
        selected_runs = row["rounds"][selection["selected_index"]]["runs"]
        # Keep the actual A1/B1/B2/A2 observations, including units, next to the
        # floor they established. A mean or null entry loses which arm occupied
        # the server. These are provenance only: every gate still rechecks live
        # productive-role saturation and derives its threshold from that run.
        updates[cell.id] = dict(instances=count, shape=shape(cell), geometry=placement,
            instrument_sha256=instrument, status="calibrated",
            observed_rate=dict(unit="ops_per_second", order=list(ORDER),
                               values=[run["rate"] for run in selected_runs]),
            observed_busy=dict(unit="percent", order=list(ORDER),
                               values=[run["busy_pct"] for run in selected_runs]),
            provenance={"when": report["started_utc"],
            "how": f"abbagate --escalate; {path}; sha256={report_digest}; "
                   f"reference={report['reference']['sha256']}; candidate={report['candidate']['sha256']}; "
                   f"lowest tested qualifying instances={count}; confirmation={selection['confirmation_instances']}"})
    require(updates, "calibration contains no deep-pipeline load floors")
    # All-or-nothing import: a red/invalid later cell must not silently salvage an
    # earlier campaign prefix. Stored floors never change the live ABBA tolerances.
    measurements["load_floors"].update(updates)
    return sorted(updates)


def self_test():
    import copy
    import tempfile
    import unittest

    class Measurements(unittest.TestCase):
        def setUp(self):
            self.config = load()
            self.cell = {"id": "unit", "mode": "2s", "read_local": 1, "overlap": 1,
                         "reorder": 1, "op": "GET", "depth": 32, "conns": 512,
                         "atomic": 1, "score": "rate", "mix": "-", "instances": 0}
            self.placement = dict(server_physical=list(range(32)), server_smt=[],
                load_physical=list(range(32, 128)), load_smt=list(range(160, 256)), split_ratio="16:16")
            self.config["load_floors"]["unit"] = dict(instances=4, shape=shape(self.cell), geometry=self.placement,
                instrument_sha256=instrument_digest(),
                status="calibrated",
                observed_rate=dict(unit="ops_per_second", order=["A", "B", "B", "A"], values=[100] * 4),
                observed_busy=dict(unit="percent", order=["A", "B", "B", "A"], values=[99.9] * 4),
                provenance={"when": "2026-09-11", "how": "synthetic negative control"})

        def test_shape_changes_invalidate_every_axis(self):
            self.assertEqual(apply_floor(self.cell, self.config, self.placement)["instances"], 4)
            for key in SHAPE:
                changed = {**self.cell, key: "changed"}
                self.assertEqual(apply_floor(changed, self.config, self.placement)["instances"], 0, key)

        def test_geometry_changes_invalidate(self):
            for key in AXES:
                changed = {**self.placement, key: "changed"}
                self.assertEqual(apply_floor(self.cell, self.config, changed)["instances"], 0, key)

        def test_historical_unknown_geometry_cannot_pin(self):
            self.config["load_floors"]["unit"]["status"] = "historical-unverified"
            self.assertEqual(apply_floor(self.cell, self.config)["instances"], 0)

        def test_workload_code_change_invalidates_unchanged_cell_flags(self):
            self.assertEqual(apply_floor(self.cell, self.config, self.placement,
                instrument_sha256="0" * 64)["instances"], 0)

        def test_ratio_has_no_core_count_fallback(self):
            self.assertEqual(ratio("correctness", 8, self.config), "6:2")
            self.assertEqual(ratio("abba", 32, self.config), "16:16")
            with self.assertRaisesRegex(ValueError, "no reviewed"):
                ratio("correctness", 32, self.config)

        def test_unknown_fields_and_missing_provenance_fail(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "config.json"
                for mutate in (lambda v: v.update(tolerance=100),
                               lambda v: v["box"]["physical_cores"].pop("provenance"),
                               lambda v: v["load_floors"]["unit"].update(observed_rate=None),
                               lambda v: v["load_floors"]["unit"]["observed_busy"]["values"].pop(),
                               lambda v: v["load_floors"]["unit"]["observed_rate"].update(unit="percent"),
                               lambda v: v["load_floors"]["unit"]["observed_busy"].update(values=[float("nan")] * 4)):
                    value = copy.deepcopy(self.config)
                    mutate(value)
                    path.write_text(json.dumps(value))
                    with self.assertRaises(ValueError):
                        load(path)

        def test_reference_digest_mutation_fails(self):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "reference"
                path.write_bytes(b"reference")
                self.config["reference_binary"].update(path=str(path), sha256=sha256(path))
                commit = self.config["reference_binary"]["commit"]
                self.assertEqual(configured_reference(commit, self.config)[0], path)
                path.write_bytes(b"different executable")
                with self.assertRaisesRegex(ValueError, "digest mismatch"):
                    configured_reference(commit, self.config)

        def test_calibration_import_replays_evidence_and_rejects_mutations(self):
            from abbagate import Cell, load_layout, ORDER, assess
            from abba_instrument import instrument_fingerprint
            from abba_evidence import null_result
            from background_environment import canonical_contract
            from _abba_test_fixtures import saturation_record
            cell = Cell(**self.cell)
            rounds = []
            for count in (1, 2):
                layout = load_layout(self.placement["load_physical"] + self.placement["load_smt"], count, cell.conns)
                rounds.append(dict(instances=count, runs=[dict(arm=arm, rate=100, busy_pct=99.9,
                    saturation=saturation_record(cell.mode), window_seconds=20, midpoint_monotonic=11,
                    complete=True, instances=count, load_layout=layout, latency_ms=1, commands=2000, pid=123,
                    artifacts=f"{cell.id}/n{count}-{index}-{arm}") for index, arm in enumerate(ORDER, 1)]))
            started = "2026-09-10T00:00:00Z"
            epoch = datetime.fromisoformat(started.replace("Z", "+00:00")).timestamp()
            instrument = instrument_fingerprint(DEFAULT.parent.parent)
            load_cpus = self.placement["load_physical"] + self.placement["load_smt"]
            monitored = list(range(32)) + load_cpus
            background = canonical_contract(None)
            source = "synthetic cell fixture\n"
            report = dict(schema=1, order=list(ORDER), window_seconds=20, elapsed_seconds=200,
                verdict="PARTIAL", statistical_verdict="PASS", run_kind="comparison", subset="full", only="",
                receipt_harness_sha256="a" * 64, comparison_trusted=False,
                measurement_valid=True, escalate=True, started_utc=started, instrument_fingerprint=instrument,
                cell_source=dict(text=source, sha256=hashlib.sha256(source.encode()).hexdigest(), total_cells=1),
                quiet_box=dict(complete=True, interference=None, policy="operational-environment-v1",
                    started_at=epoch, finished_at=epoch + 200, sample_interval_seconds=1, samples=201,
                    cpus=monitored, requested_cpus=monitored, background_environment=dict(contract=background,
                        sample_count=201, sample_artifact="synthetic.jsonl", listener_snapshots=0,
                        source={"path": None, "sha256": None}, reviewed_inventory=None)),
                environment={**self.placement, "server_cpus": list(range(32)), "load_cpus": load_cpus,
                    "python_runtime": instrument["python"], "uname": ["synthetic"], "memtier_sha256": "c" * 64,
                    "memtier_version": "fixture", "keys": 2000000, "data_bytes": 64, "key_pattern": "P:P",
                    "population_by_arm": {"A": "wire", "B": "wire"}, "background_environment": background},
                candidate={"sha256": "a" * 64}, reference={"sha256": "b" * 64},
                coverage={"ids": [cell.id], "count": 1, "pending_pins": [], "requested_pending_pins": [cell.id]},
                cells=[dict(cell=asdict(cell), rounds=rounds, verdict="PASS", assessment=assess(cell, rounds))])
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "results.json"
                path.write_text(json.dumps(report))
                config = copy.deepcopy(self.config)
                self.assertEqual(import_calibration(path, config, [cell]), [cell.id])
                self.assertEqual(config["load_floors"][cell.id]["instances"], 1)
                self.assertEqual(config["load_floors"][cell.id]["observed_rate"],
                                 dict(unit="ops_per_second", order=list(ORDER), values=[100] * 4))
                self.assertEqual(config["load_floors"][cell.id]["observed_busy"],
                                 dict(unit="percent", order=list(ORDER), values=[99.9] * 4))
                stored = Path(directory) / "config.json"
                stored.write_text(json.dumps(config))
                self.assertEqual(load(stored), config)
                self.assertIn(sha256(path), config["load_floors"][cell.id]["provenance"]["how"])
                for mutate in (
                        lambda v: v.update(measurement_valid=False),
                        lambda v: v.update(verdict="FAIL"),
                        lambda v: v.update(escalate=False),
                        lambda v: v["instrument_fingerprint"].update(sha256="0" * 64),
                        lambda v: v["quiet_box"].update(complete=False),
                        lambda v: v["quiet_box"].update(finished_at=epoch + 1),
                        lambda v: v["coverage"]["ids"].append("missing"),
                        lambda v: v["cells"][0]["cell"].update(conns=2048),
                        lambda v: v["cells"][0]["rounds"].pop(),
                        lambda v: v["cells"][0]["rounds"][1]["runs"][1].update(complete=False),
                        lambda v: v["cells"][0]["rounds"][1]["runs"][1].update(window_seconds=1),
                        lambda v: v["cells"][0]["rounds"][0]["runs"][0].update(
                            saturation=saturation_record(cell.mode, score=1))):
                    broken, unchanged = copy.deepcopy(report), copy.deepcopy(self.config)
                    mutate(broken)
                    path.write_text(json.dumps(broken))
                    with self.assertRaises(ValueError):
                        import_calibration(path, unchanged, [cell])
                    self.assertEqual(unchanged, self.config)
                null = copy.deepcopy(report)
                null["run_kind"] = "null-control"
                null["reference"]["sha256"] = null["candidate"]["sha256"]
                null["null_control"] = null_result(null, now=time.time())
                path.write_text(json.dumps(null))
                self.assertEqual(import_calibration(path, copy.deepcopy(self.config), [cell]), [cell.id])
                null["null_control"]["verdict"] = "FAIL"
                path.write_text(json.dumps(null))
                with self.assertRaisesRegex(ValueError, "null control did not complete and pass"):
                    import_calibration(path, copy.deepcopy(self.config), [cell])
                failed_latency = copy.deepcopy(report)
                p1 = replace(cell, id="latency", depth=1, score="latency")
                failed_latency["cells"].append(dict(cell=asdict(p1), verdict="FAIL"))
                failed_latency["coverage"].update(ids=[cell.id, p1.id], count=2)
                failed_latency["cell_source"]["total_cells"] = 2
                path.write_text(json.dumps(failed_latency))
                with self.assertRaisesRegex(ValueError, "nonpassing ABBA cell: latency"):
                    import_calibration(path, copy.deepcopy(self.config), [cell, p1])

    return 0 if unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(Measurements)).wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT)
    parser.add_argument("--import-calibration", type=Path)
    parser.add_argument("--cells", type=Path, default=Path(__file__).with_name("headline_cells.txt"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    measurements = load(args.config)
    if args.import_calibration:
        from abbagate import read_cells
        updated = import_calibration(args.import_calibration, measurements, read_cells(args.cells))
        temporary = args.config.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(measurements, indent=2) + "\n")
        load(temporary)
        temporary.replace(args.config)
        print("Imported measured floors: " + ", ".join(updated))
    else:
        print(json.dumps(measurements, indent=2))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyError, ValueError, OSError) as error:
        print(f"gate measurements: {error}", file=sys.stderr)
        sys.exit(1)
