#!/usr/bin/env python3
"""Bind a complete push/release gate to source contents, its binary, and actual evidence."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
from itertools import product
import json
import math
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 1
ABBA_LABEL = "headline ABBA vs last pushed binary"
# These are retirement guards, not inventories. All rows/cells in the CURRENT files are required,
# including future additions. The structural checks below also preserve the 64 original cells,
# restored 96 multi-key cells, and the 18 deliberate supplemental regimes.
MIN_ROWS = 437
NULL_MAX_AGE = 24 * 60 * 60
ORDER = ["A", "B", "B", "A"]
HARNESS_DIRS = ("tests/", ".githooks/", "bench/", "benchmarks/", "scripts/", "make/")


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


def git(root, *args, data=None):
    result = subprocess.run(["git", "-C", str(root), *args], input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    require(result.returncode == 0, result.stderr.decode(errors="replace").strip())
    return result.stdout


def object_file(path):
    before = path.lstat()
    if stat.S_ISLNK(before.st_mode):
        content, mode = os.fsencode(os.readlink(path)), "120000"
    else:
        require(stat.S_ISREG(before.st_mode), f"unsupported source file type: {path}")
        content = path.read_bytes()
        mode = "100755" if before.st_mode & 0o111 else "100644"
    after = path.lstat()
    identity = lambda s: (s.st_dev, s.st_ino, s.st_mode, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    require(identity(before) == identity(after), f"file changed while fingerprinting: {path}")
    return mode, digest(content)


def fingerprint_entries(entries):
    entries = sorted(entries, key=lambda item: os.fsencode(item["path"]))
    return {"sha256": digest(canonical(entries)), "entries": entries}


def source_fingerprint(root):
    # Git's index supplies tracked names even when ignored. Nonignored untracked sources count
    # too, and staging/committing those same bytes later changes neither this manifest nor its hash.
    # Raw file bytes and executable/symlink modes match Git trees; no HEAD/mtime/index hash is used.
    names = set(git(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split(b"\0"))
    entries = []
    for raw in names - {b""}:
        name = os.fsdecode(raw)
        path = root / name
        require(not path.parent.is_symlink(), f"symlinked source parent: {name}")
        try:
            mode, checksum = object_file(path)
        except FileNotFoundError:
            continue  # A tracked deletion has the same fingerprint before and after committing it.
        entries.append({"path": name, "mode": mode, "sha256": checksum})
    return fingerprint_entries(entries)


def harness_from_source(source):
    # A standing null may use another server binary, but never another measurement harness.
    # Include all test and driver helpers, not just abbagate.py; an imported helper is executable
    # measurement code too. Server src/ changes are deliberately outside this reuse boundary.
    return fingerprint_entries([row for row in source["entries"] if
        row["path"] == "Makefile" or row["path"].startswith(HARNESS_DIRS)])


def harness_fingerprint(root):
    return harness_from_source(source_fingerprint(root))


def tree_fingerprint(root, revision):
    rows = git(root, "ls-tree", "-r", "-z", revision + "^{tree}").split(b"\0")
    items = []
    for row in rows:
        if not row:
            continue
        header, rawpath = row.split(b"\t", 1)
        mode, kind, oid = header.split()
        require(kind == b"blob" and mode in (b"100644", b"100755", b"120000"),
                "submodules/unsupported Git modes cannot receive a source receipt")
        items.append((os.fsdecode(rawpath), mode.decode(), oid))
    payload = git(root, "cat-file", "--batch", data=b"".join(oid + b"\n" for _, _, oid in items))
    offset, entries = 0, []
    for name, mode, oid in items:
        end = payload.index(b"\n", offset)
        got, kind, size = payload[offset:end].split()
        require(got == oid and kind == b"blob", "Git returned another object")
        offset = end + 1
        length = int(size)
        content = payload[offset:offset + length]
        require(len(content) == length and payload[offset + length:offset + length + 1] == b"\n",
                "truncated Git object")
        offset += length + 1
        entries.append({"path": name, "mode": mode, "sha256": digest(content)})
    require(offset == len(payload), "unexpected Git object output")
    return fingerprint_entries(entries)


def read_json(path):
    def pairs(values):
        result = {}
        for key, value in values:
            require(key not in result, f"duplicate JSON key {key} in {path}")
            result[key] = value
        return result
    return json.loads(path.read_text(), object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"invalid {value}")))


def write_json(path, value, *, exclusive=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = canonical(value) + b"\n"
    if exclusive:
        with path.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        return
    fd, temporary = tempfile.mkstemp(prefix=".receipt-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def history_directory(root):
    directory = root / ".gate-history" / "receipts"
    # Never silently hide source: the repository must explicitly ignore local gate evidence.
    # In particular, do not mutate the common repository's info/exclude from another worktree.
    relative = ".gate-history/receipts/probe.json"
    result = subprocess.run(["git", "-C", str(root), "check-ignore", "-q", relative])
    require(result.returncode == 0, ".gate-history/ must be ignored before storing local receipts")
    require(not git(root, "ls-files", "--", ".gate-history").strip(),
            "receipt history must not contain tracked files")
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def inventory(root, path):
    # This is a schema reader, not another measurement implementation. Keep every field in the
    # receipt and compare it to abbagate's asdict(Cell) output. Unknown future fields fail closed
    # until the receipt schema is reviewed; they cannot silently escape source/inventory binding.
    cells = []
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = [field.strip() for field in line.split("|")]
        require(len(fields) in (11, 15), "unknown headline cell schema")
        ident, mode, rl, ov, ro, op, depth, conns, measured, busy, pinned = fields[:11]
        require(re.fullmatch(r"[A-Za-z0-9_-]+", ident) and mode in ("1s", "2s") and
                re.fullmatch(r"p[1-9][0-9]*", depth) and re.fullmatch(r"[1-9][0-9]*", conns) and
                re.fullmatch(r"-|[1-9][0-9]*", pinned), "invalid headline cell identity/geometry")
        require(all(re.fullmatch(prefix + r"=[01]", value) for prefix, value in
                    (("rl", rl), ("ov", ov), ("ro", ro))), "invalid cell knob")
        cell = dict(id=ident, mode=mode, read_local=int(rl[-1]), overlap=int(ov[-1]),
                    reorder=int(ro[-1]), op=op, depth=int(depth[1:]), conns=int(conns),
                    instances=0 if pinned == "-" else int(pinned), atomic=1, score="auto",
                    mix="-", smoke=False, pin_required=False)
        if len(fields) == 15:
            atomic, score, mix, smoke = fields[11:]
            require(re.fullmatch(r"atomic=[01]", atomic) and score in
                    ("score=rate", "score=latency", "score=p999") and
                    re.fullmatch(r"mix=(-|[1-9][0-9]*:[1-9][0-9]*)", mix) and
                    re.fullmatch(r"smoke=[01]", smoke), "invalid extended cell schema")
            cell.update(atomic=int(atomic[-1]), score=score[6:], mix=mix[4:],
                        smoke=smoke[-1] == "1", pin_required=cell["depth"] > 1)
        cells.append(cell)
    require(cells and len({cell["id"] for cell in cells}) == len(cells), "duplicate/empty cell inventory")
    # Requiring the complete cross products catches retirement even if someone replaces removed
    # cells with duplicates at another geometry and preserves the numeric count.
    def axis(c):
        return (c["mode"], c["read_local"], c["overlap"], c["reorder"], c["op"],
                c["depth"], c["conns"], c.get("atomic", 1), c.get("mix", "-"))
    actual = {axis(c) for c in cells}
    required = set()
    for mode, rl, ov, ro in product(("1s", "2s"), (0, 1), (0, 1), (0, 1)):
        for op, depth in product(("GET", "SET"), (1, 32)):
            required.add((mode, rl, ov, ro, op, depth, 512, 1, "-"))
        for op, depth in product(("MGET", "MSET"), (1, 8, 32)):
            required.add((mode, rl, ov, ro, op, depth, 512, 1, "-"))
    for mode in ("1s", "2s"):
        for rl in (0, 1):
            required.add((mode, rl, 1, 1, "MIX", 1, 512, 1, "7:1"))
            required.add((mode, rl, 1, 1, "GET", 32, 2048, 1, "-"))
        required.add((mode, 1, 1, 1, "MIX8", 128, 512, 1, "18:14"))
        for op in ("MGET", "MSET"):
            required.add((mode, 1, 1, 1, op, 8, 512, 0, "-"))
        for ro in (0, 1):
            required.add((mode, 0, 1, ro, "REORDER", 8, 512, 1, "95:5"))
    require(required <= actual, f"full inventory retired {len(required - actual)} required cell geometries")
    for cell in cells:
        require(cell["depth"] == 1 or cell.get("instances", 0) > 0,
                f"{cell['id']}: unmeasured load floor; re-pin with --escalate before certification")
        require(cell["op"] != "REORDER" or cell.get("score") == "p999",
                f"{cell['id']}: reorder needs p99.9 scoring")
    return {"path": str(path.relative_to(root)), "sha256": digest(path.read_bytes()),
            "count": len(cells), "cells": cells}


def ledger_rows(path, *, passing):
    rows = []
    text = path.read_text()
    require(text.endswith("\n"), f"incomplete ledger: {path}")
    for lineno, line in enumerate(text.splitlines(), 1):
        fields = line.split("\t")
        require(len(fields) == 3 and fields[0] in ("ok", "FAIL"), f"invalid ledger row {path}:{lineno}")
        verdict, seconds, label = fields
        duration = number(float(seconds), "row seconds")
        require(label and not any(c in label for c in "\r\n\0"), "invalid ledger label")
        require(not passing or verdict == "ok", f"nonpassing row: {label}")
        rows.append({"verdict": verdict, "seconds": duration, "label": label})
    return rows


def expected_count(root, nic):
    matches = re.findall(r"^EXPECT_FULL=([0-9]+)\b", (root / "tests/gate.sh").read_text(), re.M)
    require(len(matches) == 1 and int(matches[0]) >= MIN_ROWS, "full gate count missing or reduced below 437")
    return int(matches[0]) + int(nic)


def begin(root, args):
    require(args.tier in ("full", "push", "release"), "smoke/iteration gates cannot create push receipts")
    directory = history_directory(root)
    require(re.fullmatch(r"[A-Za-z0-9_.:-]+", args.run_id) and args.run_id not in (".", ".."), "invalid run ID")
    baseline, baseline_identity, withheld = [], None, None
    nic = args.nic
    try:
        baseline = ledger_rows(args.expected_ledger, passing=False)
        if getattr(args, "nic_auto", False):
            nic = any(row["label"] == "NIC regression cells (all within -3%)" for row in baseline)
        count = expected_count(root, nic)
        require(len(baseline) == count and sum(r["label"] == ABBA_LABEL for r in baseline) == 1,
                f"trusted baseline ledger must contain exactly {count} rows including the ABBA row")
        baseline_identity = {"path": str(args.expected_ledger.resolve()),
                             "sha256": digest(args.expected_ledger.read_bytes())}
    except (ValueError, OSError) as error:
        if not getattr(args, "allow_missing_baseline", False):
            raise
        # Bootstrap is a state worth preserving, never authorization to shrink/skip the gate or
        # to trust its own newly observed labels. An owner can review the completed full ledger
        # and explicitly supply it as GATE_RECEIPT_BASELINE for the next run. Automatic discovery
        # uses only baselines written after a previous receipt was successfully certified.
        baseline = []
        withheld = "missing/invalid trusted baseline: " + str(error)
        print("GATE RECEIPT PENDING: " + withheld + "; all gate work still runs; no push receipt will be issued", file=sys.stderr)
    count = expected_count(root, nic)
    source = source_fingerprint(root)
    state = {"schema": SCHEMA, "kind": "gate-start", "run_id": args.run_id, "tier": args.tier,
             "started_at": time.time(), "source": source, "harness": harness_from_source(source),
             "inventory": inventory(root, args.cells.resolve()), "expected_checks": count,
             "expected_labels": [r["label"] for r in baseline], "nic": nic,
             "baseline": baseline_identity, "withheld_reason": withheld}
    path = directory / "runs" / args.run_id / "start.json"
    write_json(path, state, exclusive=True)
    return path


def load_start(root, path):
    require(path.resolve().is_relative_to(history_directory(root).resolve() / "runs"),
            "start manifest must be in this worktree's local receipt history")
    state = read_json(path)
    require(state.get("schema") == SCHEMA and state.get("kind") == "gate-start", "invalid start manifest")
    require(source_fingerprint(root) == state["source"], "source contents/modes changed since gate start")
    require(inventory(root, root / state["inventory"]["path"]) == state["inventory"], "cell inventory changed")
    require(expected_count(root, state["nic"]) == state["expected_checks"], "gate row count changed")
    return state


def bind(root, args):
    state = load_start(root, args.start)
    binary = args.candidate.resolve()
    require(binary.is_relative_to(root), "candidate must be built inside this worktree")
    mode, checksum = object_file(binary)
    require(mode == "100755", "candidate must be an executable regular file")
    binding = {"schema": SCHEMA, "kind": "gate-candidate", "run_id": state["run_id"],
               "bound_at": time.time(), "path": str(binary.relative_to(root)), "sha256": checksum,
               "source_sha256": state["source"]["sha256"], "start_sha256": digest(args.start.read_bytes())}
    path = args.start.parent / "candidate.json"
    write_json(path, binding, exclusive=True)
    return path


def utc_seconds(value):
    require(isinstance(value, str), "missing ABBA start timestamp")
    return datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()


def validate_abba(report, state, *, now, candidate=None):
    require(report.get("schema") == 1 and report.get("verdict") == "PASS" and
            report.get("subset") == "full", "only a complete PASS of the full ABBA set can certify a push")
    require(report.get("measurement_valid") is True, "ABBA measurement validity was not certified")
    require(report.get("receipt_harness_sha256") == state["harness"]["sha256"], "ABBA harness differs")
    require(report.get("order") == ORDER, "ABBA sequence changed")
    started = utc_seconds(report.get("started_utc"))
    elapsed = number(report.get("elapsed_seconds"), "ABBA elapsed seconds", positive=True)
    require(started <= now and started + elapsed <= now + 1, "ABBA timestamps are incomplete or in the future")
    number(report.get("window_seconds"), "ABBA window", positive=True)
    source = report.get("cell_source", {})
    inv = state["inventory"]
    require(source.get("sha256") == inv["sha256"] and source.get("total_cells") == inv["count"] and
            digest(source.get("text", "").encode()) == inv["sha256"], "ABBA inventory does not match current cell file")
    ids = [cell["id"] for cell in inv["cells"]]
    require(report.get("coverage", {}).get("ids") == ids and report["coverage"].get("count") == len(ids),
            "ABBA coverage omits or repeats full-set cells")
    require(not report["coverage"].get("pending_pins"), "ABBA still has unmeasured load floors")
    for arm in ("candidate", "reference"):
        require(re.fullmatch(r"[0-9a-f]{64}", report.get(arm, {}).get("sha256", "")), f"missing {arm} binary digest")
    require(candidate is None or report["candidate"]["sha256"] == candidate["sha256"], "ABBA measured another candidate binary")
    environment = report.get("environment", {})
    for key in ("server_cpus", "load_cpus", "server_physical", "load_physical"):
        value = environment.get(key)
        require(isinstance(value, list) and value and all(type(cpu) is int and cpu >= 0 for cpu in value)
                and len(value) == len(set(value)), f"invalid ABBA {key}")
    require(len(environment["server_physical"]) <= 32 and
            not set(environment["server_cpus"]) & set(environment["load_cpus"]), "invalid regression CPU allocation")
    for key in ("uname", "memtier_sha256", "memtier_version", "keys", "data_bytes", "key_pattern", "split_ratio"):
        require(environment.get(key), f"missing measurement environment: {key}")
    quiet = report.get("quiet_box", {})
    require(quiet.get("complete") is True and quiet.get("interference", "missing") is None,
            "quiet-box evidence missing, incomplete, or contended")
    qstart = number(quiet.get("started_at"), "quiet start", positive=True)
    qend = number(quiet.get("finished_at"), "quiet end", positive=True)
    interval = number(quiet.get("sample_interval_seconds"), "quiet sample interval", positive=True)
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
    rows = report.get("cells", [])
    require(len(rows) == len(ids), "missing/extra ABBA cell results")
    windows = 0
    for cell, row in zip(inv["cells"], rows):
        require(row.get("cell") == cell, f"ABBA parameters differ for {cell['id']}")
        require(row.get("verdict") == "PASS" and row.get("instrument_valid", True) is True,
                f"nonpassing ABBA cell: {cell['id']}")
        assessment = row.get("assessment", {})
        require(assessment.get("verdict") == "PASS" and assessment.get("reasons") == [],
                f"unassessed/failed ABBA cell: {cell['id']}")
        require(assessment.get("saturation_exempt") is (cell["depth"] == 1), "invalid saturation exemption")
        require(signed_number(assessment.get("loss_pct"), "loss") <=
                number(assessment.get("threshold_pct"), "threshold"), "cell loss exceeds its threshold")
        rounds = row.get("rounds", [])
        require(rounds, f"unreached ABBA cell: {cell['id']}")
        for block in rounds:
            runs = block.get("runs", [])
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


def observations(path, state, actual, finished):
    expected = set(state["expected_labels"])
    selected = []
    seen = set()
    for line in path.read_text().splitlines():
        row = json.loads(line)
        if row.get("run_id") != state["run_id"]:
            continue
        require(row.get("schema") == 1 and row.get("timing") == "own-row" and row.get("verdict") == "ok"
                and row.get("timed_out") is False, "row history has a failure, timeout, or non-owned timing")
        require(row.get("observation_id") and row["observation_id"] not in seen, "duplicate/missing row observation ID")
        seen.add(row["observation_id"])
        when = number(row.get("recorded_at"), "row recording time", positive=True)
        require(state["started_at"] <= when <= finished, "row history is from outside this run")
        # These explicit fields come from the row recorder, not a regex stripping brackets from
        # arbitrary labels. Hidden build prerequisites remain audited but do not invent scored
        # gate rows. Every scored observation must correspond to a counted ledger occurrence.
        require(type(row.get("scored")) is bool, "row observation lacks explicit scored flag")
        if row["scored"]:
            require(row.get("ledger_label") in expected, f"unplanned scored observation: {row.get('label')}")
        else:
            require(row.get("ledger_label", "missing") is None, "unscored dependency has a ledger label")
        selected.append(row)
    key = lambda r: (r["verdict"], r["seconds"], r.get("ledger_label", r["label"]))
    require(Counter(map(key, (row for row in selected if row["scored"]))) == Counter(map(key, actual)),
            "actual ledger rows/durations lack matching independent own-row observations")
    return selected


def finish(root, args):
    state = load_start(root, args.start)
    require(state.get("baseline") is not None and not state.get("withheld_reason"),
            (state.get("withheld_reason") or "missing trusted baseline") +
            "; completed full-run evidence is retained, but its rows are not automatically trusted")
    candidate = read_json(args.start.parent / "candidate.json")
    require(candidate.get("start_sha256") == digest(args.start.read_bytes()) and
            candidate.get("source_sha256") == state["source"]["sha256"], "candidate binding is from another run")
    require(object_file(root / candidate["path"]) == ("100755", candidate["sha256"]), "candidate binary changed after binding")
    completed = read_json(args.gate_result)
    now = time.time()
    # Minimal coordinator artifact, written only AFTER all correctness children and ABBA are reaped:
    # {schema:1, run_id, tier:'push'|'release'|'full', completed:true, verdict:'PASS', exit_code:0,
    #  started_at:<same start.json value>, finished_at:<UNIX>, checks:437, passed:437, failed:0,
    #  skipped:0, abba_exit_code:0, nic_checked:false, candidate_sha256:<bound binary digest>}.
    # A human label list is an expectation, never proof of execution: receipt issuance also needs
    # the exact final ledger AND the gate_history recorder's independent per-row observations.
    require(completed.get("schema") == 1 and completed.get("run_id") == state["run_id"] and
            completed.get("tier") == state["tier"] and completed.get("completed") is True and
            completed.get("verdict") == "PASS", "gate coordinator did not complete the requested full tier")
    count = state["expected_checks"]
    for key, value in {"exit_code": 0, "checks": count, "passed": count, "failed": 0, "skipped": 0,
                       "abba_exit_code": 0, "nic_checked": state["nic"],
                       "candidate_sha256": candidate["sha256"], "started_at": state["started_at"]}.items():
        require(completed.get(key) == value and type(completed.get(key)) is type(value), f"incorrect gate completion {key}")
    ended = number(completed.get("finished_at"), "gate completion time", positive=True)
    require(state["started_at"] <= candidate["bound_at"] <= ended <= now, "invalid gate/candidate times")
    actual = ledger_rows(args.ledger, passing=True)
    require(Counter(row["label"] for row in actual) == Counter(state["expected_labels"]),
            "gate rows are missing, duplicated, or different from the trusted full baseline")
    observed = observations(args.observations, state, actual, ended)
    report, control = read_json(args.abba_result), read_json(args.null_result)
    started, environment = validate_abba(report, state, now=ended, candidate=candidate)
    require(started + 1 >= candidate["bound_at"], "ABBA predates this candidate binding")
    null_start, null_environment = validate_abba(control, state, now=ended)
    require(control["candidate"]["sha256"] == control["reference"]["sha256"], "null arms are not byte-identical")
    require(0 <= started - null_start <= NULL_MAX_AGE, "standing full null is from the future or more than 24 hours old")
    require(report["window_seconds"] == control["window_seconds"], "null used another measurement window")
    # Ports and executable pathname can differ without changing the instrument. All other
    # environment settings, including any future population/geometry fields, must match exactly.
    instrument = lambda env: {k: v for k, v in env.items() if k not in ("port", "permitted_ports", "memtier_path")}
    require(instrument(environment) == instrument(null_environment), "null used another geometry or measurement environment")
    require(source_fingerprint(root) == state["source"], "source changed while validating gate evidence")
    evidence = {"start": state, "candidate": candidate, "coordinator": completed,
                "ledger": actual, "observations": observed, "abba": report, "null": control}
    # Retain evidence locally so a later run rotating build/ logs cannot turn the hook into either
    # a false acceptance or a false rejection. This is an audit receipt, not a signature against a
    # malicious owner rewriting local files or bypassing Git hooks with --no-verify.
    evidence_path = args.start.parent / "evidence.json"
    write_json(evidence_path, evidence, exclusive=True)
    receipt = {"schema": SCHEMA, "kind": "full-gate-receipt", "verdict": "PASS", "run_id": state["run_id"],
               "source": state["source"], "candidate": candidate, "inventory": state["inventory"],
               "completed_at": ended, "evidence_sha256": digest(evidence_path.read_bytes()),
               "null_control_sha256": control["candidate"]["sha256"]}
    path = args.start.parent / "receipt.json"
    write_json(path, receipt, exclusive=True)
    # Only an issued receipt advances durable defaults. The first untrusted full run, any failed
    # run, and a 436-row correctness prefix can never bootstrap this file automatically.
    directory = history_directory(root) / "baselines"
    write_json(directory / "full-null.json", control)
    ledger = "".join(f"{row['verdict']}\t{row['seconds']!r}\t{row['label']}\n" for row in actual)
    directory.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".full-ledger-", dir=directory)
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(ledger)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, directory / "full.tsv")
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return path


def coordinator(root, args):
    state = read_json(args.start) if args.start is not None else {}
    binding_path = args.start.parent / "candidate.json" if args.start is not None else None
    binding = read_json(binding_path) if binding_path is not None and binding_path.exists() else {}
    successful = args.failed == 0 and args.abba_rc == 0 and args.cleanup_rc == 0
    result = {"schema": 1, "run_id": args.run_id, "tier": args.tier,
              "completed": args.cleanup_rc == 0, "verdict": "PASS" if successful else "FAIL",
              "exit_code": 0 if successful else 1, "started_at": state.get("started_at"),
              "finished_at": time.time(), "checks": args.passed + args.failed, "passed": args.passed,
              "failed": args.failed, "skipped": 0, "abba_exit_code": args.abba_rc,
              "nic_checked": bool(args.nic), "candidate_sha256": binding.get("sha256"),
              "cleanup_exit_code": args.cleanup_rc}
    write_json(args.output, result, exclusive=True)
    return args.output


def validate_receipt(root, path, source):
    receipt = read_json(path)
    require(receipt.get("schema") == 1 and receipt.get("kind") == "full-gate-receipt" and
            receipt.get("verdict") == "PASS" and receipt.get("source") == source, "receipt certifies another source tree")
    evidence_path = path.parent / "evidence.json"
    require(digest(evidence_path.read_bytes()) == receipt.get("evidence_sha256"), "receipt evidence changed or disappeared")
    evidence = read_json(evidence_path)
    require(evidence["start"]["source"] == source and evidence["candidate"] == receipt["candidate"] and
            evidence["start"]["inventory"] == receipt["inventory"], "receipt/evidence identity mismatch")
    require(inventory(root, root / receipt["inventory"]["path"]) == receipt["inventory"], "receipt cell inventory is stale")
    state = evidence["start"]
    count = expected_count(root, state["nic"])
    require(state["expected_checks"] == count and len(state["expected_labels"]) == count and
            state["expected_labels"].count(ABBA_LABEL) == 1, "receipt has an incomplete expected row inventory")
    completed = evidence["coordinator"]
    for key, value in {"schema": 1, "completed": True, "verdict": "PASS", "run_id": state["run_id"],
                       "tier": state["tier"], "checks": count, "passed": count, "failed": 0, "skipped": 0,
                       "exit_code": 0, "abba_exit_code": 0, "nic_checked": state["nic"]}.items():
        require(completed.get(key) == value and type(completed.get(key)) is type(value),
                f"receipt coordinator has invalid {key}")
    require(state["tier"] in ("full", "push", "release"), "smoke receipt cannot certify a push")
    actual, observed = evidence["ledger"], evidence["observations"]
    require(Counter(row["label"] for row in actual) == Counter(state["expected_labels"]) and
            all(row["verdict"] == "ok" for row in actual), "receipt has missing or failed ledger rows")
    require(len({row["observation_id"] for row in observed}) == len(observed) and all(
            row["schema"] == 1 and row["run_id"] == state["run_id"] and row["timing"] == "own-row" and
            row["timed_out"] is False and row["verdict"] == "ok" and
            type(row.get("scored")) is bool and (row.get("ledger_label") in state["expected_labels"]
            if row["scored"] else row.get("ledger_label", "missing") is None) and
            state["started_at"] <= row["recorded_at"] <= completed["finished_at"] for row in observed),
            "receipt observations are missing, failed, duplicated, or from another run")
    require(Counter((row["label"], row["seconds"]) for row in actual) ==
            Counter((row["ledger_label"], row["seconds"]) for row in observed if row["scored"]), "receipt row evidence differs")
    binary = receipt["candidate"]
    require(binary["source_sha256"] == source["sha256"] and binary["sha256"] == completed["candidate_sha256"],
            "receipt candidate/source binding differs")
    require(state["started_at"] <= binary["bound_at"] <= completed["finished_at"] <= time.time(),
            "receipt time ordering differs")
    begin_at, environment = validate_abba(evidence["abba"], state, now=completed["finished_at"], candidate=binary)
    null_at, null_environment = validate_abba(evidence["null"], state, now=completed["finished_at"])
    require(evidence["null"]["reference"]["sha256"] == evidence["null"]["candidate"]["sha256"] ==
            receipt["null_control_sha256"], "receipt null arms differ")
    require(0 <= begin_at - null_at <= NULL_MAX_AGE and begin_at + 1 >= binary["bound_at"], "receipt null is stale")
    instrument = lambda env: {k: v for k, v in env.items() if k not in ("port", "permitted_ports", "memtier_path")}
    require(instrument(environment) == instrument(null_environment) and
            evidence["abba"]["window_seconds"] == evidence["null"]["window_seconds"], "receipt null instrument differs")
    require(object_file(root / binary["path"]) == ("100755", binary["sha256"]), "receipt candidate binary changed or is missing")
    return receipt


def verify_refs(root, updates):
    live = source_fingerprint(root)
    directory = history_directory(root)
    receipts = list((directory / "runs").glob("*/receipt.json"))
    checked = []
    for local_ref, oid in updates:
        require(re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", oid), "invalid local object ID")
        if set(oid) == {"0"}:
            continue  # A deletion publishes no source or binary.
        git(root, "rev-parse", "--verify", oid + "^{commit}")  # Annotated release tags peel to commits.
        source = tree_fingerprint(root, oid)
        require(source == live, f"{local_ref}: pushed Git tree differs from the current validated worktree contents/modes")
        failures, valid = [], None
        for path in receipts:
            try:
                valid = validate_receipt(root, path, source)
                break
            except (ValueError, OSError, KeyError, TypeError) as exc:
                failures.append(str(exc))
        require(valid is not None, f"{local_ref}: no successful full gate receipt for this tree/binary; " +
                ("; ".join(sorted(set(failures))) if failures else "run the full push gate"))
        checked.append((local_ref, valid["run_id"]))
    require(source_fingerprint(root) == live, "source changed during pre-push verification")
    return checked


def install(root, *, uninstall=False):
    enabled = subprocess.run(["git", "-C", str(root), "config", "--bool", "extensions.worktreeConfig"],
                             stdout=subprocess.PIPE, check=False).stdout.strip()
    require(enabled == b"true", "extensions.worktreeConfig is not enabled; changing it would affect the common repository. "
            "Use the reversible per-command wrapper: git -c core.hooksPath=.githooks push")
    directory = history_directory(root)
    saved = directory / "hook-config.json"
    current = subprocess.run(["git", "-C", str(root), "config", "--worktree", "--get", "core.hooksPath"],
                             stdout=subprocess.PIPE, check=False)
    value = current.stdout.decode().rstrip("\n") if current.returncode == 0 else None
    target = str(root / ".githooks")
    if uninstall:
        prior = read_json(saved)
        require(value == prior["installed"], "worktree hook configuration changed since installation")
        if prior["previous"] is None:
            git(root, "config", "--worktree", "--unset-all", "core.hooksPath")
        else:
            git(root, "config", "--worktree", "core.hooksPath", prior["previous"])
        saved.unlink()
    else:
        require(not saved.exists(), "hook installation is already recorded; use --uninstall to restore it")
        require((root / ".githooks/pre-push").is_file(), "pre-push hook missing")
        write_json(saved, {"installed": target, "previous": value}, exclusive=True)
        git(root, "config", "--worktree", "core.hooksPath", target)


def self_test():
    import copy
    import shutil
    import unittest
    from unittest import mock

    def fixture_cells():
        rows = []
        def add(mode, rl, ov, ro, op, depth, conns=512, atomic=1, mix="-"):
            score = "p999" if op == "REORDER" else "latency" if depth == 1 else "rate"
            rows.append(f"v{len(rows):03} | {mode} | rl={rl} | ov={ov} | ro={ro} | {op} | p{depth} | {conns} | - | - | "
                        f"{'-' if depth == 1 else '1'} | atomic={atomic} | score={score} | mix={mix} | smoke=0")
        for mode, rl, ov, ro in product(("1s", "2s"), (0, 1), (0, 1), (0, 1)):
            for op, depth in product(("GET", "SET"), (1, 32)):
                add(mode, rl, ov, ro, op, depth)
            for op, depth in product(("MGET", "MSET"), (1, 8, 32)):
                add(mode, rl, ov, ro, op, depth)
        for mode in ("1s", "2s"):
            for rl in (0, 1):
                add(mode, rl, 1, 1, "MIX", 1, mix="7:1")
                add(mode, rl, 1, 1, "GET", 32, conns=2048)
            add(mode, 1, 1, 1, "MIX8", 128, mix="18:14")
            for op in ("MGET", "MSET"):
                add(mode, 1, 1, 1, op, 8, atomic=0)
            for ro in (0, 1):
                add(mode, 0, 1, ro, "REORDER", 8, mix="95:5")
        return "\n".join(rows) + "\n"

    class Controls(unittest.TestCase):
        def setUp(self):
            (ROOT / "build").mkdir(exist_ok=True)
            self.temp = tempfile.TemporaryDirectory(prefix="receipt-self-test-", dir=ROOT / "build")
            self.addCleanup(self.temp.cleanup)
            self.root = Path(self.temp.name) / "repo"
            self.root.mkdir()
            git(self.root, "init", "-q")
            git(self.root, "config", "user.name", "Receipt self-test")
            git(self.root, "config", "user.email", "receipt-self-test@invalid")
            (self.root / "tests").mkdir()
            (self.root / ".githooks").mkdir()
            (self.root / "build").mkdir()
            (self.root / ".gitignore").write_text("/build/\n/.gate-history/\n__pycache__/\n")
            (self.root / "tests/gate.sh").write_text("EXPECT_FULL=437\n")
            (self.root / "tests/headline_cells.txt").write_text(fixture_cells())
            shutil.copyfile(__file__, self.root / "tests/gate_receipt.py")
            shutil.copyfile(ROOT / ".githooks/pre-push", self.root / ".githooks/pre-push")
            (self.root / ".githooks/pre-push").chmod(0o755)
            (self.root / "source.cc").write_text("int original;\n")
            (self.root / "alias").symlink_to("source.cc")
            git(self.root, "add", "-A")
            git(self.root, "commit", "-qm", "initial source")
            self.old_oid = git(self.root, "rev-parse", "HEAD").decode().strip()
            # Validate changed, not-yet-committed source and a new source file. The receipt must
            # survive adding/committing their exact bytes later, but cannot approve the old HEAD.
            (self.root / "source.cc").write_text("int candidate;\n")
            (self.root / "new.h").write_text("int new_source;\n")
            self.candidate = self.root / "build/candidate"
            self.candidate.write_bytes(b"synthetic executable bytes, never run\n")
            self.candidate.chmod(0o755)
            self.ledger = self.root / "build/ledger.tsv"
            # Distinct real gate loops can publish the same AOF label. Every occurrence needs
            # its own observation; a set of labels would quietly erase one of these first rows.
            self.ledger.write_text("".join(f"ok\t1.0\trow {0 if i == 1 else i}\n" for i in range(436)) +
                                   f"ok\t1.0\t{ABBA_LABEL}\n")
            self.args = argparse.Namespace(run_id="fixture", tier="push", expected_ledger=self.ledger,
                                           cells=self.root / "tests/headline_cells.txt", nic=False)
            self.start_time = float(int(time.time()) - 40000)
            with mock.patch.object(time, "time", return_value=self.start_time):
                self.start = begin(self.root, self.args)
            with mock.patch.object(time, "time", return_value=self.start_time + 1):
                bind(self.root, argparse.Namespace(start=self.start, candidate=self.candidate))
            self.state = read_json(self.start)
            self.binding = read_json(self.start.parent / "candidate.json")
            self.report = self.make_report(self.start_time + 100, self.binding["sha256"], "b" * 64)
            self.control = self.make_report(self.start_time - 18000, "a" * 64, "a" * 64)
            ended = self.start_time + 16200
            self.completed = dict(schema=1, run_id="fixture", tier="push", completed=True, verdict="PASS",
                                  exit_code=0, started_at=self.start_time, finished_at=ended, checks=437,
                                  passed=437, failed=0, skipped=0, abba_exit_code=0, nic_checked=False,
                                  candidate_sha256=self.binding["sha256"])
            self.observed = [dict(schema=1, timing="own-row", run_id="fixture", observation_id=str(i),
                                 label=row["label"], seconds=1., verdict="ok", timed_out=False,
                                 scored=True, ledger_label=row["label"],
                                 recorded_at=self.start_time + 10 + i) for i, row in
                             enumerate(ledger_rows(self.ledger, passing=True))]
            self.finish_args = argparse.Namespace(start=self.start, ledger=self.ledger,
                observations=self.root / "build/observations.jsonl", gate_result=self.root / "build/gate.json",
                abba_result=self.root / "build/abba.json", null_result=self.root / "build/null.json")
            self.save_results()

        def make_report(self, started, candidate, reference):
            rows = []
            for cell in self.state["inventory"]["cells"]:
                runs = [dict(arm=arm, complete=True, artifacts=f"{cell['id']}/n1-{i}-{arm}", pid=100 + i,
                             rate=100., latency_ms=1., p999_ms=2., long_p999_ms=3., commands=2000,
                             window_seconds=20., busy_pct=99.) for i, arm in enumerate(ORDER, 1)]
                rows.append(dict(cell=cell, verdict="PASS", rounds=[dict(instances=1, runs=runs)],
                                 assessment=dict(verdict="PASS", reasons=[], loss_pct=-.1, threshold_pct=.1,
                                                 saturation_exempt=cell["depth"] == 1)))
            return dict(schema=1, verdict="PASS", subset="full", measurement_valid=True, order=ORDER,
                started_utc=datetime.fromtimestamp(started, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                elapsed_seconds=16000., window_seconds=20, receipt_harness_sha256=self.state["harness"]["sha256"],
                cell_source=dict(sha256=self.state["inventory"]["sha256"], total_cells=178,
                                 text=(self.root / "tests/headline_cells.txt").read_text()),
                coverage=dict(ids=[c["id"] for c in self.state["inventory"]["cells"]], count=178, pending_pins=[]),
                candidate=dict(sha256=candidate), reference=dict(sha256=reference),
                environment=dict(server_cpus=[0, 1], load_cpus=[2, 3], server_physical=[0, 1],
                                 load_physical=[2, 3], uname=["fixture"], memtier_sha256="d" * 64,
                                 memtier_version="fixture", keys=2000000, data_bytes=64, key_pattern="P:P", split_ratio="1:1"),
                quiet_box=dict(complete=True, interference=None, started_at=started + 1, finished_at=started + 15999,
                               sample_interval_seconds=1, samples=15998, cpus=[0, 1, 2, 3], requested_cpus=[0, 1, 2, 3]),
                cells=rows)

        def save_results(self):
            write_json(self.finish_args.gate_result, self.completed)
            write_json(self.finish_args.abba_result, self.report)
            write_json(self.finish_args.null_result, self.control)
            self.finish_args.observations.write_text("".join(json.dumps(row) + "\n" for row in self.observed))

        def certify_and_commit(self):
            self.receipt = finish(self.root, self.finish_args)
            with self.assertRaisesRegex(ValueError, "pushed Git tree differs"):
                verify_refs(self.root, [("old", self.old_oid)])
            git(self.root, "add", "source.cc", "new.h")
            git(self.root, "commit", "-qm", "commit exact validated contents")
            self.oid = git(self.root, "rev-parse", "HEAD").decode().strip()
            return self.receipt

        def test_actual_git_refs_hook_and_commit_after_validation(self):
            self.certify_and_commit()
            git(self.root, "update-ref", "refs/heads/certified", self.oid)
            git(self.root, "tag", "-a", "release-fixture", "-m", "same validated tree")
            tag = git(self.root, "rev-parse", "refs/tags/release-fixture").decode().strip()
            self.assertEqual(len(verify_refs(self.root, [("certified", self.oid), ("release-fixture", tag)])), 2)
            stream = f"refs/heads/certified {self.oid} refs/heads/certified {'0' * 40}\n"
            proc = subprocess.run([str(self.root / ".githooks/pre-push"), "fixture", "unused"], cwd=self.root,
                                  input=stream, text=True, capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("certified by full run fixture", proc.stdout)
            with self.assertRaisesRegex(ValueError, "pushed Git tree differs"):
                verify_refs(self.root, [("old-ref", self.old_oid)])

        def test_missing_rows_cannot_pass_by_count_or_exit_zero(self):
            rows = self.ledger.read_text().splitlines(True)
            self.ledger.write_text("".join(rows[:-1]))
            with self.assertRaisesRegex(ValueError, "missing, duplicated"):
                finish(self.root, self.finish_args)
            self.ledger.write_text("".join(rows))
            self.observed.pop(1)  # The duplicate label is still present; its second execution is not.
            self.save_results()
            with self.assertRaisesRegex(ValueError, "independent own-row"):
                finish(self.root, self.finish_args)

        def test_explicit_unscored_dependencies_and_context_do_not_change_counts(self):
            self.observed[0]["label"] += " [external binary: context]"
            self.observed.append(dict(schema=1, timing="own-row", run_id="fixture", observation_id="dependency",
                label="unscored build dependency: core.o", ledger_label=None, scored=False, seconds=2.,
                verdict="ok", timed_out=False, recorded_at=self.start_time + 20))
            self.save_results()
            self.certify_and_commit()
            self.assertEqual(len(verify_refs(self.root, [("HEAD", self.oid)])), 1)

        def test_missing_baseline_retains_start_binding_and_completed_work_without_receipt(self):
            self.args.run_id = "push:bootstrap"
            self.args.expected_ledger = self.root / "build/missing-baseline"
            self.args.allow_missing_baseline = True
            start = begin(self.root, self.args)
            state = read_json(start)
            self.assertIsNone(state["baseline"])
            self.assertIn("missing/invalid trusted baseline", state["withheld_reason"])
            bind(self.root, argparse.Namespace(start=start, candidate=self.candidate))
            args = copy.copy(self.finish_args)
            args.start = start
            with self.assertRaisesRegex(ValueError, "rows are not automatically trusted"):
                finish(self.root, args)
            self.assertFalse((start.parent / "receipt.json").exists())
            self.assertFalse((history_directory(self.root) / "baselines/full.tsv").exists())

        def test_actual_shell_receipt_blocks_do_not_short_circuit_work_or_certify_iteration(self):
            # Execute the gate's real begin/release-binding/footer blocks. Only workload bodies
            # are fake: no make/server/benchmark runs. A missing baseline must still reach both
            # workload markers, bind before them, then exit red with no receipt. Iteration reaches
            # those same markers and invokes no receipt stages even when its counters are green.
            gate = (ROOT / "tests/gate.sh").read_text()
            start_block = gate[gate.index("RECEIPT_REQUIRED=0;"):gate.index('ROW_PLAN="$RUN_DIR/row-timeouts.json"')]
            release = gate[gate.index("job_release(){"):gate.index("\njob_asan(){")]
            final = gate[gate.index("GATE_CLEANUP_RC=0\n"):]
            for tier, built, expected in (("push", 1, 1), ("iteration", 1, 0),
                                           ("push", 0, 1), ("iteration", 0, 0)):
                with self.subTest(tier=tier, built=built):
                    run_id = f"{tier}:fragment-{built}"
                    run = self.root / "build" / ("fragment-" + tier + str(built))
                    run.mkdir()
                    script = '''set -u
RUN_DIR=$TEST_RUN; TMPDIR=$TEST_RUN; ROW_RUN_ID=$TEST_RUN_ID
GATE_STARTED=$SECONDS; GATE_RECEIPT_BASELINE="$PWD/build/missing"
if [ "$TEST_BUILT" = 0 ]; then GATE_RECEIPT_BASELINE="$PWD/build/ledger.tsv"; fi
BUILD_CANDIDATE=$TEST_BUILT; BUILD_CORES=0; BUILD_JOBS=1; CANDIDATE_BINARY="$PWD/build/candidate"
LEDGER="$RUN_DIR/ledger"; TIMINGS="$RUN_DIR/timings"
PASS=0; FAIL=0
row_begin(){ :; }; pausable(){ :; }; ok(){ PASS=$((PASS+1)); }; bad(){ FAIL=$((FAIL+1)); }
cleanup(){ :; }
''' + start_block + release + '''
job_release
if [ "$RECEIPT_REQUIRED" = 1 ]; then
  if [ "$BUILD_CANDIDATE" = 1 ]; then test -f "${RECEIPT_START%/*}/candidate.json" || exit 99
  else test ! -f "${RECEIPT_START%/*}/candidate.json" || exit 98; fi
fi
printf 'correctness\\nabba\\n' > "$RUN_DIR/reached"
PASS=437; FAIL=0; ABBA_RC=0; NIC_CHECKED=0; ROW_HISTORY="$PWD/build"
ABBA_OUTPUT="$PWD/build/abba"; LEDGER="$PWD/build/ledger.tsv"
''' + final
                    process = subprocess.run(["bash"], cwd=self.root, input=script, text=True, capture_output=True,
                                             env={**os.environ, "TEST_RUN": str(run), "GATE_PURPOSE": tier,
                                                  "TEST_RUN_ID": run_id, "TEST_BUILT": str(built)})
                    self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
                    self.assertEqual((run / "reached").read_text(), "correctness\nabba\n")
                    self.assertEqual((run / "gate-result.json").exists(), tier == "push")
                    if tier == "push":
                        if built:
                            self.assertIn("rows are not automatically trusted", process.stderr)
                        else:
                            self.assertIn("external bytes cannot certify this source tree", process.stderr)
                    self.assertFalse((history_directory(self.root) / "runs" / run_id / "receipt.json").exists())

        def test_actual_begin_reads_explicit_previous_ledger_before_rotation(self):
            gate = (ROOT / "tests/gate.sh").read_text()
            block = gate[gate.index("LEDGER=${GATE_LEDGER:"):gate.index('ROW_PLAN="$RUN_DIR/row-timeouts.json"')]
            run = self.root / "build" / "explicit-baseline"
            run.mkdir()
            previous = self.ledger.read_bytes()
            script = 'set -u\nRUN_DIR=$TEST_RUN\n' + block + '\nprintf "%s\\n" "$RECEIPT_START"\n'
            process = subprocess.run(["bash"], cwd=self.root, input=script, text=True, capture_output=True,
                env={**os.environ, "TEST_RUN": str(run), "GATE_PURPOSE": "push",
                     "GATE_LEDGER": str(self.ledger), "GATE_RECEIPT_BASELINE": str(self.ledger)})
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            state = read_json(Path(process.stdout.strip()))
            self.assertEqual(state["baseline"]["sha256"], digest(previous))
            self.assertEqual(Counter(state["expected_labels"]), Counter(self.state["expected_labels"]))
            self.assertIsNone(state["withheld_reason"])
            self.assertEqual(self.ledger.read_bytes(), b"")
            self.assertEqual(Path(str(self.ledger) + ".prev").read_bytes(), previous)

        def test_ignored_generated_outputs_preserve_fingerprint_but_tracked_ignored_files_count(self):
            for relative in ("build/obj/source.o", ".gate-history/rows/index.sqlite",
                             "tests/__pycache__/helper.pyc"):
                path = self.root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"generated after gate start")
            self.assertEqual(source_fingerprint(self.root), self.state["source"])
            self.certify_and_commit()
            self.assertEqual(len(verify_refs(self.root, [("HEAD", self.oid)])), 1)
            tracked = self.root / "build/forced-tracked-header.h"
            tracked.write_text("tracked despite ignore\n")
            git(self.root, "add", "-f", "build/forced-tracked-header.h")
            fingerprint = source_fingerprint(self.root)
            self.assertTrue(any(row["path"] == "build/forced-tracked-header.h" for row in fingerprint["entries"]))
            tracked.write_text("tracked bytes changed\n")
            self.assertNotEqual(source_fingerprint(self.root), fingerprint)

        def test_actual_history_recorder_matches_context_and_unscored_prerequisites(self):
            import gate_history
            directory = self.root / "build" / "real-history"
            with mock.patch.object(time, "time", return_value=self.start_time + 500):
                for index, row in enumerate(ledger_rows(self.ledger, passing=True)):
                    gate_history.record(directory, run_id=self.args.run_id,
                        label=row["label"] + " [geometry context]", ledger_label=row["label"],
                        seconds=row["seconds"], verdict="ok", timed_out=False, observation_id=f"actual-{index}")
                gate_history.record(directory, run_id=self.args.run_id, label="unscored build dependency: test.o",
                    seconds=2., verdict="ok", timed_out=False, observation_id="actual-dependency", scored=False)
            self.finish_args.observations = directory / gate_history.HISTORY_FILE
            receipt = finish(self.root, self.finish_args)
            retained = read_json(receipt.parent / "evidence.json")["observations"]
            self.assertEqual(sum(row["scored"] for row in retained), 437)
            self.assertEqual(len(retained), 438)

        def test_future_full_row_count_needs_new_baseline_and_actual_observation(self):
            # Only this disposable Git fixture changes EXPECT. The repository's maintainer-owned
            # constants are never edited; the certificate must follow a future intentional raise.
            (self.root / "tests/gate.sh").write_text("EXPECT_FULL=438\n")
            self.args.run_id = "larger-full-run"
            with self.assertRaisesRegex(ValueError, "exactly 438 rows"):
                begin(self.root, self.args)
            self.ledger.write_text(self.ledger.read_text() + "ok\t2.0\tnew required row\n")
            with mock.patch.object(time, "time", return_value=self.start_time):
                self.start = begin(self.root, self.args)
            with mock.patch.object(time, "time", return_value=self.start_time + 1):
                bind(self.root, argparse.Namespace(start=self.start, candidate=self.candidate))
            self.state = read_json(self.start)
            self.binding = read_json(self.start.parent / "candidate.json")
            self.assertEqual(self.state["expected_checks"], 438)
            self.finish_args.start = self.start
            self.completed["run_id"] = self.args.run_id
            self.report = self.make_report(self.start_time + 100, self.binding["sha256"], "b" * 64)
            self.control = self.make_report(self.start_time - 18000, "a" * 64, "a" * 64)
            for row in self.observed:
                row["run_id"] = self.args.run_id
            self.save_results()
            with self.assertRaisesRegex(ValueError, "incorrect gate completion checks"):
                finish(self.root, self.finish_args)
            self.completed.update(checks=438, passed=438)
            self.save_results()
            with self.assertRaisesRegex(ValueError, "independent own-row observations"):
                finish(self.root, self.finish_args)
            self.observed.append(dict(schema=1, timing="own-row", run_id=self.args.run_id,
                observation_id="extra", label="new required row", ledger_label="new required row", scored=True,
                seconds=2., verdict="ok", timed_out=False, recorded_at=self.start_time + 100))
            self.save_results()
            self.assertTrue(finish(self.root, self.finish_args).is_file())

        def test_smoke_partial_failed_unreached_quiet_and_null_controls(self):
            self.args.tier = "smoke"
            with self.assertRaisesRegex(ValueError, "smoke/iteration"):
                begin(self.root, self.args)
            cases = [
                ("smoke", lambda: self.report.update(subset="smoke")),
                ("partial", lambda: self.report.update(verdict="PARTIAL")),
                ("cell failure", lambda: self.report["cells"][0].update(verdict="FAIL")),
                ("missing measurement", lambda: self.report["cells"][0]["rounds"][0]["runs"].pop()),
                ("quiet incomplete", lambda: self.report["quiet_box"].update(complete=False)),
                ("quiet failure", lambda: self.report["quiet_box"].update(interference={"pid": 123})),
                ("null bytes", lambda: self.control["candidate"].update(sha256="c" * 64)),
                ("null harness", lambda: self.control.update(receipt_harness_sha256="c" * 64)),
                ("null geometry", lambda: self.control["environment"].update(data_bytes=32)),
                ("null window", lambda: self.control.update(window_seconds=10)),
                ("candidate identity", lambda: self.report["candidate"].update(sha256="c" * 64)),
                ("future null", lambda: self.control.update(started_utc="2999-01-01T00:00:00Z")),
            ]
            baseline_report, baseline_control = copy.deepcopy(self.report), copy.deepcopy(self.control)
            for name, change in cases:
                with self.subTest(name=name):
                    self.report, self.control = copy.deepcopy(baseline_report), copy.deepcopy(baseline_control)
                    change()
                    self.save_results()
                    with self.assertRaises(ValueError):
                        finish(self.root, self.finish_args)
            self.report, self.control = baseline_report, self.make_report(self.start_time - 100000, "a" * 64, "a" * 64)
            self.save_results()
            with self.assertRaisesRegex(ValueError, "24 hours old"):
                finish(self.root, self.finish_args)

        def test_start_end_and_push_source_binary_mode_and_symlink_identity(self):
            source = self.root / "source.cc"
            original = source.read_bytes()
            source.write_bytes(original + b"changed\n")
            with self.assertRaisesRegex(ValueError, "source contents/modes changed"):
                finish(self.root, self.finish_args)
            source.write_bytes(original)
            self.certify_and_commit()
            for kind in ("content", "executable", "new untracked source", "symlink", "binary"):
                with self.subTest(kind=kind):
                    if kind == "content":
                        source.write_bytes(original + b"changed\n")
                    elif kind == "executable":
                        source.chmod(0o755)
                    elif kind == "new untracked source":
                        (self.root / "another.cc").write_text("another source")
                    elif kind == "symlink":
                        (self.root / "alias").unlink()
                        (self.root / "alias").symlink_to("new.h")
                    else:
                        self.candidate.write_bytes(b"another executable")
                    with self.assertRaises(ValueError):
                        verify_refs(self.root, [("HEAD", self.oid)])
                    source.write_bytes(original)
                    source.chmod(0o644)
                    (self.root / "another.cc").unlink(missing_ok=True)
                    (self.root / "alias").unlink()
                    (self.root / "alias").symlink_to("source.cc")

        def test_full_inventory_cannot_retire_a_multikey_geometry(self):
            cells = self.root / "tests/headline_cells.txt"
            rows = cells.read_text().splitlines(True)
            index = next(i for i, row in enumerate(rows) if "MGET" in row)
            rows[index] = rows[index].replace("MGET", "GET")
            cells.write_text("".join(rows))
            with self.assertRaisesRegex(ValueError, "retired"):
                inventory(self.root, cells)

        def test_worktree_hook_config_is_reversible_without_common_changes(self):
            # Only temporary repositories are configured; --install is never invoked on ROOT.
            common = self.root / ".git/config"
            prior = common.read_bytes()
            with self.assertRaisesRegex(ValueError, "per-command wrapper"):
                install(self.root)
            self.assertEqual(common.read_bytes(), prior)
            git(self.root, "config", "extensions.worktreeConfig", "true")
            linked = Path(self.temp.name) / "linked"
            git(self.root, "worktree", "add", "--detach", str(linked), "HEAD")
            try:
                prior = common.read_bytes()
                install(linked)
                self.assertEqual(common.read_bytes(), prior)
                self.assertEqual(git(linked, "config", "--worktree", "core.hooksPath").decode().strip(), str(linked / ".githooks"))
                install(linked, uninstall=True)
                self.assertEqual(common.read_bytes(), prior)
            finally:
                git(self.root, "worktree", "remove", "--force", str(linked))

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls))
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--uninstall", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="action")
    start = sub.add_parser("begin")
    start.add_argument("--run-id", required=True)
    start.add_argument("--tier", choices=("full", "push", "release", "iteration", "smoke"), required=True)
    start.add_argument("--expected-ledger", type=Path, required=True)
    start.add_argument("--cells", type=Path, default=ROOT / "tests/headline_cells.txt")
    start.add_argument("--nic", action="store_true")
    start.add_argument("--nic-auto", action="store_true", help="infer optional NIC inventory from the trusted baseline")
    start.add_argument("--allow-missing-baseline", action="store_true",
                       help="record a withheld bootstrap attempt so the entire authorized gate can still run")
    binding = sub.add_parser("bind")
    binding.add_argument("--start", type=Path, required=True)
    binding.add_argument("--candidate", type=Path, required=True)
    final = sub.add_parser("finish")
    for name in ("start", "gate-result", "ledger", "observations", "abba-result", "null-result"):
        final.add_argument("--" + name, type=Path, required=True)
    completed = sub.add_parser("coordinator")
    completed.add_argument("--start", type=Path)
    completed.add_argument("--output", type=Path, required=True)
    completed.add_argument("--run-id", required=True)
    completed.add_argument("--tier", choices=("full", "push", "release"), required=True)
    for name in ("passed", "failed", "abba-rc", "cleanup-rc"):
        completed.add_argument("--" + name, type=int, required=True)
    completed.add_argument("--nic", type=int, choices=(0, 1), required=True)
    sub.add_parser("fingerprint").add_argument("--harness", action="store_true")
    sub.add_parser("verify").add_argument("--ref", action="append", default=[])
    hook = sub.add_parser("pre-push")
    hook.add_argument("remote", nargs="?")
    hook.add_argument("url", nargs="?")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.install or args.uninstall:
        require(not (args.install and args.uninstall), "choose install or uninstall")
        install(ROOT, uninstall=args.uninstall)
    elif args.action in ("begin", "bind", "finish", "coordinator"):
        print(globals()[args.action](ROOT, args))
    elif args.action == "fingerprint":
        print(json.dumps(harness_fingerprint(ROOT) if args.harness else source_fingerprint(ROOT), sort_keys=True))
    elif args.action in ("pre-push", "verify"):
        if args.action == "verify":
            updates = [(ref, git(ROOT, "rev-parse", "--verify", ref).decode().strip()) for ref in (args.ref or ["HEAD"])]
        else:
            updates = []
            for line in sys.stdin:
                fields = line.split()
                require(len(fields) == 4, "invalid pre-push ref update")
                updates.append((fields[0], fields[1]))
        for ref, run in verify_refs(ROOT, updates):
            print(f"GATE RECEIPT: {ref} certified by full run {run}")
    else:
        parser.error("choose an action")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"GATE RECEIPT REFUSED: {error}", file=sys.stderr)
        raise SystemExit(1)
