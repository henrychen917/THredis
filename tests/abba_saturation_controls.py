#!/usr/bin/env python3
"""Prepare loaded probes or capture idle controls; productive-role-v1 stays UNVALIDATED.

plan writes commands only. They use abbagate's real producer and measurement loop,
20-second windows, the verified stored reference, and the candidate. Diagnostic cell copies do not establish
pins, retire inventory, or earn a gate receipt. Legacy saturation FAIL remains FAIL.

idle captures real LBSIGNALS without pretending idle sockets completed memtier work.
It shares the production ownership/cleanup and quiet observer helpers. --spin-role
requires an actual high-CPU, nonproductive role; an unarmed spinner fails loudly.
Neither command adopts a metric or lowers a threshold. Launch only on the root's
coordinated quiet box; merely preparing the plan reserves no CPUs.
"""
import argparse
from contextlib import ExitStack
from dataclasses import asdict, replace
import json
import os
from pathlib import Path
import shlex
import sys
import time

import abbagate as abba
from _gate_process import Conn, install_signals, pin_driver, server
from abba_saturation import parse_snapshot, productive_saturation
from gate_receipt import harness_fingerprint
from gate_quiet import QuietMonitor


# Eight distinct paths need their own low/loaded/confirmation evidence. A local
# GET cannot validate owner SET or multi-key scatter/gather, nor transfer between
# fused and split. The p8 multi-key knee is deliberately sampled; this is not the
# knob cross product. n1/n8/n12 provide 16/128/192 workers at 512 connections with
# the declared 192-load-CPU geometry. These are probes, NEVER shipping pins.
# Inherited P:P ranges are partitioned per process, so changing instance count may
# also change cross-client key-sequence correlation. Numerical plateaus cannot
# establish pure generator-capacity causality until that stream mapping is settled.
FAMILIES = (("h09", "fused local GET"), ("h25", "split local GET / idle executors"),
            ("h12", "fused owner SET"), ("h32", "split owner SET"),
            ("m44", "fused scatter/gather MGET p8"), ("m68", "split scatter/gather MGET p8"),
            ("m47", "fused atomic MSET p8"), ("m71", "split atomic MSET p8"))
RUNGS = (("reduced", 1), ("loaded", 8), ("confirmation", 12))

# Exact throwaway-only spin transform, provided for the coordinator's separate
# build. It is AFTER stop/role checks, normal owner dispatch, busy/idle/CPU
# accounting, and the placement-frozen acknowledgement branch. Thus empty EX
# passes consume CPU without becoming command progress or bypassing ownership.
# It suppresses the following idle sweep/park; use only fresh empty stores with
# flip/key-LB/client-LB disabled. It is not suitable as a workload server.
# The split EX control is sufficient to disprove CPU-only certification. A fused
# spinner needs a separately audited IoLoop park transform, not this EX branch.
SPINNER_FIND = "if (++idle_spins < kExSpinBudget) { sig.spins++; __builtin_ia32_pause(); continue; }"
SPINNER_REPLACE = "if (true) { sig.spins++; __builtin_ia32_pause(); continue; } // diagnostic-only empty EX spin"


def planned_cells(source):
    by_id = {cell.id: cell for cell in abba.read_cells(source)}
    result = []
    # A single active connection is concentrated useful work, unlike the seven
    # already measured idle/polling negatives. It attacks accidentally excluding
    # inactive role members from the denominator. Four sockets add no new path.
    for ident in ("h09", "h25"):
        cell = by_id[ident]
        result.append((cell, replace(cell, id=f"diag-{ident}-single", instances=1,
                                     conns=1, smoke=False), "single", "active concentrated GET negative"))
    # Breadth first reaches each family before spending on another load level.
    # Independent commands retain all four measurements, including unstable ones.
    for phase, rung in RUNGS:
        for ident, reason in FAMILIES:
            cell = by_id[ident]
            if cell.depth <= 1:
                raise ValueError(f"diagnostic source {ident} lost its throughput workload")
            result.append((cell, replace(cell, id=f"diag-{ident}-n{rung}",
                                        instances=rung, smoke=False), phase, reason))
    return result


def plan(args):
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.cells.resolve()
    rows = planned_cells(source)
    # Resolve only the existing last-push pin. No build or capability boot belongs
    # in plan creation; an arbitrary caller path is not a verified stored reference.
    commit = abba.git("rev-parse", "--verify", "origin/cpp^{commit}")
    reference = abba.manifest_reference(args.bench_bins, commit)
    if reference is None:
        raise ValueError("stored reference unavailable for origin/cpp; prepare its verified pin first")
    load_cpus = sorted(abba.cpus(args.load_cores) + abba.cpus(args.load_smt))
    abba.check_placement(abba.cpus(args.server_cores), abba.cpus(args.load_cores), (), abba.cpus(args.load_smt))
    if len(abba.cpus(args.server_cores)) != 32 or len(load_cpus) != 192:
        raise ValueError("this control plan requires 32 physical server and 192 disjoint load CPUs")
    manifest = dict(schema=2, validation="UNVALIDATED", comparison_trusted=False,
                    normal_gate_eligible=False, source_inventory=str(source), source_sha256=abba.sha256(source),
                    reference=dict(commit=commit, path=str(reference), sha256=abba.sha256(reference)),
                    candidate=dict(path=str(args.candidate_binary.resolve()), sha256=abba.sha256(args.candidate_binary)),
                    instrument_fingerprint=abba.instrument_fingerprint(abba.ROOT),
                    geometry=dict(server_cpus=abba.cpus(args.server_cores), load_cpus=load_cpus),
                    window_seconds=abba.WINDOW, blocks=len(rows), measurements=4 * len(rows),
                    notes=["No commands were launched or CPUs reserved.",
                           "Every probe compares stored reference A with candidate B; it is not a null.",
                           "Each single-connection negative must show real GET progress and reject the proposed95% role floor.",
                           "n1 is reduced load, not a promised underload. Active single GET cannot validate SET/multi-key pacing.",
                           "If a family's n1 still saturates, require that family's explicit paced follow-up; no current pacing hook is silently enabled.",
                           "Compare each arm's n8-to-n12 gain with its own observed repeatability; 192 workers does not establish generator headroom.",
                           "Inherited per-process P:P ranges may change cross-client key-sequence correlation at n1/n8/n12; numerical plateaus do not establish pure generator-capacity causality.",
                           "Retain every failed block. A complete stable decline can indicate congestion, not spare generator capacity.",
                           "Continue independent commands after statistical instability only when all four raw measurements and quiet postflight completed; never certify that campaign.",
                           "Abort further launches on quiet, identity, process, protocol, accounting, or incomplete-measurement failure.",
                           "No pin donation, metric adoption, or standing-null certification from this diagnostic campaign."],
                    probes=[])
    for original, cell, phase, reason in rows:
        fixture = args.output / (cell.id + ".cells")
        fixture.write_text("# Diagnostic copy; never a shipping pin or full inventory.\n" +
            " | ".join((cell.id, cell.mode, f"rl={cell.read_local}", f"ov={cell.overlap}",
                        f"ro={cell.reorder}", cell.op, f"p{cell.depth}", str(cell.conns),
                        "unmeasured", "unmeasured", str(cell.instances), f"atomic={cell.atomic}",
                        "score=rate", f"mix={cell.mix}", "smoke=0")) + "\n")
        argv = [sys.executable, str(abba.ROOT / "tests/abbagate.py"),
                "--candidate-binary", str(args.candidate_binary.resolve()),
                "--reference-binary", str(reference), "--bench-bins", str(args.bench_bins.resolve()),
                "--cells", str(fixture.resolve()), "--only", cell.id,
                "--subset", "full", "--collect-null", "0", "--build-reference", "0",
                "--server-cores", args.server_cores, "--server-smt", "",
                "--load-cores", args.load_cores, "--load-smt", args.load_smt,
                "--port", str(args.port), "--ports", f"{args.port}-{args.port}", "--memtier", args.memtier,
                "--output", str((args.output / cell.id).resolve())]
        if args.background_environment is not None:
            argv += ["--background-environment", str(args.background_environment.resolve())]
        layout = abba.load_layout(load_cpus, cell.instances, cell.conns)
        manifest["probes"].append(dict(source_cell=asdict(original), diagnostic_cell=asdict(cell),
            phase=phase, reason=reason, load_layout=layout, worker_threads=sum(p["threads"] for p in layout),
            fixture_sha256=abba.sha256(fixture), argv=argv, shell=shlex.join(argv)))
        print(shlex.join(argv))
    (args.output / "plan.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"PLAN ONLY: {len(rows)} blocks / {4 * len(rows)} measurements; no launches", file=sys.stderr)
    return 0


def review_probe(probe, manifest):
    # This is retained-evidence triage, not another calibration selector. A raw
    # block may be complete yet unstable; preserve that distinction and the old
    # FAIL. Missing/error evidence prohibits further live launches by the caller.
    argv = probe["argv"]
    output = Path(argv[argv.index("--output") + 1])
    result = dict(id=probe["diagnostic_cell"]["id"], phase=probe["phase"],
                  result_path=str(output / "results.json"), status="UNREACHED",
                  continuation_permitted=False, calibration_eligible=False)
    if not (output / "results.json").is_file():
        return result
    try:
        report = json.loads((output / "results.json").read_text())
        result.update(original_verdict=report.get("verdict"), result_sha256=abba.sha256(output / "results.json"))
        if (report.get("run_kind") != "comparison" or report.get("measurement_valid") is not True or
                report.get("quiet_box", {}).get("complete") is not True or
                report["quiet_box"].get("interference", "missing") is not None):
            raise ValueError("missing complete raw measurement/quiet postflight evidence")
        if report.get("instrument_fingerprint") != manifest["instrument_fingerprint"]:
            raise ValueError("measurement instrument changed after plan creation")
        if report.get("window_seconds") != manifest["window_seconds"]:
            raise ValueError("central measurement window changed")
        for key, cpus in manifest["geometry"].items():
            if report.get("environment", {}).get(key) != cpus:
                raise ValueError(f"measurement {key} differs from the planned geometry")
        for arm in ("reference", "candidate"):
            if report.get(arm, {}).get("sha256") != manifest[arm]["sha256"]:
                raise ValueError(f"{arm} identity differs from the planned binary")
        if report.get("cell_source", {}).get("sha256") != probe["fixture_sha256"]:
            raise ValueError("diagnostic fixture changed")
        cell = abba.Cell(**probe["diagnostic_cell"])
        rows = report.get("cells", [])
        if len(rows) != 1 or rows[0].get("cell") != asdict(cell):
            raise ValueError("probe did not reach exactly its planned cell")
        row = rows[0]
        blocks = row.get("rounds", [])
        if (len(blocks) != 1 or blocks[0].get("instances") != cell.instances or
                len(blocks[0].get("runs", [])) != 4):
            raise ValueError("probe did not complete exactly one four-measurement block")
        block = blocks[0]
        if tuple(run.get("arm") for run in block["runs"]) != abba.ORDER:
            raise ValueError("probe lost the ABBA order")
        scores = []
        for seq, run in enumerate(block["runs"], 1):
            if (run.get("complete") is not True or run.get("error") or
                    run.get("load_layout") != probe["load_layout"]):
                raise ValueError("incomplete measurement or changed generator layout")
            folder = output / cell.id / f"n{cell.instances}-{seq}-{run['arm']}"
            diagnostic = productive_saturation(parse_snapshot((folder / "lb-before.txt").read_bytes()),
                parse_snapshot((folder / "lb-after.txt").read_bytes()), floor_pct=abba.BUSY_FLOOR)
            if diagnostic != run.get("diagnostic_saturation"):
                # JSON stringifies integer thread IDs; canonicalize both sides.
                if json.loads(json.dumps(diagnostic)) != run.get("diagnostic_saturation"):
                    raise ValueError("saved saturation diagnostics differ from raw LBSIGNALS")
            accounting = run.get("whole_run_accounting", {}).get("commands", {}).get(cell.op, {})
            if (accounting.get("server_calls", 0) <= 0 or
                    accounting.get("completed_hdr_count") != accounting.get("server_calls") or
                    run.get("commands", 0) <= 0):
                raise ValueError("active probe lacks completed workload command accounting")
            if not any(role["ops"] > 0 for role in diagnostic["roles"].values()):
                raise ValueError("active probe recorded no workload progress")
            scores.append(dict(arm=run["arm"], rate=run["rate"], legacy_busy_pct=run["busy_pct"],
                               score_pct=diagnostic["score_pct"], proposed_floor_met=diagnostic["proposed_floor_met"],
                               roles=diagnostic["roles"], generator_cpu=run.get("generator_cpu")))
        evidence = abba.load_block_evidence(cell, block)
        # Only repeatability failures allow independent families to continue. A
        # bad layout/count/counter is infrastructure failure, never mere noise.
        other_failures = [reason for reason in evidence["validation_reasons"] if " spread exceeds " not in reason]
        if other_failures:
            raise ValueError("; ".join(other_failures))
        result.update(status="STABLE-DIAGNOSTIC" if evidence["valid"] else "INVALID-UNSTABLE",
                      continuation_permitted=True, evidence=evidence, measurements=scores,
                      original_assessment=row.get("assessment"))
        if probe["phase"] == "single":
            result["active_negative_witness"] = all(not score["proposed_floor_met"] for score in scores)
            if not result["active_negative_witness"]:
                result["status"] = "CONTROL-FAIL"
        elif probe["phase"] == "reduced":
            result["paced_followup_required_arms"] = [arm for arm in ("A", "B")
                if any(score["proposed_floor_met"] for score in scores if score["arm"] == arm)]
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        result.update(status="INVALID-INFRASTRUCTURE", reason=str(error))
    return result


def review(args):
    manifest = json.loads(args.plan.read_text())
    if (manifest.get("schema") != 2 or manifest.get("blocks") != 26 or manifest.get("measurements") != 104 or
            len(manifest.get("probes", [])) != 26 or
            len({p["diagnostic_cell"]["id"] for p in manifest["probes"]}) != 26):
        raise ValueError("expected the complete 26-block control plan")
    results = [review_probe(probe, manifest) for probe in manifest["probes"]]
    report = dict(validation="UNVALIDATED", comparison_trusted=False, normal_gate_eligible=False,
                  calibration_eligible=False, plan_path=str(args.plan.resolve()), plan_sha256=abba.sha256(args.plan),
                  expected_blocks=26, reached_blocks=sum(row["status"] != "UNREACHED" for row in results),
                  complete_raw_blocks=sum(row["continuation_permitted"] for row in results), results=results)
    by_id = {row["id"]: row for row in results}
    report["family_evidence"] = []
    for ident, reason in FAMILIES:
        family = dict(source_cell=ident, reason=reason, calibration_eligible=False,
                      generator_headroom="UNPROVEN", paired_load_comparison=None)
        stages = {phase: by_id[f"diag-{ident}-n{n}"] for phase, n in RUNGS}
        family["stages"] = {phase: row["status"] for phase, row in stages.items()}
        low = stages["reduced"]
        pace = low.get("paced_followup_required_arms", [])
        single = by_id.get(f"diag-{ident}-single", {})
        if pace and single.get("status") == "STABLE-DIAGNOSTIC" and single.get("active_negative_witness"):
            family["underload_negative_source"] = single["id"]
            pace = []  # Active GET witness does not transfer to SET or multi-key.
        family["paced_followup_required_arms"] = pace
        lower, upper = stages["loaded"], stages["confirmation"]
        if lower["status"] == upper["status"] == "STABLE-DIAGNOSTIC":
            a, b = lower["evidence"], upper["evidence"]
            comparison = dict(instances=[8, 12], workers=[a["worker_threads"], b["worker_threads"]], arms={})
            for arm in ("reference", "candidate"):
                gain = 100 * (b["rate"][f"{arm}_mean"] / a["rate"][f"{arm}_mean"] - 1)
                noise = max(a["rate"][f"{arm}_spread_pct"], b["rate"][f"{arm}_spread_pct"])
                comparison["arms"][arm] = dict(gain_pct=gain, repeatability_pct=noise,
                    descriptive_shape="gaining" if gain > noise else "congestion" if gain < -noise else "plateau")
            family["paired_load_comparison"] = comparison
        report["family_evidence"].append(family)
    report["status"] = ("INVALID" if any(row["status"].startswith("INVALID") or row["status"] == "CONTROL-FAIL" for row in results)
                        else "INCOMPLETE" if any(row["status"] == "UNREACHED" for row in results) else "COMPLETE-DIAGNOSTIC")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "review.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 1 if report["status"] == "INVALID" else 3  # never a gate or metric PASS


def control_witness(diagnostic, spin_role=None):
    if diagnostic["proposed_floor_met"]:
        raise RuntimeError("nonproductive control met the proposed saturation floor")
    if spin_role is not None:
        role = diagnostic["roles"].get(spin_role)
        if role is None:
            raise RuntimeError(f"spinner role {spin_role} absent from the actual topology")
        # A DEBUG/INFO observer may advance its one owner. It cannot certify a
        # role, and every other role member must witness zero operation progress.
        members = [row for row in diagnostic["threads"].values() if row["role"] == spin_role]
        if len(members) < 2 or sum(row["ops_delta"] != 0 for row in members) > 1:
            raise RuntimeError("spinner had non-observer operation progress; negative is not armed")
        if role["cpu_pct"] < abba.BUSY_FLOOR:
            raise RuntimeError(f"spinner not armed: {spin_role} CPU {role['cpu_pct']:.3f}% "
                               f"< {abba.BUSY_FLOOR:g}%")
    return "CONTROL-PASS"


def command_deltas(before, after):
    def count(value):
        return int(dict(field.split("=", 1) for field in value.split(","))["calls"])
    changes = {name: count(after.get(name, "calls=0")) - count(before.get(name, "calls=0"))
               for name in before.keys() | after.keys()}
    if any(value < 0 for value in changes.values()):
        raise RuntimeError("command counters reset in the negative-control interval")
    unexpected = {name: value for name, value in changes.items()
                  if value and name not in ("cmdstat_debug", "cmdstat_info")}
    if unexpected:
        raise RuntimeError(f"non-observer commands executed in the idle control: {unexpected}")
    return changes


def idle(args):
    if args.seconds != abba.WINDOW or not 0 <= args.connections <= 512:
        raise ValueError("controls use the unchanged 20-second window and 0..512 idle sockets")
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(validation="UNVALIDATED", comparison_trusted=False, verdict="FAIL",
                  measurement_valid=False, mode=args.mode, spin_role=args.spin_role,
                  idle_connections=args.connections, binary_sha256=abba.sha256(args.candidate_binary),
                  observer_commands=["INFO", "DEBUG LBSIGNALS"], window_seconds=args.seconds)
    install_signals()
    server_cpus = abba.cpus(args.server_cores)
    load_cpus = abba.cpus(args.load_cores) + abba.cpus(args.load_smt)
    if len(server_cpus) != 32:
        raise ValueError("diagnostic geometry requires the same 32 physical server CPUs as loaded probes")
    # Check the whole inherited geometry before narrowing the observer's affinity.
    abba.check_placement(server_cpus, abba.cpus(args.load_cores), (), abba.cpus(args.load_smt))
    pin_driver(args.server_cores, abba.cpu_string(load_cpus))
    fingerprint = harness_fingerprint(abba.ROOT)["sha256"]
    report["harness_sha256"] = fingerprint
    quiet = QuietMonitor(server_cpus, load_cpus, own_root_pid=os.getpid(), window_seconds=args.seconds,
                         background_environment=args.background_environment,
                         sample_artifact=args.output / "background-environment-samples.jsonl")
    try:
        quiet.start()
        knobs = ["--thread-mode", args.mode, "--read-local", "1", "--overlap", "0",
                 "--reorder", "0", "--atomic", "1", "--key-lb", "0", "--client-lb", "0",
                 "--shards", "256" if args.mode == "1s" else "128"]
        if args.mode == "2s":
            knobs += ["--ratio", "16:16", "--flip-auto", "0"]
        with server(args.candidate_binary, args.server_cores, args.port, args.output / "server", knobs) as (conn, child):
            report["pid"] = child.pid
            with ExitStack() as stack:
                for _ in range(args.connections):
                    connection = Conn("127.0.0.1", args.port)
                    stack.callback(connection.close)
                time.sleep(abba.WARMUP)
                quiet.check()
                expected = args.connections + 1
                if int(abba.info(conn, "clients")["connected_clients"]) != expected:
                    raise RuntimeError("idle sockets were not all accepted before capture")
                if conn.must("DBSIZE") != 0:
                    raise RuntimeError("negative control needs a fresh empty store")
                before_commands = abba.info(conn, "commandstats")
                before = abba.lb_snapshot(conn, args.output / "lb-before.txt")
                time.sleep(args.seconds)
                after = abba.lb_snapshot(conn, args.output / "lb-after.txt")
                after_commands = abba.info(conn, "commandstats")
                if int(abba.info(conn, "clients")["connected_clients"]) != expected:
                    raise RuntimeError("idle sockets disappeared during capture")
                if conn.must("DBSIZE") != 0:
                    raise RuntimeError("negative-control store changed during capture")
                expected_roles = {"fused": 32} if args.mode == "1s" else {"io": 16, "ex": 16}
                actual_roles = {role: sum(row["role"] == role for row in before.threads.values())
                                for role in {row["role"] for row in before.threads.values()}}
                if actual_roles != expected_roles:
                    raise RuntimeError(f"unexpected negative-control topology: {actual_roles}")
                diagnostic = productive_saturation(before, after, floor_pct=abba.BUSY_FLOOR)
                report.update(command_deltas=command_deltas(before_commands, after_commands),
                              diagnostic_saturation=diagnostic,
                              legacy_busy_pct=abba.busy_between(before.threads, after.threads)[0])
                report["control_witness"] = control_witness(diagnostic, args.spin_role)
        quiet.close()
        quiet.check()
        if harness_fingerprint(abba.ROOT)["sha256"] != fingerprint:
            raise RuntimeError("diagnostic harness changed while the control ran")
        if abba.sha256(args.candidate_binary) != report["binary_sha256"]:
            raise RuntimeError("control binary changed while the control ran")
        report.update(verdict="CONTROL-PASS", measurement_valid=True)
    except BaseException as error:
        report["reason"] = f"{type(error).__name__}: {error}"
    finally:
        quiet.close()
        report["quiet_box"] = quiet.evidence()
        (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["verdict"] == "CONTROL-PASS" else 1


def self_test():
    from contextlib import contextmanager, redirect_stdout
    import io
    import tempfile
    import unittest
    from unittest import mock

    class Controls(unittest.TestCase):
        def test_background_environment_cli_overrides_gate_environment(self):
            for extra, expected in (([], "/reviewed/env.json"),
                                    (["--background-environment", "/reviewed/explicit.json"], "/reviewed/explicit.json")):
                with self.subTest(extra=extra), \
                     mock.patch.dict(os.environ, {"GATE_ABBA_BACKGROUND_ENVIRONMENT": "/reviewed/env.json"}), \
                     mock.patch.object(sys, "argv", ["abba_saturation_controls.py", "plan",
                         "--candidate-binary", "/fixture/binary", "--output", "/fixture/output", *extra]), \
                     mock.patch(__name__ + ".plan", return_value=0) as planned:
                    self.assertEqual(main(), 0)
                self.assertEqual(planned.call_args.args[0].background_environment, Path(expected))

        def make_plan(self, root):
            binary = root / "candidate"
            binary.write_bytes(b"candidate fixture, never executed")
            binary.chmod(0o700)
            reference = root / "tomokv-reference-1234567"
            reference.write_bytes(b"different reference fixture, never executed")
            reference.chmod(0o700)
            (root / "MANIFEST.md").write_text("| `tomokv-reference-1234567` | 1234567 | pinned |\n")
            args = argparse.Namespace(output=root / "plan", bench_bins=root,
                cells=abba.ROOT / "tests/headline_cells.txt", candidate_binary=binary,
                server_cores="0-31", load_cores="32-127", load_smt="160-255",
                port=8700, memtier=sys.executable, background_environment=None)
            with redirect_stdout(io.StringIO()), mock.patch.object(abba, "git", return_value="1234567" + "0" * 33), \
                 mock.patch.object(abba, "check_placement"):
                self.assertEqual(plan(args), 0)
            return args, json.loads((args.output / "plan.json").read_text())

        def test_plan_preserves_every_source_axis_and_uses_verified_two_arm_driver(self):
            with tempfile.TemporaryDirectory() as temporary:
                args, report = self.make_plan(Path(temporary))
                self.assertEqual((report["blocks"], report["measurements"]), (26, 104))
                self.assertNotEqual(report["candidate"]["sha256"], report["reference"]["sha256"])
                self.assertEqual([p["phase"] for p in report["probes"]],
                                 ["single"] * 2 + ["reduced"] * 8 + ["loaded"] * 8 + ["confirmation"] * 8)
                for probe in report["probes"]:
                    before, after = dict(probe["source_cell"]), dict(probe["diagnostic_cell"])
                    for key in ("id", "instances", "smoke"):
                        before.pop(key); after.pop(key)
                    if probe["phase"] == "single":
                        self.assertEqual(after["conns"], 1)
                        before.pop("conns"); after.pop("conns")
                    self.assertEqual(before, after)
                    argv = probe["argv"]
                    self.assertTrue(argv[1].endswith("tests/abbagate.py"))
                    self.assertEqual(argv[argv.index("--collect-null") + 1], "0")
                    self.assertEqual(argv[argv.index("--reference-binary") + 1], report["reference"]["path"])
                    self.assertIn("--only", argv)
                    self.assertNotIn("--escalate", argv)
                    fixture = Path(argv[argv.index("--cells") + 1])
                    self.assertEqual(asdict(abba.read_cells(fixture)[0]), probe["diagnostic_cell"])
                    self.assertEqual(sum(row["threads"] * row["clients"] for row in probe["load_layout"]),
                                     probe["diagnostic_cell"]["conns"])
                    self.assertEqual(probe["worker_threads"], {"single": 1, "reduced": 16,
                                     "loaded": 128, "confirmation": 192}[probe["phase"]])

        def test_generated_commands_drive_all_104_real_loop_calls_and_retain_instability(self):
            from background_environment import canonical_contract
            import copy
            with tempfile.TemporaryDirectory() as temporary:
                args, manifest = self.make_plan(Path(temporary))
                calls = []
                def measure(runner, cell, arm, seq, instances, knobs):
                    calls.append((cell.id, arm, instances))
                    folder = runner.out / cell.id / f"n{instances}-{seq}-{arm}"
                    folder.mkdir(parents=True)
                    raw = []
                    for stamp, elapsed in ((1_000_000_000, 0), (21_000_000_000, 20_000_000_000)):
                        text = f"lbver 1 stamp_ns {stamp}\n"
                        for tid in range(32):
                            role = "fused" if cell.mode == "1s" else "io" if tid < 16 else "ex"
                            active = tid == 0 if cell.conns == 1 else True
                            clients = 1 if active and role != "ex" else 0
                            text += f"thread {tid} {role} 0 {clients} 1 {1000 if active and elapsed else 0} {elapsed if active else 0} {0 if active else elapsed} {elapsed if active else 0}\n"
                        raw.append(text.encode())
                    for name, content in zip(("lb-before.txt", "lb-after.txt"), raw):
                        (folder / name).write_bytes(content)
                    # First family n1 deliberately unstable, but every later
                    # independently planned family/rung still reaches four calls.
                    rate = 120 if cell.id == "diag-h09-n1" and seq == 3 else 100
                    return dict(arm=arm, rate=rate, instances=instances, complete=True, busy_pct=99,
                        load_layout=abba.load_layout(runner.load_cpus, instances, cell.conns), commands=1000,
                        diagnostic_saturation=productive_saturation(*(parse_snapshot(x) for x in raw), floor_pct=95),
                        whole_run_accounting={"commands": {cell.op: {"server_calls": 1000, "completed_hdr_count": 1000}}})
                quiet = mock.Mock()
                evidence = dict(complete=True, interference=None,
                    background_environment={"contract": canonical_contract(None)})
                quiet.evidence.return_value = quiet.close.return_value = evidence
                with mock.patch.object(abba, "QuietMonitor", return_value=quiet), \
                     mock.patch.object(abba.Runner, "measure", measure), \
                     mock.patch.object(abba, "accepted", return_value=True), \
                     mock.patch.object(abba, "check_placement"), mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.object(abba, "git", return_value="1234567" + "0" * 33), \
                     mock.patch.object(abba, "harness_fingerprint", return_value={"sha256": "fixed"}), \
                     mock.patch.object(abba, "instrument_fingerprint", return_value=manifest["instrument_fingerprint"]), \
                     mock.patch.dict(os.environ, {}, clear=True), redirect_stdout(io.StringIO()):
                    for probe in manifest["probes"]:
                        with mock.patch.object(sys, "argv", probe["argv"][1:]):
                            real_args = abba.parse_args()
                        self.assertIn(abba.main(real_args), (1, 3))
                self.assertEqual(len(calls), 104)
                for probe in manifest["probes"]:
                    selected = [call for call in calls if call[0] == probe["diagnostic_cell"]["id"]]
                    self.assertEqual([call[1] for call in selected], list(abba.ORDER))
                    self.assertEqual({call[2] for call in selected}, {probe["diagnostic_cell"]["instances"]})
                rows = [review_probe(probe, manifest) for probe in manifest["probes"]]
                self.assertTrue(all(row["continuation_permitted"] for row in rows), rows)
                self.assertEqual(sum(row["status"] == "INVALID-UNSTABLE" for row in rows), 1)
                self.assertTrue(all(row["active_negative_witness"] for row in rows[:2]))
                self.assertTrue(all(not row["calibration_eligible"] for row in rows))
                probe = manifest["probes"][-1]
                output = Path(probe["argv"][probe["argv"].index("--output") + 1]) / "results.json"
                retained = json.loads(output.read_text())
                for poison in ("missing-run", "reference", "quiet", "no-work", "raw-score", "instrument", "geometry", "window"):
                    broken = copy.deepcopy(retained)
                    if poison == "missing-run": broken["cells"][0]["rounds"][0]["runs"].pop()
                    elif poison == "reference": broken["reference"]["sha256"] = "different"
                    elif poison == "quiet": broken["quiet_box"]["complete"] = False
                    elif poison == "no-work": broken["cells"][0]["rounds"][0]["runs"][0]["whole_run_accounting"] = {}
                    elif poison == "instrument": broken["instrument_fingerprint"] = {}
                    elif poison == "geometry": broken["environment"]["load_cpus"] = []
                    elif poison == "window": broken["window_seconds"] = 10
                    else: broken["cells"][0]["rounds"][0]["runs"][0]["diagnostic_saturation"]["score_pct"] = 0
                    output.write_text(json.dumps(broken))
                    row = review_probe(probe, manifest)
                    self.assertEqual(row["status"], "INVALID-INFRASTRUCTURE", (poison, row))
                    self.assertFalse(row["continuation_permitted"])
                output.write_text(json.dumps(retained))
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(review(argparse.Namespace(plan=args.output / "plan.json", output=Path(temporary) / "review")), 1)
                overall = json.loads((Path(temporary) / "review/review.json").read_text())
                self.assertEqual((overall["reached_blocks"], overall["complete_raw_blocks"], overall["status"]), (26, 26, "INVALID"))
                self.assertEqual(len(overall["family_evidence"]), 8)
                for family in overall["family_evidence"]:
                    self.assertEqual(family["paired_load_comparison"]["workers"], [128, 192])
                    self.assertEqual(family["paced_followup_required_arms"], [] if family["source_cell"] in ("h09", "h25") else ["A", "B"])
                    self.assertFalse(family["calibration_eligible"])


        def test_unarmed_spinner_and_productive_control_are_not_passes(self):
            diagnostic = dict(proposed_floor_met=False,
                roles={"ex": {"cpu_pct": 99.5}},
                threads={n: dict(role="ex", ops_delta=0) for n in range(16)})
            self.assertEqual(control_witness(diagnostic, "ex"), "CONTROL-PASS")
            diagnostic["roles"]["ex"]["cpu_pct"] = 20
            with self.assertRaisesRegex(RuntimeError, "spinner not armed"):
                control_witness(diagnostic, "ex")
            diagnostic["roles"]["ex"]["cpu_pct"] = 99.5
            diagnostic["threads"][0]["ops_delta"] = 5
            diagnostic["threads"][1]["ops_delta"] = 5
            with self.assertRaisesRegex(RuntimeError, "non-observer operation progress"):
                control_witness(diagnostic, "ex")
            diagnostic["proposed_floor_met"] = True
            with self.assertRaisesRegex(RuntimeError, "met the proposed"):
                control_witness(diagnostic)

        def test_actual_idle_dispatch_requires_raw_captures_and_quiet_completion(self):
            for contaminated, changed in ((False, False), (True, False), (False, True)):
                with self.subTest(contaminated=contaminated, changed=changed), tempfile.TemporaryDirectory() as temporary:
                    output = Path(temporary) / "control"
                    binary = Path(temporary) / "binary"
                    binary.write_bytes(b"fixture; never executed")
                    args = argparse.Namespace(output=output, candidate_binary=binary, seconds=20,
                        connections=0, mode="2s", spin_role=None, server_cores="0-31",
                        load_cores="32-127", load_smt="160-255", port=8700,
                        background_environment=Path("/reviewed/environment.json"))
                    raw = []
                    for stamp, time_ns in ((1_000_000_000, 0), (21_000_000_000, 20_000_000_000)):
                        text = f"lbver 1 stamp_ns {stamp}\n"
                        for tid in range(32):
                            role = "io" if tid < 16 else "ex"
                            text += f"thread {tid} {role} 0 0 1 0 0 {time_ns} 0\n"
                        raw.append(text.encode())
                    class Observer:
                        def must(self, *command):
                            if command == ("DBSIZE",):
                                return 0
                            if command != ("DEBUG", "LBSIGNALS"):
                                raise AssertionError(command)
                            return raw.pop(0)
                    class Quiet:
                        closed = False
                        def __init__(self, *a, **kw):
                            self_contract = kw["background_environment"]
                            if self_contract != args.background_environment:
                                raise AssertionError("idle control lost reviewed environment")
                            if kw["sample_artifact"] != output / "background-environment-samples.jsonl":
                                raise AssertionError("idle control lost its raw sample artifact")
                        def start(self): pass
                        def close(self): self.closed = True
                        def check(self):
                            if contaminated and self.closed:
                                raise RuntimeError("latched foreign work")
                        def evidence(self): return dict(complete=self.closed and not contaminated)
                    @contextmanager
                    def owned_server(*a, **kw):
                        yield Observer(), argparse.Namespace(pid=123)
                    def info(_conn, section):
                        return {"connected_clients": "1"} if section == "clients" else {"cmdstat_info": "calls=5"}
                    with mock.patch.object(abba, "check_placement"), mock.patch(__name__ + ".pin_driver"), \
                         mock.patch(__name__ + ".install_signals"), mock.patch(__name__ + ".server", owned_server), \
                         mock.patch(__name__ + ".QuietMonitor", Quiet), mock.patch.object(abba, "info", info), \
                         mock.patch.object(time, "sleep"), mock.patch(__name__ + ".harness_fingerprint",
                             side_effect=[{"sha256": "stable"}, {"sha256": "changed" if changed else "stable"}]), \
                         redirect_stdout(io.StringIO()):
                        self.assertEqual(idle(args), int(contaminated or changed))
                    self.assertEqual(raw, [])
                    self.assertTrue((output / "lb-before.txt").is_file())
                    self.assertTrue((output / "lb-after.txt").is_file())
                    report = json.loads((output / "result.json").read_text())
                    self.assertFalse(report["comparison_trusted"])
                    self.assertEqual(report["measurement_valid"], not (contaminated or changed))

        def test_nonobserver_commands_and_counter_resets_fail(self):
            self.assertEqual(command_deltas({}, {"cmdstat_info": "calls=2"}), {"cmdstat_info": 2})
            with self.assertRaisesRegex(RuntimeError, "non-observer commands"):
                command_deltas({}, {"cmdstat_get": "calls=1"})
            with self.assertRaisesRegex(RuntimeError, "reset"):
                command_deltas({"cmdstat_info": "calls=2"}, {"cmdstat_info": "calls=1"})

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls))
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("plan", "review", "idle", "self-test"))
    parser.add_argument("--candidate-binary", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cells", type=Path, default=abba.ROOT / "tests/headline_cells.txt")
    parser.add_argument("--bench-bins", type=Path, default=Path("/home/user/Projects/bench-bins"))
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--server-cores", default="0-31")
    parser.add_argument("--load-cores", default="32-127")
    parser.add_argument("--load-smt", default="160-255")
    parser.add_argument("--port", type=int, default=8700)
    parser.add_argument("--memtier", default="memtier_benchmark")
    parser.add_argument("--background-environment", type=Path,
                        default=os.getenv("GATE_ABBA_BACKGROUND_ENVIRONMENT") or None)
    parser.add_argument("--mode", choices=("1s", "2s"), default="2s")
    parser.add_argument("--connections", type=int, default=0)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--spin-role", choices=("io", "ex", "fused"))
    args = parser.parse_args()
    if args.action == "self-test":
        return self_test()
    if args.action == "review":
        if args.plan is None or args.output is None:
            parser.error("review requires --plan and --output")
        return review(args)
    if args.candidate_binary is None or args.output is None:
        parser.error("--candidate-binary and --output are required")
    return plan(args) if args.action == "plan" else idle(args)


if __name__ == "__main__":
    raise SystemExit(main())
