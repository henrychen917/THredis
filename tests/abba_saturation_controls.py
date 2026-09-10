#!/usr/bin/env python3
"""Prepare loaded probes or capture idle controls; productive-role-v1 stays UNVALIDATED.

plan writes commands only. They use abbagate's real producer and measurement loop,
20-second windows and byte-identical arms. Diagnostic cell copies do not establish
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
from abba_saturation import productive_saturation
from gate_receipt import harness_fingerprint
from gate_quiet import QuietMonitor


# n1 is a reduced-load probe, not a promised underload. If it still reaches the
# plateau, an explicit paced follow-up is required; never rename it a negative.
# New multi-key n2/n8 are diagnostic load choices, NOT measured shipping pins.
PROBES = (("h25", (1, 2, 8)), ("h31", (1, 2, 8)), ("h09", (4,)),
          ("h32", (2,)), ("m68", (2, 8)), ("m71", (2, 8)))

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
    for ident, rungs in PROBES:
        cell = by_id[ident]
        if cell.depth <= 1:
            raise ValueError(f"diagnostic source {ident} lost its throughput workload")
        for rung in rungs:
            result.append((cell, replace(cell, id=f"diag-{ident}-n{rung}",
                                          instances=rung, smoke=False)))
    return result


def plan(args):
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.cells.resolve()
    rows = planned_cells(source)
    manifest = dict(validation="UNVALIDATED", comparison_trusted=False,
                    source_inventory=str(source), source_sha256=abba.sha256(source),
                    windows_seconds=abba.WINDOW, blocks=len(rows), measurements=4 * len(rows),
                    notes=["No commands were launched or CPUs reserved.",
                           "Each command preserves old FAIL and raw diagnostic evidence.",
                           "n1 must demonstrate reduced rate/demand to count as underloaded.",
                           "n8 must stop gaining within observed repeatability to prove a plateau.",
                           "h09/h32 are productive-path anchors; they alone do not prove a new plateau.",
                           "Multi-key n2/n8 are exploratory levels, not accepted cell pins.",
                           "Run argv sequentially. Retain metric FAIL and continue only after all four measurements are complete and measurement_valid is true.",
                           "Stop the campaign on quiet/preflight/measurement failure; never select a clean-looking subset of an invalid block."],
                    probes=[])
    spinner_source = abba.ROOT / "src/core/ex_loop.h"
    if spinner_source.read_text().count(SPINNER_FIND) != 1:
        raise ValueError("spinner plan is stale: empty EX spin anchor must match exactly once")
    manifest["throwaway_spinner"] = dict(source="src/core/ex_loop.h",
        source_sha256=abba.sha256(spinner_source), find=SPINNER_FIND, replace=SPINNER_REPLACE,
        permitted_control="idle --mode 2s --spin-role ex --connections 0",
        requirements="Separate detached worktree; build before quiet preflight; no source commit; remove afterwards")
    for original, cell in rows:
        fixture = args.output / (cell.id + ".cells")
        fixture.write_text("# Diagnostic copy; never a shipping pin or full inventory.\n" +
            " | ".join((cell.id, cell.mode, f"rl={cell.read_local}", f"ov={cell.overlap}",
                        f"ro={cell.reorder}", cell.op, f"p{cell.depth}", str(cell.conns),
                        "unmeasured", "unmeasured", str(cell.instances), f"atomic={cell.atomic}",
                        "score=rate", f"mix={cell.mix}", "smoke=0")) + "\n")
        argv = [sys.executable, str(abba.ROOT / "tests/abbagate.py"),
                "--candidate-binary", str(args.candidate_binary.resolve()),
                "--cells", str(fixture.resolve()), "--only", cell.id,
                "--subset", "full", "--collect-null", "1", "--build-reference", "0",
                "--server-cores", args.server_cores, "--server-smt", "",
                "--load-cores", args.load_cores, "--load-smt", args.load_smt,
                "--port", str(args.port), "--memtier", args.memtier,
                "--output", str((args.output / cell.id).resolve())]
        if args.background_environment is not None:
            argv += ["--background-environment", str(args.background_environment.resolve())]
        manifest["probes"].append(dict(source_cell=asdict(original), diagnostic_cell=asdict(cell),
                                        argv=argv, shell=shlex.join(argv)))
        print(shlex.join(argv))
    (args.output / "plan.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"PLAN ONLY: {len(rows)} blocks / {4 * len(rows)} measurements; no launches", file=sys.stderr)
    return 0


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

        def test_plan_preserves_every_source_axis_and_uses_the_real_driver(self):
            with tempfile.TemporaryDirectory() as temporary:
                args = argparse.Namespace(output=Path(temporary) / "plan",
                    cells=abba.ROOT / "tests/headline_cells.txt", candidate_binary=Path("/fixture/tomokv"),
                    server_cores="0-31", load_cores="32-127", load_smt="160-255",
                    port=8700, memtier="memtier_benchmark", background_environment=Path("/reviewed/environment.json"))
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(plan(args), 0)
                report = json.loads((args.output / "plan.json").read_text())
                self.assertEqual((report["blocks"], report["measurements"]), (12, 48))
                for probe in report["probes"]:
                    before, after = dict(probe["source_cell"]), dict(probe["diagnostic_cell"])
                    for key in ("id", "instances", "smoke"):
                        before.pop(key); after.pop(key)
                    self.assertEqual(before, after)
                    argv = probe["argv"]
                    self.assertTrue(argv[1].endswith("tests/abbagate.py"))
                    self.assertEqual(argv[argv.index("--collect-null") + 1], "1")
                    self.assertIn("--only", argv)
                    self.assertNotIn("--escalate", argv)
                    self.assertEqual(argv[argv.index("--background-environment") + 1],
                                     str(args.background_environment))
                    fixture = Path(argv[argv.index("--cells") + 1])
                    self.assertEqual(asdict(abba.read_cells(fixture)[0]), probe["diagnostic_cell"])

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
    parser.add_argument("action", choices=("plan", "idle", "self-test"))
    parser.add_argument("--candidate-binary", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cells", type=Path, default=abba.ROOT / "tests/headline_cells.txt")
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
    if args.candidate_binary is None or args.output is None:
        parser.error("--candidate-binary and --output are required")
    return plan(args) if args.action == "plan" else idle(args)


if __name__ == "__main__":
    raise SystemExit(main())
