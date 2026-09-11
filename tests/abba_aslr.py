#!/usr/bin/env python3
"""Diagnostic-only owned-server ASLR contrast; never a gate or standing null.

Eight real boots are preregistered ON/OFF/OFF/ON, then OFF/ON/ON/OFF. Both
arms restore the same server-written full snapshot and execute h09 at n8 with
the ordinary 20s counter window and 28s floating-generator lifetime. ASLR-off
changes only the owned server exec chain, using setarch x86_64 -R. It does not
fix jemalloc arena assignment, object addresses, physical page placement, or
kernel connection assignment. A rate association is not proof of causation.

The ordinary operational QuietMonitor remains mandatory. The internal diagnostic
entry point prevents even favorable observations from becoming a gate/null PASS.
No sysctl, MALLOC_CONF, server bytes, threshold, or production flag changes.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, replace
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

import abbagate as abba
from abba_experiments import (BASE_RUNNER, ExperimentRunner, SnapshotFixtures,
                              arguments_for_block, default_geometry, snapshot_metadata)
from gate_quiet import QuietMonitor


BLOCKS = (("on_off", {"A": "ON", "B": "OFF"}),
          ("off_on", {"A": "OFF", "B": "ON"}))
EXPECTED = [postures[arm] for _, postures in BLOCKS for arm in abba.ORDER]
ADDR_NO_RANDOMIZE = 0x40000


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def identity(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return {"pid": pid, "start_ticks": int(fields[19]), "parent_pid": int(fields[1])}


def require_owned(process, expected):
    if process.pid != expected["pid"] or expected["parent_pid"] != os.getpid():
        raise RuntimeError("mapping capture requires this driver's exact owned child")
    if process.poll() is not None or identity(process.pid) != expected:
        raise RuntimeError("owned server exited, changed parent, or PID was reused")


def server_command(argv, posture, setarch):
    if posture not in ("ON", "OFF"):
        raise ValueError("ASLR posture must be ON or OFF")
    # taskset and setarch both exec. Popen's PID must remain the serving PID; the
    # real producer also verifies INFO process_id against that exact Popen PID.
    return ([setarch, "x86_64", "-R"] if posture == "OFF" else []) + list(map(str, argv))


def mapped_files(text, pid, cache):
    result = []
    for line in text.splitlines():
        parts = line.split(None, 5)
        if len(parts) != 6 or "x" not in parts[1] or not parts[5].startswith("/"):
            continue
        region, permissions, offset, device, inode, name = parts
        if name.endswith(" (deleted)"):
            raise RuntimeError(f"mapped executable file was deleted: {name}")
        path = Path(f"/proc/{pid}/root") / name.lstrip("/")
        before = path.stat()
        major, minor = (int(value, 16) for value in device.split(":"))
        if (before.st_ino, os.major(before.st_dev), os.minor(before.st_dev)) != (int(inode), major, minor):
            raise RuntimeError(f"mapped executable identity changed: {name}")
        key = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
        if key not in cache:
            digest = abba.sha256(path)
            after = path.stat()
            if key != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                raise RuntimeError(f"mapped executable changed while hashing: {name}")
            cache[key] = digest
        result.append({"path": name, "sha256": cache[key], "region": region,
                       "permissions": permissions, "file_offset": int(offset, 16),
                       "load_base": int(region.split("-")[0], 16) - int(offset, 16),
                       "device": device, "inode": int(inode), "size": before.st_size})
    if not result:
        raise RuntimeError("no mapped executable files were observed")
    return result


class AddressCapture:
    """Endpoint hook only: no counters, signals, pauses, or periodic profiling.

    Runner's internal endpoint interface avoids adding an ordinary gate hook. Its
    temporary cpu_profile field is renamed by ASLRRunner before publication; the
    durable address-layout.json is separately present on every error path.
    """
    def __init__(self, folder, process, owner, posture, executable, argv):
        self.folder, self.process, self.owner = folder, process, owner
        self.executable, self.cache = executable, {}
        self.record = {"kind": "owned-server-address-layout", "status": "INCOMPLETE",
                       "posture": posture, "owner": owner, "actual_launch_argv": argv,
                       "cpu_profile_enabled": False, "captures": {}}

    def save(self):
        write_json(self.folder / "address-layout.json", self.record)

    def capture(self, name):
        if self.record["status"] == "FAIL":
            raise RuntimeError("address capture failure is latched; no retry")
        row = {"status": "INCOMPLETE", "started_monotonic": time.monotonic(),
               "started_unix": time.time(), "files": {}}
        self.record["captures"][name] = row
        self.save()
        try:
            require_owned(self.process, self.owner)
            root = Path(f"/proc/{self.process.pid}")
            exe = root / "exe"
            row["executable"] = {"path": str(exe.resolve(strict=True)), "sha256": abba.sha256(exe)}
            if row["executable"]["sha256"] != self.executable:
                raise RuntimeError("owned exec chain did not reach the frozen measurement binary")
            # Empty arguments are meaningful (the server uses --save ""). Drop
            # only the terminating NUL, never empty fields within the argv vector.
            raw_argv = (root / "cmdline").read_bytes()
            if not raw_argv.endswith(b"\0"):
                raise RuntimeError("owned executable argv is empty or incomplete")
            row["exec_argv"] = [part.decode(errors="replace") for part in raw_argv[:-1].split(b"\0")]
            row["kernel_randomize_va_space"] = int(Path("/proc/sys/kernel/randomize_va_space").read_text())
            if row["kernel_randomize_va_space"] != 2:
                raise RuntimeError("kernel ASLR policy is not 2; ON would not mean full ASLR")
            personality = (root / "personality").read_text().strip()
            row["personality_hex"] = personality
            if bool(int(personality, 16) & ADDR_NO_RANDOMIZE) != (self.record["posture"] == "OFF"):
                raise RuntimeError("actual server personality does not match requested ASLR posture")
            row["affinity"] = sorted(os.sched_getaffinity(self.process.pid))
            contents = {}
            for filename in ("maps", "smaps_rollup", "numa_maps"):
                contents[filename] = (root / filename).read_text()
                destination = self.folder / f"{name}.{filename}"
                destination.write_text(contents[filename])
                row["files"][filename] = {"path": str(destination), "sha256": abba.sha256(destination)}
            row["mapped_executables"] = mapped_files(contents["maps"], self.process.pid, self.cache)
            require_owned(self.process, self.owner)
            row.update(status="COMPLETE", finished_monotonic=time.monotonic(), finished_unix=time.time())
        except BaseException as error:
            row.update(status="FAIL", error=f"{type(error).__name__}: {error}")
            self.fail(error)
            raise
        finally:
            self.save()

    def begin(self, server, generators):
        if server is not self.process or not generators or self.record["captures"].get("before-load", {}).get("status") != "COMPLETE":
            raise RuntimeError("mapping observer did not capture the measured owned server before load")
        require_owned(self.process, self.owner)

    def finish(self, started, finished, commands):
        if self.record["status"] == "FAIL":
            raise RuntimeError("address capture failure is latched; no retry")
        self.record["counter_window"] = {"started_monotonic": started, "finished_monotonic": finished,
                                         "seconds": finished - started, "commands": commands}
        self.capture("after-window")
        self.record["status"] = "COMPLETE"
        self.save()

    def fail(self, error):
        self.record.update(status="FAIL", error=f"{type(error).__name__}: {error}")
        self.save()

    def close(self):
        self.save()  # No descriptor/child is owned by this passive observer.


class ASLRRunner(ExperimentRunner):
    def __init__(self, *args, postures, setarch, **kwargs):
        super().__init__(*args, **kwargs)
        self.postures, self.setarch, self.address = postures, setarch, None
        self.profile_factory = lambda folder: self.address

    def populate(self, cell, arm, conn, folder):
        result = super().populate(cell, arm, conn, folder)
        if self.address is None:
            raise RuntimeError("measured server launch was not observed")
        self.address.capture("before-load")
        return result

    def measure(self, cell, arm, sequence, instances, knobs):
        if instances != 8 or cell.id != "h09" or abba.WINDOW != 20 or self.load_startup_seconds:
            raise RuntimeError("ASLR diagnostic requires h09 n8, 20s window, and ordinary 28s generators")
        self.address = None
        original = self.children.start
        launches = []
        def start(argv, log, cwd):
            actual = list(map(str, argv))
            # Match the exact measured binary position, not a process-name/argv
            # substring. Memtier children and capability probes keep ordinary ASLR.
            if len(actual) > 3 and actual[:2] == ["taskset", "-c"] and actual[3] == str(self.binaries[arm]):
                if launches:
                    raise RuntimeError("more than one measured server launch in one observation")
                actual = server_command(actual, self.postures[arm], self.setarch)
                process = original(actual, log, cwd)
                launches.append(actual)
                self.address = AddressCapture(Path(cwd), process, identity(process.pid),
                    self.postures[arm], abba.sha256(self.binaries[arm]), actual)
                self.address.save()
                return process
            return original(actual, log, cwd)
        self.children.start = start
        folder = self.out / cell.id / f"n{instances}-{sequence}-{arm}"
        result = None
        try:
            result = super().measure(cell, arm, sequence, instances, knobs)
            if len(launches) != 1 or self.address is None or self.address.record["status"] != "COMPLETE":
                result["complete"] = False
                raise RuntimeError("address capture or measured server exec chain was not reached")
            return result
        finally:
            self.children.start = original
            path = folder / "measurement.json"
            if self.address and result is None and self.address.record["status"] != "FAIL":
                self.address.fail(RuntimeError("measurement did not complete"))
            if path.exists():
                document = result if result is not None else json.loads(path.read_text())
                document.pop("cpu_profile", None)
                document.update(aslr=self.postures[arm], address_layout=self.address.record if self.address else None,
                                server_argv=launches[0] if launches else document.get("server_argv"))
                write_json(path, document)


class DiagnosticQuiet(QuietMonitor):
    def set_phase(self, phase):
        # main's diagnostic callback normally labels phases. Keep the normal
        # refusal policy and periodic sampler; no unsupported legacy CPU budget.
        self.check()


def frozen_snapshot(source, candidate):
    document = json.loads(source.read_text())
    snapshot = document.get("snapshot", {})
    if snapshot.get("status") != "COMPLETE" or snapshot.get("scored") is not False or snapshot.get("seed_metadata_equal") is not True:
        raise RuntimeError("source is not a completed unscored server-generated snapshot fixture")
    digest = abba.sha256(candidate)
    if document.get("binary_fixture", {}).get("sha256") != digest:
        raise RuntimeError("candidate bytes differ from the binary which generated the snapshot")
    rows = snapshot.get("boots", [])
    if [row.get("kind") for row in rows] != ["empty", "full"] or any(row.get("status") != "COMPLETE" or row.get("scored") is not False for row in rows):
        raise RuntimeError("snapshot priming boots are missing, failed, or scored")
    metadata = [row["snapshot"] for row in rows]
    fixture = SnapshotFixtures(*(Path(row["path"]) for row in metadata), *metadata)
    fixture.validate()
    return fixture, {"path": str(source.resolve()), "sha256": abba.sha256(source),
                     "creator_binary_sha256": digest, "snapshot": snapshot}


def run_block(args, fixture, snapshot, output, postures):
    attempts = []
    options = arguments_for_block(args, fixture, output)
    options.collect_null = 1
    original = abba.Runner
    abba.Runner = lambda *a, **kw: ASLRRunner(*a, snapshot=snapshot,
        population_by_arm={"A": "snapshot", "B": "snapshot"}, attempts=attempts,
        postures=postures, setarch=args.setarch, **kw)
    def quiet(*a, **kw):
        return DiagnosticQuiet(*a, **kw)
    try:
        rc = abba.main(options, diagnostic_monitor=quiet, diagnostic_profile=0)
    finally:
        abba.Runner = original
    report = json.loads((output / "results.json").read_text())
    runs = [run for cell in report["cells"] for row in cell["rounds"] for run in row["runs"]]
    valid = (len(attempts) == len(runs) == 4 and all(run.get("complete") is True and
        run.get("address_layout", {}).get("status") == "COMPLETE" for run in runs) and
        report.get("quiet_box", {}).get("complete") is True and
        report["quiet_box"].get("interference") is None and not report.get("reason"))
    return {"postures": postures, "attempts": attempts, "exit_code": rc,
            "complete": valid, "normal_gate_eligible": False, "abba": report}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot-experiment", type=Path)
    parser.add_argument("--candidate-binary", type=Path)
    parser.add_argument("--cells", type=Path, default=abba.ROOT / "tests/headline_cells.txt")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--memtier", default="memtier_benchmark")
    parser.add_argument("--setarch", default="setarch")
    for name in ("server-cores", "server-smt", "load-cores", "load-smt"):
        parser.add_argument("--" + name, default=None)
    parser.add_argument("--ports", default="8700-8700")
    parser.add_argument("--port", type=int)
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args(argv)


def summary(blocks):
    runs = [run for block in blocks for cell in block["abba"]["cells"]
            for row in cell["rounds"] for run in row["runs"]]
    result = {"actual_sequence": [run.get("aslr") for run in runs], "by_posture": {}}
    for posture in ("ON", "OFF"):
        selected = [run for run in runs if run.get("aslr") == posture]
        rates = [run["rate"] for run in selected]
        bases = [{(entry["sha256"], entry["file_offset"]): entry["load_base"] for entry in
                  run["address_layout"]["captures"]["before-load"]["mapped_executables"]} for run in selected]
        result["by_posture"][posture] = {"rates": rates,
            "mean": statistics.mean(rates) if rates else None,
            "range_pct": 100 * (max(rates) - min(rates)) / statistics.mean(rates) if rates else None,
            "personality_hex": [run["address_layout"]["captures"]["before-load"]["personality_hex"] for run in selected],
            "mapped_file_bases_equal": len(bases) == 4 and all(row == bases[0] for row in bases)}
    # This is a contrast, not a same-posture null or saturation-floor proof. Keep
    # raw ordinary ABBA verdicts, but never use them to select the favorable block.
    return result


def main(args):
    if args.self_test:
        return self_test()
    if not args.snapshot_experiment or not args.candidate_binary or not args.output:
        raise ValueError("--snapshot-experiment, --candidate-binary and --output are required")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "run_kind": "aslr-address-contrast", "normal_gate_eligible": False,
              "measurement_valid": False, "status": "INCOMPLETE", "blocks": [],
              "plan": {"postures": EXPECTED, "boots": 8, "instances": 8, "cell": "h09",
                       "rate_window_seconds": 20, "generator_lifetime_seconds": 28,
                       "profile": 0, "population": "identical full snapshot"},
              "limitations": ["Diagnostic contrast, never a gate or standing null.",
                  "ASLR-off does not stabilize object addresses, allocator/thread order, physical pages or connection assignment.",
                  "Mappings are passive endpoint snapshots, not stop-the-world observations.",
                  "Rate counters span the central 20s; generator histograms span the full 28s.",
                  "No retry, favorable-block selection, threshold change or default adoption."]}
    try:
        if abba.WINDOW != 20 or abba.WARMUP + abba.WINDOW + abba.TAIL != 28:
            raise RuntimeError("ordinary measurement timing changed")
        if int(Path("/proc/self/personality").read_text(), 16) & ADDR_NO_RANDOMIZE:
            raise RuntimeError("driver inherited ASLR-off; ON generators/support boots would be contaminated")
        report["kernel_randomize_va_space"] = int(Path("/proc/sys/kernel/randomize_va_space").read_text())
        if report["kernel_randomize_va_space"] != 2:
            raise RuntimeError("kernel ASLR policy must already be 2; diagnostic never changes it")
        args.setarch = shutil.which(args.setarch)
        if not args.setarch or os.uname().machine != "x86_64":
            raise RuntimeError("setarch x86_64 is unavailable")
        report["setarch"] = {"path": args.setarch, "sha256": abba.sha256(Path(args.setarch))}
        geometry = default_geometry() if any(getattr(args, name) is None for name in
            ("server_cores", "server_smt", "load_cores", "load_smt")) else {}
        for name, value in geometry.items():
            if getattr(args, name) is None:
                setattr(args, name, value)
        snapshot, report["snapshot_source"] = frozen_snapshot(args.snapshot_experiment, args.candidate_binary)
        fixture = output / "fixture"
        fixture.mkdir()
        shutil.copy2(args.candidate_binary, fixture / "candidate")
        if abba.sha256(fixture / "candidate") != report["snapshot_source"]["creator_binary_sha256"]:
            raise RuntimeError("candidate changed while freezing")
        shutil.copy2(fixture / "candidate", fixture / "reference")
        cells = [cell for cell in abba.read_cells(args.cells) if cell.id == "h09"]
        if len(cells) != 1 or asdict(replace(cells[0], instances=8)) != {"id": "h09", "mode": "1s", "read_local": 1,
                "overlap": 0, "reorder": 0, "op": "GET", "depth": 32, "conns": 512,
                "instances": 8, "atomic": 1, "score": "rate", "mix": "-", "smoke": False,
                "pin_required": True}:
            raise RuntimeError("h09 no longer matches the preregistered source cell at n8")
        row = next(line for line in args.cells.read_text().splitlines() if line.split("|", 1)[0].strip() == "h09")
        fields = [field.strip() for field in row.split("|")]
        # n8 is the fixed contrast condition, not a new measured/validated gate pin.
        # Preserve the stored source pin and change only the private diagnostic cell.
        fields[10] = "8"
        (fixture / "cell.txt").write_text(" | ".join(fields) + "\n")
        report["source_cell"] = asdict(cells[0])
        report["cell"] = asdict(replace(cells[0], instances=8))
        write_json(output / "aslr.json", report)
        for name, postures in BLOCKS:
            block = run_block(args, fixture, snapshot, output / name, postures)
            report["blocks"].append(block)
            write_json(output / "aslr.json", report)
            if not block["complete"]:
                raise RuntimeError(f"{name}: incomplete/infrastructure-invalid observations; no retry")
        report["summary"] = summary(report["blocks"])
        if report["summary"]["actual_sequence"] != EXPECTED:
            raise RuntimeError("actual eight-boot ASLR sequence differs from preregistration")
        snapshot.validate()
        report["status"] = "COMPLETE_DIAGNOSTIC"
        print("| ASLR | Four rates Mops/s | Range % | Mapped file bases equal |")
        print("|---|---|---:|---|")
        for posture, row in report["summary"]["by_posture"].items():
            print(f"| {posture} | {','.join(f'{v/1e6:.6f}' for v in row['rates'])} | {row['range_pct']:.4f} | {row['mapped_file_bases_equal']} |")
        print("Complete diagnostic only. Raw assessments retained; no gate/null certificate or defaults changed.")
        return 3
    except BaseException as error:
        report.update(status="FAIL", error=f"{type(error).__name__}: {error}")
        print(report["error"], file=sys.stderr)
        return 1
    finally:
        write_json(output / "aslr.json", report)


def self_test():
    from _abba_test_fixtures import saturation_record
    import contextlib
    import io
    import unittest
    from unittest import mock

    class Controls(unittest.TestCase):
        def test_snapshot_creator_and_exact_source_bytes_are_required(self):
            with tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                binary = folder / "binary"
                binary.write_bytes(b"source binary")
                # A small header-only source fixture: real scored boots must also
                # validate snapshot frames/footer and the exact restored key set.
                header = bytes.fromhex("544f4d4f534e5000010000005000000000010000000000000100000000000000bd428b8ca0010000c8a988f02327085cdeb208f28a39fe23f0d699a4bdb9d8840330f005f03c57dc0000000000000000")
                images = []
                for name in ("empty", "full"):
                    path = folder / name
                    path.write_bytes(header)
                    images.append(snapshot_metadata(path))
                source = folder / "source.json"
                document = {"binary_fixture": {"sha256": abba.sha256(binary)}, "snapshot": {
                    "status": "COMPLETE", "scored": False, "seed_metadata_equal": True,
                    "boots": [{"kind": kind, "status": "COMPLETE", "scored": False, "snapshot": image}
                              for kind, image in zip(("empty", "full"), images)]}}
                write_json(source, document)
                frozen, provenance = frozen_snapshot(source, binary)
                self.assertEqual(provenance["sha256"], abba.sha256(source))
                self.assertEqual(frozen.full_metadata, images[1])
                binary.write_bytes(b"other binary")
                with self.assertRaisesRegex(RuntimeError, "candidate bytes differ"):
                    frozen_snapshot(source, binary)
                binary.write_bytes(b"source binary")
                (folder / "full").write_bytes(header + b"changed snapshot")
                with self.assertRaisesRegex(RuntimeError, "fixture changed"):
                    frozen_snapshot(source, binary)
                document["snapshot"]["boots"][1]["status"] = "FAIL"
                write_json(source, document)
                with self.assertRaisesRegex(RuntimeError, "priming boots"):
                    frozen_snapshot(source, binary)

        def test_prefix_only_server_and_exact_postures(self):
            command = ["taskset", "-c", "0-31", "/owned/binary", "--port", "8700"]
            self.assertEqual(server_command(command, "ON", "/usr/bin/setarch"), command)
            self.assertEqual(server_command(command, "OFF", "/usr/bin/setarch"),
                             ["/usr/bin/setarch", "x86_64", "-R", *command])
            with self.assertRaises(ValueError):
                server_command(command, "AUTO", "setarch")

        def test_real_exec_chain_personality_maps_and_pid_ownership(self):
            # Harmless pipe-waiting Python helper, no server, ports or traffic.
            # This verifies setarch->taskset->exec keeps the owned PID and permits
            # every /proc read used by the real capture. Failure is never skipped.
            if int(Path("/proc/self/personality").read_text(), 16) & ADDR_NO_RANDOMIZE:
                self.fail("self-test must start with ordinary ASLR")
            with tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                executable = Path(sys.executable).resolve()
                for posture in ("ON", "OFF", "OFF", "ON"):
                    destination = folder / str(len(list(folder.iterdir())))
                    destination.mkdir()
                    argv = server_command(["taskset", "-c", str(min(os.sched_getaffinity(0))),
                        str(executable), "-u", "-c", "import sys; print('READY',flush=True); sys.stdin.read()", ""],
                        posture, shutil.which("setarch"))
                    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, start_new_session=True)
                    try:
                        import select
                        self.assertTrue(select.select([p.stdout], [], [], 5)[0], "owned helper failed to exec")
                        self.assertEqual(p.stdout.readline(), b"READY\n")
                        owner = identity(p.pid)
                        capture = AddressCapture(destination, p, owner, posture, abba.sha256(executable), argv)
                        capture.capture("before-load")
                        capture.begin(p, [object()])
                        capture.finish(10, 30, 1)
                        self.assertEqual(capture.record["status"], "COMPLETE")
                        self.assertEqual(capture.record["captures"]["after-window"]["exec_argv"][-1], "")
                        self.assertEqual(capture.record["captures"]["after-window"]["personality_hex"],
                                         "00040000" if posture == "OFF" else "00000000")
                        with self.assertRaisesRegex(RuntimeError, "PID was reused"):
                            require_owned(p, {**owner, "start_ticks": owner["start_ticks"] + 1})
                        with self.assertRaisesRegex(RuntimeError, "exact owned child"):
                            require_owned(p, {**owner, "parent_pid": 1})
                        capture.record["posture"] = "OFF" if posture == "ON" else "ON"
                        with self.assertRaisesRegex(RuntimeError, "personality"):
                            capture.capture("wrong-posture")
                        self.assertEqual(capture.record["status"], "FAIL")
                        with self.assertRaisesRegex(RuntimeError, "latched"):
                            capture.finish(30, 50, 1)
                        capture = AddressCapture(destination, p, owner, posture, "0" * 64, argv)
                        with self.assertRaisesRegex(RuntimeError, "frozen measurement binary"):
                            capture.capture("wrong-executable")
                    finally:
                        try:
                            p.communicate(timeout=5)
                        except subprocess.TimeoutExpired:
                            p.kill()
                            p.communicate(timeout=5)
                    with self.assertRaises(RuntimeError):
                        require_owned(p, owner)

        def test_maps_failure_is_latched_and_raw_evidence_retained(self):
            with tempfile.TemporaryDirectory() as tmp:
                capture = AddressCapture(Path(tmp), mock.Mock(pid=os.getpid()),
                    {"pid": os.getpid(), "parent_pid": os.getpid(), "start_ticks": 1}, "ON", "0" * 64, [])
                with mock.patch(__name__ + ".require_owned", side_effect=FileNotFoundError("exited")):
                    with self.assertRaises(FileNotFoundError):
                        capture.capture("before-load")
                row = json.loads((Path(tmp) / "address-layout.json").read_text())
                self.assertEqual(row["status"], "FAIL")
                self.assertIn("exited", row["captures"]["before-load"]["error"])
                with self.assertRaisesRegex(RuntimeError, "no mapped executable"):
                    mapped_files("", os.getpid(), {})
                with self.assertRaisesRegex(RuntimeError, "was deleted"):
                    mapped_files("1000-2000 r-xp 0 00:01 1 /gone (deleted)", os.getpid(), {})

        def test_real_abba_producer_takes_exactly_eight_and_retains_unstable_block(self):
            for broken in (False, True):
                with self.subTest(broken=broken), tempfile.TemporaryDirectory() as tmp:
                    folder = Path(tmp)
                    fixture = folder / "fixture"
                    fixture.mkdir()
                    (fixture / "candidate").write_bytes(b"never executed fake binary")
                    (fixture / "candidate").chmod(0o700)
                    (fixture / "cell.txt").write_text("h09 | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | - | - | 4 | atomic=1 | score=rate | mix=- | smoke=0\n")
                    args = parse_args(["--server-cores", "0-31", "--server-smt", "", "--load-cores", "32-127", "--load-smt", "160-255", "--memtier", sys.executable,
                        "--candidate-binary", str(fixture / "candidate"), "--snapshot-experiment", str(folder / "source.json"),
                        "--cells", str(fixture / "cell.txt"), "--output", str(folder / "output")])
                    calls, commands = [], []
                    metadata = {"sha256": "b", "format": 1, "shards": 256, "hash_kind": 0,
                                "hash_seed": 7, "sip_k0": 8, "sip_k1": 9}
                    images = mock.Mock(empty_metadata=metadata, full_metadata=metadata)
                    class Capture:
                        def __init__(self, folder, process, owner, posture, executable, argv):
                            self.record = {"status": "INCOMPLETE", "posture": posture, "actual_launch_argv": argv, "captures": {}}
                        def save(self): pass
                        def capture(self, phase):
                            self.record[phase] = True
                            self.record["captures"][phase] = {"personality_hex": "00040000" if self.record["posture"] == "OFF" else "00000000", "mapped_executables": [
                                {"sha256": "a", "file_offset": 0, "load_base": 1000}]}
                        def begin(self, server, generators):
                            self_test_case.assertTrue(self.record["before-load"])
                        def finish(self, *args): self.record.update(status="COMPLETE")
                        def fail(self, error): self.record.update(status="FAIL")
                    self_test_case = self
                    def start(argv, log, cwd):
                        commands.append(argv)
                        return mock.Mock(pid=123)
                    def measure(runner, cell, arm, sequence, instances, knobs):
                        calls.append((runner.postures[arm], arm, sequence, instances, abba.WINDOW))
                        destination = runner.out / cell.id / f"n{instances}-{sequence}-{arm}"
                        destination.mkdir(parents=True)
                        server = runner.children.start(["taskset", "-c", "0-31", str(runner.binaries[arm]), "--port", "8700"], destination / "server.log", destination)
                        conn = mock.Mock(must=lambda *cmd: abba.KEYS if cmd == ("DBSIZE",) else b"x" * 64)
                        runner.populate(cell, arm, conn, destination)
                        generators = [runner.children.start([*runner.memtier(layout), "--test-time", "28"], destination / f"load-{i}.log", destination)
                                      for i, layout in enumerate(abba.load_layout(runner.load_cpus, instances, cell.conns))]
                        capture = runner.profile_factory(destination)
                        capture.begin(server, generators)
                        capture.finish(1, 21, 2000)
                        result = {"arm": arm, "rate": 90 if broken and len(calls) == 4 else 100,
                            "saturation": saturation_record(cell.mode, window_seconds=20.001),
                            "midpoint_monotonic": 11.0005,
                            "latency_ms": 1, "busy_pct": 99.9, "instances": instances,
                            "load_layout": abba.load_layout(runner.load_cpus, instances, cell.conns),
                            "complete": True, "window_seconds": 20.001, "cpu_profile": capture.record}
                        write_json(destination / "measurement.json", result)
                        return result
                    quiet = mock.Mock()
                    quiet.evidence.return_value = quiet.close.return_value = {"complete": True, "interference": None}
                    with mock.patch.object(BASE_RUNNER, "measure", measure), \
                         mock.patch.object(ASLRRunner, "population_environment", return_value={}), \
                         mock.patch.object(abba.Children, "start", side_effect=start), \
                         mock.patch(__name__ + ".frozen_snapshot", return_value=(images, {
                             "creator_binary_sha256": abba.sha256(fixture / "candidate")})), \
                         mock.patch(__name__ + ".AddressCapture", Capture), \
                         mock.patch(__name__ + ".identity", return_value={"pid": 123, "start_ticks": 1, "parent_pid": os.getpid()}), \
                         mock.patch.object(abba, "check_placement"), \
                         mock.patch.object(abba, "accepted", return_value=True), \
                         mock.patch(__name__ + ".DiagnosticQuiet", return_value=quiet) as factory, \
                         mock.patch.object(os, "sched_setaffinity"), \
                         mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                         contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(main(args), 3)
                    report = json.loads((folder / "output/aslr.json").read_text())
                    blocks = report["blocks"]
                    self.assertEqual(report["status"], "COMPLETE_DIAGNOSTIC")
                    self.assertFalse(report["normal_gate_eligible"])
                    self.assertEqual(calls, [(postures[arm], arm, index, 8, 20) for _, postures in BLOCKS for index, arm in enumerate(abba.ORDER, 1)])
                    self.assertEqual(len(calls), 8)
                    self.assertEqual(len(commands), 8 * 9)
                    for index, posture in enumerate(EXPECTED):
                        self.assertEqual(commands[index * 9][0] == args.setarch, posture == "OFF")
                        self.assertTrue(all(cmd[0] == "taskset" and cmd[-2:] == ["--test-time", "28"] for cmd in commands[index * 9 + 1:(index + 1) * 9]))
                    self.assertEqual(factory.call_count, 2)
                    self.assertTrue(all(block["complete"] for block in blocks))
                    self.assertEqual([len(block["attempts"]) for block in blocks], [4, 4])
                    self.assertTrue(all(call.kwargs["ports"] == (abba.select_port(args.ports, args.port)[0],)
                                        for call in factory.call_args_list))
                    self.assertTrue(all(block["abba"]["normal_gate_eligible"] is False for block in blocks))
                    if broken:
                        self.assertEqual(blocks[0]["abba"]["statistical_verdict"], "FAIL")
                        self.assertEqual(len(blocks[1]["abba"]["cells"][0]["rounds"][0]["runs"]), 4)

    return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls)).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main(parse_args()))
