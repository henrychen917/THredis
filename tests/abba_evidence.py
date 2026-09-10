#!/usr/bin/env python3
"""Shared ABBA measurement and standing-null evidence; never changes an assessment threshold."""
from datetime import datetime, timezone
import hashlib
import json
import math
import re

from abba_instrument import validate_fingerprint

ORDER = ["A", "B", "B", "A"]
NULL_MAX_AGE = 24 * 60 * 60


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, label, *, positive=False):
    require(type(value) in (int, float) and math.isfinite(value) and
            (value > 0 if positive else value >= 0), f"invalid {label}")
    return value


def signed_number(value, label):
    require(type(value) in (int, float) and math.isfinite(value), f"invalid {label}")
    return value


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def utc_seconds(value):
    require(isinstance(value, str), "missing ABBA start timestamp")
    return datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()


def validate_measurements(report, *, now, expected_source=None, expected_cells=None, harness=None, candidate=None,
                          expected_instrument=None):
    require(isinstance(report, dict), "ABBA evidence must be a JSON object")
    require(report.get("schema") == 1 and report.get("statistical_verdict") == "PASS" and
            report.get("verdict") in ("PASS", "PARTIAL"), "ABBA measurements did not all pass")
    require(report.get("measurement_valid") is True, "ABBA measurement validity was not certified")
    require(re.fullmatch(r"[0-9a-f]{64}", report.get("receipt_harness_sha256", "")), "missing ABBA harness digest")
    require(harness is None or report["receipt_harness_sha256"] == harness, "ABBA harness differs")
    instrument_sha = validate_fingerprint(report.get("instrument_fingerprint"))
    require(expected_instrument is None or instrument_sha == validate_fingerprint(expected_instrument),
            "ABBA measurement instrument differs")
    require(report.get("order") == ORDER, "ABBA sequence changed")
    started = utc_seconds(report.get("started_utc"))
    elapsed = number(report.get("elapsed_seconds"), "ABBA elapsed seconds", positive=True)
    require(started <= now and started + elapsed <= now + 1, "ABBA timestamps are incomplete or in the future")
    number(report.get("window_seconds"), "ABBA window", positive=True)
    source = report.get("cell_source", {})
    require(isinstance(source, dict) and isinstance(source.get("text"), str), "invalid ABBA cell source")
    require(re.fullmatch(r"[0-9a-f]{64}", source.get("sha256", "")) and
            digest(source.get("text", "").encode()) == source["sha256"] and
            type(source.get("total_cells")) is int and source["total_cells"] > 0,
            "invalid ABBA inventory provenance")
    if expected_source is not None:
        require(source["sha256"] == expected_source["sha256"] and
                source["total_cells"] == expected_source.get("total_cells", expected_source.get("count")),
                "ABBA inventory does not match current cell file")
    rows = report.get("cells", [])
    require(isinstance(rows, list) and rows and all(isinstance(row, dict) for row in rows), "unreached ABBA cell set")
    cells = [row.get("cell", {}) for row in rows]
    require(all(isinstance(cell, dict) for cell in cells), "invalid ABBA cell parameters")
    ids = [cell.get("id") for cell in cells]
    require(all(isinstance(ident, str) and ident for ident in ids) and len(set(ids)) == len(ids),
            "missing/duplicate ABBA cell identity")
    require(expected_cells is None or cells == expected_cells, "ABBA parameters or selected cells differ")
    require(isinstance(report.get("coverage"), dict), "invalid ABBA coverage")
    require(report["coverage"].get("ids") == ids and report["coverage"].get("count") == len(ids) and
            len(ids) <= source["total_cells"], "ABBA coverage omits or repeats selected cells")
    if report.get("subset") == "full" and not report.get("only"):
        require(len(ids) == source["total_cells"], "full ABBA coverage is incomplete")
    require(not report["coverage"].get("pending_pins"), "ABBA still has unmeasured load floors")
    for arm in ("candidate", "reference"):
        require(isinstance(report.get(arm), dict), f"invalid {arm} identity")
        require(re.fullmatch(r"[0-9a-f]{64}", report.get(arm, {}).get("sha256", "")), f"missing {arm} binary digest")
    require(candidate is None or report["candidate"]["sha256"] == candidate["sha256"], "ABBA measured another candidate binary")
    environment = report.get("environment", {})
    require(isinstance(environment, dict), "invalid ABBA environment")
    require(environment.get("python_runtime") == report["instrument_fingerprint"]["python"],
            "Python measurement environment differs from the instrument identity")
    for key in ("server_cpus", "load_cpus", "server_physical", "load_physical"):
        value = environment.get(key)
        require(isinstance(value, list) and value and all(type(cpu) is int and cpu >= 0 for cpu in value)
                and len(value) == len(set(value)), f"invalid ABBA {key}")
    require(len(environment["server_physical"]) <= 32 and
            not set(environment["server_cpus"]) & set(environment["load_cpus"]), "invalid regression CPU allocation")
    for key in ("uname", "memtier_sha256", "memtier_version", "keys", "data_bytes", "key_pattern", "split_ratio", "population_by_arm"):
        require(environment.get(key), f"missing measurement environment: {key}")
    quiet = report.get("quiet_box", {})
    require(isinstance(quiet, dict), "invalid quiet-box evidence")
    require(quiet.get("complete") is True and quiet.get("interference", "missing") is None,
            "quiet-box evidence missing, incomplete, or contended")
    qstart = number(quiet.get("started_at"), "quiet start", positive=True)
    qend = number(quiet.get("finished_at"), "quiet end", positive=True)
    number(quiet.get("sample_interval_seconds"), "quiet sample interval", positive=True)
    require(type(quiet.get("samples")) is int and quiet["samples"] >= 2 and qstart < qend <= now + 1,
            "quiet observer did not complete its sampling interval")
    monitored = quiet.get("cpus", [])
    requested = quiet.get("requested_cpus", [])
    require(set(requested) == set(environment["server_cpus"] + environment["load_cpus"]) and
            set(requested) <= set(monitored), "quiet observer did not watch the measurement CPUs")
    # The monitor sets complete only after its final sample. Sample iteration itself takes time,
    # so samples*interval is not an elapsed-time bound; requiring that would reject healthy runs.
    # Timestamp bounds and the aggregate window duration independently rule out preflight-only data.
    require(started <= qstart and qend <= started + elapsed + 1, "quiet timestamps are outside this ABBA run")
    windows = 0
    for cell, row in zip(cells, rows):
        require(row.get("verdict") == "PASS" and row.get("instrument_valid", True) is True,
                f"nonpassing ABBA cell: {cell['id']}")
        assessment = row.get("assessment", {})
        require(isinstance(assessment, dict), "invalid ABBA assessment")
        require(assessment.get("verdict") == "PASS" and assessment.get("reasons") == [],
                f"unassessed/failed ABBA cell: {cell['id']}")
        require(assessment.get("saturation_exempt") is (cell["depth"] == 1), "invalid saturation exemption")
        require(signed_number(assessment.get("loss_pct"), "loss") <=
                number(assessment.get("threshold_pct"), "threshold"), "cell loss exceeds its threshold")
        rounds = row.get("rounds", [])
        require(isinstance(rounds, list) and rounds and all(isinstance(block, dict) for block in rounds),
                f"unreached ABBA cell: {cell['id']}")
        for block in rounds:
            runs = block.get("runs", [])
            require(isinstance(runs, list) and all(isinstance(run, dict) for run in runs), "invalid ABBA runs")
            require([run.get("arm") for run in runs] == ORDER, "incomplete or reordered ABBA measurements")
            for index, run in enumerate(runs, 1):
                require(run.get("complete") is True and not run.get("error") and
                        run.get("artifacts") == f"{cell['id']}/n{block['instances']}-{index}-{run['arm']}",
                        f"incomplete measurement: {cell['id']}")
                for field in ("rate", "latency_ms", "window_seconds", "commands"):
                    number(run.get(field), "measurement " + field, positive=True)
                require(run["window_seconds"] >= report["window_seconds"], "shortened measurement window")
                require(type(run.get("pid")) is int and run["pid"] > 0, "measurement never booted a server")
                if cell["op"] == "REORDER":
                    number(run.get("p999_ms"), "short p99.9", positive=True)
                    number(run.get("long_p999_ms"), "long p99.9", positive=True)
                windows += run["window_seconds"]
    require(qend - qstart >= windows, "quiet observer did not span all measurement windows")
    return started, environment


def null_resolution(report):
    """A byte-identical gain is instrument error just as a loss is.

    Comparison assessments deliberately reject only regressions. A null instead
    requires BOTH signs of the paired delta to fit the same measured reference
    spread. Recompute from every raw block, including unselected escalation probes:
    neither a cached PASS nor selecting another rung may hide a failed control.
    There is no new floor, multiplier, or change to a code comparison's threshold.
    """
    evidence = []
    for row in report["cells"]:
        cell = row["cell"]
        metric = {"auto": "latency_ms" if cell["depth"] == 1 else "rate",
                  "rate": "rate", "latency": "latency_ms", "p999": "p999_ms"}.get(cell.get("score", "auto"))
        require(metric is not None, f"unknown scored null metric: {cell['id']}")
        metrics = [metric] + (["long_p999_ms"] if metric == "p999_ms" else [])
        for block in row["rounds"]:
            runs = block["runs"]
            require([run.get("arm") for run in runs] == ORDER, "incomplete or reordered null block")
            for scored in metrics:
                a1, b1, b2, a2 = [number(run.get(scored), "null " + scored, positive=True) for run in runs]
                denominator = number(a1 + a2, "null reference sum", positive=True)
                delta = signed_number(100 * ((b1 - a1) + (b2 - a2)) / denominator, "null paired delta")
                threshold = number(200 * abs(a1 - a2) / denominator, "null reference spread")
                require(abs(delta) <= threshold,
                        f"{cell['id']} n={block['instances']} {scored} null resolution failed: "
                        f"absolute paired delta {abs(delta):.6g}% (signed {delta:+.6g}%) "
                        f"exceeds measured reference spread {threshold:.6g}%")
                evidence.append(dict(cell=cell["id"], instances=block["instances"], metric=scored,
                                     delta_pct=delta, absolute_delta_pct=abs(delta), reference_spread_pct=threshold))
    return evidence


def null_result(report, *, now):
    validate_measurements(report, now=now)
    require(report.get("run_kind") == "null-control" and report.get("comparison_trusted") is False and
            report.get("verdict") == "PARTIAL", "control collection is not a code-comparison PASS")
    require(report["candidate"]["sha256"] == report["reference"]["sha256"], "null arms are not byte-identical")
    population = report["environment"]["population_by_arm"]
    require(isinstance(population, dict) and set(population) == {"A", "B"} and
            all(isinstance(value, str) and value for value in population.values()) and
            population["A"] == population["B"], "null arms used different population methods")
    return {"verdict": "PASS", "binary_sha256": report["candidate"]["sha256"],
            "ids": report["coverage"]["ids"], "resolution": null_resolution(report)}


def validate_null(report, *, now):
    # Validate every collected cell before matching a requested subset. A failed full control
    # cannot donate just its passing smoke rows: that would hide the instrument's own failure.
    expected = null_result(report, now=now)
    require(report.get("null_control") == expected, "null control did not complete and pass")
    return utc_seconds(report["started_utc"]), report["environment"]


def instrument(environment):
    # Ports and executable pathnames do not alter the experiment. Every other current or future
    # field must match, including the generator digest and each arm's population method.
    return {key: value for key, value in environment.items()
            if key not in ("port", "permitted_ports", "memtier_path")}


def match_null(comparison, control, *, now):
    require(comparison.get("run_kind") == "comparison", "a null collection cannot replace a regression comparison")
    started, environment = validate_measurements(comparison, now=now)
    null_started, null_environment = validate_null(control, now=now)
    require(0 <= started - null_started <= NULL_MAX_AGE,
            "standing null is from the future or more than 24 hours old")
    require(null_started + control["elapsed_seconds"] <= started + 1,
            "standing null did not finish before this comparison started")
    # Correctness tests/hooks may change between a null and a later comparison.
    # Both reports still carry the broad release hash; only the actual instrument
    # must match here. Old reports without scoped provenance fail validation above.
    require(comparison["instrument_fingerprint"] == control["instrument_fingerprint"], "null instrument differs")
    require(comparison["cell_source"]["sha256"] == control["cell_source"]["sha256"] and
            comparison["cell_source"]["total_cells"] == control["cell_source"]["total_cells"],
            "null inventory differs")
    require(comparison["window_seconds"] == control["window_seconds"], "null used another measurement window")
    require(instrument(environment) == instrument(null_environment), "null used another geometry or measurement environment")
    by_id = {row["cell"]["id"]: row["cell"] for row in control["cells"]}
    plans = {row["cell"]["id"]: [block["instances"] for block in row["rounds"]]
             for row in control["cells"]}
    for row in comparison["cells"]:
        require(by_id.get(row["cell"]["id"]) == row["cell"],
                f"null does not cover this exact cell: {row['cell']['id']}")
        require(plans[row["cell"]["id"]] == [block["instances"] for block in row["rounds"]],
                f"null used another measured load ladder: {row['cell']['id']}")
    return {"status": "MATCHED", "sha256": digest(canonical(control)),
            "control_binary_sha256": control["candidate"]["sha256"],
            "matched_ids": comparison["coverage"]["ids"], "age_seconds": started - null_started}


def validate_comparison(comparison, control, *, now):
    require(comparison.get("verdict") == "PASS" and comparison.get("comparison_trusted") is True and
            not comparison.get("only"), "comparison is partial or lacks standing-null certification")
    matched = match_null(comparison, control, now=now)
    require(comparison.get("standing_null") == matched, "comparison's matched null evidence differs")
    return utc_seconds(comparison["started_utc"]), comparison["environment"]
