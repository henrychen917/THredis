#!/usr/bin/env python3
"""Unwired calibration prototype: two training ABBA blocks and one independent holdout.

Nothing here certifies a gate or changes abbagate.assess(). A future live adapter must
supply complete normal quiet/saturation/workload evidence, whose immutable digest is
bound into each observation. The helper verifies its own mathematics and provenance;
it cannot turn an asserted prerequisite digest into proof that a server actually ran.

The empirical eight-observation range is observed resolution, NOT a confidence bound.
The additional Student prediction bound assumes iid NORMAL INDIVIDUAL boot measurements
with unchanged variance, including between calibration, holdout and later comparisons.
For C=(B1+B2-A1-A2)/2, Var(C)=sigma^2. Independent training gives C/s8 ~ t7.
Bonferroni covers 182 scored metrics in ONE prospective sweep at family error .05;
it is not a lifetime guarantee across repeated uses of a fixed calibration. Eight
observations plus a holdout cannot establish normality, independence or absence of
nonlinear drift. Both the range and the conditional bound are reported explicitly;
neither is distribution-free. Invalid historical diagnostics cannot seed this helper.

All same-binary block spans, aggregate training span, and proposed resolution must
stay <=2%. A high bound FAILS; it is never clamped or replaced by the smaller range.
The floor freezes before any holdout measurement. Failure belongs to the immutable
campaign: a passing subset cannot be salvaged, and there is no retry-until-green loop.
Only --self-test is executable; real calibration/wiring awaits a stable instrument.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import statistics

ORDER = ("A", "B", "B", "A")
FAMILY_METRICS = 182
ALPHA = .05
CEILING = 2.
MAX_AGE = 24 * 60 * 60
POLICY = "unwired-iid-normal-eight-boot-calibration-v1"


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def number(value, name, *, positive=True):
    require(type(value) in (int, float) and math.isfinite(value) and
            (value > 0 if positive else value >= 0), f"invalid {name}")
    return value


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(value):
    return hashlib.sha256(encoded(value)).hexdigest()


def sha(value, name):
    require(isinstance(value, str) and re.fullmatch("[0-9a-f]{64}", value), f"invalid {name} digest")


def t7_survival(x):
    """Integrate NIST's exact Student PDF, not a normal approximation or a literal.

    https://www.itl.nist.gov/div898/handbook/eda/section3/eda3664.htm
    Substitute x=sqrt(nu)*tan(theta): the tail is
      Gamma(4)/(sqrt(pi)*Gamma(3.5)) * integral_theta^(pi/2) cos(u)^6 du.
    The elementary reduction integral cos^n(u)du = sin(u)cos^(n-1)(u)/n
    + (n-1)/n integral cos^(n-2)(u)du gives a deterministic inversion.
    This fixed .05/(2*182) tail is comfortably above cancellation roundoff.
    """
    number(x, "Student argument", positive=False)
    def primitive(theta, n):
        if n == 0:
            return theta
        return (math.sin(theta) * math.cos(theta) ** (n - 1) / n +
                (n - 1) / n * primitive(theta, n - 2))
    constant = math.gamma(4) / (math.sqrt(math.pi) * math.gamma(3.5))
    return constant * (primitive(math.pi / 2, 6) - primitive(math.atan(x / math.sqrt(7)), 6))


def t7_quantile(tail_probability=ALPHA / (2 * FAMILY_METRICS)):
    require(0 < tail_probability < .5, "invalid Student tail probability")
    low, high = 0., 16.
    require(t7_survival(high) < tail_probability, "Student quantile outside supported bracket")
    for _ in range(64):
        middle = (low + high) / 2
        if t7_survival(middle) > tail_probability:
            low = middle
        else:
            high = middle
    return high


def span(values):
    return 200 * (max(values) - min(values)) / (max(values) + min(values))


def block(values):
    require(len(values) == 4, "ABBA requires four observations")
    for value in values:
        number(value, "metric")
    a1, b1, b2, a2 = values
    return {"range_pct": span(values), "reference_span_pct": span([a1, a2]),
            "candidate_span_pct": span([b1, b2]), "reference_mean": (a1 + a2) / 2,
            "contrast": ((b1 - a1) + (b2 - a2)) / 2}


def freeze(values):
    require(len(values) == 8, "calibration requires exactly eight observations")
    for value in values:
        number(value, "calibration value")
    blocks = [block(values[:4]), block(values[4:])]
    mean, sd = statistics.mean(values), statistics.stdev(values)
    empirical = max(values) - min(values)
    multiplier = t7_quantile()
    model = multiplier * sd
    floor = max(empirical, model)
    result = dict(mean=mean, sd=sd, empirical_range=empirical,
                  aggregate_range_pct=span(values), multiplier=multiplier,
                  conditional_model_bound=model, frozen_floor=floor,
                  resolution_pct=100 * floor / mean, blocks=blocks, reasons=[])
    if any(max(b["range_pct"], b["reference_span_pct"], b["candidate_span_pct"]) > CEILING for b in blocks):
        result["reasons"].append("calibration block exceeds 2% same-binary stability ceiling")
    if result["aggregate_range_pct"] > CEILING:
        result["reasons"].append("aggregate eight-observation range exceeds 2% stability ceiling")
    if result["resolution_pct"] > CEILING:
        result["reasons"].append("proposed resolution bound exceeds 2%; instrument unusable")
    result["verdict"] = "FAIL" if result["reasons"] else "PASS"
    return result


def holdout(frozen, values):
    result = block(values)
    result["resolution_pct"] = 100 * frozen["frozen_floor"] / result["reference_mean"]
    result["reasons"] = []
    if frozen["verdict"] != "PASS":
        result["reasons"].append("calibration failed before independent holdout")
    if max(result["range_pct"], result["reference_span_pct"], result["candidate_span_pct"]) > CEILING:
        result["reasons"].append("holdout block exceeds 2% same-binary stability ceiling")
    if result["resolution_pct"] > CEILING:
        result["reasons"].append("holdout-relative resolution exceeds 2%; instrument unusable")
    if abs(result["contrast"]) > frozen["frozen_floor"]:
        result["reasons"].append("independent holdout exceeds frozen symmetric floor")
    result["verdict"] = "FAIL" if result["reasons"] else "PASS"
    return result


def validate_manifest(manifest):
    require(set(manifest) == {"schema", "policy", "campaign_id", "created_at", "binding", "control_binary_sha256", "members"},
            "invalid calibration manifest schema")
    require(manifest["schema"] == 1 and manifest["policy"] == POLICY, "unknown calibration policy")
    require(isinstance(manifest["campaign_id"], str) and manifest["campaign_id"], "missing campaign identity")
    number(manifest["created_at"], "campaign creation timestamp")
    sha(manifest["control_binary_sha256"], "control binary")
    binding = manifest["binding"]
    require(set(binding) == {"instrument_sha256", "inventory_sha256", "environment", "window_seconds"}, "invalid measurement binding")
    sha(binding["instrument_sha256"], "instrument")
    sha(binding["inventory_sha256"], "inventory")
    require(isinstance(binding["environment"], dict) and binding["environment"], "missing complete measurement environment")
    number(binding["window_seconds"], "window seconds")
    members = manifest["members"]
    require(isinstance(members, list) and members, "empty campaign membership")
    ids = []
    for member in members:
        require(set(member) == {"cell", "metrics", "instances", "load_layout"}, "invalid cell binding")
        require(isinstance(member["cell"], dict) and isinstance(member["cell"].get("id"), str), "missing exact cell definition")
        ids.append(member["cell"]["id"])
        require(type(member["instances"]) is int and member["instances"] > 0, "missing measured load pin")
        require(isinstance(member["load_layout"], list) and len(member["load_layout"]) == member["instances"], "missing generator layout")
        metrics = member["metrics"]
        require(isinstance(metrics, list) and metrics and len(metrics) == len(set(metrics)) and
                all(metric in ("rate", "latency_ms", "p999_ms", "long_p999_ms") for metric in metrics), "invalid scored metric set")
    require(len(ids) == len(set(ids)) and all(ids), "duplicate/empty campaign cell")
    require(sum(len(m["metrics"]) for m in members) <= FAMILY_METRICS, "campaign exceeds 182-metric bound")
    encoded(manifest)  # Reject non-JSON/NaN provenance before the first producer call.


def validate_observations(manifest, member, runs):
    require(len(runs) == 12, "exactly twelve observations required; missing or extra producer calls")
    previous_end = manifest["created_at"]
    ids, processes = set(), set()
    for index, run in enumerate(runs):
        require(run.get("arm") == ORDER[index % 4] and run.get("sequence") == index + 1, "ABBA producer order differs")
        require(run.get("complete") is True and not run.get("error"), "incomplete observation")
        require(run.get("source_kind") == "normal-null-observation" and run.get("normal_gate_eligible") is True,
                "diagnostic-only observations cannot calibrate the normal instrument")
        require(run.get("binding_sha256") == digest(manifest["binding"]) and
                run.get("member_sha256") == digest(member), "observation instrument/environment/cell identity differs")
        require(run.get("binary_sha256") == manifest["control_binary_sha256"], "null observation bytes differ")
        sha(run.get("prerequisite_evidence_sha256"), "quiet/saturation/workload prerequisite evidence")
        ident = run.get("observation_id")
        require(isinstance(ident, str) and ident and ident not in ids, "reused observation")
        ids.add(ident)
        proc = run.get("process_identity")
        require(isinstance(proc, list) and len(proc) == 2 and all(type(x) is int and x > 0 for x in proc), "invalid owned process identity")
        require(tuple(proc) not in processes, "holdout/calibration reused a boot")
        processes.add(tuple(proc))
        start = number(run.get("started_at"), "observation start")
        end = number(run.get("finished_at"), "observation finish")
        require(start >= previous_end and end > start, "overlapping/nonchronological observations")
        previous_end = end
        duration = number(run.get("window_seconds"), "observed window")
        require(duration >= manifest["binding"]["window_seconds"] and end-start >= duration,
                "incomplete measured window")
        require(set(run.get("metrics", {})) == set(member["metrics"]), "missing/unexpected scored metric")
        for value in run["metrics"].values():
            number(value, "observed metric")


def evaluate_row(manifest, member, runs):
    validate_observations(manifest, member, runs)
    frozen = {metric: freeze([run["metrics"][metric] for run in runs[:8]]) for metric in member["metrics"]}
    validation = {metric: holdout(frozen[metric], [run["metrics"][metric] for run in runs[8:]]) for metric in member["metrics"]}
    verdict = "PASS" if all(v["verdict"] == "PASS" for v in validation.values()) else "FAIL"
    return dict(cell_id=member["cell"]["id"], observations=runs, frozen=frozen, holdout=validation, verdict=verdict)


def collect_campaign(manifest, producer, publish):
    """A producer call represents one real fresh boot, never a hand-made rounds list.

    publish must persist each immutable event (e.g. write_exclusive). The manifest is
    published before any measurement, and frozen statistics before call nine. There
    is deliberately no runnable live producer/CLI or alternative normal-gate path.
    """
    validate_manifest(manifest)
    manifest = copy.deepcopy(manifest)
    publish("manifest", copy.deepcopy(manifest))
    rows = []
    for member in manifest["members"]:
        observations = []
        frozen = None
        for index in range(12):
            try:
                observation = producer(copy.deepcopy(member), index + 1, ORDER[index % 4])
            except Exception as error:
                observation = {"sequence": index + 1, "arm": ORDER[index % 4], "error": f"{type(error).__name__}: {error}"}
            observations.append(copy.deepcopy(observation))
            publish(f"{member['cell']['id']}-observation-{index + 1}", copy.deepcopy(observation))
            if index == 7:
                try:
                    frozen = {metric: freeze([run["metrics"][metric] for run in observations]) for metric in member["metrics"]}
                except (KeyError, TypeError, ValueError) as error:
                    frozen = {"error": str(error)}
                publish(f"{member['cell']['id']}-frozen", copy.deepcopy(frozen))
        try:
            row = evaluate_row(manifest, member, observations)
            require(row["frozen"] == frozen, "frozen calibration changed after holdout")
        except (KeyError, TypeError, ValueError) as error:
            row = dict(cell_id=member["cell"]["id"], observations=observations, frozen=frozen,
                       verdict="FAIL", error=str(error))
        rows.append(row)
    report = dict(schema=1, policy=POLICY, normal_gate_eligible=False, manifest=manifest,
                  manifest_sha256=digest(manifest), rows=rows,
                  verdict="PASS" if all(row["verdict"] == "PASS" for row in rows) else "FAIL")
    if report["verdict"] == "PASS":
        try:
            validate_campaign(report, digest(manifest))
        except (KeyError, TypeError, ValueError) as error:
            report.update(verdict="FAIL", error=str(error))
    publish("report", copy.deepcopy(report))
    return report


def write_exclusive(path, value):
    # Hash binding prevents accidental mutation; it is not authentication. A future
    # receipt must retain the manifest digest independently of this mutable Python dict.
    with Path(path).open("xb") as file:
        file.write(encoded(value) + b"\n")


def validate_campaign(report, expected_manifest_sha256):
    require(report.get("schema") == 1 and report.get("policy") == POLICY and
            report.get("normal_gate_eligible") is False, "unknown or promoted calibration prototype")
    manifest = report["manifest"]
    validate_manifest(manifest)
    require(digest(manifest) == report.get("manifest_sha256") == expected_manifest_sha256, "campaign membership/identity changed")
    require(report.get("verdict") == "PASS", "failed campaign cannot donate passing cells")
    require(len(report["rows"]) == len(manifest["members"]), "campaign missing a member")
    previous_end = manifest["created_at"]
    observation_ids, boot_identities = set(), set()
    for member, row in zip(manifest["members"], report["rows"]):
        require(row == evaluate_row(manifest, member, row["observations"]) and row["verdict"] == "PASS",
                "campaign evidence changed or contains a failed cell")
        for observation in row["observations"]:
            require(observation["started_at"] >= previous_end, "campaign measurements overlap or change order")
            previous_end = observation["finished_at"]
            require(observation["observation_id"] not in observation_ids and
                    tuple(observation["process_identity"]) not in boot_identities,
                    "campaign reused an observation or boot across cells")
            observation_ids.add(observation["observation_id"])
            boot_identities.add(tuple(observation["process_identity"]))
    return manifest


def floor_for_measurement(report, expected_manifest_sha256, cell_id, comparison):
    """Return a diagnostic proposal, never a normal-gate threshold.

    The comparison artifact digest and its cell-local start are bound into the result.
    Reusing/repacking a report cannot renew the oldest calibration observation. A
    sweep may last >24h if each cell's own control was fresh when that cell ran.
    """
    manifest = validate_campaign(report, expected_manifest_sha256)
    require(comparison.get("binding_sha256") == digest(manifest["binding"]), "comparison environment/instrument differs")
    sha(comparison.get("artifact_sha256"), "comparison artifact")
    start = number(comparison.get("measurement_started_at"), "cell comparison start")
    for member, row in zip(manifest["members"], report["rows"]):
        if member["cell"]["id"] != cell_id:
            continue
        require(comparison.get("member_sha256") == digest(member), "comparison cell/load differs")
        first = row["observations"][0]["started_at"]
        last = row["observations"][-1]["finished_at"]
        require(last <= start and 0 <= start-first <= MAX_AGE, "cell control is unfinished, future or over 24 hours old")
        means = comparison.get("reference_means", {})
        require(set(means) == set(member["metrics"]), "missing comparison reference metrics")
        floors = {metric: 100 * row["frozen"][metric]["frozen_floor"] /
                  number(means[metric], "current reference mean") for metric in member["metrics"]}
        require(all(floor <= CEILING for floor in floors.values()), "current-relative resolution exceeds 2%; instrument unusable")
        return dict(normal_gate_eligible=False, campaign_sha256=digest(report),
                    manifest_sha256=expected_manifest_sha256, comparison=copy.deepcopy(comparison),
                    calibration_started_at=first, holdout_finished_at=last, age_seconds=start-first,
                    floor_pct=floors)
    raise ValueError("campaign does not contain requested cell")


def self_test():
    import tempfile
    import unittest
    class Controls(unittest.TestCase):
        def setup_campaign(self, count=1, poison=None):
            members = [dict(cell={"id":f"c{i}","op":"GET","depth":32}, metrics=["rate"], instances=1,
                            load_layout=[{"cpus":[32],"threads":1,"clients":1}]) for i in range(count)]
            manifest = dict(schema=1,policy=POLICY,campaign_id="fixed-control-campaign",created_at=1000.,
                control_binary_sha256="a"*64,binding=dict(instrument_sha256="b"*64,inventory_sha256="c"*64,
                environment={"server_cpus":[0],"load_cpus":[32]},window_seconds=20.),members=members)
            calls, events = [], {}
            def publish(name, value):
                self.assertNotIn(name, events)
                events[name] = copy.deepcopy(value)
            def producer(member, sequence, arm):
                calls.append((member["cell"]["id"], sequence, arm))
                if sequence > 8:
                    self.assertIn(member["cell"]["id"]+"-frozen", events)
                index = len(calls)
                value = 100 + ([0,.01,-.01,0,.01,0,0,-.01][sequence-1] if sequence <= 8 else 0)
                run = dict(arm=arm,sequence=sequence,complete=True,observation_id=f"owned-{index}",
                    source_kind="normal-null-observation",normal_gate_eligible=True,
                    process_identity=[index,100+index],started_at=1000+index*30,finished_at=1021+index*30,
                    window_seconds=20.,metrics={"rate":value},binary_sha256="a"*64,
                    binding_sha256=digest(manifest["binding"]),member_sha256=digest(member),
                    prerequisite_evidence_sha256="d"*64)
                if poison:
                    poison(member, sequence, run)
                return run
            report = collect_campaign(manifest, producer, publish)
            return manifest, report, calls, events

        def test_nist_table_and_simultaneous_multiplier(self):
            # NIST's PRIMARY table row nu=7, probabilities .9,.95,.975,.99,.995,.999.
            # https://www.itl.nist.gov/div898/handbook/eda/section3/eda3672.htm
            for cdf, printed in ((.9,1.415),(.95,1.895),(.975,2.365),(.99,2.998),(.995,3.499),(.999,4.785)):
                self.assertAlmostEqual(t7_quantile(1-cdf), printed, delta=.0005)
            value=t7_quantile()
            self.assertAlmostEqual(value,6.7106972935,places=8)
            self.assertAlmostEqual(t7_survival(value), ALPHA/(2*FAMILY_METRICS),places=14)

        def test_actual_producer_is_twelve_fresh_calls_with_a_preholdout_freeze(self):
            manifest,report,calls,events=self.setup_campaign()
            self.assertEqual(calls,[("c0",i+1,ORDER[i%4]) for i in range(12)])
            self.assertEqual(report["verdict"],"PASS")
            self.assertEqual(report["rows"][0]["frozen"],events["c0-frozen"])
            validate_campaign(report,digest(manifest))
            self.assertFalse(report["normal_gate_eligible"])

        def test_holdout_failure_cannot_widen_floor_or_donate_passing_member(self):
            def poison(member,n,run):
                if member["cell"]["id"]=="c1" and n>8 and run["arm"]=="B":run["metrics"]["rate"]=101
            manifest,report,calls,events=self.setup_campaign(2,poison)
            self.assertEqual(len(calls),24)
            self.assertEqual([r["verdict"] for r in report["rows"]],["PASS","FAIL"])
            self.assertEqual(report["rows"][1]["frozen"],events["c1-frozen"])
            with self.assertRaisesRegex(ValueError,"cannot donate"):validate_campaign(report,digest(manifest))
            report["verdict"]="PASS";report["rows"].pop()
            with self.assertRaisesRegex(ValueError,"missing a member"):validate_campaign(report,digest(manifest))

        def test_block_aggregate_and_high_resolution_fail_without_clamping(self):
            for values, reason in (([98,100,100,102]*2,"block exceeds"),
                    ([98.99]*4+[101.01]*4,"aggregate eight"),([100,101]*4,"resolution bound exceeds")):
                frozen=freeze(values)
                self.assertEqual(frozen["verdict"],"FAIL")
                self.assertTrue(any(reason in why for why in frozen["reasons"]))
                self.assertEqual(frozen["frozen_floor"],max(frozen["empirical_range"],frozen["conditional_model_bound"]))
            frozen=freeze([100]*8)
            self.assertEqual(holdout(frozen,[100,103,103,100])["verdict"],"FAIL")
            self.assertEqual(frozen["frozen_floor"],0)

        def test_raw_evidence_identity_and_floor_tampering_are_rejected(self):
            for poison in (lambda r:r["rows"][0]["frozen"]["rate"].update(frozen_floor=2),
                           lambda r:r["rows"][0]["observations"][8].update(binary_sha256="e"*64),
                           lambda r:r["rows"][0]["observations"][8].update(process_identity=[1,101]),
                           lambda r:r["rows"][0]["observations"][8].update(started_at=1001),
                           lambda r:r["manifest"]["members"][0]["cell"].update(depth=1)):
                manifest,report,_,_=self.setup_campaign(); pinned=digest(manifest);poison(report)
                with self.assertRaises(ValueError):validate_campaign(report,pinned)

        def test_diagnostic_inputs_and_cross_cell_reuse_are_not_calibration(self):
            def diagnostic(member,n,run):
                run.update(source_kind="background-qualification",normal_gate_eligible=False)
            manifest,report,calls,_=self.setup_campaign(poison=diagnostic)
            self.assertEqual(len(calls),12)
            self.assertEqual(report["verdict"],"FAIL")
            self.assertIn("diagnostic-only",report["rows"][0]["error"])
            manifest,report,_,_=self.setup_campaign(2)
            report["rows"][1]["observations"][0]["observation_id"]="owned-1"
            with self.assertRaisesRegex(ValueError,"across cells"):validate_campaign(report,digest(manifest))

        def test_per_cell_age_and_original_timestamps_are_bound(self):
            manifest,report,_,_=self.setup_campaign(); member=manifest["members"][0]
            comparison=dict(binding_sha256=digest(manifest["binding"]),member_sha256=digest(member),
                artifact_sha256="e"*64,measurement_started_at=1500.,reference_means={"rate":100.})
            value=floor_for_measurement(report,digest(manifest),"c0",comparison)
            self.assertEqual(value["age_seconds"],470)
            self.assertEqual(value["comparison"],comparison)
            for start in (1200.,1030+MAX_AGE+1):
                comparison["measurement_started_at"]=start
                with self.assertRaisesRegex(ValueError,"cell control"):floor_for_measurement(report,digest(manifest),"c0",comparison)
            comparison["measurement_started_at"]=1500.;comparison["reference_means"]["rate"]=.01
            with self.assertRaisesRegex(ValueError,"resolution exceeds"):floor_for_measurement(report,digest(manifest),"c0",comparison)

        def test_long_campaign_uses_each_actual_cell_measurement_timestamp(self):
            manifest,report,_,_=self.setup_campaign(2)
            for run in report["rows"][1]["observations"]:
                run["started_at"]+=90000;run["finished_at"]+=90000
            self.assertGreater(report["rows"][1]["observations"][-1]["finished_at"]-manifest["created_at"],MAX_AGE)
            for index,start in ((0,1500.),(1,92000.)):
                member=manifest["members"][index]
                comparison=dict(binding_sha256=digest(manifest["binding"]),member_sha256=digest(member),
                    artifact_sha256="e"*64,measurement_started_at=start,reference_means={"rate":100.})
                proposed=floor_for_measurement(report,digest(manifest),f"c{index}",comparison)
                self.assertLess(proposed["age_seconds"],MAX_AGE)

        def test_failed_producer_preserves_twelve_attempts_and_campaign_failure(self):
            def poison(member,n,run):
                if n==3:raise RuntimeError("measurement never completed")
            manifest,report,calls,_=self.setup_campaign(poison=poison)
            self.assertEqual(len(calls),12)
            self.assertEqual(report["verdict"],"FAIL")
            self.assertIn("measurement never completed",report["rows"][0]["observations"][2]["error"])

        def test_exclusive_publication_cannot_rewrite_failed_history(self):
            with tempfile.TemporaryDirectory() as tmp:
                path=Path(tmp)/"report.json";write_exclusive(path,{"verdict":"FAIL"})
                with self.assertRaises(FileExistsError):write_exclusive(path,{"verdict":"PASS"})
                self.assertEqual(json.loads(path.read_text())["verdict"],"FAIL")
    return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls)).wasSuccessful() else 1


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test",action="store_true",required=True)
    parser.parse_args()
    raise SystemExit(self_test())
