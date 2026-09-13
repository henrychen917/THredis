#!/usr/bin/env python3
"""Build controls, or measure only in the maintainer-scheduled box slot.

Uses Runner.measure directly, not the gate, and preserves all argv/raw artifacts and failures.
The allowed geometry is 0-31 server, 32-111 load, no SMT. It is deliberately labelled as a
different CPU geometry from the archived 0-31 / 32-127+160-255 campaign. Never earns a gate PASS.
Build (starts no server):
  build --output build/readlocal-obs/arms
Run examples (each four consecutive boots per cell; no automatic reruns):
  run --output build/readlocal-obs/reproduction
  --order PRE,PRE,PRE,PRE --cells h11,h15,h27,h31
  --order POST,POST,POST,POST --cells h11,h15,h27,h31
  --order PRE,POST,POST,PRE --profile 1 --connections 128 --instances 1
  --order POST,POST,POST,POST --cells h11 --window 60
  --order POST,POST,POST,POST --cells h11 --warmup 30
"""
import argparse
from dataclasses import asdict, replace
import io
import json
import os
from pathlib import Path
import shutil
import signal
import statistics
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
ARMS = ROOT / "build/readlocal-obs/arms"
BASE = "87b88cc4eb7d791c87b83a43215454d38c0f72f8"
sys.path.insert(0, str(ROOT / "tests"))
import abbagate as abba
from abba_profile import WindowProfile
from gate_quiet import QuietMonitor


def pad_source(source):
    """Same optional allocation size/alignment as POST; PRE execution and INFO stay intact.

    The edit exists only in an archived source tree below build/. No production switch ships.
    A stale anchor is an error rather than a silently unpadded control.
    """
    old = "    std::atomic<bool> lane_active{false};\n};"
    new = ("    std::atomic<bool> lane_active{false};\n"
           "    alignas(64) unsigned char observation_padding[192]{};\n};")
    size = 'static_assert(sizeof(ReadLocalThreadState) == 384,\n'
    if source.count(old) != 1 or source.count(size) != 1:
        raise ValueError("PAD source no longer matches the reviewed PRE layout")
    return source.replace(old, new).replace(size,
        'static_assert(offsetof(ReadLocalThreadState, observation_padding) == 384);\n'
        'static_assert(sizeof(ReadLocalThreadState) == 576,\n')


def build_arms(argv):
    parser = argparse.ArgumentParser(description="Build PRE/POST/PAD; never boots a server")
    parser.add_argument("--output", type=Path, default=ARMS)
    parser.add_argument("--cores", default="104-111")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args(argv)
    if not set(abba.cpus(args.cores)) <= set(range(112)) or args.jobs < 1:
        parser.error("build CPUs must stay in 0-111 and jobs must be positive")
    out = args.output.resolve()
    out.relative_to(ROOT / "build")
    out.mkdir(parents=True, exist_ok=False)
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    # Compile a committed, reviewable POST. Untracked logs/reports do not enter a build.
    subprocess.run(["git", "diff", "--exit-code", "HEAD", "--", "src", "Makefile"],
                   cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    manifest = dict(schema=1, base=BASE, post=head, status="INCOMPLETE", arms={},
        compiler=subprocess.check_output(["g++", "--version"], text=True).splitlines()[0],
        sizes=dict(pre=384, post=576, pad=576, post_and_pad_alignment=64),
        measurement_status="NOT_RUN")
    def save():
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    try:
        for arm in ("PRE", "POST", "PAD"):
            print(f"building {arm} on CPUs {args.cores}, jobs={args.jobs}", flush=True)
            source = out / ("source-" + arm.lower())
            source.mkdir()
            revision = head if arm == "POST" else BASE
            archive = subprocess.check_output(["git", "archive", revision], cwd=ROOT)
            with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
                tree.extractall(source, filter="data")
            if arm == "PAD":
                header = source / "src/core/thread.h"
                header.write_text(pad_source(header.read_text()))
            command = ["taskset", "-c", args.cores, "make", f"-j{args.jobs}", "CXX=g++"]
            env = os.environ.copy()
            # Environment (not make command-line) preserves the t_string target-specific flag.
            env["CXXFLAGS"] = "-std=c++20 -O2 -g -Wall -Wextra -march=native -pthread"
            entry = manifest["arms"][arm] = dict(source_revision=revision, command=command,
                cxxflags=env["CXXFLAGS"], pad_only=arm == "PAD")
            save()
            with (out / (arm.lower() + "-build.log")).open("w") as log:
                subprocess.run(command, cwd=source, env=env, stdout=log,
                               stderr=subprocess.STDOUT, check=True)
            binary = out / ("tomokv-" + arm.lower())
            os.link(source / "build/tomokv", binary)
            entry.update(path=str(binary.relative_to(ROOT)), sha256=abba.sha256(binary))
            save()
        manifest["status"] = "COMPLETE"
    except BaseException as error:
        manifest.update(status="FAILED", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        save()


def run(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pre", type=Path, default=ARMS / "tomokv-pre")
    parser.add_argument("--post", type=Path, default=ARMS / "tomokv-post")
    parser.add_argument("--pad", type=Path, default=ARMS / "tomokv-pad")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cells", default="h11,h15,h27,h31")
    parser.add_argument("--order", default="PRE,PRE,PRE,PRE")
    parser.add_argument("--connections", type=int, default=512)
    parser.add_argument("--instances", type=int, default=4)
    parser.add_argument("--window", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--profile", type=int, choices=(0, 1), default=0)
    parser.add_argument("--port", type=int, choices=range(10300, 10320), default=10300)
    args = parser.parse_args(argv)
    order = args.order.split(",")
    if len(order) != 4 or set(order) - {"PRE", "POST", "PAD"}:
        parser.error("order must name four PRE/POST/PAD boots")
    if min(args.connections, args.instances, args.window, args.warmup) <= 0:
        parser.error("connection count, instances and windows must be positive")
    requested = args.cells.split(",")
    cells = {c.id: c for c in abba.read_cells(ROOT / "tests/headline_cells.txt")}
    if len(set(requested)) != len(requested) or set(requested) - cells.keys():
        parser.error("cells must be distinct IDs from headline_cells.txt")
    abba.WINDOW, abba.WARMUP = args.window, args.warmup  # This process only; no harness source edits.
    out = args.output.resolve()
    out.relative_to(ROOT)  # All artifacts remain in this lane's worktree.
    out.mkdir(parents=True, exist_ok=False)
    binaries = {}
    for arm, path in (("PRE", args.pre), ("POST", args.post), ("PAD", args.pad)):
        if arm not in order:
            continue
        target = out / ("binary-" + arm)
        shutil.copy2(path, target)
        binaries[arm] = target
    geometry = argparse.Namespace(server_cores="0-31", server_smt="", load_cores="32-111", load_smt="",
                                  port=args.port, memtier=shutil.which("memtier_benchmark"))
    if not geometry.memtier:
        raise RuntimeError("memtier_benchmark is unavailable")
    abba.check_placement(abba.cpus(geometry.server_cores), abba.cpus(geometry.load_cores))
    children = abba.Children()
    runner = abba.Runner(geometry, out, binaries, children)
    if args.profile:
        runner.profile_factory = WindowProfile
    report = dict(schema=1, kind="read-local-diagnostic", normal_gate_eligible=False,
                  status="INCOMPLETE", order=order, geometry=vars(geometry),
                  historical_geometry_match=False, window=args.window, warmup=args.warmup,
                  binaries={k: abba.sha256(p) for k, p in binaries.items()}, cells=[], summary=[])
    quiet = QuietMonitor(runner.server_cpus, runner.load_cpus, ports=[args.port],
                         sample_artifact=out / "quiet.jsonl")
    def save():
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    def interrupted(signum, _frame):
        raise InterruptedError(f"signal {signum}")
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    try:
        # This read-only preflight detects contention; it does not reserve or authorize the box.
        quiet.start()
        original_affinity = os.sched_getaffinity(0)
        os.sched_setaffinity(0, {111})
        for ident in requested:
            cell = replace(cells[ident], conns=args.connections, instances=args.instances)
            row = dict(cell=asdict(cell), runs=[])
            report["cells"].append(row)
            knobs = {"thread-mode": cell.mode, "read-local": cell.read_local,
                     "overlap": cell.overlap, "reorder": cell.reorder,
                     "key-lb": 1, "client-lb": 1}
            for sequence, arm in enumerate(order, 1):
                quiet.check()
                try:
                    row["runs"].append(runner.measure(cell, arm, sequence, args.instances, knobs))
                finally:
                    save()
            for arm in dict.fromkeys(order):
                runs = [r for r in row["runs"] if r["arm"] == arm]
                rates = [r["rate"] for r in runs]
                profile = [r["cpu_profile"]["derived"] for r in runs] if args.profile else []
                report["summary"].append(dict(cell=ident, arm=arm,
                    rates_mops=[x / 1e6 for x in rates], mean_mops=statistics.mean(rates) / 1e6,
                    spread_pct=100 * (max(rates) - min(rates)) / statistics.mean(rates),
                    ipc=statistics.mean(p["ipc"] for p in profile) if profile else None,
                    approx_instr_per_op=statistics.mean(p["approx_instructions_per_central_command"]
                                                        for p in profile) if profile else None,
                    approx_cycles_per_op=statistics.mean(p["approx_cycles_per_central_command"]
                                                         for p in profile) if profile else None))
            save()
        report["status"] = "COMPLETE"
    except BaseException as error:
        report.update(status="FAILED", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        try:
            children.close()  # Only the subprocess PIDs this driver started.
        finally:
            if "original_affinity" in locals():
                os.sched_setaffinity(0, original_affinity)
            report["quiet"] = quiet.close()
            save()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "build":
        build_arms(sys.argv[2:])
    elif len(sys.argv) > 1 and sys.argv[1] == "run":
        run(sys.argv[2:])
    else:
        raise SystemExit(__doc__)
