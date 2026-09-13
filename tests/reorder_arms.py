#!/usr/bin/env python3
"""Prepare exact R6 measurement sources beneath build/; never run a workload.

The source controls separate EX-sort removal, private IO publication and IO sorting
without adding runtime knobs. PAD adds inert members in the same existing padding
as POST while retaining PRE behavior. Build each copy with the repository Makefile.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

from reorder_scope import ROOT, destination_path, replace_once


def revision(name):
    return subprocess.check_output(
        ["git", "rev-parse", "--verify", name + "^{commit}"], cwd=ROOT, text=True).strip()


def sources(ref):
    archive = subprocess.check_output(
        ["git", "archive", ref, "src", "third_party", "Makefile"], cwd=ROOT)
    with tarfile.open(fileobj=io.BytesIO(archive)) as entries:
        result = {}
        for entry in entries:
            if entry.isdir():
                continue
            if not entry.isfile():
                raise ValueError(f"expected regular source file: {entry.name}")
            result[entry.name] = entries.extractfile(entry).read()
        return result


def edit(tree, path, anchor, replacement):
    tree[path] = replace_once(tree[path].decode(), anchor, replacement).encode()


def digest(tree):
    checksum = hashlib.sha256()
    for name, data in sorted(tree.items()):
        checksum.update(name.encode() + b"\0" + data)
    return checksum.hexdigest()


def controls(pre, post):
    # Validate every injection before creating any artifact. A changed baseline must
    # fail explicitly instead of silently producing a mislabeled measurement arm.
    exfifo = pre.copy()
    for anchor in (
        "                if (__builtin_expect(reorder_enabled_, false))\n"
        "                    srv_->mode_schedule_stats(self_->id()).note_reorder(\n"
        "                        held, ex_schedule_batch(batch, held));\n",
        "        if (__builtin_expect(reorder_enabled_, false))\n"
        "            srv_->mode_schedule_stats(self_->id()).note_reorder(n, ex_schedule_batch(batch, n));\n",
    ):
        edit(exfifo, "src/core/ex_loop.h", anchor, "")
    fifo = post.copy()
    edit(fifo, "src/core/io_loop.h",
         "                    const ReorderResult result = ex_schedule_batch(batch, count);",
         "                    const ReorderResult result{}; // FIFO admission control.")
    pad = pre.copy()
    edit(pad, "src/exec/masked_queue.h",
         "        uint64_t arena_capacity_at_lane_full_sum = 0;",
         "        uint64_t arena_capacity_at_lane_full_sum = 0;\n"
         "        uint32_t r6_admission_padding_ = 0;")
    edit(pad, "src/core/io_loop.h",
         "    bool targeted_ifid_ = false;",
         "    bool targeted_ifid_ = false;\n"
         "    bool r6_selector_padding_[2] = {};")
    return {"pre": pre, "pre-exfifo": exfifo, "admit-fifo": fifo, "post": post, "pad": pad}


def prepare(directory, pre_ref, post_ref):
    directory = destination_path(directory)
    if directory.exists():
        raise ValueError(f"destination already exists: {directory}")
    pre_ref, post_ref = revision(pre_ref), revision(post_ref)
    pre, post = sources(pre_ref), sources(post_ref)
    arms = controls(pre, post)
    manifest = dict(pre_ref=pre_ref, post_ref=post_ref,
                    generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    arms={})
    for name, tree in arms.items():
        baseline = pre if name in ("pre", "pre-exfifo", "pad") else post
        manifest["arms"][name] = dict(
            source_sha256=digest(tree),
            changed_files=[key for key in sorted(set(tree) | set(baseline))
                           if tree.get(key) != baseline.get(key)],
            binary=str(directory / name / "build/tomokv"))
        for path, data in tree.items():
            target = directory / name / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pre", required=True, help="exact PRE commit")
    parser.add_argument("--post", default="HEAD", help="exact POST commit (default HEAD)")
    parser.add_argument("--directory", type=Path, default=ROOT / "build/r6-arms")
    args = parser.parse_args()
    print(json.dumps(prepare(args.directory, args.pre, args.post), indent=2))


if __name__ == "__main__":
    main()
