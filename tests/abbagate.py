#!/usr/bin/env python3
"""Same-session headline ABBA gate. No historical rate is a performance target.

WHY NOT STORED REFERENCE NUMBERS. A stored rate goes stale the moment the kernel, compiler,
microcode or machine changes; this tree's old tests/gate_refs.txt was pinned to a kernel that no
longer runs, and the gate's own comments then called every verdict provisional. Measuring both arms
in ONE session on ONE box removes drift, thermal state and machine configuration as variables. The
only difference left between the arms is the code.

THE REFERENCE is the caller's --reference-binary, whose digest is recorded and matched against a
last-push manifest pin when available. Without that argument, origin/cpp is resolved to a full
commit id, then matched against /home/user/Projects/bench-bins/MANIFEST.md. A filename containing
"headline" is NOT evidence of identity; two different digests for one commit are ambiguous and skip.
Missing or unbuildable reference => loud SKIP, never a pass: a correctness-only run must not be able
to call itself a clean gate.

ABBA, NOT A-THEN-B. Every cell runs reference, candidate, candidate, reference and takes the paired
difference. Non-interleaved A/B has produced wrong verdicts on this box before, and a 20-second
window exposes drift that a 90-second window hides. ABBA cancels a LINEAR trend when the run
midpoints are evenly spaced; it cannot promise cancellation of arbitrary interference, periodic
noise or nonlinear drift, so both arm spreads are always reported and a quiet box still matters.

THE THRESHOLD IS DERIVED, NOT CHOSEN. T for a block IS that block's observed reference spread
(100*|A2-A1|/Abar). There is no 3% default, no fixed noise floor, no multiplier, no historical
calibration, and no candidate-dependent widening. A quiet box therefore produces a STRICTER gate,
which is the correct incentive. Two samples measure an observed span, not a confidence interval:
the gate detects losses larger than that span, and a loss inside it is below the session's own
demonstrated resolution -- say that, do not pretend to more.

THE 2% INSTABILITY BOUNDARY IS A VALIDITY CHECK, NOT A REGRESSION ALLOWANCE. An arm whose spread
exceeds 2% fails the MEASUREMENT. Without this, a wildly noisy reference manufactures a permissive
threshold and waves a real regression through. A failed block is never rerun until it happens to
pass, and the best of several runs is never selected.

SATURATION IS A PRECONDITION, not a nice-to-have: an unsaturated cell has headroom that absorbs a
regression, so it cannot detect one at any repetition count. Pinned load levels run one ABBA block
and must still satisfy the busy floor. Unpinned cells, or --escalate, search until the fastest arm
stops gaining AND measured busy is at the required level. Depth 1 is exempt and
scored as latency -- it is round-trip bound by Little's law. Process CPU is NOT substituted for busy
percentage: doing so hides exactly the unsaturated case this check exists to catch.

THE VERDICT NAMES THE WORST CELL. It is the conjunction of cell verdicts; no average across GET,
SET, thread modes or cells can hide the one cell that fails. A failed precondition outranks passing
cells. --only is a diagnostic selection and yields PARTIAL with exit 3, never a complete-tier pass.

Exit 0: every cell passed; 1: failure; 3: loud skip or successful partial diagnostic.
Comparison PASS additionally requires a recent matching standing null. Missing/invalid controls
leave successful measurements PARTIAL and untrusted. --collect-null 1 freezes identical arms and
collects its own null verdict without a prior control; its outer PARTIAL/3 cannot gate a push.
--self-test is serverless. All other runs own and reap only their subprocess PIDs.
"""
import argparse
from dataclasses import asdict, dataclass, replace
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time

from _lib import Conn
from gateplan import validate_axes, read_topology, permitted_cpus, default_physical
from gate_quiet import QuietMonitor, QuietViolation
from abba_saturation import parse_snapshot, productive_saturation, self_test as saturation_self_test
from gate_receipt import harness_fingerprint, read_json
from abba_evidence import match_null, null_result
from abba_instrument import instrument_fingerprint
from abba_workloads import (workload_arguments, prepare_long_keys, merged_tail,
                            require_workload_witness, workload_command_names,
                            memtier_workload_counts, require_workload_accounting)

ROOT = Path(__file__).resolve().parents[1]
WINDOW = 20
WARMUP = 3
TAIL = 5
KEYS = 2_000_000
MIN_BUSY = 98.0        # the busy level we PREFER, and still record; no longer a hard gate
BUSY_FLOOR = 95.0      # below this a cell is rejected outright, plateau or not
#
# SATURATION IS ESTABLISHED BY A RATE PLATEAU, NOT BY A BUSY PERCENTAGE ALONE (owner ruling
# 2026-09-10). Demanding >=98% busy in every run fails a candidate FOR BEING FASTER: a quicker
# server does the same offered work with less CPU, so on 2026-09-10 the h12 SET cell sat at 97.1%
# busy while delivering 24.6 Mops/s against the reference's 22.2 at 98.4% -- an 11% gain the gate
# refused to certify. Adding 50% more load generator threads did not move it; the server simply
# could not be pinned at 98% by any load this box can offer.
#
# The evidence that no headroom is absorbing a regression is that MORE LOAD NO LONGER RAISES THE
# RATE. That is measured directly, and it also fixes a second defect: escalation used to judge on
# the HIGHEST instance count, but past the optimum a write cell falls into congestion collapse --
# h12 peaked at 26.6 Mops/s with 2 instances and decayed to 24.6 by 16, so the verdict was being
# taken on a deliberately degraded block. The peak block is the measurement; blocks above it exist
# to PROVE it is a peak.
# Load-generator escalation ladder. 8 was not enough: on 2026-09-10 the h12 SET cell left the
# CANDIDATE arm at 97.1% busy while the reference sat at 98.4%, because the candidate was 10.7%
# faster and therefore did the same offered work with less CPU. A faster server needs MORE load to
# saturate, so capping the ladder at 8 makes an improvement fail the saturation precondition -- the
# gate would reject exactly the changes it exists to certify. The 98% floor itself is correct and
# stays: it is the project's "escalate until the fastest arm stops gaining AND server idle <= 2%".
LADDER = (1, 2, 4, 8, 16)
# Project measurement-integrity boundary, NOT the regression tolerance.
MAX_SPREAD = 2.0
ORDER = ("A", "B", "B", "A")


class Skip(RuntimeError):
    pass


@dataclass(frozen=True)
class Cell:
    id: str
    mode: str
    read_local: int
    overlap: int
    reorder: int
    op: str
    depth: int
    conns: int
    instances: int = 0     # PINNED load-generator instance count; 0 = unpinned, search for it
    atomic: int = 1
    score: str = "auto"
    mix: str = "-"         # READ:WRITE for MIX/MIX8; short:long for REORDER
    smoke: bool = False
    pin_required: bool = False

    @property
    def metric(self):
        return ("latency_ms" if self.depth == 1 else "rate") if self.score == "auto" else {
            "rate": "rate", "latency": "latency_ms", "p999": "p999_ms"}[self.score]


def read_cells(path):
    cells = []
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = [x.strip() for x in line.split("|")]
        if len(fields) not in (11, 15):
            raise ValueError(f"{path}:{lineno}: expected 11 legacy or 15 extended pipe-separated fields")
        ident, mode, rl, ov, ro, op, depth, conns, _measured, _busy, pinned = fields[:11]
        if (not re.fullmatch(r"[A-Za-z0-9_-]+", ident) or mode not in ("1s", "2s")
                or op not in ("GET", "SET", "MGET", "MSET", "MIX", "MIX8", "REORDER")
                or not re.fullmatch(r"p[1-9][0-9]*", depth)
                or not re.fullmatch(r"[1-9][0-9]*", conns)
                or any(not re.fullmatch(prefix + "=[01]", value)
                       for prefix, value in (("rl", rl), ("ov", ov), ("ro", ro)))):
            raise ValueError(f"{path}:{lineno}: unsupported/malformed cell: {line}")
        # The last column PINS the load level. It is a test parameter, like the connection count
        # beside it -- NOT a stored performance number, which this tier refuses on principle. It
        # exists because escalating from one instance on every cell of every run re-derives a search
        # whose answer we already have, at four measurements a rung. Unpinned ("-") falls back to
        # the search, and the run says so.
        extra = {}
        if len(fields) == 15:
            atomic, score, mix, smoke = fields[11:]
            if (not re.fullmatch(r"atomic=[01]", atomic)
                    or score not in ("score=rate", "score=latency", "score=p999")
                    or not re.fullmatch(r"mix=(-|[1-9][0-9]*:[1-9][0-9]*)", mix)
                    or not re.fullmatch(r"smoke=[01]", smoke)
                    or not re.fullmatch(r"-|[1-9][0-9]*", pinned)):
                raise ValueError(f"{path}:{lineno}: malformed extended workload fields")
            extra = dict(atomic=int(atomic[-1]), score=score[6:], mix=mix[4:],
                         smoke=smoke[-1] == "1", pin_required=int(depth[1:]) > 1)
        cell = Cell(ident, mode, int(rl[-1]), int(ov[-1]), int(ro[-1]),
                    op, int(depth[1:]), int(conns),
                    int(pinned) if re.fullmatch(r"[1-9][0-9]*", pinned) else 0, **extra)
        if ((cell.op in ("MIX", "MIX8", "REORDER")) != (cell.mix != "-")
                or (cell.op == "REORDER") != (cell.metric == "p999_ms")
                or (cell.depth == 1 and cell.metric == "rate")):
            raise ValueError(f"{path}:{lineno}: workload, mix and scoring disagree")
        cells.append(cell)
    if not cells or len({c.id for c in cells}) != len(cells):
        raise ValueError("headline cells must be nonempty with unique IDs")
    return cells


def selected_cells(cells, subset, only=""):
    selected = [cell for cell in cells if subset == "full" or cell.smoke]
    if not selected:
        raise ValueError(f"cell source has no {subset} cells")
    if only:
        requested = set(only.split(","))
        if requested - {cell.id for cell in selected}:
            raise ValueError("--only names a cell absent from the selected subset")
        selected = [cell for cell in selected if cell.id in requested]
    return selected


def coverage(cells):
    return {"count": len(cells), "ids": [cell.id for cell in cells],
            "modes": sorted({cell.mode for cell in cells}),
            "operations": sorted({cell.op for cell in cells}),
            "commands": sorted({command for cell in cells for command in workload_command_names(cell)}),
            "depths": sorted({cell.depth for cell in cells}),
            "connections": sorted({cell.conns for cell in cells}),
            "atomic": sorted({cell.atomic for cell in cells}),
            "scores": sorted({cell.metric for cell in cells}),
            "pending_pins": [cell.id for cell in cells if cell.depth > 1 and not cell.instances]}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capture(argv, cwd=ROOT, timeout=30):
    return subprocess.run([str(a) for a in argv], cwd=cwd, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)


def git(*args):
    p = capture(["git", *args])
    if p.returncode:
        raise RuntimeError(p.stdout.strip())
    return p.stdout.strip()


def cpus(spec):
    if not spec:
        return []
    result = set()
    for part in spec.split(","):
        if not re.fullmatch(r"[0-9]+(-[0-9]+)?", part):
            raise ValueError(f"invalid CPU list: {spec}")
        bounds = [int(x) for x in part.split("-")]
        lo, hi = bounds[0], bounds[-1]
        if hi < lo:
            raise ValueError(f"invalid CPU range: {part}")
        result.update(range(lo, hi + 1))
    return sorted(result)


def cpu_string(values):
    return ",".join(str(c) for c in values)


def check_placement(server_cpus, load_cpus, server_smt=(), load_smt=()):
    validate_axes(server_cpus, server_smt, load_cpus, load_smt)
    if not 2 <= len(server_cpus) <= 32:
        raise ValueError("ABBA requires 2-32 physical server cores; the headline geometry caps at 32")


def resolve_geometry(args):
    """Default to 32 real server cores and lend every other permitted core to load."""
    if all(getattr(args, key) is not None for key in ("server_cores", "load_cores", "load_smt")):
        return
    topology = read_topology()
    available = permitted_cpus(topology)
    physical = default_physical(available, topology)
    server = None if args.server_cores is None else cpus(args.server_cores)
    load = None if args.load_cores is None else cpus(args.load_cores)
    if server is None and load is None:
        if len(physical) <= 32:
            raise ValueError("ABBA default needs 32 physical server cores plus separate load cores; "
                             "supply explicit CPU axes for a smaller diagnostic geometry")
        server, load = physical[:32], physical[32:]
    elif server is None or load is None:
        supplied = load if server is None else server
        occupied = {topology[cpu] for cpu in supplied}
        remaining = [cpu for cpu in physical if topology[cpu] not in occupied]
        if server is None:
            server = remaining[:32]
        else:
            load = remaining
    args.server_cores, args.load_cores = cpu_string(server), cpu_string(load)
    if args.load_smt is None:
        # Omission enables generator headroom; an explicitly empty --load-smt
        # reserves those siblings. Server siblings can never be assigned to load.
        occupied = {topology[cpu] for cpu in server}
        args.load_smt = cpu_string(sorted({sibling for cpu in load for sibling in topology[cpu]
                                           if sibling in available and sibling not in load
                                           and topology[cpu] not in occupied}))


def select_port(ports, port):
    if ports is None:
        first = last = 8700 if port is None else port
    else:
        if not re.fullmatch(r"[0-9]+-[0-9]+", ports):
            raise ValueError("--ports must be first-last")
        first, last = map(int, ports.split("-"))
    if not 1 <= first <= last <= 65535:
        raise ValueError("--ports must be an ascending range within 1-65535")
    chosen = first if port is None else port
    if not first <= chosen <= last:
        raise ValueError(f"--port {chosen} lies outside --ports {first}-{last}")
    return chosen, (first, last)


def load_layout(load_cpus, n, conns):
    """Keep the cell's TOTAL connections fixed; partition physical/SMT pairs together."""
    if not 1 <= n <= conns:
        raise ValueError("every load instance needs at least one connection")
    groups, seen = [], set()
    for cpu in load_cpus:
        if cpu in seen:
            continue
        topology = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        siblings = set(cpus(topology.read_text().strip())) if topology.exists() else {cpu}
        group = sorted(siblings.intersection(load_cpus))
        groups.append(group)
        seen.update(group)
    if n > len(groups):
        raise ValueError("more load instances than physical CPU groups")
    assignments = []
    for i in range(n):
        assigned = sorted(c for g in groups[i * len(groups) // n:(i + 1) * len(groups) // n]
                          for c in g)
        assignments.append(assigned)
    # Existing cells include pin=3 with 512 TOTAL connections. Rounding that to 510
    # changes the workload; skipping it never runs the row. Preserve the existing
    # equal layouts when divisible. Otherwise distribute whole clients per thread:
    # 16 threads x (10,11,11) clients preserves 512 and the generator's thread count.
    # Splitting 170/171/171 would force only 10/9/9 threads under memtier's -t/-c grammar.
    common_threads = max(t for t in range(1, min(16, min(map(len, assignments)), conns // n) + 1)
                         if conns % t == 0)
    client_units = conns // common_threads
    result = []
    for i, assigned in enumerate(assignments):
        if conns % n:
            clients = (i + 1) * client_units // n - i * client_units // n
            result.append({"cpus": assigned, "threads": common_threads, "clients": clients})
            continue
        per_instance = conns // n
        threads = max(t for t in range(1, min(16, len(assigned), per_instance) + 1)
                      if per_instance % t == 0)
        result.append({"cpus": assigned, "threads": threads, "clients": per_instance // threads})
    return result


def spread(a, b):
    return 200.0 * abs(a - b) / (a + b)


def paired(runs, metric="rate"):
    if len(runs) != 4 or tuple(r["arm"] for r in runs) != ORDER:
        raise ValueError("measurements must be A1, B1, B2, A2 (ABBA)")
    values = [r[metric] for r in runs]
    if any(not math.isfinite(v) or v <= 0 for v in values):
        raise ValueError(f"invalid {metric}: {values}")
    a1, b1, b2, a2 = values
    a, b = (a1 + a2) / 2, (b1 + b2) / 2
    # Adjacent differences, with both pairs oriented candidate minus reference.
    delta = 100 * ((b1 - a1) + (b2 - a2)) / (a1 + a2)
    return {"metric": metric, "reference": [a1, a2], "candidate": [b1, b2],
            "reference_mean": a, "candidate_mean": b, "delta_pct": delta,
            "pair_deltas_pct": [100 * (b1 - a1) / a1, 100 * (b2 - a2) / a2],
            "reference_spread_pct": spread(a1, a2), "candidate_spread_pct": spread(b1, b2),
            "threshold_pct": spread(a1, a2)}


def fastest_mean(round_):
    p = paired(round_["runs"])
    return max(p["reference_mean"], p["candidate_mean"])


def peak_index(rounds):
    """Index of the block where the fastest arm peaked.

    Escalating past the peak is congestion, not saturation, so the peak block is what gets judged.
    """
    return max(range(len(rounds)), key=lambda i: fastest_mean(rounds[i]))


def assess(cell, rounds):
    peak = peak_index(rounds)
    current = rounds[peak]
    rate = paired(current["runs"])
    p = paired(current["runs"], cell.metric)
    reasons = []
    for name in ("reference", "candidate"):
        if p[f"{name}_spread_pct"] > MAX_SPREAD:
            reasons.append(f"{name} spread exceeds the project's {MAX_SPREAD:g}% stability boundary")
    # Positive loss always means regression, for both throughput and latency.
    loss = -p["delta_pct"] if cell.metric == "rate" else p["delta_pct"]
    if loss > p["threshold_pct"]:
        reasons.append("paired regression exceeds measured reference spread")
    long_tail = None
    if cell.metric == "p999_ms":
        # Short-command tail is the reorder benefit, but it may not be bought by
        # starving long commands. Both class tails use the same ABBA decision law.
        long_tail = paired(current["runs"], "long_p999_ms")
        if any(long_tail[f"{arm}_spread_pct"] > MAX_SPREAD for arm in ("reference", "candidate")):
            reasons.append("long-command p99.9 exceeds the stability boundary")
        if long_tail["delta_pct"] > long_tail["threshold_pct"]:
            reasons.append("long-command p99.9 regression exceeds measured reference spread")
    gain, plateau_noise = None, None
    if cell.depth > 1:
        # A peak is only a peak if something above it failed to beat it. Without a higher probe the
        # curve may still be climbing and this block is simply the last one we happened to run.
        if cell.instances and len(rounds) == 1 and rounds[0]["instances"] == cell.instances:
            # PINNED: the search was run once and its outcome recorded in the cells file, so this
            # run does not re-derive it. What it must still prove is that the pin STILL HOLDS --
            # otherwise a candidate that outgrows the pinned load is silently measured in headroom,
            # which is the exact failure this tier exists to prevent. Busy is the only saturation
            # evidence available without a second rung, so it is checked and its failure names the
            # remedy rather than just reporting a number.
            if any(r["busy_pct"] < BUSY_FLOOR for r in current["runs"]):
                reasons.append(
                    f"pinned load level {cell.instances} no longer saturates this cell "
                    f"(busy {min(r['busy_pct'] for r in current['runs']):.1f}% < {BUSY_FLOOR:g}%); "
                    f"re-pin it with --escalate and update the cells file")
        elif peak == len(rounds) - 1:
            reasons.append("no higher-instance saturation probe above the peak block")
        else:
            above = paired(rounds[peak + 1]["runs"])
            fast = max(rate["reference_mean"], rate["candidate_mean"])
            beyond = max(above["reference_mean"], above["candidate_mean"])
            gain = 100 * (beyond / fast - 1)
            plateau_noise = max(above["reference_spread_pct"], rate["reference_spread_pct"])
            if gain > plateau_noise:
                reasons.append("fastest arm is still gaining with more load instances")
        if not (cell.instances and len(rounds) == 1) and any(
                r["busy_pct"] < BUSY_FLOOR for r in current["runs"]):
            reasons.append(f"server below the {BUSY_FLOOR:g}% busy floor in some ABBA run")
    return {**p, "throughput": rate, "long_tail": long_tail, "instances": current["instances"],
            "busy_pct_abba": [r["busy_pct"] for r in current["runs"]],
            "loss_pct": loss, "margin_pct": loss - p["threshold_pct"],
            "fastest_gain_pct": gain, "plateau_noise_pct": plateau_noise,
            "saturation_exempt": cell.depth == 1,
            "verdict": "FAIL" if reasons else "PASS", "reasons": reasons}


def saturation_done(cell, rounds):
    """Stop escalating once the peak block is proven -- i.e. a HIGHER instance count exists and did
    not beat it -- and that peak block is itself stable. Escalating further only walks deeper into
    congestion and cannot change the verdict, since the peak is what gets judged."""
    if cell.depth == 1:
        return True
    if len(rounds) < 2:
        return False
    peak = peak_index(rounds)
    if peak == len(rounds) - 1:
        return False                      # still climbing; the top block is the best so far
    a = assess(cell, rounds)
    return (a["fastest_gain_pct"] is not None
            and a["fastest_gain_pct"] <= a["plateau_noise_pct"]
            and min(a["busy_pct_abba"]) >= BUSY_FLOOR
            and a["throughput"]["reference_spread_pct"] <= MAX_SPREAD
            and a["throughput"]["candidate_spread_pct"] <= MAX_SPREAD)


def overall(rows):
    failed = [r for r in rows if r["verdict"] != "PASS"]
    pool = failed or rows
    # A precondition/error failure outranks a throughput win elsewhere.
    worst = max(pool, key=lambda r: (not bool(r.get("assessment")),
                                    r.get("assessment", {}).get("margin_pct", 0)))
    return ("FAIL" if failed else "PASS"), worst["cell"]["id"]


def manifest_reference(directory, commit):
    manifest = directory / "MANIFEST.md"
    if not manifest.is_file():
        return None
    matches = []
    for line in manifest.read_text().splitlines():
        if not line.startswith("|"):
            continue
        hashes = re.findall(r"(?<![0-9a-f])[0-9a-f]{7,40}(?![0-9a-f])", line)
        if not any(commit.startswith(h) for h in hashes):
            continue
        names = re.findall(r"tomokv-[A-Za-z0-9_.-]+", line)
        for name in names:
            binary = directory / name
            if binary.is_file() and os.access(binary, os.X_OK):
                matches.append(binary.resolve())
    matches = sorted(set(matches))
    if len(matches) > 1 and len({sha256(p) for p in matches}) != 1:
        raise Skip(f"ambiguous manifest: multiple different binaries for origin/cpp {commit}")
    return matches[0] if matches else None


class Children:
    def __init__(self):
        self.active = []

    def start(self, argv, log, cwd):
        with log.open("w") as stream:
            p = subprocess.Popen([str(a) for a in argv], cwd=cwd, stdout=stream,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        self.active.append(p)
        return p

    def stop(self, p):
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=10)
        if p in self.active:
            self.active.remove(p)

    def close(self):
        for p in list(reversed(self.active)):
            self.stop(p)


def stop_build(process):
    if process.poll() is not None:
        return
    # make and its compiler children have a private session. Find that session's members and
    # signal their exact PIDs; command-line matching can match the gate's own invoking shell.
    process.send_signal(signal.SIGSTOP)
    owned = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit() or int(entry.name) in (os.getpid(), os.getppid()):
            continue
        try:
            fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
            if int(fields[3]) == process.pid and int(entry.name) != process.pid:
                owned.append(int(entry.name))
        except (FileNotFoundError, ProcessLookupError):
            continue
    for pid in owned:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    process.kill()
    process.wait()


def resolve_reference(args, out):
    if args.reference_binary is not None:
        binary = args.reference_binary.resolve()
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise Skip(f"reference executable unavailable: {binary}")
        # An explicit path is the caller's identity assertion. Record its digest and any matching
        # pin, without inventing a source commit when the supplied binary has no manifest entry.
        provenance = {"source": "explicit --reference-binary", "path": str(binary),
                      "sha256": sha256(binary), "commit": "caller-supplied, unverified"}
        try:
            commit = git("rev-parse", "--verify", "origin/cpp^{commit}")
            provenance["last_pushed_commit"] = commit
            pinned = manifest_reference(args.bench_bins, commit)
            if pinned and sha256(pinned) == provenance["sha256"]:
                provenance.update(commit=commit, ref="origin/cpp",
                                  manifest=str(args.bench_bins / "MANIFEST.md"))
        except (RuntimeError, Skip):
            pass
        return binary, provenance
    try:
        commit = git("rev-parse", "--verify", "origin/cpp^{commit}")
    except RuntimeError as e:
        raise Skip(f"no origin/cpp reference: {e}") from e
    binary = manifest_reference(args.bench_bins, commit)
    provenance = {"commit": commit, "ref": "origin/cpp", "manifest": str(args.bench_bins / "MANIFEST.md")}
    if binary:
        provenance.update(source="pinned MANIFEST.md binary", path=str(binary))
    elif args.build_reference:
        src = out / "reference-source"
        src.mkdir()
        archive = out / "reference.tar"
        p = capture(["git", "archive", "--format=tar", "-o", archive, commit])
        if p.returncode:
            raise Skip(f"reference archive unavailable: {p.stdout}")
        with tarfile.open(archive) as tar:
            tar.extractall(src, filter="data")
        build_cpus = sorted(set(cpus(args.server_cores) + cpus(args.server_smt)
                                + cpus(args.load_cores) + cpus(args.load_smt)))
        # The reference Makefile already compiles separate objects in parallel and carries its
        # own per-TU flags. Keep that build grammar, using the supplied budget before measuring.
        argv = ["taskset", "-c", cpu_string(build_cpus), "make", f"-j{len(build_cpus)}"]
        print(f"REFERENCE: no matching pin; building {commit}; log {out / 'reference-build.log'}", flush=True)
        # Compiler descendants are owned by this make invocation and are reaped only on abort.
        with (out / "reference-build.log").open("w") as log:
            p = subprocess.Popen(argv, cwd=src, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
            try:
                rc = p.wait(timeout=1800)
            except subprocess.TimeoutExpired as e:
                raise Skip("reference build timed out; see reference-build.log") from e
            finally:
                if p.poll() is None:
                    stop_build(p)
        if rc:
            raise Skip(f"could not build reference {commit}; see reference-build.log")
        binary = src / "build/tomokv"
        provenance.update(source="built origin/cpp", path=str(binary), build_argv=argv,
                          compiler=capture(["g++", "--version"]).stdout.splitlines()[0])
    else:
        raise Skip(f"NO REFERENCE BINARY for origin/cpp {commit}; no matching MANIFEST.md pin; "
                   "enable --build-reference 1 or refresh the pin. This is NOT a pass.")
    provenance["sha256"] = sha256(binary)
    return binary, provenance


def accepted(binary, name, value):
    p = capture([binary, f"--{name}", str(value), "--help"], timeout=10)
    if p.returncode == 0 and "usage:" in p.stdout:
        return True
    if "unknown argument" in p.stdout and f"--{name}" in p.stdout:
        return False
    raise RuntimeError(f"cannot probe {binary.name} --{name}: {p.stdout[:500]}")


# --overlap and --reorder are RENAMES of knobs the pushed reference already has, not new features.
# The reference at c8e61f646 accepts --x-overlap and --x-ex-sched; they are simply absent from its
# --help, so probing by name reports them missing. Translating lets every headline cell run against
# the reference instead of being dropped -- and dropping was the dangerous option: silently omitting
# --overlap 1 compared overlap-on against overlap-off and reported "+17.02%" as a code win.
LEGACY_KNOBS = {"overlap": "x-overlap", "reorder": "x-ex-sched"}


def legacy_value(name, value, mode):
    """Translate a candidate knob value into the reference's older grammar.

    reorder maps directly (both accept 0|1), and so does 2s overlap. 1s overlap does NOT: the older
    binary accepted 0|1|2 there, and the knob work collapsed 1s to 0|1 by mapping "on" to the
    FULLEST schedule, which was old value 2. Old 1 was measured as a loser and no current knob
    preserves it, so mapping 1s "on" to --x-overlap 1 would compare the candidate's surviving
    schedule against an arm that was deleted for losing -- flattering the candidate.
    """
    if name == "overlap" and value and mode == "1s":
        return 2
    return value


class NotComparable(RuntimeError):
    """The reference cannot run this cell's knobs, so no verdict is meaningful."""


def knob_plan(cell, support):
    wanted = {"thread-mode": cell.mode, "read-local": cell.read_local,
              "overlap": cell.overlap, "reorder": cell.reorder}
    plans, notes = {}, []
    for arm in ("A", "B"):
        plans[arm] = {}
        for name, value in wanted.items():
            if support[arm][name]:
                plans[arm][name] = value
            elif arm == "A" and name in LEGACY_KNOBS and support[arm].get(LEGACY_KNOBS[name]):
                old, translated = LEGACY_KNOBS[name], legacy_value(name, value, cell.mode)
                plans[arm][old] = translated
                notes.append(f"reference takes --{name} {value} as --{old} {translated} "
                             f"({cell.mode}); a rename, so the arms run the same configuration")
            elif arm == "A" and name != "thread-mode" and not value:
                # Omitting a knob the reference lacks is only sound when the cell asked for it OFF,
                # because 0 IS this project's legacy behaviour for every knob ("0 means off and must
                # allocate nothing"). Then both arms really are running the same experiment.
                notes.append(f"reference predates --{name}; requested {value} is its legacy "
                             f"behaviour, so the arms remain comparable")
            elif arm == "A" and name != "thread-mode":
                # Requested ON, and the reference cannot do it. Silently dropping the flag here
                # compares the FEATURE against its own absence and reports the difference as if it
                # were a code change. On 2026-09-10 that turned h12 (2s, --overlap 1) into a
                # "+17.02%" candidate win that was nothing but overlap-on versus overlap-off: the
                # reference c8e61f646 predates the knob. A cell whose knobs the reference cannot
                # honour is NOT COMPARABLE against that reference, and must skip loudly rather than
                # produce a verdict -- the note alone was printed and then ignored.
                raise NotComparable(
                    f"cell needs --{name} {value} but the reference predates that knob; "
                    f"comparing against its legacy behaviour would measure the feature, not the code")
            else:
                raise RuntimeError(f"{arm} does not accept required --{name}")
    return plans, notes


def lb_snapshot(conn, path):
    raw = conn.must("DEBUG", "LBSIGNALS")
    if not isinstance(raw, bytes):
        raise RuntimeError("DEBUG LBSIGNALS returned no telemetry")
    path.write_bytes(raw)
    return parse_snapshot(raw)


def busy_between(start, end):
    if start.keys() != end.keys():
        raise RuntimeError("thread topology changed during measurement")
    busy, idle, per_thread = 0, 0, {}
    for tid in start:
        b, i = end[tid]["busy"] - start[tid]["busy"], end[tid]["idle"] - start[tid]["idle"]
        if b < 0 or i < 0 or b + i <= 0 or start[tid]["role"] != end[tid]["role"]:
            raise RuntimeError("missing, reset, or changed-role busy counters")
        busy += b
        idle += i
        per_thread[tid] = 100 * b / (b + i)
    return 100 * busy / (busy + idle), per_thread


def busy_deltas(start, end):
    busy_between(start, end)  # Keep the same role/topology/reset validation.
    # Raw role-specific counters are diagnostic evidence, not a new saturation
    # rule. Read-local can leave split executors idle; flipctl.cc also documents
    # io submit/reap work absent from busy_ns. Retain both counters plus the
    # observed snapshot interval so live results can distinguish those cases
    # from insufficient generator capacity before anyone changes the instrument.
    return {tid: {"role": start[tid]["role"],
                  "busy_ns": end[tid]["busy"] - start[tid]["busy"],
                  "idle_ns": end[tid]["idle"] - start[tid]["idle"]} for tid in start}


def info(conn, section):
    raw = conn.must("INFO", section)
    return dict(line.split(":", 1) for line in raw.decode().splitlines() if ":" in line)


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def memtier_totals(path, cell, connections):
    data = json.loads(path.read_text())
    totals = data["ALL STATS"]["Totals"]
    rate, latency = float(totals["Ops/sec"]), float(totals["Latency"])
    if not all(math.isfinite(x) and x > 0 for x in (rate, latency)):
        raise RuntimeError(f"invalid memtier totals in {path}")
    if not {"Connection Errors", "Connection Errors/sec"} <= totals.keys():
        raise RuntimeError(f"missing memtier connection-error counters: {path}")
    if any(float(totals.get(field, 0)) != 0 for field in
           ("Errors", "Errors/sec", "Connection Errors", "Connection Errors/sec")):
        raise RuntimeError(f"memtier reported errors: {path}")
    return {"rate": rate, "latency_ms": latency,
            "connection_errors": totals["Connection Errors"],
            "connection_errors_per_second": totals["Connection Errors/sec"],
            **memtier_workload_counts(cell, data, connections)}


def require_unbound_port(port):
    # Correctness closes connections on this same port before ABBA starts. A plain bind
    # rejects their TIME_WAIT sockets even after the listener and every server PID are gone.
    # Match the server's address reuse, but NEVER enable REUSEPORT: this probe must still
    # reject an actual listener, including one which opted into shared-port listeners.
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(("127.0.0.1", port))


class Runner:
    def __init__(self, args, out, binaries, children):
        self.args, self.out, self.binaries, self.children = args, out, binaries, children
        self.server_cpus = sorted(cpus(args.server_cores) + cpus(args.server_smt))
        self.load_cpus = sorted(cpus(args.load_cores) + cpus(args.load_smt))
        self.legacy_reorder_controls = {}
        self.legacy_reorder_failures = {}

    def legacy_reorder_control(self, cell, arm, knobs):
        if cell.op != 'REORDER' or arm != 'A' or 'x-ex-sched' not in knobs:
            return None
        # Only the known legacy grammar may use this fallback. The current candidate continues
        # to require its during-window counter. Adding telemetry to an old binary would change
        # the performance reference; instead observe actual dispatch/execution inversions on its
        # unchanged bytes, with FIFO as the negative control. This precedes population/timing.
        # LB is off in the witness to rule out producer/owner changes; measured cells retain their
        # original LB settings. This proves engagement in the directed control, not in the scored
        # interval, whose GET/BITCOUNT progress and long-service-cost checks remain mandatory.
        digest = sha256(self.binaries[arm])
        key = digest, cell.mode
        if key in self.legacy_reorder_failures:
            raise RuntimeError(self.legacy_reorder_failures[key])
        if key not in self.legacy_reorder_controls:
            from legacy_reorder_witness import run_control
            folder = self.out / f'legacy-reorder-{arm}-{cell.mode}'
            args = argparse.Namespace(server_cores=self.args.server_cores,
                server_smt=self.args.server_smt, port=self.args.port,
                attempts=16, blocker_bytes=16 * 1024 * 1024, blocker_count=4)
            controls = []
            for reorder in (0, 1):
                row = run_control(args, self.binaries[arm], folder / f'reorder-{reorder}',
                                  cell.mode, reorder)
                controls.append(row)
                if row['verdict'] != 'PASS':
                    reason = (f'legacy {cell.mode} reorder={reorder} control failed: '
                              + row.get('reason', 'no reason'))
                    self.legacy_reorder_failures[key] = reason
                    raise RuntimeError(reason)
            if sha256(self.binaries[arm]) != digest:
                raise RuntimeError('legacy reference changed during engagement controls')
            artifact = folder / 'controls.json'
            artifact.write_text(json.dumps(controls, indent=2) + '\n')
            self.legacy_reorder_controls[key] = dict(verdict='PASS', mode=cell.mode,
                controls=[0, 1], binary_sha256=digest, artifact=str(artifact.relative_to(self.out)),
                artifact_sha256=sha256(artifact),
                scope='live unscored OFF/ON execution-order control; no scored-window permutation count')
        return self.legacy_reorder_controls[key]

    def population_environment(self):
        return {"population_by_arm": {"A": "wire", "B": "wire"}}

    def memtier(self, layout):
        return ["taskset", "-c", cpu_string(layout["cpus"]), self.args.memtier,
                "-s", "127.0.0.1", "-p", str(self.args.port), "--protocol=redis",
                "-t", str(layout["threads"]), "-c", str(layout["clients"]),
                "--key-minimum=1", f"--key-maximum={KEYS}", "--key-pattern=P:P",
                "-d", "64", "--distinct-client-seed", "--hide-histogram"]

    def prepare_data(self, cell, arm, folder):
        # Experiment hook, called before boot. Production retains wire population;
        # snapshot experiments can copy their own fixture here without changing it.
        pass

    def populate(self, cell, arm, conn, folder):
        population = self.memtier({"cpus": self.load_cpus, "threads": 8, "clients": 8})
        population += ["--pipeline=32", "--ratio=1:0", "-n", "allkeys"]
        pop = self.children.start(population, folder / "populate.log", folder)
        if pop.wait(timeout=180):
            raise RuntimeError("key population failed")
        self.children.stop(pop)
        if conn.must("DBSIZE") != KEYS:
            raise RuntimeError(f"population did not create exactly {KEYS} keys")
        if cell.op == "REORDER":
            extra = prepare_long_keys(conn)
            if conn.must("DBSIZE") != KEYS + extra["keys"]:
                raise RuntimeError("long-blocker population changed the short-key population")
            return extra
        return None

    def measure(self, cell, arm, sequence, instances, knobs):
        legacy_control = self.legacy_reorder_control(cell, arm, knobs)
        folder = self.out / cell.id / f"n{instances}-{sequence}-{arm}"
        folder.mkdir(parents=True)
        layout = load_layout(self.load_cpus, instances, cell.conns)
        # Never connect to or terminate an existing listener, even if it speaks TomoKV.
        require_unbound_port(self.args.port)
        command = ["taskset", "-c", cpu_string(self.server_cpus), self.binaries[arm],
                   "--port", str(self.args.port), "--bind", "127.0.0.1", "--atomic", str(cell.atomic),
                   "--enable-debug-command", "yes", "--save", "", "--appendonly", "no",
                   "--dir", str(folder)]
        # Defaults are made explicit so changes to placement cannot masquerade as code gains.
        ex = len(self.server_cpus) // 2
        if cell.mode == "2s":
            command += ["--ratio", f"{len(self.server_cpus) - ex}:{ex}", "--flip-auto", "0"]
        command += ["--shards", str(min(8 * (ex if cell.mode == "2s" else len(self.server_cpus)), 256))]
        for name, value in knobs.items():
            command += [f"--{name}", str(value)]
        started = time.monotonic()
        srv, conn, generators = None, None, []
        log = folder / "server.log"
        result = {"arm": arm, "instances": instances, "complete": False,
                  "server_argv": [str(x) for x in command],
                  "load_layout": layout, "artifacts": str(folder.relative_to(self.out))}
        print(f"  {cell.id} n={instances} {sequence}:{arm} boot/populate/{WINDOW}s", flush=True)
        try:
            self.prepare_data(cell, arm, folder)
            srv = self.children.start(command, log, folder)
            deadline = time.monotonic() + 30
            while True:
                if srv.poll() is not None:
                    raise RuntimeError(f"server exited {srv.returncode}: {log.read_text()[-1000:]}")
                try:
                    conn = Conn("127.0.0.1", self.args.port, timeout=10)
                    identity = info(conn, "server")
                    if int(identity["process_id"]) != srv.pid:
                        raise RuntimeError("listener PID is not our child")
                    break
                except (OSError, EOFError):
                    if conn:
                        conn.close()
                        conn = None
                    if time.monotonic() >= deadline:
                        raise RuntimeError("server boot timed out")
                    time.sleep(0.1)
            result["pid"] = srv.pid
            for name, value in {"atomic": cell.atomic, **knobs}.items():
                actual = conn.must("CONFIG", "GET", name)
                if actual != [name.encode(), str(value).encode()]:
                    raise RuntimeError(f"boot did not apply {name}={value}: {actual!r}")
            result["population"] = self.populate(cell, arm, conn, folder)
            result["populate_seconds"] = time.monotonic() - started
            # Bracket ALL generators, after wire/snapshot population and any service-
            # cost probes. Only the named workload command counters enter accounting;
            # INFO/DEBUG and client protocol setup never become phantom workload ops.
            result["whole_run_commandstats_before"] = info(conn, "commandstats")
            result["whole_run_clients_before"] = info(conn, "clients")
            for i, placement in enumerate(layout):
                argv = self.memtier(placement) + workload_arguments(cell) + [f"--pipeline={cell.depth}",
                        f"--test-time={WARMUP + WINDOW + TAIL}",
                        f"--json-out-file={folder / f'load-{i}.json'}"]
                generators.append(self.children.start(argv, folder / f"load-{i}.log", folder))
                result.setdefault("load_argv", []).append(argv)
            # The counter window excludes setup/teardown and is the SAME for all LGs.
            time.sleep(WARMUP)
            if any(p.poll() is not None for p in generators):
                raise RuntimeError("load generator exited before the measurement window")
            if int(info(conn, "clients")["connected_clients"]) != cell.conns + 1:
                raise RuntimeError("not all requested load connections are active")
            before_lb = lb_snapshot(conn, folder / "lb-before.txt")
            before_lb_at = time.monotonic()
            if len(before_lb.threads) != len(self.server_cpus):
                raise RuntimeError("server thread count differs from requested CPU geometry")
            roles = {role: sum(row["role"] == role for row in before_lb.threads.values())
                     for role in {row["role"] for row in before_lb.threads.values()}}
            expected_roles = ({"fused": len(self.server_cpus)} if cell.mode == "1s"
                              else {"io": len(self.server_cpus) - ex, "ex": ex})
            if roles != expected_roles:
                raise RuntimeError(f"actual thread roles {roles} differ from {expected_roles}")
            result["thread_roles"] = roles
            before_mode = info(conn, "server") if cell.op == "REORDER" else {}
            before_commands = info(conn, "commandstats")
            before = info(conn, "stats")
            before_cpu, t0 = cpu_seconds(srv.pid), time.monotonic()
            time.sleep(WINDOW)
            after = info(conn, "stats")
            t1, after_cpu = time.monotonic(), cpu_seconds(srv.pid)
            after_lb = lb_snapshot(conn, folder / "lb-after.txt")
            after_lb_at = time.monotonic()
            after_commands = info(conn, "commandstats")
            after_mode = info(conn, "server") if cell.op == "REORDER" else {}
            if any(p.poll() is not None for p in generators):
                raise RuntimeError(f"load generator ended inside the {WINDOW}-second window")
            if int(info(conn, "clients")["connected_clients"]) != cell.conns + 1:
                raise RuntimeError("load connections disappeared during measurement")
            commands = int(after["total_commands_processed"]) - int(before["total_commands_processed"]) - 1
            if commands <= 0:
                raise RuntimeError("no commands completed")
            misses = int(after["keyspace_misses"]) - int(before["keyspace_misses"])
            if misses != 0:
                raise RuntimeError(f"GETs missed prepopulated keys: {misses}")
            busy, per_thread = busy_between(before_lb.threads, after_lb.threads)
            result.update(rate=commands / (t1 - t0), commands=commands, window_seconds=t1 - t0,
                          midpoint_monotonic=(t0 + t1) / 2, busy_pct=busy, thread_busy_pct=per_thread,
                          thread_activity_deltas=busy_deltas(before_lb.threads, after_lb.threads),
                          lb_snapshot_window_seconds=after_lb_at - before_lb_at,
                          # Preparation only: the old busy_pct still controls assess()
                          # and pin failure. This field can never validate its own rule.
                          diagnostic_saturation=productive_saturation(
                              before_lb, after_lb, floor_pct=BUSY_FLOOR),
                          cpu_pct=100 * (after_cpu - before_cpu) / ((t1 - t0) * len(self.server_cpus)),
                          info_before=before, info_after=after)
            result["workload_witness"] = require_workload_witness(
                cell, before_commands, after_commands, before_mode, after_mode, legacy_control)
            totals = result["memtier"] = []
            for i, p in enumerate(generators):
                if p.wait(timeout=30):
                    raise RuntimeError(f"load generator {i} failed; see {folder}")
            # All processes have drained and exited before the second endpoint.
            # Keep the central WINDOW calculation above unchanged: these wider
            # endpoints establish counter integrity, not a second throughput rate.
            result["whole_run_commandstats_after"] = info(conn, "commandstats")
            result["whole_run_clients_after"] = info(conn, "clients")
            for i, placement in enumerate(layout):
                totals.append(memtier_totals(folder / f"load-{i}.json", cell,
                                            placement["threads"] * placement["clients"]))
            result["whole_run_accounting"] = require_workload_accounting(
                cell, result["whole_run_commandstats_before"], result["whole_run_commandstats_after"], totals)
            total_rate = sum(t["rate"] for t in totals)
            result.update(complete=True, memtier=totals, memtier_rate=total_rate,
                          latency_ms=sum(t["latency_ms"] * t["rate"] for t in totals) / total_rate)
            if cell.metric == "p999_ms":
                result.update(merged_tail([json.loads((folder / f"load-{i}.json").read_text())
                                           for i in range(len(generators))],
                                          count_bounds=[row["outstanding_bound"] for row in totals]))
                # Memtier's HDR spans its entire run. State that separately from the
                # central counter window; startup/warmup/tail samples are not silently
                # represented as a histogram of only WINDOW seconds.
                result["histogram_window_seconds"] = WARMUP + WINDOW + TAIL
        except BaseException as e:
            result["error"] = f"{type(e).__name__}: {e}"
            raise
        finally:
            if conn:
                conn.close()
            for p in generators:
                self.children.stop(p)
            if srv:
                self.children.stop(srv)
            result["wall_seconds"] = time.monotonic() - started
            (folder / "measurement.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"    {result['rate']/1e6:.5f}M/s busy={result['busy_pct']:.3f}% "
              f"CPU={result['cpu_pct']:.3f}% latency={result['latency_ms']:.5f}ms", flush=True)
        return result


def print_cell(row):
    c = row["cell"]
    for note in row.get("notes", []):
        print(f"  {c['id']} COMPATIBILITY: {note}", flush=True)
    if "assessment" not in row:
        print(f"{c['id']} {row['verdict']}: {row['reason']}", flush=True)
        return
    a = row["assessment"]
    units, scale = (("ms (short-command p99.9)", 1) if a["metric"] == "p999_ms" else
                    ("ms (depth-1 latency; saturation exempt)", 1) if c["depth"] == 1 else ("Mops/s", 1e6))
    av, bv = [v / scale for v in a["reference"]], [v / scale for v in a["candidate"]]
    print(f"{c['id']} A={av[0]:.6f},{av[1]:.6f} B={bv[0]:.6f},{bv[1]:.6f} {units} "
          f"paired={a['delta_pct']:+.4f}% spread A/B={a['reference_spread_pct']:.4f}/"
          f"{a['candidate_spread_pct']:.4f}% threshold={a['threshold_pct']:.4f}% "
          f"busy(ABBA)={','.join(f'{x:.3f}' for x in a['busy_pct_abba'])}% "
          f"instances={a['instances']} {a['verdict']}", flush=True)
    if a["fastest_gain_pct"] is not None:
        print(f"  fastest-arm gain at higher instance count={a['fastest_gain_pct']:+.4f}% "
              f"vs measured plateau noise={a['plateau_noise_pct']:.4f}%", flush=True)
    for reason in a["reasons"]:
        print(f"  FAIL: {reason}", flush=True)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true")
    p.add_argument("--subset", choices=("smoke", "full"), default="full",
                   help="smoke is the 15-cell iteration design; full is required for push/release")
    p.add_argument("--list-cells", action="store_true", help="print selected coverage as JSON without CPU work")
    p.add_argument("--candidate-binary", "--candidate", dest="candidate", type=Path,
                   default=Path(os.getenv("GATE_ABBA_CANDIDATE", ROOT / "build/tomokv")))
    p.add_argument("--reference-binary", type=Path,
                   default=Path(os.environ["GATE_ABBA_REFERENCE"]) if os.getenv("GATE_ABBA_REFERENCE") else None)
    p.add_argument("--cells", type=Path, default=Path(os.getenv("GATE_ABBA_CELLS", ROOT / "tests" / "headline_cells.txt")))
    p.add_argument("--bench-bins", type=Path, default=Path(os.getenv("GATE_ABBA_BINS", "/home/user/Projects/bench-bins")))
    p.add_argument("--build-reference", type=int, choices=(0, 1), default=int(os.getenv("GATE_ABBA_BUILD_REFERENCE", "1")))
    # The gate supplies its planned highest-budget geometry, capped at 32 physical server cores.
    # Standalone derives the same 32-real-core limit from topology, with all
    # remaining cores and their permitted SMT siblings assigned to generators.
    # Explicit --load-smt '' reserves those siblings; server siblings stay reserved.
    p.add_argument("--server-cores", default=os.getenv("GATE_ABBA_CORES"))
    p.add_argument("--server-smt", default=os.getenv("GATE_ABBA_SERVER_SMT", ""))
    p.add_argument("--load-cores", default=os.getenv("GATE_ABBA_LOAD_CORES"))
    p.add_argument("--load-smt", default=os.getenv("GATE_ABBA_LOAD_SMT"))
    p.add_argument("--ports", default=os.getenv("GATE_ABBA_PORTS"),
                   help="permitted first-last bind range; only its first port is needed")
    p.add_argument("--port", type=int,
                   default=int(os.environ["GATE_ABBA_PORT"]) if os.getenv("GATE_ABBA_PORT") else None,
                   help="optional single port inside --ports; standalone default 8700")
    p.add_argument("--memtier", default=os.getenv("GATE_ABBA_MEMTIER", "memtier_benchmark"))
    p.add_argument("--output", type=Path, default=None)
    p.add_argument("--collect-null", type=int, choices=(0, 1), default=0,
                   help="1 freezes one executable into identical arms and collects a null; always PARTIAL/exit 3")
    p.add_argument("--null-result", type=Path, default=Path(os.getenv("GATE_ABBA_NULL", os.getenv(
        "GATE_RECEIPT_NULL", ROOT / ".gate-history/receipts/baselines/full-null.json"))),
                   help="recent matched null required for comparison PASS; missing controls retain untrusted diagnostics")
    p.add_argument("--escalate", action="store_true",
                   help="ignore pinned load levels and search the ladder; use this to RE-PIN a cell "
                        "after the gate reports its pinned level no longer saturates")
    p.add_argument("--only", default="", help="comma-separated IDs; partial diagnostic, never a full-tier PASS")
    p.add_argument("--max-instances", type=int, choices=LADDER, default=16,
                   help="load-instance ceiling (default 16); 1 cannot prove unpinned deep-pipeline saturation")
    return p.parse_args()


def main(args, *, diagnostic_monitor=None):
    if args.list_cells:
        cells = selected_cells(read_cells(args.cells), args.subset, args.only)
        print(json.dumps({"subset": args.subset, **coverage(cells)}, indent=2))
        return 0
    start = time.monotonic()
    out = (args.output or ROOT / "build" / f"abbagate-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}").resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "verdict": "FAIL", "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "window_seconds": WINDOW, "order": list(ORDER), "cells": [], "output": str(out),
              "subset": args.subset, "only": args.only,
              "run_kind": "null-control" if args.collect_null else "comparison", "comparison_trusted": False}
    if diagnostic_monitor is not None:
        # Internal-only background qualification may observe the real measurement loop while
        # auditing an unvalidated quiet-screening rule. It must NEVER transiently publish a
        # consumable null or receipt, even if every raw statistical assessment passes.
        report.update(run_kind="background-qualification", normal_gate_eligible=False,
                      measurement_valid=False)
    if args.collect_null:
        report["null_control"] = {"verdict": "FAIL", "reason": "control has not completed"}
    children = Children()
    quiet = None
    rc = 1
    original_affinity = os.sched_getaffinity(0)

    def invalidate_instrument(reason):
        report.update(verdict="FAIL", reason=reason, measurement_valid=False, comparison_trusted=False)
        for row in report["cells"]:
            row["instrument_valid"] = False
            row["instrument_failure"] = reason

    def interrupted(signum, _frame):
        raise InterruptedError(f"interrupted by signal {signum}")

    old_handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
    for sig in old_handlers:
        signal.signal(sig, interrupted)
    try:
        quiet_file = os.getenv("GATE_QUIET_FILE")
        if quiet_file:
            quiet_path = Path(quiet_file)
            age = time.time() - quiet_path.stat().st_mtime if quiet_path.exists() else -1
            if age < 60 * float(os.getenv("GATE_QUIET_MINUTES", "3")):
                raise QuietViolation(f"quiet file {quiet_path} is absent or too recent; no CPU work started")
        resolve_geometry(args)
        server_physical, load_physical = cpus(args.server_cores), cpus(args.load_cores)
        server_smt, load_smt = cpus(args.server_smt), cpus(args.load_smt)
        check_placement(server_physical, load_physical, server_smt, load_smt)
        server_cpus, load_cpus = sorted(server_physical + server_smt), sorted(load_physical + load_smt)
        args.port, permitted_ports = select_port(args.ports, args.port)
        # The driver also generates control traffic and collects counters. Keep it on load CPUs
        # even when invoked from a shell that was pinned to a correctness worker's server slot.
        os.sched_setaffinity(0, load_cpus)
        cells = read_cells(args.cells)
        report["cell_source"] = {"path": str(args.cells.resolve()), "sha256": sha256(args.cells),
                                 "text": args.cells.read_text(), "total_cells": len(cells)}
        cells = selected_cells(cells, args.subset, args.only)
        report["coverage"] = coverage(cells)
        pending = [cell.id for cell in cells if cell.pin_required and not cell.instances]
        if pending and not args.escalate:
            raise ValueError("unmeasured load floors for " + ",".join(pending) +
                             "; calibrate with --escalate and record the validated pins before gating")
        # Standing nulls can use another server binary, but must use these exact harness bytes.
        # Capture before the quiet observer starts, and check again after its final sample so
        # fingerprinting itself never becomes foreign CPU work inside a measurement interval.
        report["receipt_harness_sha256"] = harness_fingerprint(ROOT)["sha256"]
        report["instrument_fingerprint"] = instrument_fingerprint(ROOT)
        # Freeze the chosen artifact before measurements. A missing control does not remove any
        # authorized workload; successful raw observations remain explicitly untrusted instead.
        control, control_error = None, None
        if not args.collect_null:
            try:
                control = read_json(args.null_result)
            except (OSError, ValueError) as error:
                control_error = f"standing null unavailable: {args.null_result}: {error}"
                print("ABBA UNTRUSTED: " + control_error + "; all measurements still run", flush=True)
        quiet = (diagnostic_monitor or QuietMonitor)(server_cpus, load_cpus,
                    own_root_pid=os.getpid(), window_seconds=WINDOW)
        quiet.start()  # Fail before reference builds, capability probes, or server boots.
        report["quiet_box"] = quiet.evidence()
        if not args.candidate.is_file() or not os.access(args.candidate, os.X_OK):
            raise RuntimeError(f"candidate executable unavailable: {args.candidate}")
        binaries = {}
        if args.collect_null:
            # Copy the candidate ONCE, then derive the other arm from that frozen file. Resolving
            # a pushed reference here would create a circular prerequisite and could compare
            # different bytes. A null proves repeatability of this instrument, not source identity.
            binaries["B"] = out / "binary-B"
            shutil.copy2(args.candidate.resolve(), binaries["B"])
            reference = binaries["B"]
            provenance = {"source": "byte-identical null control", "commit": "not-a-code-comparison",
                          "sha256": sha256(reference)}
            copies = (("A", reference),)
        else:
            reference, provenance = resolve_reference(args, out)
            copies = (("A", reference), ("B", args.candidate.resolve()))
        report["reference"] = provenance
        for arm, source in copies:
            dest = out / f"binary-{arm}"
            shutil.copy2(source, dest)
            binaries[arm] = dest
        report["candidate"] = {"path": str(args.candidate.resolve()), "sha256": sha256(binaries["B"]),
                               "workspace_commit": git("rev-parse", "HEAD"),
                               "workspace_status": git("status", "--short")}
        if sha256(binaries["A"]) != provenance["sha256"]:
            raise RuntimeError("reference changed while copying")
        print(f"REFERENCE {provenance['source']} {provenance['commit']} sha256={provenance['sha256']}", flush=True)
        print(f"CANDIDATE {args.candidate} sha256={report['candidate']['sha256']}", flush=True)
        args.memtier = shutil.which(args.memtier)
        if not args.memtier:
            raise RuntimeError("memtier_benchmark not available")
        args.memtier = str(Path(args.memtier).resolve())
        runner = Runner(args, out, binaries, children)
        report["environment"] = {"uname": list(os.uname()),
                                 "python_runtime": report["instrument_fingerprint"]["python"],
                                 "server_cpus": server_cpus,
                                 "server_physical": server_physical, "server_smt": server_smt,
                                 "load_physical": load_physical, "load_smt": load_smt,
                                 "load_instance_ceiling": min(args.max_instances, len(load_physical)),
                                 "load_cpus": load_cpus, "port": args.port,
                                 "permitted_ports": permitted_ports, "keys": KEYS,
                                 "data_bytes": 64, "key_pattern": "P:P", "atomic": "per-cell",
                                 "split_ratio": f"{len(server_cpus)-len(server_cpus)//2}:{len(server_cpus)//2}",
                                 "split_flip_auto": 0, "memtier_path": args.memtier,
                                 "memtier_sha256": sha256(Path(args.memtier)),
                                 "memtier_version": capture([args.memtier, "--version"]).stdout.strip(),
                                 **runner.population_environment()}
        print(f"GEOMETRY server={args.server_cores} ({len(server_physical)} physical cores) "
              f"server-smt={args.server_smt or '(reserved)'} ({len(server_cpus)} threads) "
              f"load={args.load_cores} load-smt={args.load_smt or '(reserved)'} "
              f"port={args.port} allowed={permitted_ports[0]}-{permitted_ports[1]}; "
              "source headline file records 32 server cores; actual geometry recorded above. "
              "Cell connections are TOTAL, shared across load instances. Split uses fixed even ratio, flip=0.", flush=True)
        quiet.check()
        support = {arm: {name: accepted(binary, name, value) for name, value in
                        (("thread-mode", "1s"), ("read-local", 0), ("overlap", 0), ("reorder", 0),
                         ("x-overlap", 0), ("x-ex-sched", 0))}
                   for arm, binary in binaries.items()}
        quiet.check()
        report["accepted_knobs"] = support
        for cell in cells:
            row = {"cell": asdict(cell), "verdict": "FAIL", "rounds": []}
            report["cells"].append(row)
            try:
                plans, row["notes"] = knob_plan(cell, support)
                row["knobs"] = plans
                for note in row["notes"]:
                    print(f"  {cell.id} COMPATIBILITY: {note}", flush=True)
                pinned = cell.depth > 1 and cell.instances and not args.escalate
                ladder = (cell.instances,) if pinned else LADDER
                row["load_ladder"] = list(ladder)
                # Clearing the assessment pin matters even at --max-instances=1:
                # --escalate must prove its peak with a higher probe, never borrow
                # the very stored saturation evidence the caller asked to ignore.
                assessed_cell = replace(cell, instances=0) if args.escalate else cell
                if pinned:
                    print(f"  {cell.id} PINNED load={cell.instances}; one ABBA block (4 measurements)", flush=True)
                    ceiling = min(args.max_instances, cell.conns, len(load_physical))
                    if cell.instances > ceiling:
                        raise ValueError(f"pinned load level {cell.instances} exceeds the instance/connection/"
                                         f"physical-core ceiling {ceiling}; provide its required load budget "
                                         "or re-pin it with --escalate")
                elif cell.depth > 1:
                    print(f"  {cell.id} {'ESCALATE ignores pin=' + str(cell.instances) if cell.instances else 'UNPINNED'}: "
                          f"searching load ladder {','.join(map(str, ladder))}; record the validated pin", flush=True)
                for n in ladder:
                    # Each generator owns at least one physical load core; its explicitly
                    # enabled SMT siblings travel with that core, not as another instance.
                    # At a small budget, assess the last possible block normally: an unproven
                    # plateau remains FAIL instead of attempting an impossible placement.
                    if n > args.max_instances or n > cell.conns or n > len(load_physical):
                        break
                    if cell.conns % n and not pinned:
                        continue
                    round_ = {"instances": n, "runs": []}
                    row["rounds"].append(round_)
                    for sequence, arm in enumerate(ORDER, 1):
                        quiet.check()
                        if diagnostic_monitor is not None:
                            quiet.set_phase(f"measurement:{cell.id}:n{n}:{sequence}:{arm}")
                        round_["runs"].append(runner.measure(cell, arm, sequence, n, plans[arm]))
                        if diagnostic_monitor is not None:
                            quiet.set_phase("between-measurements")
                        quiet.check()
                    row["assessment"] = assess(assessed_cell, row["rounds"])
                    row["verdict"] = row["assessment"]["verdict"]
                    print_cell(row)
                    if saturation_done(assessed_cell, row["rounds"]):
                        break
            except (InterruptedError, QuietViolation):
                raise
            except NotComparable as e:
                # Distinct from a measurement error: nothing went wrong with the box, the cell just
                # cannot be posed to this reference at all. Still a counted failure -- a tier that
                # skipped these quietly would report a clean gate while silently not testing them.
                row.pop("assessment", None)
                row.update(verdict="FAIL", reason=f"not comparable against this reference: {e}")
                print_cell(row)
            except Exception as e:
                row.pop("assessment", None)
                row.update(verdict="FAIL", reason=f"measurement error: {e}")
                print_cell(row)
            finally:
                children.close()
                report["quiet_box"] = quiet.evidence()
                report["elapsed_seconds"] = time.monotonic() - start
                (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        # Join and take one final sample before producing a PASS/exit code. A
        # cleanup-only check in finally would run after Python chose that code.
        report["quiet_box"] = quiet.close()
        quiet.check()
        if harness_fingerprint(ROOT)["sha256"] != report["receipt_harness_sha256"]:
            invalidate_instrument("measurement harness changed during the ABBA tier")
            raise RuntimeError(report["reason"])
        if instrument_fingerprint(ROOT) != report["instrument_fingerprint"]:
            invalidate_instrument("measurement instrument changed during the ABBA tier")
            raise RuntimeError(report["reason"])
        report["measurement_valid"] = diagnostic_monitor is None
        report["elapsed_seconds"] = time.monotonic() - start
        report["statistical_verdict"], report["worst_cell"] = overall(report["cells"])
        report["verdict"] = report["statistical_verdict"]
        if report["statistical_verdict"] == "PASS":
            report["verdict"] = "PARTIAL"
            if diagnostic_monitor is not None:
                report["null_control"] = {"verdict": "UNTRUSTED", "reason":
                    "background qualification is diagnostic only; not a standing null or a gate PASS"}
                print("BACKGROUND QUALIFICATION: raw cells pass; instrument remains UNTRUSTED", flush=True)
            elif args.collect_null:
                report["null_control"] = null_result(report, now=time.time())
                print("NULL CONTROL PASS: selected cells passed with byte-identical arms; not a code-comparison PASS", flush=True)
            else:
                try:
                    if control_error:
                        raise ValueError(control_error)
                    report["standing_null"] = match_null(report, control, now=time.time())
                    # Retain the exact accepted control beside this comparison. Receipts use this
                    # frozen file, never a default path that another successful run may replace.
                    (out / "null-control.json").write_text(json.dumps(control, indent=2) + "\n")
                    if not args.only:
                        report["comparison_trusted"] = True
                        report["verdict"] = "PASS"
                except (OSError, ValueError, TypeError, KeyError) as error:
                    report["standing_null"] = {"status": "UNTRUSTED", "reason": str(error)}
                    print(f"ABBA UNTRUSTED: {error}; raw assessments retained", flush=True)
        print(f"ABBA {args.subset} {report['verdict']} worst={report['worst_cell']} "
              f"({len(cells)}/{report['cell_source']['total_cells']} cells); results={out / 'results.json'}", flush=True)
        rc = 1 if report["verdict"] == "FAIL" else 3 if report["verdict"] == "PARTIAL" else 0
    except Skip as e:
        report.update(verdict="SKIP", reason=str(e))
        print(f"ABBA SKIP — NOT A PASS: {e}", file=sys.stderr, flush=True)
        rc = 3
    except (Exception, KeyboardInterrupt) as e:
        report.update(verdict="FAIL", reason=f"{type(e).__name__}: {e}", comparison_trusted=False)
        if isinstance(e, QuietViolation):
            invalidate_instrument(report["reason"])
        print(f"ABBA FAIL: {report['reason']}", file=sys.stderr, flush=True)
        rc = 1
    finally:
        # Complete reaping even if the user presses Ctrl-C again during teardown.
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, signal.SIG_IGN)
        children.close()
        if quiet is not None:
            report["quiet_box"] = quiet.close()
            # A reference-resolution SKIP or other error can finish before the
            # normal final check. Contention still outranks that outcome, including
            # interference first discovered by the observer's cleanup sample.
            try:
                quiet.check()
            except QuietViolation as exc:
                if report.get("measurement_valid") is not False:
                    print(f"ABBA FAIL: {exc}", file=sys.stderr, flush=True)
                invalidate_instrument(f"QuietViolation: {exc}")
                rc = 1
        report["elapsed_seconds"] = time.monotonic() - start
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"ABBA elapsed={report['elapsed_seconds']:.1f}s; {out / 'results.json'}", flush=True)
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
        os.sched_setaffinity(0, original_affinity)
    return rc


def self_test():
    import contextlib
    import io
    import unittest
    from unittest import mock
    (ROOT / "build").mkdir(exist_ok=True)

    class ABBA(unittest.TestCase):
        def setUp(self):
            self.cell = Cell("h01", "1s", 1, 0, 0, "GET", 32, 512)
            # Serverless loop tests replace the process observer too. Dedicated
            # negative controls below inject failures through the same main path.
            self.quiet = mock.Mock()
            self.quiet.evidence.return_value = {"interference": None, "samples": 2}
            self.quiet.close.return_value = {"interference": None, "samples": 3, "complete": True}
            patcher = mock.patch(__name__ + ".QuietMonitor", return_value=self.quiet)
            self.quiet_factory = patcher.start()
            self.addCleanup(patcher.stop)

        def test_full_coverage_preserves_original_axes_and_restores_multikey(self):
            from itertools import product
            cells = read_cells(ROOT / "tests/headline_cells.txt")
            self.assertEqual(len(cells), 178)
            original = [cell for cell in cells if cell.id.startswith("h")]
            self.assertEqual(len(original), 64)
            axes = lambda cell: (cell.mode, cell.read_local, cell.overlap, cell.reorder, cell.op, cell.depth)
            self.assertEqual({axes(cell) for cell in original},
                             set(product(("1s", "2s"), (0, 1), (0, 1), (0, 1), ("GET", "SET"), (1, 32))))
            multi = [cell for cell in cells if cell.id.startswith("m")]
            self.assertEqual({axes(cell) for cell in multi},
                             set(product(("1s", "2s"), (0, 1), (0, 1), (0, 1), ("MGET", "MSET"), (1, 8, 32))))
            self.assertEqual(len(multi), 96)
            self.assertEqual({cell.atomic for cell in cells}, {0, 1})
            self.assertEqual({cell.conns for cell in cells}, {512, 2048})

        def test_smoke_is_fifteen_justified_cells_not_a_cross_product(self):
            cells = selected_cells(read_cells(ROOT / "tests/headline_cells.txt"), "smoke")
            self.assertEqual(len(cells), 15)
            for mode in ("1s", "2s"):
                sweep = [cell for cell in cells if cell.mode == mode and cell.op == "GET"]
                self.assertEqual({(cell.read_local, cell.overlap, cell.reorder) for cell in sweep},
                                 {(1, 1, 1), (0, 1, 1), (1, 0, 1), (0, 0, 0)})
                self.assertTrue(all(cell.depth == 32 for cell in sweep))
                tail = [cell for cell in cells if cell.mode == mode and cell.op == "REORDER"]
                self.assertEqual({cell.reorder for cell in tail}, {0, 1})
                self.assertTrue(all(cell.depth > 1 and cell.metric == "p999_ms" for cell in tail))
            self.assertEqual({cell.op for cell in cells}, {"GET", "SET", "MGET", "MSET", "REORDER"})
            self.assertIn(1, {cell.depth for cell in cells})

        def test_arbitrary_workloads_issue_eight_keys_and_correct_mix_direction(self):
            for op in ("MGET", "MSET"):
                args = workload_arguments(replace(self.cell, op=op))
                command = next(arg for arg in args if arg.startswith("--command="))
                self.assertEqual(command.count("__key__"), 8)
                self.assertEqual(command.count("__data__"), 8 if op == "MSET" else 0)
            self.assertEqual(workload_arguments(replace(self.cell, op="MIX", mix="7:1")), ["--ratio=1:7"])
            args = workload_arguments(replace(self.cell, op="MIX8", mix="18:14"))
            self.assertEqual([arg for arg in args if arg.startswith("--command-ratio=")],
                             ["--command-ratio=18", "--command-ratio=14"])
            args = workload_arguments(replace(self.cell, op="REORDER", mix="95:5"))
            self.assertIn("--command=BITCOUNT blocker:__key__", args)
            self.assertFalse(any("BLPOP" in arg or "MGET" in arg for arg in args))

        def test_missing_workload_or_scheduler_engagement_is_red(self):
            cell = replace(self.cell, op="REORDER", score="p999", mix="95:5", reorder=1)
            before = {"cmdstat_get": "calls=10", "cmdstat_bitcount": "calls=10"}
            after = {"cmdstat_get": "calls=100", "cmdstat_bitcount": "calls=20"}
            mode = {"reorder_permuted_runs": "0"}
            with self.assertRaisesRegex(RuntimeError, "BITCOUNT did not execute"):
                require_workload_witness(cell, before, {**after, "cmdstat_bitcount": "calls=10"}, mode, mode)
            with self.assertRaisesRegex(RuntimeError, "permutation witness"):
                require_workload_witness(cell, before, after, mode, mode)
            evidence = require_workload_witness(cell, before, after, mode, {"reorder_permuted_runs": "1"})
            self.assertEqual(evidence["BITCOUNT"]["calls"], 10)
            with self.assertRaisesRegex(RuntimeError, "permutation witness"):
                require_workload_witness(replace(cell, reorder=0), before, after, mode,
                                         {"reorder_permuted_runs": "1"})
            control = dict(verdict='PASS', mode=cell.mode, controls=[0, 1])
            evidence = require_workload_witness(cell, before, after, {}, {}, control)
            self.assertEqual(evidence['legacy_reorder_control'], control)
            for rejected in (None, {**control, 'verdict': 'FAIL'},
                             {**control, 'mode': '2s' if cell.mode == '1s' else '1s'},
                             {**control, 'controls': [1]}):
                with self.subTest(control=rejected), self.assertRaisesRegex(RuntimeError, 'live legacy'):
                    require_workload_witness(cell, before, after, {}, {}, rejected)
            # A fallback cannot excuse an available counter which failed to advance,
            # a disappearing counter, or a workload command that was never executed.
            for start, end in ((mode, mode), (mode, {}), ({}, mode)):
                with self.assertRaises(RuntimeError):
                    require_workload_witness(cell, before, after, start, end, control)
            with self.assertRaisesRegex(RuntimeError, 'BITCOUNT did not execute'):
                require_workload_witness(cell, before, {**after, 'cmdstat_bitcount': 'calls=10'}, {}, {}, control)

        @staticmethod
        def accounting_document(counts, reported=None):
            import base64
            import struct
            import zlib
            # A real decodable one-bin HDR, with the producer count controlled
            # separately. The saved producer fixture below independently tests
            # the decoder; these controls test accounting, not percentile shape.
            stats = {"Runtime": {"Interrupted": "false"}}
            for name, count in counts.items():
                number, payload = count << 1, bytearray()
                while number >= 128:
                    payload.append((number & 127) | 128)
                    number >>= 7
                payload.append(number)
                body = struct.pack(">IIiiQQd", 0x1c849303, len(payload), 0, 3, 1, 1000000, 1.0) + payload
                compressed = zlib.compress(body)
                encoded = base64.b64encode(struct.pack(">II", 0x1c849304, len(compressed)) + compressed).decode()
                stats[name.capitalize() + "s"] = {"Count": (reported or counts)[name],
                    "Percentile Latencies": {"p99.90": 0.0, "Histogram log format": {"Compressed Histogram": encoded}}}
            stats["Totals"] = {"Count": sum((reported or counts).values()), "Ops/sec": 1000,
                               "Latency": 1, "Connection Errors": 0, "Connection Errors/sec": 0}
            return {"ALL STATS": stats}

        def test_whole_run_counts_use_logical_multikey_units_and_exact_hdr(self):
            cell = replace(self.cell, op="MIX8", depth=8, conns=2, mix="18:14")
            document = self.accounting_document({"MGET": 18000, "MSET": 14000},
                                                 {"MGET": 17993, "MSET": 14009})
            producer = memtier_workload_counts(cell, document, 2)
            self.assertEqual(producer["count_hdr_absolute_difference"], 16)
            before = {"cmdstat_mget": "calls=100", "cmdstat_mset": "calls=200", "cmdstat_info": "calls=10"}
            after = {"cmdstat_mget": "calls=18100", "cmdstat_mset": "calls=14200", "cmdstat_info": "calls=9999"}
            witness = require_workload_accounting(cell, before, after, [producer])
            self.assertEqual(witness["commands"]["MGET"]["server_calls"], 18000)  # not eight keys per op
            with self.assertRaisesRegex(RuntimeError, "connections differ"):
                require_workload_accounting(replace(cell, conns=4), before, after, [producer])
            for count in (18099, 18101, 100, 36100, 144100):
                with self.subTest(count=count), self.assertRaisesRegex(RuntimeError, "accounting mismatch"):
                    require_workload_accounting(cell, before, {**after, "cmdstat_mget": f"calls={count}"}, [producer])
            # The bound applies to the SUM of both errors, not independently to
            # every command; one command cannot hide the other's discrepancy.
            too_far = self.accounting_document({"MGET": 18000, "MSET": 14000},
                                               {"MGET": 17992, "MSET": 14009})
            with self.assertRaisesRegex(RuntimeError, "17 exceeds finite outstanding bound 16"):
                memtier_workload_counts(cell, too_far, 2)

        def test_count_hdr_tail_validation_uses_the_same_finite_bound(self):
            from abba_workloads import command_histogram
            for difference in (-16, 16):
                doc = self.accounting_document({"GET": 5000}, {"GET": 5000 + difference})
                self.assertEqual(sum(command_histogram(doc, "GET", count_bound=16).values()), 5000)
                with self.assertRaisesRegex(ValueError, "exceeds outstanding bound"):
                    command_histogram(doc, "GET", count_bound=15)
                with self.assertRaises(ValueError):
                    command_histogram(doc, "GET")

        def test_accounting_rejects_interrupted_errors_unknown_and_missing_commands(self):
            import copy
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                path = Path(tmp) / "load.json"
                base = self.accounting_document({"GET": 5000})
                bad = []
                item = copy.deepcopy(base)
                item["ALL STATS"]["Runtime"]["Interrupted"] = "true"
                bad.append(item)
                item = copy.deepcopy(base)
                item["ALL STATS"]["Totals"]["Connection Errors"] = 1
                bad.append(item)
                item = copy.deepcopy(base)
                del item["ALL STATS"]["Totals"]["Connection Errors"]
                bad.append(item)
                item = copy.deepcopy(base)
                item["ALL STATS"]["Sets"] = {"Count": 1}
                bad.append(item)
                item = copy.deepcopy(base)
                item["ALL STATS"]["Totals"]["Count"] += 1
                bad.append(item)
                item = copy.deepcopy(base)
                del item["ALL STATS"]["Gets"]
                bad.append(item)
                for i, document in enumerate(bad):
                    with self.subTest(case=i), self.assertRaises((RuntimeError, KeyError)):
                        path.write_text(json.dumps(document))
                        memtier_totals(path, self.cell, 2)
                path.write_text(json.dumps(base))
                self.assertEqual(memtier_totals(path, self.cell, 2)["reported_counts"], {"GET": 5000})

        def test_real_measure_brackets_all_generators_and_rejects_counter_mutants(self):
            from types import SimpleNamespace
            # Exercise Runner.measure itself, including argv production, generator
            # waits, JSON parsing, both endpoint reads and failure artifacts. Only
            # process/network/time boundaries are fake; the accounting is real.
            for corruption in (0, -1, 10000):
                with self.subTest(corruption=corruption), tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                    directory, events, generators = Path(tmp), [], []
                    phase = {"window": 0, "finished": 0}
                    srv = SimpleNamespace(pid=123, poll=lambda: None)
                    def start(argv, log, cwd):
                        if "--protocol=redis" not in argv:
                            return srv
                        events.append("start")
                        path = Path(next(arg.split("=", 1)[1] for arg in argv if arg.startswith("--json-out-file=")))
                        path.write_text(json.dumps(self.accounting_document({"GET": 5000})))
                        def wait(timeout):
                            phase["finished"] += 1
                            events.append("finish")
                            return 0
                        process = SimpleNamespace(poll=lambda: None, wait=wait)
                        generators.append(process)
                        return process
                    children = SimpleNamespace(start=start, stop=lambda process: None)
                    conn = SimpleNamespace(must=lambda *args: [args[-1].encode(), b"1"], close=lambda: None)
                    def snapshot(_conn, section):
                        if section == "server":
                            return {"process_id": "123"}
                        if section == "clients":
                            return {"connected_clients": "1" if not generators or phase["finished"] == 2 else "5"}
                        if section == "commandstats":
                            events.append(("commandstats", len(generators), phase["finished"]))
                            count = (10100 + corruption if phase["finished"] == 2 else
                                     6100 if phase["window"] == 2 else 2100 if phase["window"] else 100)
                            return {"cmdstat_get": f"calls={count}", "cmdstat_info": "calls=99"}
                        self.assertEqual(section, "stats")
                        return {"total_commands_processed": "6101" if phase["window"] == 2 else "2100",
                                "keyspace_misses": "0"}
                    def sleep(seconds):
                        self.assertIn(seconds, (WARMUP, WINDOW))
                        phase["window"] += 1
                    cell = replace(self.cell, conns=4)
                    args = SimpleNamespace(server_cores="0-1", server_smt="", load_cores="2-3", load_smt="",
                                           port=9090, memtier="never-executed-memtier")
                    runner = Runner(args, directory, {"A": Path("never-executed-server")}, children)
                    lb = SimpleNamespace(threads={i: {"role": "fused", "busy": 10, "idle": 0} for i in (0, 1)})
                    with mock.patch.multiple(__name__, require_unbound_port=mock.Mock(), Conn=mock.Mock(return_value=conn),
                            info=mock.Mock(side_effect=snapshot), lb_snapshot=mock.Mock(return_value=lb),
                            cpu_seconds=mock.Mock(return_value=0), busy_between=mock.Mock(return_value=(99, {})),
                            busy_deltas=mock.Mock(return_value={}), productive_saturation=mock.Mock(return_value={})), \
                         mock.patch.object(runner, "populate", return_value=None), \
                         mock.patch.object(time, "sleep", side_effect=sleep), contextlib.redirect_stdout(io.StringIO()):
                        if corruption:
                            with self.assertRaisesRegex(RuntimeError, "accounting mismatch"):
                                runner.measure(cell, "A", 1, 2, {})
                        else:
                            result = runner.measure(cell, "A", 1, 2, {})
                            self.assertEqual(result["commands"], 4000)
                            self.assertEqual(result["whole_run_accounting"]["commands"]["GET"]["server_calls"], 10000)
                    self.assertEqual(events[0], ("commandstats", 0, 0))
                    self.assertEqual(events[-1], ("commandstats", 2, 2))
                    retained = json.loads((directory / cell.id / "n2-1-A/measurement.json").read_text())
                    self.assertEqual(retained["complete"], not bool(corruption))
                    self.assertIn("whole_run_commandstats_after", retained)
                    self.assertEqual(len(retained["memtier"]), 2)

        def test_legacy_control_requires_both_live_verdicts_and_caches_only_success(self):
            from types import SimpleNamespace
            import legacy_reorder_witness
            cell = replace(self.cell, op='REORDER', mode='1s')
            with tempfile.TemporaryDirectory() as tmp:
                folder = Path(tmp)
                binary = folder / 'binary'
                binary.write_bytes(b'exact legacy bytes')
                runner = Runner(SimpleNamespace(server_cores='0-7', server_smt='',
                    load_cores='8-15', load_smt='', port=9090), folder, {'A': binary, 'B': binary}, None)
                self.assertIsNone(runner.legacy_reorder_control(cell, 'B', {'x-ex-sched': 1}))
                self.assertIsNone(runner.legacy_reorder_control(cell, 'A', {'reorder': 1}))
                calls = []
                def run(args, exact_binary, out, mode, reorder):
                    calls.append(reorder)
                    out.mkdir(parents=True, exist_ok=False)
                    return dict(verdict='PASS' if reorder == 0 else 'FAIL', reason='never permuted')
                with mock.patch.object(legacy_reorder_witness, 'run_control', side_effect=run):
                    with self.assertRaisesRegex(RuntimeError, 'reorder=1 control failed'):
                        runner.legacy_reorder_control(cell, 'A', {'x-ex-sched': 1})
                self.assertEqual(calls, [0, 1])
                self.assertEqual(runner.legacy_reorder_controls, {})
                with self.assertRaisesRegex(RuntimeError, 'reorder=1 control failed'):
                    runner.legacy_reorder_control(cell, 'A', {'x-ex-sched': 1})
                self.assertEqual(calls, [0, 1], 'failed engagement controls were retried into green')

        def test_long_tail_cannot_regress_behind_short_tail_improvement(self):
            cell = replace(self.cell, op="REORDER", score="p999", mix="95:5", instances=1)
            round_ = self.round([100] * 4)
            for run in round_["runs"]:
                run["p999_ms"] = 2 if run["arm"] == "A" else 1
                run["long_p999_ms"] = 2 if run["arm"] == "A" else 3
            result = assess(cell, [round_])
            self.assertEqual(result["verdict"], "FAIL")
            self.assertIn("long-command p99.9 regression exceeds measured reference spread", result["reasons"])
            self.assertFalse(result["saturation_exempt"])

        def test_hdr_decoder_matches_recorded_memtier_output_and_rejects_corruption(self):
            from abba_workloads import decode_histogram, percentile
            # Actual memtier GET HDR from the 2026-09-10 h01/n4-1-A artifact.
            # Producer p99.90=1.575ms; this fixture is independent of our encoder.
            encoded = (
                'HISTFAAAA3d4nC2IbWxaZRhA+7wvhY4WuIxBK1m3qJlJY6axWaLR1I+pWzYzly7DzGxLdM50SY1xMXE/jMtkuNIrpRToHa3ISHdL'
                'KaO0q5Qh62iHV0oI61hDWiQUkZKWkqwiYkPpDTFmnh/nJGe3yiCpqcHHap6A/i//Pz37y6GaNwpPBg9xm9ChdwKgxFpunu8QM7sD'
                'LeRb5HnaBJMJ0HqRdRQXxjjetdr8NI+17jD/XG98IHAYiWpip+m6dHWzMXVbblE1k5m95bvP6O7tCxtb6KH9zP2XVvtfLv3YljO8'
                'zYwcufLH8egDBb10Wkd+HEl1UJHP1L9+mfZ8rSqqwDzeBxtzFvBvOSGlmoHEykMwOldANfsPGMPdKGQ2I8rvQt6ZWVRcnkc5dhnp'
                'E2uoPFlEyWAFLd6qorjyKo70d+GMphvbHvfghalevLrZi+lHRnyN1WNdTz92rPfhzY1unDeosJ/aRtX8OirlE8gTnEOV7Slk+96K'
                'QgESFf/ahNV0BpjhRxCZmwF3Zhyy41ZQb+shf1sNuceXq8FLpRtfqJnOXKmD1p9XRs95fefo0CeusU9nf7hgX+vM3LrIJr8y3/lG'
                '0/MdKP8mwdlngKJyANj7FiB/s4F9aQScG6PgsjjAPWQHOjoM4ewNsKcHYP66DmLDXRDsvRzLXSSXOsqhM36yPbN4WGVo0zgOxLee'
                '99/dx+r3uibkSqfMcUfirBDWP4WpJYHWJoiwDfNXBaZuAUsL6C2BNilkR0WloMgUIEy94uqY2FMQa5fF1JZ4+qY4PipevEekvcTG'
                'lCgWFiYHhJ6fBIPDDW5ffbTEn6zsoB7WxVd4iXVuleRah2uTUU71W87sAF4tINqHoiSy/Q5MCHQjUNABe0ndaT9t+bByZvFE8hil'
                'yLSX31s4YW7PnUwqgsepU9mzDoXv5LTCddh6JH1U826xzfma6dX5V6ZbvS9a93tbSk9faw43U0+ZpSU5JbNKtYRjp4ZgiIzIJZok'
                'GMmChJHSUr/U3WTcZZQxkpgkLrvSmJDlGgd3UY1UU1makLglKSJIMPKYvEIMiRKEsmGwjuUqeT7kQ2YUhTLEoAJJSMHRz33w0QcN'
                'p4Ty1tY9By88d5APasB75G/y3q9/AeoA8RDvAJ+PujDinEXAmQHEVcMEciAf9nEieKI2WJfllfgBbhbnwAO1r/8LDBOjCQ=='
            )
            histogram = decode_histogram(encoded)
            self.assertEqual(sum(histogram.values()), 165919453)
            self.assertEqual(percentile(histogram, 99.9), 1.575)
            for invalid in (encoded[:-8], "invalid", "AAAA"):
                with self.subTest(invalid=invalid[:10]), self.assertRaises(Exception):
                    decode_histogram(invalid)

        def test_histograms_merge_counts_instead_of_averaging_percentiles(self):
            documents = [{"short": {10: 100000}, "long": {20: 10000}},
                         {"short": {1000: 1000}, "long": {2000: 1000}}]
            with mock.patch("abba_workloads.command_histogram",
                            side_effect=lambda doc, name, **kw: doc["short" if name == "GET" else "long"]):
                tails = merged_tail(documents)
            self.assertEqual(tails["p999_ms"], 1)
            self.assertEqual(tails["long_p999_ms"], 2)
            self.assertEqual(tails["short_count"], 101000)

        def test_new_unmeasured_pins_fail_before_reference_or_measurement(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                out = Path(tmp) / "out"
                source = Path(tmp) / "unmeasured-cells"
                source.write_text("u01 | 1s | rl=1 | ov=1 | ro=1 | MGET | p8 | 512 | - | - | - | atomic=1 | score=rate | mix=- | smoke=1\n")
                # Exercise the missing-pin precondition even inside a two-CPU gate worker.
                # Synthetic placement is validated separately and never schedules real work here.
                with mock.patch.dict(os.environ, {}, clear=True), \
                     mock.patch.object(sys, "argv", ["abbagate.py", "--subset", "smoke", "--output", str(out),
                         "--cells", str(source), "--server-cores", "0-31", "--server-smt", "",
                         "--load-cores", "32-63", "--load-smt", ""]):
                    args = parse_args()
                with mock.patch(__name__ + ".resolve_reference", side_effect=AssertionError("unmeasured pin reached reference")), \
                     mock.patch(__name__ + ".check_placement"), mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                     contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(main(args), 1)
                result = json.loads((out / "results.json").read_text())
                self.assertIn("unmeasured load floors", result["reason"])
                self.assertIn("--escalate", result["reason"])
                self.assertEqual(result["cells"], [])

        def round(self, rates, n=1, busy=99.5, latency=None):
            return {"instances": n, "runs": [dict(arm=arm, rate=rate, busy_pct=busy,
                    latency_ms=(latency or [1, 1, 1, 1])[i])
                    for i, (arm, rate) in enumerate(zip(ORDER, rates))]}

        def test_abba_cancels_linear_drift(self):
            p = paired(self.round([100, 100.1, 100.2, 100.3])["runs"])
            self.assertAlmostEqual(p["delta_pct"], 0)
            self.assertAlmostEqual(p["threshold_pct"], 100 * .3 / 100.15)
            self.assertGreater(p["pair_deltas_pct"][0], 0)
            self.assertLess(p["pair_deltas_pct"][1], 0)

        def test_aabb_is_rejected(self):
            runs = self.round([100] * 4)["runs"]
            runs[1]["arm"], runs[3]["arm"] = "A", "B"
            with self.assertRaises(ValueError):
                paired(runs)

        def test_regression_cannot_hide_in_reference_noise(self):
            rounds = [self.round([100, 98, 98, 100.1], n) for n in (1, 2)]
            a = assess(self.cell, rounds)
            self.assertEqual(a["verdict"], "FAIL")
            self.assertGreater(a["loss_pct"], a["threshold_pct"])
            self.assertTrue(saturation_done(self.cell, rounds))

        def test_flat_rate_just_under_98_busy_is_saturated(self):
            # The 2026-09-10 h12 shape: more load stops raising the rate while the server sits a
            # little under 98% busy, because the candidate is FASTER and needs less CPU for the
            # same offered work. That is a plateau, not an unsaturated cell, and it must not fail.
            rounds = [self.round([100] * 4, n, busy=97.1) for n in (1, 2, 4, 8)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "PASS")

        def test_flat_rate_below_the_busy_floor_still_fails(self):
            # A plateau does not excuse an idle server: below the floor the cell is rejected even
            # though more load changes nothing, because that much headroom can absorb a regression.
            rounds = [self.round([100] * 4, n, busy=BUSY_FLOOR - 1) for n in (1, 2, 4, 8)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")
            self.assertFalse(saturation_done(self.cell, rounds))

        def test_verdict_is_taken_at_the_peak_not_the_last_block(self):
            # Congestion collapse: h12 peaked at 2 instances and decayed by 16. Judging the last
            # block scores a deliberately degraded measurement. The peak is the measurement; the
            # block above it exists only to prove it is a peak.
            rounds = [self.round([100, 100, 100, 100], 1),
                      self.round([200, 240, 240, 200], 2),   # peak, candidate clearly ahead
                      self.round([150, 150, 150, 150], 4)]   # congestion past the peak
            a = assess(self.cell, rounds)
            self.assertEqual(a["instances"], 2)
            self.assertGreater(a["delta_pct"], 15)
            self.assertEqual(a["verdict"], "PASS")

        def test_reference_lacking_a_requested_on_knob_is_not_comparable(self):
            # h12 (2s, --overlap 1) against c8e61f646, which predates --overlap: dropping the flag
            # measured overlap-on vs overlap-off and called it a "+17.02%" code win.
            support = {"A": {"thread-mode": True, "read-local": True, "overlap": False,
                             "reorder": False},
                       "B": {"thread-mode": True, "read-local": True, "overlap": True,
                             "reorder": True}}
            on = Cell("h12", "2s", 0, 1, 0, "SET", 32, 512)
            with self.assertRaises(NotComparable):
                knob_plan(on, support)
            # ... but a knob requested OFF is exactly the reference's legacy behaviour, so that
            # cell stays comparable and only earns a note.
            off = Cell("h01", "1s", 1, 0, 0, "GET", 32, 512)
            plans, notes = knob_plan(off, support)
            self.assertTrue(any("legacy behaviour" in n for n in notes))
            self.assertNotIn("overlap", plans["A"])

        def test_a_pinned_cell_needs_no_second_rung(self):
            pinned = Cell("hp", "2s", 0, 1, 0, "SET", 32, 512, instances=2)
            rounds = [self.round([100, 100, 100, 100], 2)]
            self.assertEqual(assess(pinned, rounds)["verdict"], "PASS")

        def test_a_pin_that_stops_saturating_fails_and_says_how_to_fix_it(self):
            # A candidate fast enough to outgrow its pinned load must not be measured in headroom.
            pinned = Cell("hp", "2s", 0, 1, 0, "SET", 32, 512, instances=2)
            rounds = [self.round([100] * 4, 2, busy=BUSY_FLOOR - 5)]
            a = assess(pinned, rounds)
            self.assertEqual(a["verdict"], "FAIL")
            self.assertTrue(any("re-pin" in r for r in a["reasons"]), a["reasons"])

        def test_a_peak_at_the_top_is_not_yet_proven(self):
            # Still climbing: without a higher probe that fails to beat it, the top block might
            # simply be the last one we ran.
            rounds = [self.round([100] * 4, 1), self.round([200] * 4, 2)]
            self.assertIn("no higher-instance saturation probe above the peak block",
                          assess(self.cell, rounds)["reasons"])
            self.assertFalse(saturation_done(self.cell, rounds))

        def test_fastest_arm_still_gaining_fails(self):
            rounds = [self.round([100, 110, 110, 100], 1), self.round([100, 120, 120, 100], 2)]
            self.assertFalse(saturation_done(self.cell, rounds))
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")

        def test_one_probe_cannot_prove_plateau(self):
            self.assertEqual(assess(self.cell, [self.round([100] * 4)])["verdict"], "FAIL")

        def test_busy_floor_applies_to_every_run_of_the_judged_block(self):
            # One idle run inside the block being judged is enough to reject it.
            rounds = [self.round([100] * 4, n) for n in (1, 2)]
            self.assertEqual(peak_index(rounds), 0)
            rounds[0]["runs"][2]["busy_pct"] = 50
            self.assertFalse(saturation_done(self.cell, rounds))
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")

        def test_reference_noise_cannot_turn_a_bad_session_green(self):
            rounds = [self.round([100, 99, 99, 103], n) for n in (1, 2)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")

        def test_stable_equal_arms_pass(self):
            rounds = [self.round([100, 100, 100, 100], n) for n in (1, 2)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "PASS")
            self.assertEqual(assess(self.cell, rounds)["threshold_pct"], 0)

        def test_depth_one_uses_latency_and_is_exempt(self):
            c = Cell("p1", "2s", 0, 0, 0, "GET", 1, 512)
            a = assess(c, [self.round([100] * 4, busy=10, latency=[1, 1.1, 1.1, 1.001])])
            self.assertEqual(a["verdict"], "FAIL")
            self.assertTrue(a["saturation_exempt"])
            self.assertGreater(a["loss_pct"], 9)
            self.assertEqual(assess(c, [self.round([100] * 4, busy=10)])["verdict"], "PASS")

        def test_worst_failure_not_an_average(self):
            rows = [{"cell": {"id": "slow"}, "verdict": "FAIL", "assessment": {"margin_pct": 3}}]
            rows += [{"cell": {"id": str(i)}, "verdict": "PASS", "assessment": {"margin_pct": -90}}
                     for i in range(31)]
            self.assertEqual(overall(rows), ("FAIL", "slow"))

        def test_historical_numbers_are_not_inputs(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                f = Path(tmp) / "cells"
                f.write_text("h01 | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | garbage | stale | ignored\n")
                self.assertEqual(read_cells(f), [self.cell])
                f.write_text(f.read_text() * 2)
                with self.assertRaises(ValueError):
                    read_cells(f)

        def test_reference_must_match_last_push(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                binary = directory / "tomokv-old-1234567"
                binary.write_text("old")
                binary.chmod(0o700)
                (directory / "MANIFEST.md").write_text("| `tomokv-old-1234567` | 1234567 | old |\n")
                self.assertIsNone(manifest_reference(directory, "abcdef0" + "0" * 33))
                self.assertEqual(manifest_reference(directory, "1234567" + "0" * 33), binary)

        def test_missing_knobs_are_reported_and_candidate_keeps_them(self):
            support = {arm: {k: True for k in ("thread-mode", "read-local", "overlap", "reorder")}
                       for arm in ("A", "B")}
            support["A"].update(overlap=False, reorder=False)
            plans, notes = knob_plan(self.cell, support)
            self.assertEqual(len(notes), 2)
            self.assertNotIn("overlap", plans["A"])
            self.assertIn("overlap", plans["B"])

        def test_idle_spinning_is_not_busy(self):
            start = {0: {"role": "fused", "busy": 0, "idle": 0}}
            end = {0: {"role": "fused", "busy": 1, "idle": 99}}
            self.assertEqual(busy_between(start, end)[0], 1)
            with self.assertRaises(RuntimeError):
                busy_between(start, start)

        def test_raw_role_deltas_preserve_idle_executor_evidence(self):
            start = {0: {"role": "io", "busy": 10, "idle": 20},
                     1: {"role": "ex", "busy": 30, "idle": 40}}
            end = {0: {"role": "io", "busy": 108, "idle": 22},
                   1: {"role": "ex", "busy": 30, "idle": 140}}
            self.assertEqual(busy_deltas(start, end), {
                0: {"role": "io", "busy_ns": 98, "idle_ns": 2},
                1: {"role": "ex", "busy_ns": 0, "idle_ns": 100}})
            self.assertEqual(busy_between(start, end), (49, {0: 98, 1: 0}))

        def test_load_escalation_preserves_total_connections(self):
            load = list(range(64, 128)) + list(range(192, 256))
            for n in (1, 2, 3, 4, 8):
                layout = load_layout(load, n, 512)
                self.assertEqual(sum(x["threads"] * x["clients"] for x in layout), 512)
                assigned = [c for x in layout for c in x["cpus"]]
                self.assertEqual(sorted(assigned), load)
                self.assertEqual(len(assigned), len(set(assigned)))

        def fake_main(self, *, pin="-", depth=32, escalate=False, busy=99.9,
                      climbing=False, ceiling=16, contend_after=None, reference_error=None):
            # Invoke main() and its real load layout, not assess() with fabricated
            # rounds. The regression was in the loop that PRODUCES rounds, and a
            # pin=3/512 fixture also catches silently skipping a non-doubling pin.
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                binary = directory / "candidate"
                binary.write_bytes(b"test identity; never executed")
                binary.chmod(0o700)
                source = directory / "cells"
                source.write_text(f"hp | 1s | rl=1 | ov=0 | ro=0 | GET | p{depth} | 512 | stale | stale | {pin}\n")
                output = directory / "out"
                argv = ["abbagate.py", "--candidate", str(binary), "--cells", str(source),
                        "--output", str(output), "--memtier", sys.executable,
                        "--server-cores", "0-31", "--load-cores", "32-127",
                        "--max-instances", str(ceiling)] + (["--escalate"] if escalate else [])
                with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, {}, clear=True):
                    args = parse_args()
                order, layouts = [], []
                self.support_calls = []

                def support_probe(*args):
                    self.support_calls.append(args)
                    return True

                def measure(runner, cell, arm, sequence, instances, knobs):
                    order.append((instances, arm))
                    layouts.append(load_layout(runner.load_cpus, instances, cell.conns))
                    if contend_after == len(order):
                        self.quiet.check.side_effect = QuietViolation("PID 123 (foreign): one CPU tick")
                        witness = {"interference": {"processes": [{"pid": 123, "cpu_ticks": 1}]},
                                   "samples": 4, "complete": False}
                        self.quiet.evidence.return_value = witness
                        self.quiet.close.return_value = witness
                    return dict(arm=arm, rate=instances * 100 if climbing else 100,
                                busy_pct=busy, latency_ms=1)

                provenance = dict(source="test", commit="0" * 40, sha256=sha256(binary))
                stream = io.StringIO()
                with mock.patch.object(Runner, "measure", measure), \
                     mock.patch(__name__ + ".resolve_reference", return_value=(binary, provenance),
                                side_effect=reference_error), \
                     mock.patch(__name__ + ".accepted", side_effect=support_probe), \
                     mock.patch(__name__ + ".check_placement"), \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                     contextlib.redirect_stdout(stream):
                    rc = main(args)
                return rc, order, layouts, json.loads((output / "results.json").read_text()), stream.getvalue()

        def test_contender_invalidates_real_loop_without_retry_or_threshold_change(self):
            threshold_before = paired(self.round([100, 100, 100, 100])["runs"])["threshold_pct"]
            rc, order, _, report, _ = self.fake_main(pin=4, contend_after=2)
            self.assertEqual(rc, 1)
            self.assertEqual(order, [(4, "A"), (4, "B")])
            self.assertFalse(report["measurement_valid"])
            self.assertEqual(report["quiet_box"]["interference"]["processes"][0]["pid"], 123)
            self.assertEqual(len(report["cells"][0]["rounds"][0]["runs"]), 2)
            self.assertFalse(report["cells"][0]["instrument_valid"])
            self.assertIn("QuietViolation", report["reason"])
            self.assertEqual(paired(self.round([100, 100, 100, 100])["runs"])["threshold_pct"], threshold_before)

        def test_changed_harness_invalidates_the_real_measurement_loop(self):
            with mock.patch(__name__ + ".harness_fingerprint", side_effect=[
                    {"sha256": "a" * 64}, {"sha256": "b" * 64}]):
                rc, measurements, _, report, _ = self.fake_main(pin="4")
            self.assertEqual(rc, 1)
            self.assertEqual(len(measurements), 4)
            self.assertFalse(report["measurement_valid"])
            self.assertFalse(report["cells"][0]["instrument_valid"])
            self.assertIn("harness changed", report["reason"])

        def test_changed_instrument_invalidates_the_real_measurement_loop(self):
            original = instrument_fingerprint(ROOT)
            with mock.patch(__name__ + ".instrument_fingerprint", side_effect=[
                    original, {**original, "sha256": "b" * 64}]):
                rc, measurements, _, report, _ = self.fake_main(pin="4")
            self.assertEqual((rc, len(measurements)), (1, 4))
            self.assertFalse(report["measurement_valid"])
            self.assertIn("instrument changed", report["reason"])

        def test_contended_preflight_never_reaches_support_or_measurement(self):
            self.quiet.start.side_effect = QuietViolation("foreign compiler is active")
            rc, order, _, report, _ = self.fake_main(pin=4)
            self.assertEqual(rc, 1)
            self.assertEqual(order, [])
            self.assertEqual(report["cells"], [])
            self.assertEqual(self.support_calls, [])
            self.assertIn("foreign compiler", report["reason"])
            self.assertFalse(report["measurement_valid"])

        def test_real_main_passes_actual_measurement_window_to_quiet_monitor(self):
            for window in (10, 20):
                with mock.patch(__name__ + ".WINDOW", window):
                    _, measurements, _, _, _ = self.fake_main(pin=4)
                self.assertEqual(len(measurements), 4)
                self.assertEqual(self.quiet_factory.call_args.kwargs["window_seconds"], window)

        def test_final_observation_can_fail_completed_real_loop(self):
            def close():
                self.quiet.check.side_effect = QuietViolation("foreign activity at final sample")
                return {"interference": {"error": "final sample"}, "samples": 5, "complete": False}
            self.quiet.close.side_effect = close
            rc, order, _, report, _ = self.fake_main(pin=4)
            self.assertEqual(rc, 1)
            self.assertEqual(len(order), 4)
            self.assertEqual(report["verdict"], "FAIL")
            self.assertFalse(report["measurement_valid"])
            self.assertFalse(report["cells"][0]["instrument_valid"])

        def test_contamination_during_reference_skip_still_fails_whole_tier(self):
            def close():
                self.quiet.check.side_effect = QuietViolation("foreign work during reference lookup")
                return {"interference": {"error": "foreign work"}, "complete": False}
            self.quiet.close.side_effect = close
            rc, order, _, report, _ = self.fake_main(pin=4, reference_error=Skip("reference missing"))
            self.assertEqual(rc, 1)
            self.assertEqual(order, [])
            self.assertEqual(report["verdict"], "FAIL")
            self.assertFalse(report["measurement_valid"])

        def test_pin_drives_real_loop_to_exactly_four_measurements(self):
            for pin in (3, 4):
                with self.subTest(pin=pin):
                    rc, order, layouts, result, output = self.fake_main(pin=pin)
                    self.assertEqual(rc, 3, output)  # No standing null: raw success cannot be trusted PASS.
                    self.assertEqual(result["statistical_verdict"], "PASS")
                    self.assertEqual(order, [(pin, arm) for arm in ORDER])
                    self.assertEqual(len(result["cells"][0]["rounds"]), 1)
                    self.assertIn("PINNED", output)
                    for layout in layouts:
                        self.assertEqual(sum(x["threads"] * x["clients"] for x in layout), 512)
                    if pin == 3:
                        self.assertEqual([x["threads"] for x in layouts[0]], [16, 16, 16])
                        self.assertEqual([x["clients"] for x in layouts[0]], [10, 11, 11])

        def test_escalate_ignores_pin_and_drives_full_ladder(self):
            rc, order, _, result, output = self.fake_main(pin=3, escalate=True, climbing=True)
            self.assertEqual(order, [(n, arm) for n in LADDER for arm in ORDER])
            self.assertEqual(len(order), 20)
            self.assertEqual(rc, 1, output)  # Still rising at the ceiling is unproven saturation.
            self.assertIn("ESCALATE ignores pin=3", output)
            self.assertIn("no higher-instance saturation probe above the peak block",
                          result["cells"][0]["assessment"]["reasons"])

        def test_escalate_cannot_borrow_pin_for_a_single_block(self):
            rc, order, _, result, _ = self.fake_main(pin=1, escalate=True, ceiling=1)
            self.assertEqual(order, [(1, arm) for arm in ORDER])
            self.assertEqual(rc, 1)
            self.assertIn("no higher-instance saturation probe above the peak block",
                          result["cells"][0]["assessment"]["reasons"])

        def test_pin_that_outgrows_load_fails_after_four_and_names_remedy(self):
            rc, order, _, result, _ = self.fake_main(pin=3, busy=BUSY_FLOOR - 1)
            self.assertEqual(order, [(3, arm) for arm in ORDER])
            self.assertEqual(rc, 1)
            reasons = result["cells"][0]["assessment"]["reasons"]
            self.assertTrue(any("re-pin it with --escalate" in reason for reason in reasons), reasons)

        def test_unpinned_real_loop_searches_and_says_so(self):
            rc, order, _, _, output = self.fake_main()
            self.assertEqual(rc, 3, output)
            self.assertEqual(order, [(n, arm) for n in (1, 2) for arm in ORDER])
            self.assertIn("UNPINNED", output)

        def test_depth_one_ignores_deep_pipeline_pin(self):
            rc, order, _, result, output = self.fake_main(pin=3, depth=1, busy=1)
            self.assertEqual(rc, 3, output)
            self.assertEqual(order, [(1, arm) for arm in ORDER])
            self.assertTrue(result["cells"][0]["assessment"]["saturation_exempt"])

        def test_pin_beyond_budget_is_loud_not_an_unreached_measurement(self):
            rc, order, _, result, output = self.fake_main(pin=4, ceiling=2)
            self.assertEqual(rc, 1, output)
            self.assertEqual(order, [])
            self.assertIn("pinned load level 4 exceeds", result["cells"][0]["reason"])

        def test_explicit_axes_and_binary_arguments(self):
            argv = ["abbagate.py", "--candidate-binary", "/candidate", "--reference-binary", "/reference",
                    "--server-cores", "0-7", "--server-smt", "128-135", "--load-cores", "8-15",
                    "--load-smt", "136-143", "--ports", "19000-19009"]
            with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, {}, clear=True):
                args = parse_args()
            self.assertEqual(args.candidate, Path("/candidate"))
            self.assertEqual(args.reference_binary, Path("/reference"))
            runner = Runner(args, Path("/unused"), {}, Children())
            self.assertEqual(runner.server_cpus, list(range(8)) + list(range(128, 136)))
            self.assertEqual(runner.load_cpus, list(range(8, 16)) + list(range(136, 144)))
            self.assertEqual(select_port(args.ports, args.port), (19000, (19000, 19009)))
            with mock.patch.object(sys, "argv", ["abbagate.py"]), \
                 mock.patch.dict(os.environ, {}, clear=True):
                defaults = parse_args()
            self.assertEqual((defaults.server_smt, defaults.load_smt), ("", None))

        def test_standalone_defaults_use_all_other_physical_and_smt_load_cores(self):
            topology = {cpu: frozenset((cpu % 128, cpu % 128 + 128)) for cpu in range(256)}
            for explicit_smt in (None, ""):
                with self.subTest(load_smt=explicit_smt):
                    args = argparse.Namespace(server_cores=None, server_smt="", load_cores=None, load_smt=explicit_smt)
                    with mock.patch(__name__ + ".read_topology", return_value=topology), \
                         mock.patch(__name__ + ".permitted_cpus", return_value=set(range(256))):
                        resolve_geometry(args)
                    self.assertEqual(cpus(args.server_cores), list(range(32)))
                    self.assertEqual(cpus(args.load_cores), list(range(32, 128)))
                    self.assertEqual(cpus(args.load_smt), list(range(160, 256)) if explicit_smt is None else [])

        def test_port_boundaries_reject_any_bind_outside_the_budget(self):
            self.assertEqual(select_port("7899-7899", None), (7899, (7899, 7899)))
            self.assertEqual(select_port("1-65535", 65535)[0], 65535)
            for permitted, selected in (("9000-9001", 8999), ("9000-9001", 9002),
                                        ("0-10", None), ("10-9", None), ("9-65536", None),
                                        ("9", None), ("9,10", None)):
                with self.subTest(permitted=permitted, selected=selected), self.assertRaises(ValueError):
                    select_port(permitted, selected)

        def test_bind_guard_accepts_owned_time_wait_that_plain_bind_rejects(self):
            import errno
            # Make the accepted peer the active closer, putting OUR server port into
            # TIME_WAIT. Connect only to this listener, on a kernel-assigned ephemeral port.
            with socket.socket() as listener, socket.socket() as client:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.settimeout(2)
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                address = listener.getsockname()
                client.settimeout(2)
                client.connect(address)
                remote = client.getsockname()
                peer, _ = listener.accept()
                peer.close()
                self.assertEqual(client.recv(1), b"")
            expected = [f"0100007F:{address[1]:04X}", f"0100007F:{remote[1]:04X}"]
            deadline = time.monotonic() + 2
            while True:
                states = [row.split()[3] for row in Path("/proc/net/tcp").read_text().splitlines()[1:]
                          if row.split()[1:3] == expected]
                if states == ["06"]:  # Prove TIME_WAIT opened; never skip the window.
                    break
                self.assertLess(time.monotonic(), deadline, f"owned connection never entered TIME_WAIT: {states}")
                time.sleep(.01)
            with socket.socket() as old_probe, self.assertRaises(OSError) as caught:
                old_probe.bind(address)
            self.assertEqual(caught.exception.errno, errno.EADDRINUSE)
            require_unbound_port(address[1])

        def test_bind_guard_rejects_live_listener_even_with_reuseport(self):
            import errno
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                with self.assertRaises(OSError) as caught:
                    require_unbound_port(listener.getsockname()[1])
                self.assertEqual(caught.exception.errno, errno.EADDRINUSE)

        def test_placement_caps_physical_cores_and_preserves_explicit_smt(self):
            with mock.patch(__name__ + ".validate_axes") as validate:
                check_placement(list(range(32)), list(range(32, 64)), list(range(128, 160)), [])
                validate.assert_called_once_with(list(range(32)), list(range(128, 160)),
                                                 list(range(32, 64)), [])
                with self.assertRaises(ValueError):
                    check_placement(list(range(33)), [40])

        def test_explicit_reference_records_digest_without_fabricating_a_commit(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                binary = directory / "reference"
                binary.write_bytes(b"caller identified reference")
                binary.chmod(0o700)
                with mock.patch.object(sys, "argv", ["abbagate.py", "--reference-binary", str(binary)]):
                    args = parse_args()
                with mock.patch(__name__ + ".git", side_effect=RuntimeError("no remote")):
                    actual, provenance = resolve_reference(args, directory)
                self.assertEqual(actual, binary)
                self.assertEqual(provenance["sha256"], sha256(binary))
                self.assertEqual(provenance["commit"], "caller-supplied, unverified")

        def test_real_orchestration_order_and_negative_control(self):
            # Exercise the actual driver loop and JSON/exit verdict, replacing only the
            # expensive measurement boundary. Removing comparison or reordering AABB fails.
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                binary = directory / "candidate"
                binary.write_bytes(b"test executable identity; never executed")
                binary.chmod(0o700)
                source = directory / "cells"
                source.write_text("h01 | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | stale | stale | stale\n")
                for candidate_rate, expected in ((100, 3), (98, 1)):
                    output = directory / str(candidate_rate)
                    argv = ["abbagate.py", "--candidate", str(binary), "--cells", str(source),
                            "--output", str(output), "--memtier", sys.executable, "--max-instances", "2",
                            "--server-cores", "0-31", "--server-smt", "",
                            "--load-cores", "32-127", "--load-smt", ""]
                    # A correctness worker has only its small load affinity. Defaulting this
                    # serverless fixture to that affinity prevented main() from reaching even
                    # one fake measurement; an expected setup error proves no regression check.
                    # Keep fixture geometry and parser defaults independent of the live gate.
                    with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, {}, clear=True):
                        args = parse_args()
                    order = []

                    def measure(_self, cell, arm, sequence, instances, knobs):
                        order.append((instances, arm))
                        return dict(arm=arm, rate=100 if arm == "A" else candidate_rate,
                                    busy_pct=99.9, latency_ms=1)

                    provenance = dict(source="test", commit="0" * 40, sha256=sha256(binary))
                    with mock.patch.object(Runner, "measure", measure), \
                         mock.patch(__name__ + ".resolve_reference", return_value=(binary, provenance)), \
                         mock.patch(__name__ + ".accepted", return_value=True), \
                         mock.patch(__name__ + ".check_placement"), \
                         mock.patch.object(os, "sched_setaffinity"), \
                         mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                         contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(main(args), expected)
                    self.assertEqual(order, [(n, a) for n in (1, 2) for a in ORDER])
                    result = json.loads((output / "results.json").read_text())
                    self.assertEqual(result["worst_cell"], "h01")
                    self.assertEqual(result["verdict"], "PARTIAL" if expected == 3 else "FAIL")

        def test_real_null_collection_comparison_and_subset_controls(self):
            import copy
            from abba_evidence import validate_comparison
            # Both control and comparison are produced by main's real measurement loop. A fake
            # clock replaces only elapsed workload time; boot/generator calls remain fake. This
            # observes counts and verdict publication rather than constructing rounds for assess.
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
                directory = Path(temporary)
                binary = directory / "candidate"
                binary.write_bytes(b"frozen identity, never executed")
                binary.chmod(0o700)
                source = directory / "cells"
                source.write_text(
                    "n1 | 1s | rl=1 | ov=1 | ro=1 | GET | p32 | 512 | - | - | 4 | atomic=1 | score=rate | mix=- | smoke=1\n"
                    "n2 | 2s | rl=0 | ov=1 | ro=1 | SET | p32 | 512 | - | - | 4 | atomic=1 | score=rate | mix=- | smoke=0\n")
                epoch, ticks = int(time.time()) - 10000, [0.]
                original_gmtime = time.gmtime
                sequence = [0]

                def run(*, collect=False, subset="full", control=None, candidate_rate=100, only=""):
                    sequence[0] += 1
                    out = directory / f"run-{sequence[0]}"
                    null_path = directory / "standing.json"
                    if control is not None:
                        null_path.write_text(json.dumps(control))
                    else:
                        null_path.unlink(missing_ok=True)
                    argv = ["abbagate.py", "--candidate", str(binary), "--cells", str(source), "--output", str(out),
                        "--memtier", sys.executable, "--server-cores", "0-31", "--load-cores", "32-127", "--load-smt", "",
                        "--subset", subset, "--collect-null", str(int(collect)), "--null-result", str(null_path)]
                    if only:
                        argv += ["--only", only]
                    with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, {}, clear=True):
                        args = parse_args()
                    calls = []
                    quiet_started = epoch + ticks[0]
                    cpus_ = list(range(128))
                    quiet = mock.Mock()
                    def evidence():
                        return dict(complete=True, interference=None, started_at=quiet_started,
                            finished_at=epoch + ticks[0], samples=max(2, int(epoch + ticks[0] - quiet_started)),
                            sample_interval_seconds=1, cpus=cpus_, requested_cpus=cpus_)
                    quiet.evidence.side_effect = evidence
                    quiet.close.side_effect = evidence
                    def measure(_runner, cell, arm, index, instances, knobs):
                        calls.append((cell.id, instances, arm))
                        ticks[0] += WINDOW + 8
                        return dict(arm=arm, rate=100 if arm == "A" else candidate_rate, busy_pct=99.9,
                            latency_ms=1, complete=True, commands=2000, pid=123, window_seconds=WINDOW,
                            artifacts=f"{cell.id}/n{instances}-{index}-{arm}")
                    provenance = dict(source="fake reference", commit="0" * 40, sha256=sha256(binary))
                    with mock.patch.object(Runner, "measure", measure), \
                         mock.patch(__name__ + ".resolve_reference", return_value=(binary, provenance)) as resolve, \
                         mock.patch(__name__ + ".accepted", return_value=True), \
                         mock.patch(__name__ + ".check_placement"), \
                         mock.patch(__name__ + ".QuietMonitor", return_value=quiet), \
                         mock.patch.object(os, "sched_setaffinity"), \
                         mock.patch.object(time, "time", side_effect=lambda: epoch + ticks[0]), \
                         mock.patch.object(time, "monotonic", side_effect=lambda: ticks[0]), \
                         mock.patch.object(time, "gmtime", side_effect=lambda seconds=None:
                             original_gmtime(epoch + ticks[0] if seconds is None else seconds)), \
                         mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                         contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                        rc = main(args)
                    if collect:
                        resolve.assert_not_called()
                    return rc, calls, json.loads((out / "results.json").read_text()), out

                rc, calls, control, control_out = run(collect=True)
                self.assertIn("null_control", control, control)
                self.assertEqual((rc, len(calls), control["verdict"], control["null_control"]["verdict"]),
                                 (3, 8, "PARTIAL", "PASS"))
                self.assertFalse(control["comparison_trusted"])
                self.assertEqual((control_out / "binary-A").read_bytes(), (control_out / "binary-B").read_bytes())
                binary.write_bytes(b"a later candidate may reuse this instrument control")
                rc, calls, report, _ = run()
                self.assertEqual((rc, len(calls), report["statistical_verdict"], report["verdict"]),
                                 (3, 8, "PASS", "PARTIAL"))
                self.assertTrue(report["measurement_valid"])
                self.assertFalse(report["comparison_trusted"])
                for subset, count in (("full", 8), ("smoke", 4)):
                    rc, calls, report, out = run(control=control, subset=subset)
                    self.assertEqual((rc, len(calls), report["verdict"]), (0, count, "PASS"), report)
                    self.assertTrue(report["comparison_trusted"])
                    self.assertNotEqual(report["candidate"]["sha256"], control["candidate"]["sha256"])
                    self.assertEqual(read_json(out / "null-control.json"), control)
                    validate_comparison(report, control, now=epoch + ticks[0])
                changed_correctness = copy.deepcopy(control)
                changed_correctness["receipt_harness_sha256"] = "f" * 64
                rc, calls, report, _ = run(control=changed_correctness, subset="smoke")
                self.assertEqual((rc, len(calls), report["comparison_trusted"]), (0, 4, True))
                rc, calls, report, _ = run(control=control, only="n1")
                self.assertEqual((rc, len(calls), report["verdict"], report["comparison_trusted"]),
                                 (3, 4, "PARTIAL", False))
                rc, calls, report, _ = run(control=[])
                self.assertEqual((rc, len(calls), report["statistical_verdict"], report["verdict"]),
                                 (3, 8, "PASS", "PARTIAL"))
                defects = {
                    "failed unselected cell": lambda c: c["cells"][1].update(verdict="FAIL"),
                    "instrument": lambda c: c["instrument_fingerprint"].update(sha256="f" * 64),
                    "old hash only": lambda c: c.pop("instrument_fingerprint"),
                    "generator": lambda c: c["environment"].update(memtier_sha256="f" * 64),
                    "window": lambda c: c.update(window_seconds=10),
                    "different bytes": lambda c: c["candidate"].update(sha256="f" * 64),
                    "quiet": lambda c: c["quiet_box"].update(complete=False),
                    "missing cell": lambda c: c["cells"].pop(),
                    "population": lambda c: c["environment"]["population_by_arm"].update(B="snapshot"),
                    "changed pin": lambda c: c["cells"][0]["cell"].update(instances=8),
                }
                for name, defect in defects.items():
                    broken = copy.deepcopy(control)
                    defect(broken)
                    with self.subTest(defect=name):
                        rc, calls, report, _ = run(control=broken, subset="smoke")
                        self.assertEqual((rc, len(calls), report["statistical_verdict"], report["verdict"]),
                                         (3, 4, "PASS", "PARTIAL"))
                        self.assertFalse(report["comparison_trusted"])
                rc, calls, report, _ = run(control=control, candidate_rate=98)
                self.assertEqual((rc, len(calls), report["statistical_verdict"], report["verdict"]),
                                 (1, 8, "FAIL", "FAIL"))
                rc, calls, report, _ = run(collect=True, candidate_rate=98)
                self.assertEqual((rc, len(calls), report["statistical_verdict"], report["null_control"]["verdict"]),
                                 (1, 8, "FAIL", "FAIL"))
                ticks[0] += 86401
                rc, calls, report, _ = run(control=control)
                self.assertEqual((rc, len(calls), report["verdict"]), (3, 8, "PARTIAL"))
                self.assertIn("24 hours", report["standing_null"]["reason"])
                # Execute the gate's actual ABBA exit classifier with the collection's exit 3.
                # Its counted row must go red even though null_control itself passed.
                gate = (ROOT / "tests/gate.sh").read_text()
                block = gate[gate.index('case "$ABBA_RC" in'):gate.index("\nphase abba-end")]
                script = 'PASS=0; FAIL=0; ABBA_RC=3\nok(){ PASS=$((PASS+1)); }; bad(){ FAIL=$((FAIL+1)); };\n'
                checked = subprocess.run(["bash"], input=script + block + '\n[ "$PASS:$FAIL" = 0:1 ]\n',
                    text=True, capture_output=True)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

        def test_physical_load_ceiling_keeps_unproven_saturation_red(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                binary = directory / "candidate"
                binary.write_bytes(b"test executable identity; never executed")
                binary.chmod(0o700)
                source = directory / "cells"
                source.write_text("h01 | 1s | rl=1 | ov=0 | ro=0 | GET | p32 | 512 | stale | stale | -\n")
                output = directory / "out"
                argv = ["abbagate.py", "--candidate-binary", str(binary), "--cells", str(source),
                        "--output", str(output), "--memtier", sys.executable,
                        "--server-cores", "0-7", "--load-cores", "8-15", "--load-smt", "136-143"]
                with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, {}, clear=True):
                    args = parse_args()
                order = []

                def measure(_self, cell, arm, sequence, instances, knobs):
                    order.append((instances, arm))
                    if instances > 8:
                        raise RuntimeError("SMT cannot manufacture a ninth physical load group")
                    return dict(arm=arm, rate=instances * 100, busy_pct=99.9, latency_ms=1)

                provenance = dict(source="test", commit="0" * 40, sha256=sha256(binary))
                with mock.patch.object(Runner, "measure", measure), \
                     mock.patch(__name__ + ".resolve_reference", return_value=(binary, provenance)), \
                     mock.patch(__name__ + ".accepted", return_value=True), \
                     mock.patch(__name__ + ".check_placement"), \
                     mock.patch.object(os, "sched_setaffinity"), \
                     mock.patch.dict(os.environ, {"GATE_QUIET_FILE": ""}), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main(args), 1)
                self.assertEqual(order, [(n, arm) for n in (1, 2, 4, 8) for arm in ORDER])
                result = json.loads((output / "results.json").read_text())
                row = result["cells"][0]
                self.assertEqual(result["verdict"], "FAIL")
                self.assertEqual(result["environment"]["load_instance_ceiling"], 8)
                self.assertNotIn("reason", row)  # no placement exception replaces measurement evidence
                self.assertIn("no higher-instance saturation probe above the peak block",
                              row["assessment"]["reasons"])

        def test_only_owned_children_are_stopped(self):
            with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                directory = Path(tmp)
                children = Children()
                outsider = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
                try:
                    child = children.start([sys.executable, "-c", "import time; time.sleep(60)"],
                                           directory / "child.log", directory)
                    children.close()
                    self.assertIsNotNone(child.poll())
                    self.assertIsNone(outsider.poll())
                finally:
                    children.close()
                    outsider.terminate()
                    outsider.wait(timeout=10)

        def test_build_abort_stops_only_pids_in_its_owned_session(self):
            children = Children()
            outsider = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
            build = None
            try:
                with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                    directory = Path(tmp)
                    pidfile = directory / "compiler.pid"
                    script = ("import subprocess,sys,time; from pathlib import Path; "
                              "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)']); "
                              f"Path({str(pidfile)!r}).write_text(str(p.pid)); time.sleep(60)")
                    build = children.start([sys.executable, "-c", script], directory / "make.log", directory)
                    deadline = time.monotonic() + 5
                    while not pidfile.exists() and time.monotonic() < deadline:
                        time.sleep(.01)
                    self.assertTrue(pidfile.exists())
                    compiler_pid = int(pidfile.read_text())
                    stop_build(build)
                    self.assertIsNotNone(build.poll())
                    self.assertIsNone(outsider.poll())
                    # A killed grandchild may remain a zombie until PID 1 reaps it; it cannot do
                    # CPU work. Check that state without ever discovering a process by its argv.
                    status = Path(f"/proc/{compiler_pid}/stat")
                    deadline = time.monotonic() + 5
                    while status.exists() and time.monotonic() < deadline:
                        if status.read_text().rsplit(")", 1)[1].split()[0] == "Z":
                            break
                        time.sleep(.01)
                    if status.exists():
                        self.assertEqual(status.read_text().rsplit(")", 1)[1].split()[0], "Z")
            finally:
                if build and build.poll() is None:
                    stop_build(build)
                children.close()
                outsider.terminate()
                outsider.wait(timeout=10)

    return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ABBA)).wasSuccessful() else 1


if __name__ == "__main__":
    args = parse_args()
    sys.exit(max(self_test(), saturation_self_test()) if args.self_test else main(args))
