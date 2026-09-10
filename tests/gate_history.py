#!/usr/bin/env python3
"""Own-row timing history for gate.sh; legacy verdict intervals are never exact timings.

The runner supplies an explicit start/end duration, excluding queue/collection waits. A
plan is frozen before the run, so parallel rows cannot change one another's deadline.
Verdicts are retained as evidence, but only successful, non-expired observations size
deadlines: a hang must not teach the next gate to wait longer for the same hang.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import sqlite3
import statistics
import sys
import tempfile
import time
import unittest
import uuid


SCHEMA = 1
HISTORY_FILE = "row-observations.jsonl"
INDEX_FILE = "row-observations.index.sqlite3"
INDEX_CHECKPOINT = "row-observations.index.json"
INDEX_SCHEMA = 1
# Recorded 2026-09-10, cx-gatefast/build/gate-run.*/jobs/*/family.tsv: completed
# one-row families have differential medians 358.31/357.98 s (max/median 1.003),
# lruclock median 155.25 s (126.44..200.14, max/median 1.289), and flipctl median
# 118.16 s (115.17..120.15, 1.017). Four medians leaves substantial headroom over
# the slowest observed spread. The 30 s floor protects tiny rows from scheduling
# variance. 900 s without exact history is over twice the slowest recorded battery.
# Older /tmp/claude-1000/{ga,gateart}/ ledgers used verdict-to-verdict intervals;
# lruclock's unchanged label spans 6.1..256.1 s and warm/cold release builds span
# 0.1..48.6 s. Never treat those intervals as exact samples, and retain twice the
# successful maximum as a guard against multimodal build/clock timings.
DEFAULT_MULTIPLIER = 4.0
DEFAULT_FLOOR = 30.0
DEFAULT_FALLBACK = 900.0

# These rows can relink from cache in a second and compile from scratch after a header
# change. Fast history alone says nothing about the first cold run (parbuild's recorded
# cold cost is 58 s). Keep the established 900 s fallback until cache state is an explicit
# timing context. Always-fresh standalone compiler rows do not need this exception.
CACHED_BUILD_ROWS = frozenset(("release build (+footprint locks)", "ASAN build",
    "read-local ownership-invariant build", "atomic survivors unit build",
    "netcmd regression build", "waits config publication + admission unit",
    "reads never wait for retirement quiescence", "storage flags regression",
    "storage deadline-sidecar regression"))


def protect_mechanism_budget(label: str, row: dict) -> dict:
    row = dict(row)
    minimum, reason = 0, ""
    if "lruclock" in label:
        # This battery waits for the next production bucket, so a healthy 6 s row can
        # need a full 256 s on the next run. Twice the bucket guards the unobserved phase.
        minimum, reason = 512, "two production 256s clock buckets"
    elif label in CACHED_BUILD_ROWS or label.startswith("build dependency: "):
        minimum, reason = DEFAULT_FALLBACK, "cold cache state not distinguished"
    if minimum:
        row["timeout_seconds"] = max(row["timeout_seconds"], minimum)
        row["mechanism_floor_seconds"] = minimum
        row["basis"] += f"; mechanism-floor={minimum:g}s ({reason})"
    return row


def dependency_label(argv: list[str]) -> str:
    """Stable build output/target identity, independent of source lines or worker slots."""
    for index, argument in enumerate(argv):
        if Path(argument).name == "parbuild.sh" and index + 1 < len(argv):
            return canonical_label("build dependency: " + Path(argv[index + 1]).name)
        if Path(argument).name == "make":
            targets = sorted(value for value in argv[index + 1:] if value.startswith("build/"))
            return canonical_label("build dependency: make " + (",".join(targets) or "default"))
    raise ValueError("hidden build has no recognized make target or parbuild output")


def abba_context(argv: list[str]) -> str:
    # Parse the actual measurement argv; importing the module and its parser starts no
    # server. Include the source digest so adding cells cannot inherit a short old matrix.
    import abbagate
    previous = sys.argv
    try:
        sys.argv = ["abbagate.py", *argv]
        args = abbagate.parse_args()
    finally:
        sys.argv = previous
    context = {"subset": getattr(args, "subset", "full"),
               "window": getattr(args, "window", abbagate.WINDOW),
               "cells-sha256": hashlib.sha256(Path(args.cells).read_bytes()).hexdigest()}
    context.update({axis: getattr(args, axis) for axis in
                    ("server_cores", "server_smt", "load_cores", "load_smt")})
    context.update(only=args.only, escalate=int(args.escalate), max_instances=args.max_instances)
    return "; ".join(f"{key}={value}" for key, value in context.items())


def canonical_label(label: str) -> str:
    """The same limited normalization as gate.sh; knob geometry stays significant."""
    if not isinstance(label, str) or not label or any(c in label for c in "\t\r\n\0"):
        raise ValueError("row label must be nonempty and contain no tab/newline/NUL")
    label = re.sub(r"(direct|hits|records|skipped|suppressed|zc_sends)=[0-9]+", r"\1=N", label)
    label = re.sub(r"(dispatched==executed) \([0-9]+\)", r"\1 (N)", label)
    label = re.sub(r"(atomic MGET/MSET floor) \([0-9]+/s", r"\1 (N/s", label)
    return re.sub(r"(TLS connection slots all freed) \([0-9]+/[0-9]+\)",
                  r"\1 (N/N)", label)


def finite_number(value: object, name: str, *, positive: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be a number")
    result = float(value)
    if not math.isfinite(result) or result < 0 or (positive and result == 0):
        raise ValueError(f"{name} must be finite and {'positive' if positive else 'nonnegative'}")
    return result


def validate_observation(row: object) -> dict:
    if not isinstance(row, dict) or row.get("schema") != SCHEMA:
        raise ValueError("unsupported/missing observation schema")
    if row.get("timing") != "own-row":
        raise ValueError("exact history requires timing='own-row'")
    label = canonical_label(row.get("label"))
    if label != row["label"]:
        raise ValueError("stored row identity is not canonical")
    for key in ("run_id", "observation_id"):
        if not isinstance(row.get(key), str) or not row[key] or any(c in row[key] for c in "\t\r\n\0"):
            raise ValueError(f"invalid {key}")
    finite_number(row.get("seconds"), "seconds")
    finite_number(row.get("recorded_at"), "recorded_at", positive=True)
    if row.get("verdict") not in ("ok", "FAIL") or type(row.get("timed_out")) is not bool:
        raise ValueError("invalid verdict/timed_out")
    if row["timed_out"] and row["verdict"] != "FAIL":
        raise ValueError("an expired row cannot pass")
    # Old observations remain readable; newly appended evidence always states whether it
    # produced a ledger row. Receipts can then compare exact multisets without stripping
    # timeout contexts heuristically or mistaking hidden build timings for gate coverage.
    if "scored" in row or "ledger_label" in row:
        if type(row.get("scored")) is not bool:
            raise ValueError("invalid scored marker")
        if row["scored"]:
            if canonical_label(row.get("ledger_label")) != row["ledger_label"]:
                raise ValueError("stored ledger identity is not canonical")
        elif row.get("ledger_label") is not None:
            raise ValueError("unscored observation cannot name a ledger row")
    return row


def decode_history(data: str, source: Path) -> list[dict]:
    if data and not data.endswith("\n"):
        raise ValueError(f"{source}: incomplete final record")
    rows = []
    seen = set()
    for number, line in enumerate(data.splitlines(), 1):
        try:
            row = validate_observation(json.loads(line))
            if row["observation_id"] in seen:
                raise ValueError("duplicate observation_id")
            seen.add(row["observation_id"])
            rows.append(row)
        except (ValueError, TypeError) as exc:
            raise ValueError(f"{source}:{number}: {exc}") from exc
    return rows


def read_history(directory: Path) -> list[dict]:
    path = directory / HISTORY_FILE
    if not path.exists():
        return []
    with path.open(encoding="utf-8") as stream:
        fcntl.flock(stream, fcntl.LOCK_SH)
        return decode_history(stream.read(), path)


def file_identity(info: os.stat_result) -> dict[str, int]:
    # ctime catches an in-place edit even when a caller restores the old mtime; inode/device
    # catch replacement. These are cache invalidation evidence, never substitutes for validating
    # an unfamiliar JSONL file. Readers/prepare always decode every authoritative observation.
    return {name: getattr(info, "st_" + name) for name in
            ("dev", "ino", "size", "mtime_ns", "ctime_ns")}


def fsync_directory(directory: Path) -> None:
    descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def publish_index_checkpoint(directory: Path, stream) -> None:
    atomic_json(directory / INDEX_CHECKPOINT, {"schema": INDEX_SCHEMA,
        "history": file_identity(os.fstat(stream.fileno())),
        "index": file_identity((directory / INDEX_FILE).stat())})
    fsync_directory(directory)


def connect_index(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    # A single-file DELETE journal keeps the checkpoint's index identity meaningful. Writers
    # already serialize on the authoritative JSONL flock; WAL would add another publication log.
    try:
        if connection.execute("PRAGMA journal_mode").fetchone()[0] != "delete":
            raise ValueError("unsupported history index journal mode")
        connection.execute("PRAGMA synchronous=FULL")
        return connection
    except BaseException:
        connection.close()
        raise


def index_key(observation_id: str) -> str:
    # JSON permits escaped surrogates. The cache must preserve every ID accepted by the
    # authoritative schema rather than narrow it to SQLite's UTF-8 TEXT binding grammar.
    # The ASCII JSON representation also equates surrogate pairs with their decoded scalar.
    return json.dumps(observation_id, ensure_ascii=True)


def rebuild_index(directory: Path, stream, path: Path) -> sqlite3.Connection:
    stream.seek(0)
    rows = decode_history(stream.read(), path) # Corrupt history is never repaired or discarded.
    source = file_identity(os.fstat(stream.fileno()))
    with tempfile.NamedTemporaryFile(dir=directory, prefix="history-index.", delete=False) as temporary:
        temporary_path = Path(temporary.name)
    connection = None
    try:
        connection = connect_index(temporary_path)
        connection.executescript("""
            CREATE TABLE observation_ids (id TEXT PRIMARY KEY) WITHOUT ROWID;
            CREATE TABLE checkpoint (singleton INTEGER PRIMARY KEY CHECK(singleton=1), source TEXT NOT NULL);
        """)
        connection.executemany("INSERT INTO observation_ids VALUES (?)",
                               ((index_key(row["observation_id"]),) for row in rows))
        connection.execute("INSERT INTO checkpoint VALUES (1, ?)", (json.dumps(source, sort_keys=True),))
        connection.commit()
        connection.close()
        connection = None
        # Derived cache only: a stale SQLite journal must not be replayed onto the replacement.
        for suffix in ("-journal", "-wal", "-shm"):
            Path(str(directory / INDEX_FILE) + suffix).unlink(missing_ok=True)
        os.replace(temporary_path, directory / INDEX_FILE)
        publish_index_checkpoint(directory, stream)
        return connect_index(directory / INDEX_FILE)
    finally:
        if connection is not None:
            connection.close()
        temporary_path.unlink(missing_ok=True)
        Path(str(temporary_path) + "-journal").unlink(missing_ok=True)


def verified_index(directory: Path, stream, path: Path) -> sqlite3.Connection:
    source = file_identity(os.fstat(stream.fileno()))
    connection = None
    try:
        checkpoint = json.loads((directory / INDEX_CHECKPOINT).read_text())
        if checkpoint != {"schema": INDEX_SCHEMA, "history": source,
                           "index": file_identity((directory / INDEX_FILE).stat())}:
            raise ValueError("history/index checkpoint changed")
        connection = connect_index(directory / INDEX_FILE)
        stored = connection.execute("SELECT source FROM checkpoint WHERE singleton=1").fetchone()
        if stored is None or json.loads(stored[0]) != source:
            raise ValueError("index transaction and JSONL checkpoint differ")
        return connection
    except (OSError, ValueError, sqlite3.Error):
        if connection is not None:
            connection.close()
    # Missing/cache-corrupt/stale state includes a crash after the durable JSONL append but
    # before either index commit or checkpoint rename. Revalidate the entire authoritative file
    # before replacing this disposable index, so middle corruption and duplicate IDs still fail.
    return rebuild_index(directory, stream, path)


def commit_index(connection: sqlite3.Connection, stream) -> None:
    connection.execute("UPDATE checkpoint SET source=? WHERE singleton=1",
                       (json.dumps(file_identity(os.fstat(stream.fileno())), sort_keys=True),))
    connection.commit()


def record(directory: Path, *, run_id: str, label: str, seconds: float,
           verdict: str, timed_out: bool = False, observation_id: str | None = None,
           scored: bool = True, ledger_label: str | None = None) -> None:
    row = validate_observation({"schema": SCHEMA, "timing": "own-row",
        "run_id": run_id, "observation_id": observation_id or uuid.uuid4().hex,
        "label": canonical_label(label), "seconds": seconds, "verdict": verdict,
        "timed_out": timed_out, "recorded_at": time.time(), "scored": scored,
        "ledger_label": canonical_label(ledger_label or label) if scored else None})
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / HISTORY_FILE
    # JSONL remains the durable audit and the lock. Its stat-bound, disposable ID index avoids
    # decoding all previous runs once per row. Any unfamiliar file/index forces full validation.
    # 2026-09-10 file-only timing, 5,000 prior rows + 100 fsynced appends (median of three):
    # PRE 6.117 s; POST 0.178 s including index creation, 0.109 s with an existing index.
    # This excludes Python startup and is not a measurement of end-to-end gate duration.
    with path.open("a+", encoding="utf-8") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        connection = verified_index(directory, stream, path)
        try:
            key = index_key(row["observation_id"])
            if connection.execute("SELECT 1 FROM observation_ids WHERE id=?", (key,)).fetchone():
                raise ValueError(f"{path}: duplicate observation_id {row['observation_id']}")
            connection.execute("BEGIN IMMEDIATE")
            connection.execute("INSERT INTO observation_ids VALUES (?)", (key,))
            stream.write(json.dumps(row, sort_keys=True, allow_nan=False) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
            # Never commit the ID before its JSONL record is durable. A failure from here on
            # leaves the old checkpoint, so recovery finds (and cannot duplicate) this record.
            commit_index(connection, stream)
        finally:
            connection.close()
        publish_index_checkpoint(directory, stream)


def verdict_history(directory: Path) -> dict:
    """Summarize immutable observations without conflating duplicate labels with runs.

    Append order is the durable observation order. Per-run verdicts aggregate every context:
    identical labels sometimes occur twice in one gate, and parallel completion order cannot
    establish a temporal regression between those contexts. Such a mixed run is reported loudly,
    separately from transitions between run aggregates. No final gate marker is required, so
    observations from an interrupted run remain evidence instead of disappearing.
    """
    observations = read_history(directory)
    grouped = defaultdict(dict)
    run_order = {}
    for index, observation in enumerate(observations):
        run_id = observation["run_id"]
        run_order.setdefault(run_id, index)
        grouped[observation["label"]].setdefault(run_id, []).append(observation)
    labels = {}
    for label, runs in sorted(grouped.items()):
        run_summaries = []
        for run_id in sorted(runs, key=run_order.__getitem__):
            rows = runs[run_id]
            failures = sum(row["verdict"] == "FAIL" for row in rows)
            verdict = "mixed" if 0 < failures < len(rows) else "FAIL" if failures else "ok"
            run_summaries.append({"run_id": run_id, "verdict": verdict,
                "observations": len(rows), "failures": failures,
                "first_recorded_at": rows[0]["recorded_at"],
                "last_recorded_at": rows[-1]["recorded_at"],
                "last_observation_id": rows[-1]["observation_id"]})
        transitions = []
        for previous, current in zip(run_summaries, run_summaries[1:]):
            if previous["verdict"] != current["verdict"]:
                transitions.append({"from_run": previous["run_id"], "from_verdict": previous["verdict"],
                    "to_run": current["run_id"], "to_verdict": current["verdict"],
                    "recorded_at": current["last_recorded_at"]})
        failures = sum(run["failures"] for run in run_summaries)
        total = sum(run["observations"] for run in run_summaries)
        failing_runs = sum(run["failures"] > 0 for run in run_summaries)
        mixed = [run["run_id"] for run in run_summaries if run["verdict"] == "mixed"]
        labels[label] = {"observations": total, "failures": failures,
            "failure_fraction": failures / total, "runs": len(run_summaries),
            "failing_runs": failing_runs, "failing_run_fraction": failing_runs / len(run_summaries),
            "verdict_flipped": 0 < failures < total, "mixed_runs": mixed,
            "transitions": transitions, "latest_transition": transitions[-1] if transitions else None,
            "latest_run": run_summaries[-1], "run_history": run_summaries}
    return {"schema": SCHEMA, "observations": len(observations), "runs": len(run_order),
        "labels": labels, "flipping_labels": sum(row["verdict_flipped"] for row in labels.values())}


def format_verdict_history(report: dict) -> str:
    lines = [f"GATE HISTORY: {len(report['labels'])} labels, {report['observations']} observations, "
             f"{report['runs']} runs; {report['flipping_labels']} verdict-changing labels"]
    for label, row in report["labels"].items():
        if not row["verdict_flipped"]:
            continue
        latest = row["latest_transition"]
        transition = (f"{latest['from_verdict']} ({latest['from_run']}) -> "
                      f"{latest['to_verdict']} ({latest['to_run']}) at "
                      f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(latest['recorded_at']))}"
                      if latest else "none between runs")
        lines.append(f"  DEFECT NEEDS FIX: {label}; FAIL observations "
            f"{row['failures']}/{row['observations']} ({100 * row['failure_fraction']:.2f}%); "
            f"runs containing FAIL {row['failing_runs']}/{row['runs']}; "
            f"between-run transitions={len(row['transitions'])}; latest {transition}")
        if row["mixed_runs"]:
            mixed = [run for run in row["run_history"] if run["verdict"] == "mixed"]
            detail = ", ".join(f"{run['run_id']} ({run['failures']}/{run['observations']} FAIL)" for run in mixed)
            lines.append("    DEFECT: mixed verdicts within the same run: " + detail +
                "; repeated-label contexts are not distinguishable in this history, so these are "
                "observation rates, not independent rerun probabilities")
    if report["flipping_labels"]:
        # Historical failures remain visible after a current pass. The current gate's rows own
        # its verdict: history alone cannot say whether code changed and fixed an earlier defect.
        # Never erase failures or silently reset history to resolve this message. If resolution
        # tracking is added, require an explicit record naming the fix commit/code hash while
        # retaining every original observation; no automatic grace period or tolerance applies.
        lines.append("  Historical flips remain visible even when the latest run passes; "
                     "the current gate verdict is determined by its current rows.")
    return "\n".join(lines)


def import_legacy(paths: list[Path]) -> tuple[dict[str, list[float]], list[str]]:
    rows = defaultdict(list)
    sources = []
    seen = set()
    for path in paths:
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest in seen:
            continue  # Archived copies of a ledger are one run, not extra evidence.
        seen.add(digest)
        sources.append(str(path.resolve()))
        for number, line in enumerate(data.decode("utf-8").splitlines(), 1):
            parts = line.split("\t")
            try:
                if len(parts) != 3 or parts[0] not in ("ok", "FAIL"):
                    raise ValueError("expected verdict<TAB>seconds<TAB>label; use .timings for two-column ledgers")
                duration = finite_number(float(parts[1]), "legacy seconds")
                label = canonical_label(parts[2])
                if parts[0] == "ok":
                    rows[label].append(duration)
            except (ValueError, TypeError) as exc:
                raise ValueError(f"{path}:{number}: {exc}") from exc
    return rows, sources


def summary(values: list[float]) -> dict:
    if not values:
        return {"samples": 0, "median_seconds": None, "min_seconds": None,
                "max_seconds": None, "max_over_median": None}
    median = statistics.median(values)
    return {"samples": len(values), "median_seconds": median,
            "min_seconds": min(values), "max_seconds": max(values),
            "max_over_median": max(values) / median if median else None}


def prepare(directory: Path, imports: list[Path], *, multiplier: float = DEFAULT_MULTIPLIER,
            floor: float = DEFAULT_FLOOR, fallback: float = DEFAULT_FALLBACK) -> dict:
    multiplier = finite_number(multiplier, "multiplier", positive=True)
    floor = finite_number(floor, "floor", positive=True)
    fallback = finite_number(fallback, "fallback", positive=True)
    if multiplier < 1 or fallback < floor:
        raise ValueError("multiplier must be >=1 and fallback must be >=floor")
    exact = defaultdict(list)
    all_labels = set()
    for row in read_history(directory):
        all_labels.add(row["label"])
        if row["verdict"] == "ok" and not row["timed_out"]:
            exact[row["label"]].append(row["seconds"])
    legacy, sources = import_legacy(imports)
    all_labels.update(legacy)
    rows = {}
    for label in sorted(all_labels):
        item = summary(exact[label])
        old = summary(legacy.get(label, []))
        if item["samples"]:
            limit = max(floor, multiplier * item["median_seconds"], 2 * item["max_seconds"])
            basis = "own-row-history"
        else:
            limit = fallback
            basis = "no-own-row-history-conservative-default"
        # Historical intervals are only upper-bound evidence, never the row's median.
        # Retain the largest successful historical observation so a cold rebuild or a
        # full 256-second clock bucket is not shortened by many warm/short observations.
        if old["samples"]:
            limit = max(limit, 2 * old["max_seconds"])
            item["legacy_interval"] = old
        item.update(timeout_seconds=math.ceil(limit), basis=basis)
        rows[label] = protect_mechanism_budget(label, item)
    return {"schema": SCHEMA, "created_at": time.time(),
            "defaults": {"multiplier": multiplier, "floor_seconds": floor,
                         "fallback_seconds": math.ceil(fallback)},
            "legacy_sources": sources, "rows": rows}


def budget(plan: dict, label: str) -> dict:
    if not isinstance(plan, dict) or plan.get("schema") != SCHEMA or not isinstance(plan.get("rows"), dict):
        raise ValueError("invalid timeout plan")
    defaults = plan.get("defaults", {})
    fallback = finite_number(defaults.get("fallback_seconds"), "plan fallback", positive=True)
    row = plan["rows"].get(canonical_label(label), {
        "timeout_seconds": fallback, "median_seconds": None,
        "basis": "no-history-conservative-default"})
    finite_number(row.get("timeout_seconds"), "plan timeout", positive=True)
    if row.get("median_seconds") is not None:
        finite_number(row["median_seconds"], "plan median")
    if not isinstance(row.get("basis"), str) or any(c in row["basis"] for c in "\t\r\n"):
        raise ValueError("invalid timeout basis")
    if "mechanism_floor_seconds" not in row:
        row = protect_mechanism_budget(canonical_label(label), row)
    return row


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=path.name + ".",
                                     delete=False, encoding="utf-8") as stream:
        temporary = Path(stream.name)
        try:
            json.dump(value, stream, sort_keys=True, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def process_identity(pid: int) -> tuple[int, int] | None:
    """Return (parent, start ticks); command names/argv are never ownership evidence."""
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return int(fields[1]), int(fields[19])
    except (FileNotFoundError, ProcessLookupError):
        return None


def descendants(pid: int, start: int) -> dict[int, int]:
    current = process_identity(pid)
    if current is None or current[1] != start:
        return {}
    table = {}
    for path in Path("/proc").iterdir():
        if path.name.isdecimal():
            child = int(path.name)
            identity = process_identity(child)
            if identity is not None:
                table[child] = identity
    found = {}
    frontier = {pid}
    # The watcher and its descendants are excluded even though it is a child of the
    # row shell. This walks ancestry, never argv text that can match the calling shell.
    excluded = {os.getpid()}
    while frontier:
        next_frontier = set()
        for child, (parent, born) in table.items():
            if parent in frontier and child not in excluded and child not in found:
                found[child] = born
                next_frontier.add(child)
        frontier = next_frontier
    return found


def signal_identity(pid: int, start: int, sig: int) -> bool:
    identity = process_identity(pid)
    if identity is None or identity[1] != start:
        return False
    # pidfd pins the process across the identity check and signal; numeric PID reuse
    # between /proc inspection and os.kill must not target an unrelated process.
    try:
        descriptor = os.pidfd_open(pid)
    except ProcessLookupError:
        return False
    try:
        identity = process_identity(pid)
        if identity is None or identity[1] != start:
            return False
        signal.pidfd_send_signal(descriptor, sig)
        return True
    except ProcessLookupError:
        return False
    finally:
        os.close(descriptor)


def process_alive(pid: int, start: int) -> bool:
    """A zombie is stopped work awaiting its direct parent's wait(), not a live server."""
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return int(fields[19]) == start and fields[0] != "Z"
    except (FileNotFoundError, ProcessLookupError):
        return False


def reap_tree(pid: int, parent_start: int, *, grace: float = 5.0) -> dict:
    """Bound teardown of this caller's children, identified by ancestry and start ticks.

    The shell waits its direct children after this returns. Never wait indefinitely for an
    uninterruptible child: report the surviving identity and let the gate stay red instead.
    """
    grace = finite_number(grace, "grace", positive=True)
    if pid != os.getppid() or pid <= 1 or parent_start <= 0:
        raise ValueError("reap-tree can only clean up its own direct parent")
    identity = process_identity(pid)
    if identity is None or identity[1] != parent_start:
        raise ValueError("reap-tree parent PID/start identity is no longer live")
    owned = {}

    def capture():
        for child, born in descendants(pid, parent_start).items():
            if owned.get(child) == born:
                continue
            owned[child] = born
            # A paused compiler/server must run before TERM can invoke its cleanup.
            signal_identity(child, born, signal.SIGCONT)
            signal_identity(child, born, signal.SIGTERM)

    capture()
    end = time.monotonic() + grace
    while time.monotonic() < end:
        capture()  # Cleanup handlers may launch children; include them while still owned.
        if not any(process_alive(child, born) for child, born in owned.items()):
            return {"owned": owned, "survivors": {}, "forced": {}}
        time.sleep(min(0.05, max(0.0, end - time.monotonic())))
    forced = {child: born for child, born in owned.items() if process_alive(child, born)}
    for child, born in forced.items():
        signal_identity(child, born, signal.SIGKILL)
    # SIGKILL is asynchronous. Give the kernel a bounded interval to leave runnable state
    # before permitting ABBA; a D-state survivor is a loud teardown failure, never a wait hang.
    end = time.monotonic() + min(grace, 1.0)
    while time.monotonic() < end:
        survivors = {child: born for child, born in owned.items() if process_alive(child, born)}
        if not survivors:
            return {"owned": owned, "survivors": {}, "forced": forced}
        time.sleep(min(0.01, max(0.0, end - time.monotonic())))
    return {"owned": owned, "forced": forced,
            "survivors": {child: born for child, born in owned.items() if process_alive(child, born)}}


def signal_child(pid: int, start: int, parent: int, parent_start: int) -> None:
    # Only the caller's exact sibling can be its row watcher. Reparenting or a recycled PID
    # is not ownership; the pidfd in signal_identity closes the final check/signal race.
    caller = process_identity(parent)
    if parent != os.getppid() or caller is None or caller[1] != parent_start:
        raise ValueError("signal-child caller parent identity changed")
    identity = process_identity(pid)
    if identity is None:
        return
    if identity != (parent, start):
        raise ValueError("signal-child target PID/start or direct-parent identity changed")
    signal_identity(pid, start, signal.SIGKILL)


def cancellation_receipt(path: Path, fields: tuple[str, ...]) -> None:
    # The shell reads one complete receipt only after its atomic publication. A fragment,
    # older pause/rearm generation, or unrelated PID cannot explain a watcher's exit0.
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=path.name + ".",
                                     delete=False, encoding="utf-8") as stream:
        temporary = Path(stream.name)
        try:
            stream.write("\t".join(("CANCELLED", *fields)) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def watch(pid: int, parent_start: int, marker: Path, *, seconds: float | None = None,
          deadline: float | None = None, grace: float = 5.0, generation: str | None = None,
          cancel_request: Path | None = None, cancel_receipt: Path | None = None) -> None:
    grace = finite_number(grace, "grace", positive=True)
    if pid <= 1 or parent_start <= 0:
        raise ValueError("watch requires a non-init parent PID and positive start ticks")
    if (seconds is None) == (deadline is None):
        raise ValueError("supply exactly one of seconds and deadline")
    if seconds is not None:
        deadline = time.monotonic() + finite_number(seconds, "seconds", positive=True)
    else:
        deadline = finite_number(deadline, "deadline", positive=True)
    identity = process_identity(pid)
    if identity is None or identity[1] != parent_start:
        raise ValueError("row shell PID/start identity is no longer live")
    if os.getppid() != pid:
        raise ValueError("watch can only supervise its own direct parent")
    cancellation = (generation, cancel_request, cancel_receipt)
    if any(value is not None for value in cancellation) and not all(value is not None for value in cancellation):
        raise ValueError("watch cancellation requires generation, request, and receipt together")
    if generation is not None and not re.fullmatch(rf"{pid}\.{parent_start}\.[1-9][0-9]*", generation):
        raise ValueError("watch cancellation generation must name its parent PID/start and sequence")
    self_identity = process_identity(os.getpid())
    if self_identity is None or self_identity[0] != pid:
        raise ValueError("watch cannot establish its own direct-parent/PID/start identity")
    cancellation_fields = tuple(map(str, (pid, parent_start, os.getpid(), self_identity[1], generation)))
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        # A TERM delivered during Bash's pre-exec transition was observed being lost while
        # this Python process then slept for the entire 900s budget. The parent's persistent
        # request survives that transition. Expiry is checked FIRST: a late cancellation
        # cannot suppress its marker or steal the five-second descendant escalation below.
        if cancel_request is not None and cancel_request.exists():
            expected = "\t".join(("CANCEL", *cancellation_fields)) + "\n"
            if cancel_request.read_text() != expected:
                raise ValueError("watch cancellation request does not match this PID/start/generation")
            if time.monotonic() >= deadline:
                break
            cancellation_receipt(cancel_receipt, cancellation_fields)
            return
        time.sleep(min(remaining, 0.1))
        identity = process_identity(pid)
        if identity is None or identity[1] != parent_start:
            return
    # Once expiry starts, the shell must wait for cleanup instead of cancelling it.
    # Ignoring TERM closes the marker-publication/cancellation race on this side;
    # row_finish also checks elapsed wall time against its frozen budget.
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    owned = descendants(pid, parent_start)
    atomic_json(marker, {"state": "TIMEOUT", "pid": pid, "parent_start": parent_start,
        "deadline": deadline, "expired_at": time.monotonic(), "descendants": owned})
    for child, born in owned.items():
        signal_identity(child, born, signal.SIGTERM)
    signal_identity(pid, parent_start, signal.SIGUSR1)
    end = time.monotonic() + grace
    while time.monotonic() < end:
        remaining = False
        for child, born in owned.items():
            if process_alive(child, born):
                remaining = True
                break
        if not remaining:
            return
        time.sleep(min(0.05, max(0.0, end - time.monotonic())))
    for child, born in owned.items():
        signal_identity(child, born, signal.SIGKILL)


class HistoryTests(unittest.TestCase):
    def setUp(self):
        build = Path(__file__).resolve().parents[1] / "build"
        build.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=build)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def add(self, seconds, verdict="ok", timed_out=False, label="test"):
        record(self.directory, run_id="run", label=label, seconds=seconds,
               verdict=verdict, timed_out=timed_out)

    def test_success_median_and_hang_cannot_enlarge_it(self):
        for seconds in (10, 12, 14):
            self.add(seconds)
        self.add(900, "FAIL", True)
        self.add(1000, "FAIL")
        row = budget(prepare(self.directory, []), "test")
        self.assertEqual((row["median_seconds"], row["samples"], row["timeout_seconds"]), (12, 3, 48))

    def test_floor_and_cold_build_guard(self):
        for seconds in (0.01, 0.01, 48.6):
            self.add(seconds)
        self.assertEqual(budget(prepare(self.directory, []), "test")["timeout_seconds"], 98)
        self.add(0.001, label="tiny")
        self.assertEqual(budget(prepare(self.directory, []), "tiny")["timeout_seconds"], 30)

    def test_first_cold_cache_and_unseen_clock_phase_keep_mechanism_floors(self):
        for label in ("ASAN build", "release build (+footprint locks)",
                      "build dependency: core-concurrency-tsan"):
            self.add(.1, label=label)
            row = budget(prepare(self.directory, []), label)
            self.assertEqual(row["timeout_seconds"], 900)
            self.assertIn("cold cache state not distinguished", row["basis"])
        self.add(6, label="eviction lruclock (split, atomic 0)")
        row = budget(prepare(self.directory, []), "eviction lruclock (split, atomic 0)")
        self.assertEqual(row["timeout_seconds"], 512)
        self.assertIn("two production 256s clock buckets", row["basis"])
        self.add(.1, label="flip controller model unit")
        self.assertEqual(budget(prepare(self.directory, []), "flip controller model unit")["timeout_seconds"], 30)

    def test_hidden_build_names_are_independent_of_cpu_slot_and_source_line(self):
        a = ["taskset", "-c", "0-9", "tests/parbuild.sh", "/tree/build/core-tsan", "/tree/objects"]
        b = ["taskset", "-c", "80-95", "tests/parbuild.sh", "/other/build/core-tsan", "/other/objects"]
        self.assertEqual(dependency_label(a), dependency_label(b))
        self.assertEqual(dependency_label(a), "build dependency: core-tsan")
        self.assertNotEqual(dependency_label(a), dependency_label(b[:4] + ["/other/build/waits-tsan"] + b[5:]))
        self.assertEqual(dependency_label(["taskset", "-c", "0-9", "make", "-j10", "build/a", "build/b"]),
                         dependency_label(["make", "-j128", "build/b", "build/a"]))

    def test_abba_context_uses_actual_geometry_window_and_cell_bytes(self):
        import abbagate
        from unittest import mock
        cells = self.directory / "cells.txt"
        cells.write_text("first cell source\n")
        argv = ["--cells", str(cells), "--server-cores", "0-7", "--load-cores", "8-15"]
        original = abba_context(argv)
        self.assertIn("subset=full", original)
        self.assertIn("server_cores=0-7", original)
        self.assertIn(f"window={abbagate.WINDOW}", original)
        self.assertNotEqual(original, abba_context(argv + ["--server-smt", "128-135"]))
        with mock.patch.object(abbagate, "WINDOW", abbagate.WINDOW / 2):
            self.assertNotEqual(original, abba_context(argv))
        cells.write_text("second cell source\n")
        self.assertNotEqual(original, abba_context(argv))
        # The pre-subset ABBA parser still exists until its independent commit lands.
        # Preserve that parser's real argv handling while exercising the new subset field.
        parse = abbagate.parse_args
        def smoke():
            args = parse()
            args.subset = "smoke"
            return args
        full = abba_context(argv)
        with mock.patch.object(abbagate, "parse_args", smoke):
            self.assertNotEqual(full, abba_context(argv))

    def test_no_history_says_default(self):
        row = budget(prepare(self.directory, []), "unseen")
        self.assertIsNone(row["median_seconds"])
        self.assertEqual(row["timeout_seconds"], 900)
        self.assertIn("default", row["basis"])

    def test_legacy_never_becomes_exact_and_duplicates_count_once(self):
        a = self.directory / "old.tsv"
        b = self.directory / "copy.tsv"
        a.write_text("ok\t6.1\tlruclock\nok\t256.1\tlruclock\n")
        b.write_bytes(a.read_bytes())
        row = budget(prepare(self.directory, [a, b]), "lruclock")
        self.assertEqual(row["samples"], 0)
        self.assertEqual(row["legacy_interval"]["samples"], 2)
        self.assertEqual(row["timeout_seconds"], 900)
        self.assertIsNone(row["median_seconds"])

    def test_corrupt_history_cannot_be_read_or_appended(self):
        (self.directory / HISTORY_FILE).write_text('{"schema":1')
        with self.assertRaisesRegex(ValueError, "incomplete"):
            prepare(self.directory, [])
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.add(1)

    def test_healthy_index_appends_and_deduplicates_without_decoding_old_history(self):
        from unittest import mock
        record(self.directory, run_id="old", label="first", seconds=1, verdict="ok", observation_id="first")
        before = (self.directory / HISTORY_FILE).read_bytes()
        with mock.patch(__name__ + ".decode_history", side_effect=AssertionError("old JSONL was reread")):
            with self.assertRaisesRegex(ValueError, "duplicate observation_id"):
                record(self.directory, run_id="new", label="other", seconds=2, verdict="ok", observation_id="first")
            self.assertEqual((self.directory / HISTORY_FILE).read_bytes(), before)
            for number in range(10):
                record(self.directory, run_id="new", label="new", seconds=2, verdict="ok",
                       observation_id=f"new-{number}")
        with mock.patch(__name__ + ".decode_history", wraps=decode_history) as decode:
            self.assertEqual(len(read_history(self.directory)), 11)
            prepare(self.directory, [])
            self.assertEqual(decode.call_count, 2, "read/prepare must still validate the complete audit")

    def test_corrupt_middle_with_restored_mtime_and_truncated_tail_are_never_appended(self):
        for number in range(3):
            record(self.directory, run_id="old", label="first", seconds=1, verdict="ok", observation_id=str(number))
        path = self.directory / HISTORY_FILE
        original = path.read_bytes()
        info = path.stat()
        lines = original.splitlines(keepends=True)
        lines[1] = lines[1].replace(b'"schema": 1', b'"schema": 9')
        changed = b"".join(lines)
        self.assertEqual(len(changed), len(original))
        path.write_bytes(changed)
        os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))
        with self.assertRaisesRegex(ValueError, "schema"):
            self.add(1)
        self.assertEqual(path.read_bytes(), changed)
        path.write_bytes(original[:-2])
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.add(1)
        self.assertEqual(path.read_bytes(), original[:-2])

    def test_index_preserves_all_json_string_ids_including_escaped_surrogates(self):
        identities = ("測定", "\U00010000", "\ud800", "\udc00")
        for identity in identities:
            record(self.directory, run_id="old", label="row", seconds=1,
                   verdict="ok", observation_id=identity)
        (self.directory / INDEX_CHECKPOINT).unlink()
        for identity in identities:
            with self.assertRaisesRegex(ValueError, "duplicate observation_id"):
                record(self.directory, run_id="new", label="row", seconds=1,
                       verdict="ok", observation_id=identity)
        with self.assertRaisesRegex(ValueError, "duplicate observation_id"):
            record(self.directory, run_id="new", label="row", seconds=1,
                   verdict="ok", observation_id="\ud800\udc00")
        self.assertEqual([row["observation_id"] for row in read_history(self.directory)], list(identities))

    def test_duplicate_in_external_history_edit_fails_before_rebuilding_index(self):
        record(self.directory, run_id="old", label="first", seconds=1, verdict="ok", observation_id="first")
        path = self.directory / HISTORY_FILE
        duplicated = path.read_bytes() * 2
        path.write_bytes(duplicated)
        with self.assertRaisesRegex(ValueError, "duplicate observation_id"):
            self.add(1)
        self.assertEqual(path.read_bytes(), duplicated)

    def test_replaced_history_file_forces_full_validation(self):
        from unittest import mock
        self.add(1)
        path = self.directory / HISTORY_FILE
        previous = path.stat()
        replacement = self.directory / "replacement"
        replacement.write_bytes(path.read_bytes())
        os.utime(replacement, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        os.replace(replacement, path)
        with mock.patch(__name__ + ".decode_history", wraps=decode_history) as decode:
            self.add(2)
            self.assertEqual(decode.call_count, 1)
        self.assertEqual(len(read_history(self.directory)), 2)

    def test_modified_missing_and_corrupt_indexes_rebuild_without_losing_duplicate_checks(self):
        from unittest import mock
        record(self.directory, run_id="old", label="first", seconds=1, verdict="ok", observation_id="first")
        original = (self.directory / HISTORY_FILE).read_bytes()
        for damage in ("delete-id", "corrupt-database", "missing-checkpoint", "corrupt-checkpoint"):
            with self.subTest(damage=damage):
                if damage == "delete-id":
                    with sqlite3.connect(self.directory / INDEX_FILE) as connection:
                        connection.execute("DELETE FROM observation_ids")
                elif damage == "corrupt-database":
                    (self.directory / INDEX_FILE).write_bytes(b"not a database")
                elif damage == "missing-checkpoint":
                    (self.directory / INDEX_CHECKPOINT).unlink()
                else:
                    (self.directory / INDEX_CHECKPOINT).write_text("{")
                with mock.patch(__name__ + ".decode_history", wraps=decode_history) as decode:
                    with self.assertRaisesRegex(ValueError, "duplicate observation_id"):
                        record(self.directory, run_id="new", label="first", seconds=1,
                               verdict="ok", observation_id="first")
                    self.assertEqual(decode.call_count, 1)
                self.assertEqual((self.directory / HISTORY_FILE).read_bytes(), original)

    def test_crash_after_append_or_index_commit_recovers_authoritative_record(self):
        import subprocess
        root = Path(__file__).resolve().parent
        for boundary in ("commit_index", "publish_index_checkpoint"):
            with self.subTest(boundary=boundary):
                directory = self.directory / boundary
                record(directory, run_id="old", label="first", seconds=1, verdict="ok", observation_id="first")
                code = '''import os, sys
sys.path.insert(0, sys.argv[1])
import gate_history as history
from pathlib import Path
setattr(history, sys.argv[3], lambda *args: os._exit(71))
history.record(Path(sys.argv[2]), run_id="interrupted", label="crashed", seconds=2,
               verdict="ok", observation_id="crashed")
'''
                result = subprocess.run([sys.executable, "-c", code, str(root), str(directory), boundary], timeout=5)
                self.assertEqual(result.returncode, 71)
                self.assertEqual([row["observation_id"] for row in read_history(directory)], ["first", "crashed"])
                preserved = (directory / HISTORY_FILE).read_bytes()
                with self.assertRaisesRegex(ValueError, "duplicate observation_id"):
                    record(directory, run_id="recovery", label="crashed", seconds=2,
                           verdict="ok", observation_id="crashed")
                self.assertEqual((directory / HISTORY_FILE).read_bytes(), preserved)
                record(directory, run_id="recovery", label="last", seconds=3, verdict="ok", observation_id="last")
                self.assertEqual(len(read_history(directory)), 3)

    def test_parallel_indexed_appends_preserve_every_observation(self):
        import subprocess
        root = Path(__file__).resolve().parent
        code = '''import sys
sys.path.insert(0, sys.argv[1])
from pathlib import Path
from gate_history import record
for index in range(8):
    record(Path(sys.argv[2]), run_id="parallel", label="row", seconds=index,
           verdict="ok", observation_id=f"{sys.argv[3]}-{index}")
'''
        children = [subprocess.Popen([sys.executable, "-c", code, str(root), str(self.directory), str(worker)])
                    for worker in range(8)]
        try:
            for child in children:
                self.assertEqual(child.wait(timeout=10), 0)
        finally:
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
        rows = read_history(self.directory)
        self.assertEqual({row["observation_id"] for row in rows},
                         {f"{worker}-{index}" for worker in range(8) for index in range(8)})
        self.assertEqual(len(rows), 64)

    def test_invalid_duration_or_expired_pass_rejected(self):
        for value in (-1, float("nan"), float("inf"), True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.add(value)
        with self.assertRaisesRegex(ValueError, "cannot pass"):
            self.add(1, timed_out=True)

    def test_canonical_preserves_geometry(self):
        self.assertEqual(canonical_label("armed hits=123 atomic 1 p32"), "armed hits=N atomic 1 p32")
        self.assertNotEqual(canonical_label("atomic 0"), canonical_label("atomic 1"))
        self.assertEqual(canonical_label("TLS connection slots all freed (3/4)"),
                         "TLS connection slots all freed (N/N)")

    def test_malformed_legacy_refused(self):
        path = self.directory / "old.tsv"
        for content in ("ok\trow\n", "skip\t1\trow\n", "ok\tnan\trow\n"):
            path.write_text(content)
            with self.assertRaises(ValueError):
                prepare(self.directory, [path])

    def test_real_cli_records_prepares_and_resolves(self):
        import subprocess
        command = [sys.executable, str(Path(__file__).resolve())]
        subprocess.run(command + ["record", "--history", str(self.directory), "--run-id", "cli",
            "--label", "test hits=3", "--seconds", "20", "--verdict", "ok"], check=True)
        output = self.directory / "plan.json"
        subprocess.run(command + ["prepare", "--history", str(self.directory), "--output", str(output)], check=True)
        result = subprocess.check_output(command + ["budget", "--plan", str(output), "--label", "test hits=4"], text=True)
        self.assertEqual(result.strip(), "80\t20\town-row-history")

    def run_watch_shell(self, body: str, seconds: str = "0.15"):
        import subprocess
        marker = self.directory / "timeout.json"
        shell = r'''set -u
trap 'wait "$watcher" || :; exit 124' USR1
read -r -a stat_fields < "/proc/$$/stat"
parent_start=${stat_fields[21]}
python3 "$1" watch --pid "$$" --parent-start "$parent_start" --seconds "$3" --grace 0.1 --marker "$2" &
watcher=$!
eval "$4"
'''
        return subprocess.run(["bash", "-c", shell, "watch-test", str(Path(__file__).resolve()),
            str(marker), seconds, body], timeout=5, capture_output=True, text=True), marker

    def test_watch_interrupts_infinite_builtin_loop(self):
        result, marker = self.run_watch_shell("while :; do :; done")
        self.assertEqual(result.returncode, 124, result.stderr)
        self.assertEqual(json.loads(marker.read_text())["state"], "TIMEOUT")

    def test_watch_cancelled_completed_row(self):
        result, marker = self.run_watch_shell('kill -TERM "$watcher"; wait "$watcher" || :')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(marker.exists())

    def test_watch_kills_owned_term_ignorer_and_leaves_outsider_alive(self):
        import subprocess
        outsider = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            body = 'python3 -c "import os,signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); print(os.getpid(),flush=True); time.sleep(30)" & wait "$!"'
            result, marker = self.run_watch_shell(body)
            self.assertEqual(result.returncode, 124, result.stderr)
            owned = int(result.stdout.strip())
            self.assertIn(str(owned), json.loads(marker.read_text())["descendants"])
            self.assertIsNone(outsider.poll())
            identity = process_identity(owned)
            if identity is not None:
                state = Path(f"/proc/{owned}/stat").read_text().rsplit(")", 1)[1].split()[0]
                self.assertEqual(state, "Z", "TERM-ignoring owned process survived escalation")
        finally:
            outsider.terminate()
            outsider.wait(timeout=5)


    def bounded_shell(self, argv, root, directory, environment, timeout):
        # This item-one control must work before mutation mode exists. Own one shell,
        # capture its descendants by PID/start, and reap orphaned test children locally.
        # In particular the poisoned old wait must not leave its 30s watcher behind.
        import ctypes
        import subprocess
        from types import SimpleNamespace
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        if libc.prctl(37, ctypes.byref(previous), 0, 0, 0) != 0 or libc.prctl(36, 1, 0, 0, 0) != 0:
            raise OSError(ctypes.get_errno(), "cannot scope cancellation control as subreaper")
        own_pid = os.getpid()
        own_children = Path(f"/proc/{own_pid}/task/{own_pid}/children")
        preexisting = {int(pid): process_identity(int(pid)) for pid in own_children.read_text().split()}
        owned, process, expired = {}, None, False
        logfile = directory / 'owned-command.log'

        def capture():
            if process is not None:
                identity = process_identity(process.pid)
                if identity is not None and identity[1] == owned.get(process.pid):
                    owned.update(descendants(process.pid, identity[1]))
            # Only children orphaned by this fixture are new direct children of this
            # synchronous test process; pre-existing children remain outside its ownership.
            for token in own_children.read_text().split():
                pid = int(token)
                identity = process_identity(pid)
                if identity is not None and identity != preexisting.get(pid):
                    owned[pid] = identity[1]

        def reap():
            for pid, born in tuple(owned.items()):
                if process is not None and pid == process.pid:
                    continue  # Popen alone owns its direct child's wait status.
                if process_identity(pid) == (own_pid, born):
                    try:
                        os.waitpid(pid, os.WNOHANG)
                    except ChildProcessError:
                        pass

        try:
            with logfile.open('w') as log:
                process = subprocess.Popen(argv, cwd=root, env=environment, stdout=log, stderr=subprocess.STDOUT)
                identity = process_identity(process.pid)
                if identity is not None:
                    owned[process.pid] = identity[1]
                deadline = time.monotonic() + timeout
                while process.poll() is None and time.monotonic() < deadline:
                    capture()
                    reap()
                    time.sleep(.01)
                expired = process.poll() is None
        finally:
            # Clean descendants first, giving the shell a chance to consume wait status
            # and leave normally. Every cleanup phase is bounded even in the poisoned case.
            for sig, grace in ((signal.SIGTERM, 1.0), (signal.SIGKILL, 1.0)):
                until = time.monotonic() + grace
                while True:
                    capture()
                    live = {pid: born for pid, born in owned.items() if process_alive(pid, born)}
                    children = {pid: born for pid, born in live.items()
                                if process is None or pid != process.pid}
                    # Do not interrupt a healthy shell that is now publishing its row
                    # after the stranded watcher stopped. It has this first grace to exit.
                    targets = children or (live if sig == signal.SIGKILL or time.monotonic() >= until else {})
                    for pid, born in targets.items():
                        signal_identity(pid, born, sig)
                    reap()
                    if process is not None:
                        process.poll()
                    if not live or time.monotonic() >= until:
                        break
                    time.sleep(.01)
            capture()
            reap()
            leaked = {pid: born for pid, born in owned.items() if process_alive(pid, born)}
            if process is not None:
                process.poll()
            libc.prctl(36, previous.value, 0, 0, 0)
        observed = dict(returncode=process.returncode, expired=expired, leaked_pids=leaked)
        (directory / 'owned-command.json').write_text(json.dumps(observed, indent=2) + '\n')
        return SimpleNamespace(**observed, stdout=logfile.read_text(), stderr='')

    def shell_row(self, body, timeout=1.0, extra_functions="", owned_timeout=None):
        import subprocess
        root = Path(__file__).resolve().parents[1]
        gate = (root / 'tests/gate.sh').read_text()
        helpers = gate[gate.index('say(){'):gate.index('\nledger_labels(){')]
        directory = Path(self.temp.name)
        plan = prepare(directory / 'history', [])
        plan['rows']['fixture'] = dict(timeout_seconds=timeout, median_seconds=.05, basis='own-row-history')
        atomic_json(directory / 'plan.json', plan)
        setup = '''set -u
TMPDIR="$FIXTURE"; LEDGER="$FIXTURE/ledger"; TIMINGS="$FIXTURE/timings"
ROW_PLAN="$FIXTURE/plan.json"; ROW_HISTORY="$FIXTURE/history"; ROW_RUN_ID=fixture
PASS=0; FAIL=0
quiet_wait(){ :; }
: > "$LEDGER"; : > "$TIMINGS"
'''
        command_line = ['bash', '-c', setup + helpers + '\n' + extra_functions + '\n' + body]
        environment = dict(os.environ, FIXTURE=str(directory))
        if owned_timeout is None:
            result = subprocess.run(command_line, cwd=root, env=environment,
                                    capture_output=True, text=True, timeout=10)
        else:
            result = self.bounded_shell(command_line, root, directory, environment, owned_timeout)
        rows = (directory / 'ledger').read_text().splitlines()
        return result, [row.split('\t') for row in rows], read_history(directory / 'history')

    def test_real_shell_scope_expiry_cannot_report_success(self):
        result, rows, history = self.shell_row('row_begin fixture\nsleep 60\nok fixture\n', .15)
        self.assertEqual(result.returncode, 124, result.stderr)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][0], 'FAIL')
        self.assertEqual(rows[0][2], 'fixture')
        self.assertIn('TIMEOUT 0.15s; median=0.05s', result.stdout)
        self.assertTrue(history[0]['timed_out'])

    def test_real_shell_missing_scope_is_a_failure(self):
        result, rows, history = self.shell_row('ok fixture\n')
        self.assertEqual(rows, [['FAIL', '0', 'fixture']])
        self.assertIn('missing row_begin', result.stdout)
        self.assertFalse(history)  # No invented exact timing for work that was not observed.

    def test_unexpected_watchdog_exit_still_fails_after_ownership_guards(self):
        broken_monitor = '''row_watch(){
  python3 -c 'raise SystemExit(17)' &
  ROW_WATCHDOG=$!
}
'''
        result, rows, history = self.shell_row(
            'row_begin fixture\nwait "$ROW_WATCHDOG"\nok fixture\n', extra_functions=broken_monitor)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([(row[0], row[2]) for row in rows], [("FAIL", "fixture")])
        self.assertIn("row watchdog exited unexpectedly: 17", result.stdout)
        self.assertEqual(history[0]["verdict"], "FAIL")
        self.assertFalse(history[0]["timed_out"])

    def test_cancel_request_survives_preexec_first_term_loss(self):
        lose_first_term = '''python3(){
  if [ "${2-}" = watch ]; then
    # Deterministically model the retained Bash pre-exec transition: the first TERM is
    # consumed, after which the same PID execs a normally TERM-sensitive Python watcher.
    trap 'printf consumed > "$FIXTURE/first-term"' TERM
    : > "$FIXTURE/preexec-ready"
    while [ ! -f "$FIXTURE/first-term" ]; do sleep .01; done
    trap - TERM
    exec /usr/bin/python3 "$@"
  else command python3 "$@"
  fi
}
'''
        body = '''row_begin fixture
while [ ! -f "$FIXTURE/preexec-ready" ]; do sleep .01; done
ok fixture
'''
        result, rows, history = self.shell_row(body, 30, lose_first_term, owned_timeout=3)
        self.assertFalse(result.expired, result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(result.leaked_pids, result.stdout)
        self.assertEqual([(row[0], row[2]) for row in rows], [('ok', 'fixture')])
        self.assertEqual(history[0]['verdict'], 'ok')
        self.assertTrue((self.directory / 'first-term').exists(), 'first TERM was never consumed')
        receipts = list(self.directory.glob('*.cancelled'))
        self.assertEqual(len(receipts), 1, 'request was not acknowledged by the execed watcher')
        receipt = receipts[0].read_text().strip().split('\t')
        self.assertEqual(receipt[0], 'CANCELLED')
        self.assertEqual(receipt[5].split('.')[:2], receipt[1:3])

        # Poison only cancellation with the inherited unconditional wait. This must fail
        # the same bounded actual-process experiment; hand-constructing classify() inputs
        # would miss the precise production failure that this control is for.
        for path in (self.directory / 'first-term', self.directory / 'preexec-ready'):
            path.unlink()
        old_unwatch = '''row_unwatch(){
  if [ "$ROW_WATCHDOG" -gt 0 ]; then
    kill -TERM "$ROW_WATCHDOG" 2>/dev/null
    wait "$ROW_WATCHDOG" 2>/dev/null || :
    ROW_WATCHDOG=0
  fi
}
'''
        poisoned, _, _ = self.shell_row(body, 30, lose_first_term + old_unwatch, owned_timeout=3)
        self.assertTrue(poisoned.expired, 'unbounded old cancellation unexpectedly completed')
        self.assertFalse(poisoned.leaked_pids, 'negative control left an owned watcher running')

    def test_watchdog_exit_zero_without_receipt_is_still_red(self):
        broken = '''row_watch(){
  python3 -c 'raise SystemExit(0)' &
  ROW_WATCHDOG=$!
}
'''
        result, rows, history = self.shell_row(
            'row_begin fixture\nwait "$ROW_WATCHDOG"\nok fixture\n', extra_functions=broken)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(rows[0][0], 'FAIL')
        self.assertIn('row watchdog exited unexpectedly: 0', result.stdout)
        self.assertEqual(history[0]['verdict'], 'FAIL')

    def test_pause_rearm_has_distinct_cancellation_generations(self):
        # Repeated scopes share a PID and marker; their request/receipt paths must not.
        result, rows, _ = self.shell_row('''row_begin fixture
first=$ROW_WATCH_GENERATION
row_unwatch
row_watch
[ "$first" != "$ROW_WATCH_GENERATION" ] || exit 71
ok fixture
''', 5)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(rows[0][0], 'ok', result.stdout)

    def test_stalled_cancellation_is_bounded_reaped_and_red(self):
        stalled = '''python3(){
  if [ "${2-}" = watch ]; then
    exec /usr/bin/python3 -c 'import os,signal,time; from pathlib import Path; signal.signal(signal.SIGTERM,signal.SIG_IGN); Path(os.environ["FIXTURE"]+"/stalled-ready").touch(); time.sleep(60)'
  else command python3 "$@"
  fi
}
'''
        result, rows, history = self.shell_row('''row_begin fixture
while [ ! -f "$FIXTURE/stalled-ready" ]; do sleep .01; done
ok fixture
''', 30, stalled, owned_timeout=9)
        self.assertFalse(result.expired, result.stdout)
        self.assertFalse(result.leaked_pids, result.stdout)
        self.assertEqual(rows[0][0], 'FAIL', result.stdout)
        self.assertIn('exceeded its bounded grace', result.stdout)
        self.assertEqual(history[0]['verdict'], 'FAIL')
        self.assertFalse(history[0]['timed_out'], 'monitor failure is not a fabricated row expiry')

    def test_cancel_receipt_rejects_stale_generation_or_extra_records(self):
        body = '''ROW_WATCH_PARENT=101; ROW_WATCH_PARENT_START=202
ROW_WATCHDOG=303; ROW_WATCHDOG_START=404; ROW_WATCH_GENERATION=101.202.2
ROW_WATCH_RECEIPT="$FIXTURE/receipt"
printf 'CANCELLED\\t101\\t202\\t303\\t404\\t101.202.1\\n' > "$ROW_WATCH_RECEIPT"
row_watch_receipt && exit 81
printf 'CANCELLED\\t101\\t202\\t303\\t404\\t101.202.2\\nextra\\n' > "$ROW_WATCH_RECEIPT"
row_watch_receipt && exit 82
printf 'CANCELLED\\t101\\t202\\t303\\t404\\t101.202.2\\t\\n' > "$ROW_WATCH_RECEIPT"
row_watch_receipt && exit 84
printf 'CANCELLED\\t101\\t202\\t303\\t404\\t101.202.2' > "$ROW_WATCH_RECEIPT"
row_watch_receipt && exit 85
printf 'CANCELLED\\t101\\t202\\t303\\t404\\t101.202.2\\n' > "$ROW_WATCH_RECEIPT"
row_watch_receipt || exit 83
'''
        result, rows, _ = self.shell_row(body)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(rows, [])

    def test_late_cancellation_cannot_override_expiry(self):
        from unittest.mock import patch
        marker = self.directory / 'expired.json'
        request, receipt = self.directory / 'request', self.directory / 'receipt'
        request.write_text('CANCEL\t111\t22\t222\t33\t111.22.1\n')
        identities = lambda pid: (0, 22) if pid == 111 else (111, 33)
        with patch(__name__ + '.process_identity', side_effect=identities), \
             patch.object(os, 'getpid', return_value=222), patch.object(os, 'getppid', return_value=111), \
             patch.object(time, 'monotonic', return_value=11), patch.object(signal, 'signal'), \
             patch(__name__ + '.descendants', return_value={}), patch(__name__ + '.signal_identity') as sent:
            watch(111, 22, marker, deadline=10, generation='111.22.1',
                  cancel_request=request, cancel_receipt=receipt)
        self.assertEqual(json.loads(marker.read_text())['state'], 'TIMEOUT')
        self.assertFalse(receipt.exists())
        sent.assert_called_once_with(111, 22, signal.SIGUSR1)

    def test_forced_watcher_signal_rejects_reuse_and_foreign_parent(self):
        from unittest.mock import patch
        for target_identity in ((111, 999), (999, 33)):
            with self.subTest(target=target_identity), patch.object(os, 'getppid', return_value=111), \
                 patch(__name__ + '.process_identity', side_effect=lambda pid:
                       (0, 22) if pid == 111 else target_identity), \
                 patch(__name__ + '.signal_identity') as sent:
                with self.assertRaisesRegex(ValueError, 'target PID/start or direct-parent'):
                    signal_child(222, 33, 111, 22)
                sent.assert_not_called()

    def test_real_shell_records_only_the_rows_own_duration(self):
        result, rows, history = self.shell_row('sleep 1\nrow_begin fixture\nsleep .03\nok fixture\n', 5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(rows[0][0], 'ok', result.stdout)
        self.assertGreaterEqual(float(rows[0][1]), .03)
        self.assertLess(float(rows[0][1]), 1)
        self.assertEqual(float(rows[0][1]), history[0]['seconds'])

    def test_real_hidden_build_scope_records_success_and_failure_without_gate_rows(self):
        root = Path(__file__).resolve().parents[1]
        gate = (root / "tests/gate.sh").read_text()
        functions = gate[gate.index("pausable(){"):gate.index("\npy(){")]
        body = '''QUIET_FILE=""
make(){ sleep .02; return "$BUILD_RC"; }
BUILD_RC=0; pausable make -j8 build/example || exit 81
BUILD_RC=7; pausable make -j2 build/example; [ "$?" = 7 ] || exit 82
'''
        result, rows, history = self.shell_row(body, extra_functions=functions)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(rows, [])
        self.assertEqual([(row["label"], row["verdict"]) for row in history],
                         [("build dependency: make build/example", verdict) for verdict in ("ok", "FAIL")])
        self.assertTrue(all(row["seconds"] >= .02 for row in history))
        self.assertTrue(all(row["scored"] is False and row["ledger_label"] is None for row in history))

    def run_real_job(self, behavior):
        import subprocess
        root = Path(__file__).resolve().parents[1]
        gate = (root / "tests/gate.sh").read_text()
        helpers = gate[gate.index("say(){"):gate.index("\nledger_labels(){")]
        cleanup = gate[gate.index("reap_children(){"):gate.index("\ntrap 'cleanup")]
        jobs = gate[gate.index("job_finalize(){"):gate.index("\nstart_workers(){")]
        directory = self.directory
        plan = prepare(directory / "history", [])
        plan["rows"]["expires"] = dict(timeout_seconds=.4, median_seconds=.1, basis="fixture")
        atomic_json(directory / "plan.json", plan)
        setup = r'''set -u
ROW_PLAN="$RUN_DIR/plan.json"; ROW_HISTORY="$RUN_DIR/history"; ROW_RUN_ID=fixture
PASS=0; FAIL=0; WORKER_PIDS=(); SRV=0; GLOBCASE_ORACLE=0; MMPID=0; ABBA_PID=0; PAUSABLE_PID=0
quiet_wait(){ :; }
set_slot(){ CORES="$FIXTURE_CPU"; LOAD_CORES="$FIXTURE_CPU"; }
job_label(){ printf 'fixture family\n'; }
job_body(){
  row_begin first
  if [ "$BEHAVIOR" = inherited-finalizer ]; then
    # Replay the captured pre-exec state deterministically: a cancelled watchdog child
    # still has the worker's EXIT trap and active row, but does not own either one.
    inherited_exit=$(trap -p EXIT)
    (
      ROW_WATCHDOG=0
      eval "$inherited_exit"
      printf '%s\t%s\n' "$BASHPID" "$CLEANUP_OWNER" > "$RUN_DIR/inherited-finalizer"
      exit 0
    )
    [ ! -s "$LEDGER" ] && [ ! -e "$TMPDIR/done" ] || return 91
  else
    sleep .02
  fi
  ok first
  if [ "$BEHAVIOR" = timeout ]; then
    row_begin expires
    sleep 60
    ok expires
  elif [ "$BEHAVIOR" = teardown ]; then
    python3 -c 'import os,signal,time; from pathlib import Path; signal.signal(signal.SIGTERM,signal.SIG_IGN); Path(os.environ["RUN_DIR"]+"/child").write_text(str(os.getpid())); time.sleep(60)' &
    SRV=$!
    while [ ! -s "$RUN_DIR/child" ]; do sleep .01; done
  fi
}
'''
        outsider = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
        try:
            result = subprocess.run(["bash", "-c", "\n".join((setup, helpers, cleanup, jobs,
                                     'run_job fixture 0'))], cwd=root,
                env=dict(os.environ, RUN_DIR=str(directory), BEHAVIOR=behavior,
                         FIXTURE_CPU=str(min(os.sched_getaffinity(0)))),
                capture_output=True, text=True, timeout=12)
            self.assertIsNone(outsider.poll(), "teardown signalled a process outside its ancestry")
        finally:
            outsider.terminate()
            outsider.wait(timeout=5)
        job = directory / "jobs/fixture"
        self.assertTrue((job / "ledger").exists(), result.stdout + result.stderr)
        return result, job, [line.split("\t") for line in (job / "ledger").read_text().splitlines()]

    def test_inherited_exit_trap_cannot_finalize_the_workers_active_row(self):
        result, job, rows = self.run_real_job("inherited-finalizer")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + (job / "output.log").read_text())
        child, owner = (self.directory / "inherited-finalizer").read_text().split()
        self.assertNotEqual(child, owner, "the replay must execute in a different process")
        self.assertEqual([(row[0], row[2]) for row in rows], [("ok", "first")])
        self.assertEqual((job / "done").read_text(), "0\t1\t0\n")
        self.assertEqual([(row["label"], row["verdict"]) for row in read_history(self.directory / "history")],
                         [("first", "ok")])

    def test_actual_job_timeout_preserves_partial_ledger_and_publishes_done(self):
        result, job, rows = self.run_real_job("timeout")
        self.assertEqual(result.returncode, 124, result.stdout + result.stderr + (job / "output.log").read_text())
        self.assertEqual([(row[0], row[2]) for row in rows], [("ok", "first"), ("FAIL", "expires")])
        self.assertTrue(all(len(row) == 3 for row in rows))
        self.assertGreaterEqual(float(rows[1][1]), .4)
        self.assertEqual((job / "done").read_text().strip().split("\t"), ["124", "1", "1"])
        history = read_history(self.directory / "history")
        self.assertEqual(len(history), 2)
        self.assertTrue(history[1]["timed_out"])
        self.assertEqual([(row["scored"], row["ledger_label"], row["run_id"]) for row in history],
                         [(True, label, "fixture") for label in ("first", "expires")])
        self.assertTrue((job / "family.tsv").is_file())

    def test_recovery_never_overwrites_partial_rows_or_writes_old_two_columns(self):
        import subprocess
        root = Path(__file__).resolve().parents[1]
        gate = (root / "tests/gate.sh").read_text()
        recover = gate[gate.index("job_recover(){"):gate.index("\njob_finalize(){")]
        directory = self.directory / "jobs/fixture"
        directory.mkdir(parents=True)
        for name in ("ledger", "timings"):
            (directory / name).write_text("ok\t0.123\tfirst\n")
        body = '\njob_label(){ printf "fixture family\\n"; }\njob_recover fixture 0 137\njob_recover fixture 0 137\n'
        result = subprocess.run(["bash", "-uc", recover + body], cwd=root,
            env=dict(os.environ, RUN_DIR=str(self.directory)), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((directory / "ledger").read_text(),
                         "ok\t0.123\tfirst\nFAIL\t0\tfixture family\n")
        self.assertEqual((directory / "done").read_text(), "137\t1\t1\n")

    def test_actual_job_term_ignoring_teardown_is_bounded_and_reaped(self):
        started = time.monotonic()
        result, job, rows = self.run_real_job("teardown")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr + (job / "output.log").read_text())
        self.assertLess(time.monotonic() - started, 9)
        self.assertEqual([(row[0], row[2]) for row in rows], [("ok", "first"), ("FAIL", "fixture family")])
        self.assertEqual((job / "done").read_text().strip().split("\t"), ["1", "1", "1"])
        self.assertIn("forced SIGKILL", (job / "output.log").read_text())
        self.assertIsNone(process_identity(int((self.directory / "child").read_text())))

    def test_actual_abba_timeout_publishes_fragment_at_its_original_position(self):
        import subprocess
        root = Path(__file__).resolve().parents[1]
        gate = (root / "tests/gate.sh").read_text()
        helpers = gate[gate.index("say(){"):gate.index("\nledger_labels(){")]
        cleanup = gate[gate.index("reap_children(){"):gate.index("\ntrap 'cleanup")]
        directory = self.directory
        plan = prepare(directory / "history", [])
        label = "headline ABBA vs last pushed binary"
        plan["rows"][label + " [smoke fixture]"] = dict(timeout_seconds=.3, median_seconds=.1, basis="fixture")
        atomic_json(directory / "plan.json", plan)
        (directory / "ledger").write_text("ok\t.1\tbefore\nok\t.2\tafter\n")
        (directory / "timings").write_text("")
        setup = r'''set -u
PASS=0; FAIL=0; WORKER_PIDS=(); SRV=0; GLOBCASE_ORACLE=0; MMPID=0; ABBA_PID=0; PAUSABLE_PID=0
ROW_PLAN="$RUN_DIR/plan.json"; ROW_HISTORY="$RUN_DIR/history"; ROW_RUN_ID=fixture; TMPDIR="$RUN_DIR"
ABBA_LEDGER="$RUN_DIR/ledger"; ABBA_TIMINGS="$RUN_DIR/timings"; ABBA_PREFIX_ROWS=1; ABBA_PENDING=1
LEDGER="$RUN_DIR/abba.ledger"; TIMINGS="$RUN_DIR/abba.timings"; : > "$LEDGER"; : > "$TIMINGS"
quiet_wait(){ :; }
'''
        body = '''trap 'cleanup || exit 1' EXIT
row_begin "headline ABBA vs last pushed binary" "smoke fixture"
sleep 60
ok "headline ABBA vs last pushed binary"
'''
        result = subprocess.run(["bash", "-c", "\n".join((setup, helpers, cleanup, body))], cwd=root,
            env=dict(os.environ, RUN_DIR=str(directory)), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 124, result.stdout + result.stderr)
        rows = [row.split("\t") for row in (directory / "ledger").read_text().splitlines()]
        self.assertEqual([(row[0], row[2]) for row in rows],
                         [("ok", "before"), ("FAIL", label), ("ok", "after")])
        self.assertGreaterEqual(float(rows[1][1]), .3)

    def test_inherited_root_cleanup_cannot_publish_the_parents_abba_fragment(self):
        import subprocess
        root = Path(__file__).resolve().parents[1]
        gate = (root / "tests/gate.sh").read_text()
        cleanup = gate[gate.index("reap_children(){"):gate.index("\ntrap 'cleanup")]
        (self.directory / "ledger").write_text("ok\t.1\tbefore\nok\t.2\tafter\n")
        (self.directory / "timings").write_text("")
        (self.directory / "abba.ledger").write_text("ok\t.3\tabba\n")
        (self.directory / "abba.timings").write_text("ok\t.3\tabba\n")
        setup = '''set -u
WORKER_PIDS=(); SRV=0; GLOBCASE_ORACLE=0; MMPID=0; ABBA_PID=0; PAUSABLE_PID=0
ABBA_LEDGER="$RUN_DIR/ledger"; ABBA_TIMINGS="$RUN_DIR/timings"; ABBA_PREFIX_ROWS=1; ABBA_PENDING=1
row_unwatch(){ :; }
'''
        body = '''trap 'cleanup || exit 1' EXIT
inherited_exit=$(trap -p EXIT)
(
  eval "$inherited_exit"
  printf '%s\\t%s\\n' "$BASHPID" "$CLEANUP_OWNER" > "$RUN_DIR/inherited-root"
  exit 0
)
[ "$(wc -l < "$ABBA_LEDGER")" = 2 ] || exit 92
'''
        result = subprocess.run(["bash", "-c", "\n".join((setup, cleanup, body))], cwd=root,
            env=dict(os.environ, RUN_DIR=str(self.directory)), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        child, owner = (self.directory / "inherited-root").read_text().split()
        self.assertNotEqual(child, owner)
        self.assertEqual((self.directory / "ledger").read_text(),
                         "ok\t.1\tbefore\nok\t.3\tabba\nok\t.2\tafter\n")
        self.assertEqual((self.directory / "timings").read_text(), "ok\t.3\tabba\n")


class FlakeHistoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def add(self, run_id, verdict="ok", label="row", observation_id=None):
        record(self.directory, run_id=run_id, label=label, seconds=1,
               verdict=verdict, observation_id=observation_id)

    def test_pass_fail_pass_across_runs_keeps_both_transitions_and_rate(self):
        for run_id, verdict in (("run-1", "ok"), ("run-2", "FAIL"), ("run-3", "ok")):
            self.add(run_id, verdict)
        report = verdict_history(self.directory)
        row = report["labels"]["row"]
        self.assertEqual((row["failures"], row["observations"], row["runs"], row["failing_runs"]), (1, 3, 3, 1))
        self.assertEqual(row["failure_fraction"], 1 / 3)
        self.assertEqual(len(row["transitions"]), 2)
        self.assertEqual(row["latest_transition"]["from_run"], "run-2")
        self.assertEqual(row["latest_transition"]["to_verdict"], "ok")
        text = format_verdict_history(report)
        self.assertIn("DEFECT NEEDS FIX: row", text)
        self.assertIn("FAIL observations 1/3 (33.33%)", text)
        self.assertIn("runs containing FAIL 1/3", text)
        self.assertIn("FAIL (run-2) -> ok (run-3)", text)

    def test_stable_pass_and_stable_fail_are_not_verdict_flips(self):
        for run_id in ("first", "second", "third"):
            self.add(run_id, "ok", "passes")
            self.add(run_id, "FAIL", "fails")
        report = verdict_history(self.directory)
        self.assertEqual(report["flipping_labels"], 0)
        self.assertEqual(report["labels"]["fails"]["failures"], 3)
        self.assertNotIn("DEFECT", format_verdict_history(report))

    def test_same_run_duplicates_are_observations_not_extra_runs(self):
        self.add("first", "ok")
        self.add("first", "FAIL")
        self.add("second", "ok")
        row = verdict_history(self.directory)["labels"]["row"]
        self.assertEqual((row["observations"], row["runs"], row["failing_runs"]), (3, 2, 1))
        self.assertEqual(row["mixed_runs"], ["first"])
        self.assertEqual(row["latest_transition"]["from_verdict"], "mixed")
        self.assertIn("repeated-label contexts are not distinguishable", format_verdict_history(verdict_history(self.directory)))

    def test_single_mixed_run_is_still_a_loud_defect(self):
        self.add("only", "ok")
        self.add("only", "FAIL")
        report = verdict_history(self.directory)
        row = report["labels"]["row"]
        self.assertTrue(row["verdict_flipped"])
        self.assertEqual(row["transitions"], [])
        text = format_verdict_history(report)
        self.assertIn("DEFECT NEEDS FIX: row", text)
        self.assertIn("latest none between runs", text)
        self.assertIn("only (1/2 FAIL)", text)

    def test_every_flipping_label_is_printed_without_truncation(self):
        for index in range(24):
            self.add("first", "ok", f"row-{index:02}")
            self.add("second", "FAIL", f"row-{index:02}")
        report = verdict_history(self.directory)
        self.assertEqual(report["flipping_labels"], 24)
        self.assertEqual(format_verdict_history(report).count("DEFECT NEEDS FIX:"), 24)

    def test_interrupted_run_keeps_completed_observations(self):
        self.add("complete", "ok")
        # There is deliberately no run-complete marker. An interrupted run's red row is evidence.
        self.add("interrupted", "FAIL")
        before = (self.directory / HISTORY_FILE).read_bytes()
        row = verdict_history(self.directory)["labels"]["row"]
        self.assertEqual((row["runs"], row["failures"]), (2, 1))
        self.assertEqual((self.directory / HISTORY_FILE).read_bytes(), before)

    def test_incomplete_append_is_loud_and_preserves_existing_bytes(self):
        self.add("first", "ok")
        self.add("second", "FAIL")
        path = self.directory / HISTORY_FILE
        path.write_bytes(path.read_bytes() + b'{"schema":1')
        before = path.read_bytes()
        with self.assertRaisesRegex(ValueError, "incomplete final record"):
            verdict_history(self.directory)
        with self.assertRaisesRegex(ValueError, "incomplete final record"):
            self.add("third", "ok")
        self.assertEqual(path.read_bytes(), before)

    def test_duplicate_event_is_rejected_without_changing_denominator(self):
        self.add("first", "ok", observation_id="same-event")
        before = (self.directory / HISTORY_FILE).read_bytes()
        with self.assertRaisesRegex(ValueError, "duplicate observation_id"):
            self.add("first", "FAIL", observation_id="same-event")
        self.assertEqual((self.directory / HISTORY_FILE).read_bytes(), before)
        self.assertEqual(verdict_history(self.directory)["labels"]["row"]["observations"], 1)

    def test_parallel_process_appends_keep_every_observation(self):
        import multiprocessing
        context = multiprocessing.get_context("fork")
        children = [context.Process(target=record, args=(self.directory,), kwargs={
            "run_id": "parallel", "label": "row", "seconds": 1,
            "verdict": "FAIL" if index % 3 == 0 else "ok", "observation_id": f"event-{index}"})
            for index in range(12)]
        for child in children:
            child.start()
        try:
            for child in children:
                child.join(timeout=10)
                self.assertEqual(child.exitcode, 0)
        finally:
            for child in children:
                if child.is_alive():
                    child.terminate()
                    child.join(timeout=5)
        rows = read_history(self.directory)
        self.assertEqual({row["observation_id"] for row in rows}, {f"event-{index}" for index in range(12)})
        row = verdict_history(self.directory)["labels"]["row"]
        self.assertEqual((row["observations"], row["failures"], row["runs"]), (12, 4, 1))
        self.assertEqual(row["mixed_runs"], ["parallel"])

    def test_run_order_uses_first_durable_observation_not_wall_clock(self):
        self.add("first", "ok")
        self.add("second", "FAIL")
        self.add("first", "ok")  # Late parallel completion from the earlier run.
        path = self.directory / HISTORY_FILE
        rows = read_history(self.directory)
        for index, row in enumerate(rows):
            row["recorded_at"] = 300 - index
        path.write_text("".join(json.dumps(row) + "\n" for row in rows))
        row = verdict_history(self.directory)["labels"]["row"]
        self.assertEqual([run["run_id"] for run in row["run_history"]], ["first", "second"])
        self.assertEqual(row["latest_transition"]["to_run"], "second")

    def test_report_cli_surfaces_flips_without_overriding_current_gate_verdict(self):
        import subprocess
        self.add("before", "FAIL")
        self.add("after", "ok")
        command = [sys.executable, str(Path(__file__).resolve()), "report", "--history", str(self.directory)]
        result = subprocess.run(command, check=True, text=True, capture_output=True)
        self.assertIn("DEFECT NEEDS FIX", result.stdout)
        data = json.loads(subprocess.check_output(command + ["--json"], text=True))
        self.assertEqual(data["flipping_labels"], 1)
        path = self.directory / HISTORY_FILE
        path.write_bytes(path.read_bytes() + b'{')
        result = subprocess.run(command, text=True, capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("GATE HISTORY ERROR", result.stderr)

    def test_empty_history_reports_zero_without_creating_or_truncating_it(self):
        report = verdict_history(self.directory)
        self.assertEqual((report["observations"], report["runs"], report["flipping_labels"]), (0, 0, 0))
        self.assertFalse((self.directory / HISTORY_FILE).exists())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--history", type=Path, required=True)
    p.add_argument("--import-ledger", type=Path, action="append", default=[])
    p.add_argument("--output", type=Path)
    p.add_argument("--multiplier", type=float, default=DEFAULT_MULTIPLIER)
    p.add_argument("--floor", "--floor-seconds", dest="floor", type=float, default=DEFAULT_FLOOR)
    p.add_argument("--fallback", "--fallback-seconds", dest="fallback", type=float, default=DEFAULT_FALLBACK)
    p = sub.add_parser("report", help="surface verdict flips and observed failure rates across all runs")
    p.add_argument("--history", type=Path, required=True)
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("record")
    p.add_argument("--history", type=Path, required=True)
    p.add_argument("--run-id", required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--seconds", type=float, required=True)
    p.add_argument("--verdict", choices=("ok", "FAIL"), required=True)
    p.add_argument("--timed-out", action="store_true")
    p.add_argument("--observation-id")
    p.add_argument("--scored", type=int, choices=(0, 1), default=1)
    p.add_argument("--ledger-label")
    p = sub.add_parser("budget")
    p.add_argument("--plan", type=Path, required=True)
    p.add_argument("--label", required=True)
    p = sub.add_parser("canonical")
    p.add_argument("label")
    p = sub.add_parser("watch")
    p.add_argument("--pid", type=int, required=True)
    p.add_argument("--parent-start", type=int, required=True)
    p.add_argument("--marker", type=Path, required=True)
    timing = p.add_mutually_exclusive_group(required=True)
    timing.add_argument("--seconds", type=float)
    timing.add_argument("--deadline", type=float)
    p.add_argument("--grace", type=float, default=5.0)
    p.add_argument("--generation")
    p.add_argument("--cancel-request", type=Path)
    p.add_argument("--cancel-receipt", type=Path)
    p = sub.add_parser("signal-child")
    p.add_argument("--pid", type=int, required=True)
    p.add_argument("--start", type=int, required=True)
    p.add_argument("--parent", type=int, required=True)
    p.add_argument("--parent-start", type=int, required=True)
    p = sub.add_parser("identity")
    p.add_argument("--pid", type=int, required=True)
    p = sub.add_parser("reap-tree")
    p.add_argument("--pid", type=int, required=True)
    p.add_argument("--parent-start", type=int, required=True)
    p.add_argument("--grace", type=float, default=5.0)
    p = sub.add_parser("dependency-label")
    p.add_argument("arguments", nargs=argparse.REMAINDER)
    p = sub.add_parser("abba-context")
    p.add_argument("arguments", nargs=argparse.REMAINDER)
    sub.add_parser("self-test")
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            result = prepare(args.history, args.import_ledger, multiplier=args.multiplier,
                             floor=args.floor, fallback=args.fallback)
            if args.output:
                atomic_json(args.output, result)
            else:
                print(json.dumps(result, sort_keys=True, indent=2, allow_nan=False))
        elif args.command == "report":
            result = verdict_history(args.history)
            print(json.dumps(result, sort_keys=True, indent=2, allow_nan=False)
                  if args.json else format_verdict_history(result))
        elif args.command == "record":
            record(args.history, run_id=args.run_id, label=args.label, seconds=args.seconds,
                   verdict=args.verdict, timed_out=args.timed_out, observation_id=args.observation_id,
                   scored=bool(args.scored), ledger_label=args.ledger_label)
        elif args.command == "budget":
            row = budget(json.loads(args.plan.read_text()), args.label)
            median = "-" if row["median_seconds"] is None else f"{row['median_seconds']:g}"
            print(f"{row['timeout_seconds']:g}\t{median}\t{row['basis']}")
        elif args.command == "canonical":
            print(canonical_label(args.label))
        elif args.command == "watch":
            watch(args.pid, args.parent_start, args.marker, seconds=args.seconds,
                  deadline=args.deadline, grace=args.grace, generation=args.generation,
                  cancel_request=args.cancel_request, cancel_receipt=args.cancel_receipt)
        elif args.command == "signal-child":
            signal_child(args.pid, args.start, args.parent, args.parent_start)
        elif args.command == "identity":
            identity = process_identity(args.pid)
            if identity is None:
                raise ValueError("PID does not exist")
            print(identity[1])
        elif args.command == "reap-tree":
            result = reap_tree(args.pid, args.parent_start, grace=args.grace)
            if result["survivors"]:
                print(f"GATE TEARDOWN ERROR: owned PID/start survivors after KILL: {result['survivors']}",
                      file=sys.stderr)
                return 1
            if result["forced"]:
                print(f"GATE TEARDOWN ERROR: forced SIGKILL after {args.grace:g}s for owned PID/start: {result['forced']}",
                      file=sys.stderr)
                return 3 # Everything stopped: the shell can reap, but must keep the verdict red.
        elif args.command in ("dependency-label", "abba-context"):
            arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
            print(dependency_label(arguments) if args.command == "dependency-label" else abba_context(arguments))
        else:
            suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(case)
                                       for case in (HistoryTests, FlakeHistoryTests))
            return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    except (OSError, ValueError, TypeError, KeyError, sqlite3.Error) as exc:
        print(f"GATE HISTORY ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
