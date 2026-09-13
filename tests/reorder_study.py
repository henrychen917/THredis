#!/usr/bin/env python3
"""Materialize R2's decomposition and print paired ABBA counters; never run a workload.

Copies stay under build/. The six source arms replace development switches, so
the server keeps only --overlap 0|1 and --reorder 0|1. Compare adjacent arms:
pre -> metadata -> placement -> run-gate -> post. screen-only retains the real
class screen and batch telemetry but removes permutation: it measures screening
cost and is also a negative control for the live permutation witness.

The report uses counts from the same PMC group, never IPC alone. The ABBA
instrument's PMC window encompasses its central command window; per-command
ratios therefore retain its 'approximate' qualification. It does not turn a
missing measurement, a scope probe, or an unarmed cell into a competition result.
"""

import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import shutil
import statistics
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ARMS = ("pre", "metadata", "placement", "run-gate", "screen-only", "post")


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"decomposition anchor changed: {old!r}")
    return source.replace(old, new, 1)


def prefetch_first(source):
    source = replace_once(source,
        "                if (__builtin_expect(reorder_enabled_, false))\n"
        "                    srv_->mode_schedule_stats(self_->id()).note_reorder(\n"
        "                        held, ex_schedule_batch(batch, held));\n"
        "                prefetch_exec_batch(batch, held);",
        "                prefetch_exec_batch(batch, held);\n"
        "                if (__builtin_expect(reorder_enabled_, false))\n"
        "                    srv_->mode_schedule_stats(self_->id()).note_reorder(\n"
        "                        held, ex_schedule_batch(batch, held));")
    return replace_once(source,
        "        if (__builtin_expect(reorder_enabled_, false))\n"
        "            srv_->mode_schedule_stats(self_->id()).note_reorder(n, ex_schedule_batch(batch, n));\n"
        "        prefetch_exec_batch(batch, n);",
        "        prefetch_exec_batch(batch, n);\n"
        "        if (__builtin_expect(reorder_enabled_, false))\n"
        "            srv_->mode_schedule_stats(self_->id()).note_reorder(n, ex_schedule_batch(batch, n));")


def prepare(directory, ref):
    directory = directory.resolve()
    if directory == ROOT / "build" or not directory.is_relative_to(ROOT / "build"):
        raise ValueError("decomposition copies must stay beneath this worktree's build/")
    if directory.exists():
        raise ValueError(f"destination already exists: {directory}")
    commit = subprocess.check_output(
        ["git", "rev-parse", "--verify", ref + "^{commit}"], cwd=ROOT, text=True).strip()
    archive = subprocess.check_output(
        ["git", "archive", commit, "src", "third_party", "Makefile"], cwd=ROOT)
    pre_loop = subprocess.check_output(
        ["git", "show", commit + ":src/core/ex_loop.h"], cwd=ROOT, text=True)
    placement = prefetch_first(pre_loop)
    post_loop = (ROOT / "src/core/ex_loop.h").read_text()
    screen_only = replace_once(post_loop,
        "note_reorder(n, ex_schedule_batch(batch, n))", "note_reorder(n, ReorderResult{})")
    directory.mkdir(parents=True)
    manifest = {"pre_commit": commit, "arms": {}, "measured": False}
    for arm in ARMS:
        target = directory / arm
        target.mkdir()
        with tarfile.open(fileobj=io.BytesIO(archive)) as entries:
            entries.extractall(target, filter="data")
        if arm != "pre":
            for name in ("src/cmd/command.h", "src/cmd/commands.cc", "src/main.cc"):
                shutil.copy2(ROOT / name, target / name)
        if arm in ("placement", "run-gate"):
            (target / "src/core/ex_loop.h").write_text(placement)
        if arm in ("run-gate", "screen-only", "post"):
            shutil.copy2(ROOT / "src/core/reorder.h", target / "src/core/reorder.h")
        if arm in ("screen-only", "post"):
            (target / "src/core/ex_loop.h").write_text(screen_only if arm == "screen-only" else post_loop)
        digest = hashlib.sha256()
        for path in sorted(target.rglob("*")):
            if path.is_file():
                digest.update(str(path.relative_to(target)).encode() + b"\0" + path.read_bytes())
        manifest["arms"][arm] = {"source_sha256": digest.hexdigest(), "path": str(target),
                                  "identity_control": arm == "screen-only"}
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def positive(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError("missing/nonpositive measurement")
    return value


def counters(run):
    if not run.get("complete"):
        raise ValueError("incomplete ABBA run")
    profile = run["cpu_profile"]
    if profile["status"] != "COMPLETE":
        raise ValueError("incomplete PMC profile")
    totals = profile["derived"]["totals"]
    commands = positive(run["commands"])
    cycles, instructions = positive(totals["cycles"]), positive(totals["instructions"])
    return dict(rate=positive(run["rate"]), ipc=instructions / cycles,
                instructions=instructions / commands, cycles=cycles / commands)


def report(data):
    if not data.get("cells"):
        raise ValueError("no measured cells")
    rows = []
    for result in data["cells"]:
        cell = result["cell"]
        rounds = result.get("rounds", [])
        if not rounds:
            raise ValueError(f"{cell['id']}: no ABBA rounds")
        # Keep every reported round. Selecting a final/fastest round could discard a reversal.
        for index, round_ in enumerate(rounds):
            runs = round_["runs"]
            if [r.get("arm") for r in runs] != ["A", "B", "B", "A"]:
                raise ValueError(f"{cell['id']}: incomplete/unordered ABBA quartet")
            values = {arm: [counters(r) for r in runs if r["arm"] == arm] for arm in ("A", "B")}
            summary = {}
            for arm, samples in values.items():
                summary[arm] = {key: statistics.median(s[key] for s in samples) for key in samples[0]}
                # Ratios of separately aggregated medians need not multiply exactly. Display
                # IPC from the displayed instructions/cycles so the verdict remains inspectable.
                summary[arm]["ipc"] = summary[arm]["instructions"] / summary[arm]["cycles"]
                if cell.get("op") == "REORDER":
                    for key in ("p999_ms", "long_p999_ms"):
                        summary[arm][key] = statistics.median(
                            positive(r[key]) for r in runs if r["arm"] == arm)
            before, after = summary["A"], summary["B"]
            rows.append(dict(cell=cell, round=index + 1, pre=before, post=after,
                cycles_delta_pct=100 * (after["cycles"] / before["cycles"] - 1),
                required_ipc_gain_pct=100 * (after["instructions"] / before["instructions"] - 1),
                armed=cell.get("read_local") == cell.get("atomic") == 1))
    return rows


def markdown(rows):
    print("PMC counts/central commands are approximate; IPC below = displayed instr/op / cycles/op.")
    print("Key/client balancing must also be verified in each run's launch/config evidence.")
    print("\n| Cell | Mode | Work | Depth | RL/A/O/R | Round | Arm | Rate M/s | IPC | Instr/op | Cycles/op |")
    print("|---|---|---|---:|---|---:|---|---:|---:|---:|---:|")
    for row in rows:
        cell = row["cell"]
        knobs = "/".join(str(cell[k]) for k in ("read_local", "atomic", "overlap", "reorder"))
        for arm in ("pre", "post"):
            value = row[arm]
            print(f"| {cell['id']} | {cell['mode']} | {cell['op']} | {cell['depth']} | {knobs} | "
                  f"{row['round']} | {arm.upper()} | {value['rate'] / 1e6:.6f} | {value['ipc']:.4f} | "
                  f"{value['instructions']:.2f} | {value['cycles']:.2f} |")
    print("\n| Cell/round | Cycles/op change | IPC gain needed to pay for instructions | RL/atomics armed |")
    print("|---|---:|---:|---|")
    for row in rows:
        print(f"| {row['cell']['id']}/{row['round']} | {row['cycles_delta_pct']:+.3f}% | "
              f"{row['required_ipc_gain_pct']:+.3f}% | {'yes' if row['armed'] else 'no'} |")
    tails = [row for row in rows if row["cell"].get("op") == "REORDER"]
    if tails:
        print("\n| Cell/round | PRE GET p99.9 ms | POST GET p99.9 ms | PRE BITCOUNT p99.9 ms | POST BITCOUNT p99.9 ms |")
        print("|---|---:|---:|---:|---:|")
        for row in tails:
            pre, post = row["pre"], row["post"]
            print(f"| {row['cell']['id']}/{row['round']} | {pre['p999_ms']:.4f} | {post['p999_ms']:.4f} | "
                  f"{pre['long_p999_ms']:.4f} | {post['long_p999_ms']:.4f} |")
    print("\nPayoff is observed only in tested cells with lower cycles/op. A numeric load/depth/mix "
          "break-even needs adjacent measured controls; this report does not extrapolate one.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    prep = commands.add_parser("prepare")
    prep.add_argument("--directory", type=Path, default=ROOT / "build/reorder-R2-arms")
    prep.add_argument("--ref", default="87b88cc4e")
    show = commands.add_parser("report")
    show.add_argument("results", type=Path)
    commands.add_parser("self-test")
    args = parser.parse_args()
    if args.action == "prepare":
        print(json.dumps(prepare(args.directory, args.ref), indent=2))
    elif args.action == "report":
        markdown(report(json.loads(args.results.read_text())))
    else:
        self_test()


def self_test():
    import copy

    def run(arm, instructions, cycles):
        return dict(arm=arm, complete=True, commands=100, rate=100,
            cpu_profile=dict(status="COMPLETE", derived=dict(
                totals=dict(instructions=instructions, cycles=cycles))))

    def fixture():
        # IPC rises by 10%, yet twice as many instructions make cycles/op worse.
        return {"cells": [dict(cell=dict(id="control", read_local=1, atomic=1),
            rounds=[dict(runs=[run("A", 1000, 500), run("B", 2200, 1000),
                              run("B", 2200, 1000), run("A", 1000, 500)])])]}

    row = report(fixture())[0]
    assert row["post"]["ipc"] > row["pre"]["ipc"]
    assert row["cycles_delta_pct"] == 100
    assert math.isclose(row["required_ipc_gain_pct"], 120)
    assert row["post"]["instructions"] / row["post"]["ipc"] == row["post"]["cycles"]
    bad = [{"cells": []}]
    for kind in ("incomplete", "no-pmc", "zero", "nonfinite", "order", "partial"):
        data = copy.deepcopy(fixture())
        runs = data["cells"][0]["rounds"][0]["runs"]
        if kind == "incomplete": runs[0]["complete"] = False
        elif kind == "no-pmc": runs[0]["cpu_profile"]["status"] = "INVALID"
        elif kind == "zero": runs[0]["commands"] = 0
        elif kind == "nonfinite": runs[0]["rate"] = float("nan")
        elif kind == "order": runs[0]["arm"] = "B"
        elif kind == "partial": runs.pop()
        bad.append(data)
    for data in bad:
        try:
            report(data)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid measurement accepted")
    for value in ("nothing", "doubled doubled"):
        try:
            replace_once(value, "doubled", "replacement")
        except ValueError:
            pass
        else:
            raise AssertionError("ambiguous source anchor accepted")
    print("PASS reorder study: IPC-only win rejected; missing/invalid counters and anchors rejected")


if __name__ == "__main__":
    main()
