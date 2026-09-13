#!/usr/bin/env python3
"""Prepare disposable R4 decomposition sources; never compile, boot, or measure them.

PRE is the exact supplied revision. PAD only occupies the schedule sidecar's
existing tail padding, matching POST's footprint with no scheduling change.
FLOOR retains R4's batch plumbing but uses
PRE's static predictor with sampling compiled out. LOOKUP adds only a table
lookup, seeded from PRE's boot classes. SAMPLE adds only windowed observation
to FLOOR's static order. POST is the current candidate without substitutions.
Compare every arm at the same armed geometry; these sources add no server knobs.
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


def once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"study anchor changed ({source.count(old)}): {old!r}")
    return source.replace(old, new, 1)


def fingerprint(path):
    digest = hashlib.sha256()
    for item in sorted(path.rglob("*")):
        if item.is_file():
            digest.update(str(item.relative_to(path)).encode() + b"\0" + item.read_bytes())
    return digest.hexdigest()


def prepare(directory, revision):
    directory = directory.resolve()
    if not directory.is_relative_to(ROOT / "build") or directory == ROOT / "build":
        raise ValueError("study copies must be beneath this worktree's build/ directory")
    if directory.exists():
        raise ValueError(f"destination already exists: {directory}")
    revision = subprocess.check_output(
        ["git", "rev-parse", "--verify", revision + "^{commit}"], cwd=ROOT, text=True).strip()
    archive = subprocess.check_output(
        ["git", "archive", revision, "src", "third_party", "Makefile"], cwd=ROOT)
    directory.mkdir(parents=True)
    pre = directory / "pre"
    pre.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as entries:
        entries.extractall(pre, filter="data")
    post = directory / "post"
    post.mkdir()
    for name in ("src", "third_party"):
        shutil.copytree(ROOT / name, post / name)
    shutil.copy2(ROOT / "Makefile", post / "Makefile")

    # POST uses all 16 previously unnamed tail bytes without moving any existing field.
    # Keep this control independent of FLOOR: that arm also changes batch code and allocates
    # the estimator, so it cannot isolate the effect of naming/initializing those bytes.
    pad = directory / "pad"
    shutil.copytree(pre, pad)
    header = pad / "src/core/orthog.h"
    header.write_text(once(header.read_text(),
        "    std::atomic<OverlapSchedule> overlap_schedule{OverlapSchedule::None};",
        "    std::atomic<OverlapSchedule> overlap_schedule{OverlapSchedule::None};\n"
        "    alignas(void*) unsigned char r4_padding[16]{}; // disposable padding control"))

    old = (pre / "src/core/reorder.h").read_text()
    start = old.index("inline bool ex_sched_candidate(")
    end = old.index("\nstruct ExScheduleKey", start)
    static_candidate = once(old[start:end], "const Task& task, uint8_t& length)",
        "const Task& task, uint8_t& length, [[maybe_unused]] const ExReorderCosts& costs)")

    for arm in ("floor", "lookup", "sample"):
        target = directory / arm
        shutil.copytree(post, target)
        # The reference itself supplies the verb classes AND armed-write promotion. Do not
        # invent pseudo-measured costs to reproduce them, or let a different boot posture slip in.
        for name in ("src/cmd/command.h", "src/cmd/commands.cc", "src/main.cc"):
            shutil.copy2(pre / name, target / name)
        header = target / "src/core/reorder.h"
        source = header.read_text()
        if arm in ("floor", "sample"):
            start = source.index("inline bool ex_sched_candidate(")
            end = source.index("inline uint32_t ExReorderCosts::sample_index", start)
            source = source[:start] + static_candidate + "\n" + source[end:]
        if arm in ("floor", "lookup"):
            start = source.index("inline uint32_t ExReorderCosts::sample_index")
            end = source.index("\nstruct ExScheduleKey", start)
            source = source[:start] + (
                "inline uint32_t ExReorderCosts::sample_index(const Task*, uint32_t n, uint32_t) {\n"
                "    return n; // disposable study: no sample clocks, scans or updates\n}\n"
            ) + source[end:]
        if arm == "lookup":
            source = once(source, "        classes_.fill(kExSchedUnknown);",
                "        classes_.fill(kExSchedUnknown);\n"
                "        for (uint32_t id = 0; id < command_registry_size(); id++)\n"
                "            classes_[id] = command_registry_at(id)->length_class;")
        header.write_text(source)

    manifest = {
        "pre_ref": revision,
        "post_ref": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "post_source_clean": subprocess.run(
            ["git", "diff", "--quiet", "HEAD", "--", "src", "third_party", "Makefile"],
            cwd=ROOT, check=False).returncode == 0,
        "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "source_sha256": {arm: fingerprint(directory / arm)
                          for arm in ("pre", "pad", "floor", "lookup", "sample", "post")},
        "measured": False,
        "comparisons": {
            "pad-pre": "schedule sidecar tail-padding initialization; no scheduler changes",
            "floor-pre": "batch plumbing, placement and cold storage control",
            "lookup-floor": "per-command cached lookup with identical static decisions",
            "sample-floor": "window sampling and EWMA with identical static decisions",
            "post-pre": "complete learned policy, including interactions",
        },
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=ROOT / "build/r4-study")
    parser.add_argument("--pre", default="87b88cc4e", help="exact static-class reference revision")
    args = parser.parse_args()
    print(json.dumps(prepare(args.directory, args.pre), indent=2))
