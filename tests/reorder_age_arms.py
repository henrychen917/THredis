#!/usr/bin/env python3
"""Prepare fixed-revision R5 measurement arms and serverless negative controls.

This only writes source copies beneath build/. It never builds or runs anything.
Both revisions are explicit: after a checkpoint, HEAD is no longer the PRE arm.
Use reorder_scope.py separately for the shared, unscored S3 instrument.
"""

import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("src", "third_party", "Makefile", "tests/reorder_unit.cc",
           "tests/core_concurrency_unit.cc")


def revision(ref):
    return subprocess.check_output(
        ["git", "rev-parse", "--verify", ref + "^{commit}"], cwd=ROOT, text=True).strip()


def archive(ref, directory):
    data = subprocess.check_output(["git", "archive", ref, *SOURCES], cwd=ROOT)
    directory.mkdir()
    with tarfile.open(fileobj=io.BytesIO(data)) as entries:
        entries.extractall(directory, filter="data")


def change_once(source, anchor, replacement):
    if source.count(anchor) != 1:
        raise ValueError(f"expected exactly one R5 anchor: {anchor!r}")
    return source.replace(anchor, replacement, 1)


def prepare(directory, pre_ref, post_ref):
    directory = directory.resolve()
    if not directory.is_relative_to(ROOT / "build") or directory == ROOT / "build":
        raise ValueError("arm copies must be beneath this worktree's build/ directory")
    if directory.exists():
        raise ValueError(f"refusing to overwrite existing arms: {directory}")
    pre_ref, post_ref = revision(pre_ref), revision(post_ref)
    if pre_ref == post_ref:
        raise ValueError("PRE and POST must be different revisions")
    directory.mkdir(parents=True)
    pre, post = directory / "pre", directory / "post"
    archive(pre_ref, pre)
    archive(post_ref, post)
    header_path = Path("src/core/reorder.h")
    original, header = (pre / header_path).read_text(), (post / header_path).read_text()
    start = header.index("inline int32_t ex_schedule_arrival(")
    end = header.index("\n}\n", start) + 3
    # Keep the POST producer and selector contract, with PRE's unchanged cost scheduler.
    stamps = directory / "stamp-only"
    shutil.copytree(post, stamps)
    (stamps / header_path).write_text(change_once(
        original, "namespace tomo {\n", "namespace tomo {\n\n" + header[start:end] + "\n"))
    for name in ("reorder_unit.cc", "core_concurrency_unit.cc"):
        shutil.copy2(pre / "tests" / name, stamps / "tests" / name)

    mutations = {
        "mutant-fifo": ("    ReorderResult result;\n",
                        "    ReorderResult result;\n    return result;\n"),
        "mutant-cost": (
            "        return arrivals[a] < arrivals[b] || (arrivals[a] == arrivals[b] && a < b);",
            "        const auto ca = tasks[a].client->rob().at(tasks[a].op_id).spec->length_class;\n"
            "        const auto cb = tasks[b].client->rob().at(tasks[b].op_id).spec->length_class;\n"
            "        return ca < cb || (ca == cb && a < b);"),
        "mutant-chain": (
            "            if (index <= previous || tasks[index].op_id <= tasks[previous].op_id) return 1;",
            "            (void)previous; // deliberately remove the connection-order rejection"),
        "mutant-truncate": ("    ReorderResult result;\n",
                            "    n = std::min(n, uint32_t{32});\n    ReorderResult result;\n"),
    }
    for name, (anchor, replacement) in mutations.items():
        mutant = directory / name
        shutil.copytree(post, mutant)
        (mutant / header_path).write_text(change_once(header, anchor, replacement))

    arms = ("pre", "stamp-only", "post", *mutations)
    hashes = {
        arm: {str(path.relative_to(directory / arm)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sorted((directory / arm).rglob("*")) if path.is_file()}
        for arm in arms
    }
    manifest = dict(
        pre_commit=pre_ref, post_commit=post_ref, compiled=False, executed=False,
        runtime_controls=["--overlap 0|1", "--reorder 0|1"],
        decomposition={"pre -> stamp-only": "arrival encoding with PRE's cost scheduler",
                       "stamp-only -> post": "age policy with identical stamped input",
                       "pre -> post": "complete R5 change",
                       "pre ro0 -> post ro0": "disabled producer cost and allocation control"},
        expected_mutant_failures={
            "mutant-fifo": "first required permutation is absent",
            "mutant-cost": "older long operation must precede a younger point operation",
            "mutant-chain": "inconsistent own stamps must preserve the input order",
            "mutant-truncate": "33-task and larger cases require the complete suffix"},
        files=hashes)
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return dict(directory=str(directory), pre_commit=pre_ref, post_commit=post_ref, arms=arms)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pre", required=True, help="exact baseline revision")
    parser.add_argument("--post", required=True, help="exact R5 revision")
    parser.add_argument("--directory", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.directory, args.pre, args.post), indent=2))


if __name__ == "__main__":
    main()
