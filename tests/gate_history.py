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
import statistics
import sys
import tempfile
import time
import unittest
import uuid


SCHEMA = 1
HISTORY_FILE = "row-observations.jsonl"
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


def record(directory: Path, *, run_id: str, label: str, seconds: float,
           verdict: str, timed_out: bool = False, observation_id: str | None = None) -> None:
    row = validate_observation({"schema": SCHEMA, "timing": "own-row",
        "run_id": run_id, "observation_id": observation_id or uuid.uuid4().hex,
        "label": canonical_label(label), "seconds": seconds, "verdict": verdict,
        "timed_out": timed_out, "recorded_at": time.time()})
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / HISTORY_FILE
    # A single locked append protects parallel workers. Verify the complete existing file
    # before mutation: corrupt history must fail visibly, never silently disappear.
    with path.open("a+", encoding="utf-8") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        stream.seek(0)
        existing = decode_history(stream.read(), path)
        if any(old["observation_id"] == row["observation_id"] for old in existing):
            raise ValueError(f"{path}: duplicate observation_id {row['observation_id']}")
        stream.write(json.dumps(row, sort_keys=True, allow_nan=False) + "\n")
        stream.flush()
        os.fsync(stream.fileno())


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
        rows[label] = item
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


def watch(pid: int, parent_start: int, marker: Path, *, seconds: float | None = None,
          deadline: float | None = None, grace: float = 5.0) -> None:
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
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
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
            identity = process_identity(child)
            if identity is not None and identity[1] == born:
                remaining = True
                break
        if not remaining:
            return
        time.sleep(min(0.05, max(0.0, end - time.monotonic())))
    for child, born in owned.items():
        signal_identity(child, born, signal.SIGKILL)


class HistoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
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


    def shell_row(self, body, timeout=1.0):
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
'''
        result = subprocess.run(['bash', '-c', setup + helpers + '\n' + body], cwd=root,
            env=dict(os.environ, FIXTURE=str(directory)), capture_output=True, text=True, timeout=10)
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

    def test_real_shell_records_only_the_rows_own_duration(self):
        result, rows, history = self.shell_row('sleep 1\nrow_begin fixture\nsleep .03\nok fixture\n', 5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(rows[0][0], 'ok', result.stdout)
        self.assertGreaterEqual(float(rows[0][1]), .03)
        self.assertLess(float(rows[0][1]), 1)
        self.assertEqual(float(rows[0][1]), history[0]['seconds'])


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
    p = sub.add_parser("identity")
    p.add_argument("--pid", type=int, required=True)
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
                   verdict=args.verdict, timed_out=args.timed_out, observation_id=args.observation_id)
        elif args.command == "budget":
            row = budget(json.loads(args.plan.read_text()), args.label)
            median = "-" if row["median_seconds"] is None else f"{row['median_seconds']:g}"
            print(f"{row['timeout_seconds']:g}\t{median}\t{row['basis']}")
        elif args.command == "canonical":
            print(canonical_label(args.label))
        elif args.command == "watch":
            watch(args.pid, args.parent_start, args.marker, seconds=args.seconds,
                  deadline=args.deadline, grace=args.grace)
        elif args.command == "identity":
            identity = process_identity(args.pid)
            if identity is None:
                raise ValueError("PID does not exist")
            print(identity[1])
        else:
            suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(case)
                                       for case in (HistoryTests, FlakeHistoryTests))
            return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"GATE HISTORY ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
