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
and must still satisfy the productive-role occupancy floor. Unpinned cells, or --escalate,
search until EACH arm stops gaining AND its productive bottleneck role meets the floor.
Raw legacy all-thread busy is retained but cannot penalize legitimate idle executors.
Depth 1 is exempt and
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
from gate_measurements import (load as load_measurements, ratio as measured_ratio,
                               apply_floor, configured_reference, instrument_digest)
from gate_quiet import QuietMonitor, QuietViolation
from abba_saturation import (parse_snapshot, productive_saturation, bottleneck_saturation,
                            replay_saturation, require_saturation_window, SATURATION_FLOOR,
                            self_test as saturation_self_test)
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
BUSY_FLOOR = SATURATION_FLOOR  # productive-role occupancy; plateau remains independently required
# Run-to-run scatter of one binary's own rate and occupancy on identical bytes, measured by the
# standing nulls (0.71%; 0.6-1.0% across 15 cells). Used to decide when two saturated rungs are the
# same plateau, and how far a single occupancy sample may sit under the floor before the block is
# judged unsaturated. Lives here because load_calibration imports this module.
PLATEAU_TOLERANCE_PCT = 1.0
#
# SATURATION IS ESTABLISHED BY A RATE PLATEAU, NOT BY A BUSY PERCENTAGE ALONE (owner ruling
# 2026-09-10). Demanding >=98% busy in every run fails a candidate FOR BEING FASTER: a quicker
# server does the same offered work with less CPU, so on 2026-09-10 the h12 SET cell sat at 97.1%
# busy while delivering 24.6 Mops/s against the reference's 22.2 at 98.4% -- an 11% gain the gate
# refused to certify. Adding 50% more load generator threads did not move it; the server simply
# could not be pinned at 98% by any load this box can offer.
#
# Escalation judges the lowest TESTED saturated rung whose two arms stop gaining beyond
# their own measured repeatability under a stable, higher-worker-capacity probe. A stable
# decline may confirm the earlier saturated rung but is explicitly labeled congestion:
# h12 peaked at 26.6 Mops/s with 2 instances and decayed to 24.6 by 16. Neither that decline
# nor a worker-count increase proves unused generator headroom. Never pad a pin into it.
# Load-generator escalation ladder. 8 was not enough: on 2026-09-10 the h12 SET cell left the
# CANDIDATE arm at 97.1% busy while the reference sat at 98.4%, because the candidate was 10.7%
# faster and therefore did the same offered work with less CPU. A faster server needs MORE load to
# saturate, so capping the ladder at 8 makes an improvement fail the saturation precondition -- the
# gate would reject exactly the changes it exists to certify. The preferred diagnostic
# level remains 98%; the enforced BUSY_FLOOR above is unchanged.
# At 512 connections on the default 192 load CPUs, n=12 assigns 16 workers per
# process and two or three clients per worker: all 192 workers serve exactly 512
# connections. n=8 uses 128 workers; n=16 also uses only 128 because its equal
# connection shares must divide the worker count. Keep n=16 as a separate probe,
# but selection must not credit its smaller worker pool as increased capacity.
LADDER = (1, 2, 4, 8, 12, 16)
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


def read_cells(path, *, placement=None):
    cells = []
    measurements = load_measurements()
    instrument = (instrument_digest() if any(floor["status"] == "calibrated"
                  for floor in measurements["load_floors"].values()) else None)
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
        # Production measurements have one home. The reserved legacy columns remain
        # readable for private diagnostic fixtures, whose pins never modify production.
        if path.resolve() == (ROOT / "tests/headline_cells.txt").resolve() and fields[8:11] != ["-"] * 3:
            raise ValueError(f"{path}:{lineno}: measurements belong in gate_measurements.json")
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
        if pinned == "-":
            cell = apply_floor(cell, measurements, placement=placement, instrument_sha256=instrument)
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
    """Numerical throughput peak, retained as a diagnostic rather than a load floor."""
    return max(range(len(rounds)), key=lambda i: fastest_mean(rounds[i]))


def saturation_score(run, cell):
    evidence = replay_saturation(run.get("saturation"), floor_pct=BUSY_FLOOR, mode=cell.mode)
    return require_saturation_window(evidence, run)["score_pct"]


# A null run measures resolution and must not be vetoed by the flat rules it is measuring.
NULL_MODE = "null-mode"


def resolution_bounds(control, cell_id):
    """Per-metric floors this cell earned from the standing null, or None without one.

    spread     : the largest spread either arm showed on identical bytes -- the stability bound
                 cannot honestly be tighter than what the same binary repeats to;
    abs_delta  : the largest |paired delta| on identical bytes -- the threshold cannot honestly
                 be tighter than the instrument's own between-arm error.
    Both are MAXIMUMS over every block the null ran, so an unselected noisy probe still counts.
    """
    if not control:
        return None
    rows = [r for r in (control.get("null_control") or {}).get("resolution") or [] if r.get("cell") == cell_id]
    if not rows:
        return None
    bounds = {}
    for r in rows:
        b = bounds.setdefault(r["metric"], {"spread": 0.0, "abs_delta": 0.0})
        b["spread"] = max(b["spread"], r.get("reference_spread_pct", 0.0), r.get("candidate_spread_pct", 0.0))
        b["abs_delta"] = max(b["abs_delta"], r.get("absolute_delta_pct", 0.0))
    return bounds


def spread_limit(metric, bounds):
    if bounds is NULL_MODE:
        return math.inf
    if bounds and metric in bounds:
        return max(MAX_SPREAD, bounds[metric]["spread"])
    return MAX_SPREAD


def load_block_evidence(cell, block, bounds=None):
    """Validate every measured block, including probes not selected for the comparison.

    A noisy probe must not certify a quieter neighbor. Keep its failure even if a later
    rung would have looked better; searching again cannot erase a bad measurement.
    """
    n, runs = block["instances"], block["runs"]
    reasons = []
    rate = paired(runs)
    metrics = [cell.metric]
    if cell.depth > 1:
        metrics.append("rate")  # Escalation's plateau needs stable throughput too.
    if cell.metric == "p999_ms":
        metrics.append("long_p999_ms")
    for metric in dict.fromkeys(metrics):
        values = paired(runs, metric)
        limit = spread_limit(metric, bounds)
        for arm in ("reference", "candidate"):
            if values[f"{arm}_spread_pct"] > limit:
                reasons.append(f"{arm} {metric} spread exceeds the project's {limit:g}% stability boundary")
    if any(run.get("complete") is not True or run.get("error") for run in runs):
        reasons.append("incomplete or failed ABBA measurement")
    if any(run.get("instances") != n for run in runs):
        reasons.append("measurement instance count differs from its block")
    if any(not isinstance(run.get("busy_pct"), (int, float)) or
           not math.isfinite(run["busy_pct"]) or not 0 <= run["busy_pct"] <= 100 for run in runs):
        reasons.append("invalid server busy measurement")
    saturation = []
    for index, run in enumerate(runs, 1):
        try:
            saturation.append(saturation_score(run, cell))
        except (ValueError, TypeError) as error:
            reasons.append(f"run {index}:{run.get('arm', '?')}: {error}")
    layout = runs[0].get("load_layout")
    if not isinstance(layout, list) or len(layout) != n or any(
            run.get("load_layout") != layout for run in runs):
        reasons.append("missing or inconsistent generator layouts across ABBA arms")
        workers = None
    else:
        workers = 0
        connections = 0
        assigned = []
        for placement in layout:
            threads, clients, cpus_ = (placement.get(key) for key in ("threads", "clients", "cpus"))
            if (type(threads) is not int or type(clients) is not int or
                    threads <= 0 or clients <= 0 or not isinstance(cpus_, list) or
                    not cpus_ or threads > len(cpus_) or
                    any(type(cpu) is not int or cpu < 0 for cpu in cpus_)):
                reasons.append("invalid generator thread/client/CPU layout")
                workers = None
                break
            workers += threads
            connections += threads * clients
            assigned.extend(cpus_)
        if workers is not None and (connections != cell.conns or len(assigned) != len(set(assigned))):
            reasons.append("generator layout changes total connections or shares assigned CPUs")
    return {"instances": n, "valid": not reasons, "validation_reasons": reasons,
            "minimum_busy_pct": min(run["busy_pct"] for run in runs),
            "minimum_saturation_pct": min(saturation) if len(saturation) == 4 else None,
            "rate": rate, "worker_threads": workers, "load_layout": layout}


def select_load_floor(cell, rounds, bounds=None):
    """Choose the lowest TESTED stable plateau, with a larger worker-capacity probe.

    Compare each arm with itself at the next rung. Taking max(A,B) before comparing can
    hide one arm still climbing behind the other's different knee. Choosing a numerical
    maximum first also makes the later gain test tautological: its successor cannot win.
    Neither a worker-count increase nor a plateau proves unused generator capacity; that
    remains explicitly unproven until the separate live load controls establish it.
    """
    if any(type(block["instances"]) is not int or block["instances"] <= 0 for block in rounds):
        raise ValueError("invalid load rung")
    if any(a["instances"] >= b["instances"] for a, b in zip(rounds, rounds[1:])):
        raise ValueError("load escalation must use increasing distinct rungs")
    evidence = [load_block_evidence(cell, block, bounds) for block in rounds]
    invalid = [f"measurement n={row['instances']}: {reason}"
               for row in evidence for reason in row["validation_reasons"]]
    numerical_peak = peak_index(rounds)
    pinned = bool(cell.depth > 1 and cell.instances and len(rounds) == 1 and
                  rounds[0]["instances"] == cell.instances)
    selected, confirmation = None, None
    tested = []
    for index, current in enumerate(evidence):
        row = {**current, "rejection_reasons": list(current["validation_reasons"])}
        tested.append(row)
        if cell.depth == 1 or pinned:
            if not invalid:
                selected = index
            continue
        if current["minimum_saturation_pct"] is None or current["minimum_saturation_pct"] < BUSY_FLOOR:
            row["rejection_reasons"].append(f"server below the {BUSY_FLOOR:g}% productive-role floor in some ABBA run")
        if index + 1 == len(evidence):
            row["rejection_reasons"].append("no higher-instance confirmation block")
            continue
        above = evidence[index + 1]
        row["confirmation_instances"] = above["instances"]
        row["confirmation_worker_threads"] = above["worker_threads"]
        if not above["valid"]:
            row["rejection_reasons"].append("higher-instance confirmation measurement is invalid")
        if (current["worker_threads"] is None or above["worker_threads"] is None or
                above["worker_threads"] <= current["worker_threads"]):
            row["rejection_reasons"].append("higher instance count did not increase generator worker capacity")
        row["arm_gains_pct"], row["arm_repeatability_pct"], row["arm_shapes"] = {}, {}, {}
        for arm in ("reference", "candidate"):
            gain = 100 * (above["rate"][f"{arm}_mean"] / current["rate"][f"{arm}_mean"] - 1)
            noise = max(current["rate"][f"{arm}_spread_pct"], above["rate"][f"{arm}_spread_pct"])
            row["arm_gains_pct"][arm], row["arm_repeatability_pct"][arm] = gain, noise
            row["arm_shapes"][arm] = ("gaining" if gain > noise else
                                      "congestion" if gain < -noise else "plateau")
            if gain > noise:
                row["rejection_reasons"].append(f"{arm} still gains beyond its measured repeatability")
        # A stable decline can confirm the earlier saturated peak, as before. It proves
        # congestion at the probe, not spare generator capacity or a reason to pad the pin.
        shapes = row["arm_shapes"].values()
        row["confirmation_shape"] = ("gaining" if "gaining" in shapes else
                                      "congestion" if "congestion" in shapes else "plateau")
        if selected is None and not invalid and not row["rejection_reasons"]:
            selected, confirmation = index, index + 1
    chosen = tested[selected] if selected is not None else None
    return {"method": "lowest-tested-confirmed-rung-v1", "measurement_valid": not invalid,
            "measurement_failures": invalid,
            "status": "INVALID" if invalid else "EXEMPT" if cell.depth == 1 else
                      "PINNED" if pinned else "CONFIRMED" if chosen else "UNPROVEN",
            "selected_index": selected, "confirmation_index": confirmation,
            "lowest_tested_qualifying_instances": chosen["instances"] if chosen and not pinned and cell.depth > 1 else None,
            "confirmation_instances": evidence[confirmation]["instances"] if confirmation is not None else None,
            "numerical_peak_instances": rounds[numerical_peak]["instances"],
            "generator_headroom": "UNPROVEN", "tested_rungs": tested,
            "lower_rung_rejections": [dict(instances=row["instances"], reasons=row["rejection_reasons"])
                                       for row in tested[:selected if selected is not None else len(tested)]]}


def assess(cell, rounds, bounds=None):
    selection = select_load_floor(cell, rounds, bounds)
    selected = selection["selected_index"]
    # An unqualified peak is retained for diagnosis only. Its row stays FAIL and cannot
    # become a pin recommendation, standing null, or trusted performance result.
    current = rounds[selected if selected is not None else peak_index(rounds)]
    rate = paired(current["runs"])
    p = paired(current["runs"], cell.metric)
    reasons = list(selection["measurement_failures"])
    loss = -p["delta_pct"] if cell.metric == "rate" else p["delta_pct"]
    # The threshold can never be tighter than the instrument's own between-arm error, which the
    # standing null measured on identical bytes for this very cell. Without that floor, a
    # reference that happened to repeat to 0.2% failed a candidate for a 0.7% drift the same
    # binary shows against itself.
    threshold = p["threshold_pct"]
    if bounds and bounds is not NULL_MODE and cell.metric in bounds:
        threshold = max(threshold, bounds[cell.metric]["abs_delta"])
    p = {**p, "threshold_pct": threshold, "threshold_source": "null-floor" if threshold != p["threshold_pct"] else "reference-spread"}
    if bounds is not NULL_MODE and loss > threshold:
        reasons.append("paired regression exceeds measured reference spread")
    long_tail = None
    if cell.metric == "p999_ms":
        long_tail = paired(current["runs"], "long_p999_ms")
        # Same contract as the primary metric: the blocker command's p99.9 threshold is floored by
        # the null's measured error for THIS metric, and a null run never vetoes on it. Missed on the
        # first pass; it failed t02 on identical bytes after the other fourteen cells passed.
        long_threshold = long_tail["threshold_pct"]
        if bounds and bounds is not NULL_MODE and "long_p999_ms" in bounds:
            long_threshold = max(long_threshold, bounds["long_p999_ms"]["abs_delta"])
        long_tail = {**long_tail, "threshold_pct": long_threshold}
        if bounds is not NULL_MODE and long_tail["delta_pct"] > long_threshold:
            reasons.append("long-command p99.9 regression exceeds measured reference spread")
    gain, plateau_noise = None, None
    if cell.depth > 1:
        if selection["status"] == "PINNED":
            occupancy = [saturation_score(run, cell) for run in current["runs"]]
            # Judge the BLOCK's occupancy by its mean, not by its worst single run. min() of four
            # noisy samples is biased low, so a cell whose true occupancy sits near the floor fails
            # about half the time by chance -- t01 (1s + overlap, REORDER p8) measured 95.26, 95.43
            # at calibration and 94.24 / 95.35 / 95.39 / 95.21 at its pinned rung on IDENTICAL bytes:
            # mean 95.05% against a 95% floor, one sample 0.76pp under. That is measurement scatter,
            # not lost saturation, and re-pinning cannot fix it because the cell's occupancy does not
            # rise with load. A genuinely unsaturated rung moves the mean, and a single run far below
            # the floor still fails via the spread guard below.
            mean_occupancy = sum(occupancy) / len(occupancy)
            worst_allowed = BUSY_FLOOR - PLATEAU_TOLERANCE_PCT
            if mean_occupancy < BUSY_FLOOR or min(occupancy) < worst_allowed:
                reasons.append(
                    f"pinned load level {cell.instances} no longer saturates this cell "
                    f"(productive-role occupancy mean {mean_occupancy:.1f}%, worst {min(occupancy):.1f}%, "
                    f"floor {BUSY_FLOOR:g}%); "
                    f"re-pin it with --escalate and import the calibration into gate_measurements.json")
        elif selected is None:
            reasons.append("no lowest tested load rung has valid saturation and higher-capacity plateau confirmation")
            reasons.extend(f"n={row['instances']}: {reason}"
                           for row in selection["lower_rung_rejections"] for reason in row["reasons"])
        else:
            chosen = selection["tested_rungs"][selected]
            # Retain the old display fields for raw consumers; decisions use BOTH arm-specific
            # comparisons above, never the envelope of whichever arm happens to be fastest.
            gain = max(chosen["arm_gains_pct"].values())
            plateau_noise = max(chosen["arm_repeatability_pct"].values())
    return {**p, "throughput": rate, "long_tail": long_tail, "instances": current["instances"],
            "busy_pct_abba": [r["busy_pct"] for r in current["runs"]],
            "saturation_pct_abba": [saturation_score(run, cell) if not selection["measurement_failures"]
                                    else None for run in current["runs"]],
            "loss_pct": loss, "margin_pct": loss - p["threshold_pct"],
            "fastest_gain_pct": gain, "plateau_noise_pct": plateau_noise,
            "load_selection": selection, "measurement_valid": selection["measurement_valid"],
            "saturation_exempt": cell.depth == 1,
            "verdict": "FAIL" if reasons else "PASS", "reasons": reasons}


def saturation_done(cell, rounds, bounds=None):
    selection = select_load_floor(cell, rounds, bounds)
    return selection["measurement_valid"] and selection["status"] in ("EXEMPT", "CONFIRMED")


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
    # The released identity is reviewed with the other measured inputs. A changed
    # filename, byte stream or last-push commit must not silently choose a new arm.
    if args.bench_bins.resolve() == Path("/home/user/Projects/bench-bins"):
        try:
            return configured_reference(commit)
        except ValueError as error:
            raise Skip(str(error)) from error
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


# Translate names and schedule values only. c8e61f646 accepts --x-overlap and
# --x-ex-sched, but acceptance does not prove an equivalent effective state:
# read-local is inert there outside fused overlap 0. Runner checks INFO SERVER
# before population. Silently omitting --overlap 1 previously compared on against
# off and reported "+17.02%" as a code win; a mapping cannot excuse that mismatch.
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
                             f"({cell.mode}); name/value mapping only; effective boot state is checked separately")
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


def generator_cpu_endpoint(process):
    """Read only this owned child's CPU counters, retaining identity and sampling bounds.

    /proc utime+stime includes the whole process, including coordinator threads. It
    is not a worker-only profile and CPU consumption is not proof of useful progress.
    Missing/exited/reused processes cannot contribute invented zero CPU samples.
    """
    if process.poll() is not None:
        raise RuntimeError(f"load generator PID {process.pid} exited before CPU endpoint")
    read_started = time.monotonic()
    try:
        raw = Path(f"/proc/{process.pid}/stat").read_text()
    except OSError as error:
        raise RuntimeError(f"load generator PID {process.pid} CPU endpoint unavailable: {error}") from error
    read_finished = time.monotonic()
    try:
        fields = raw.rsplit(")", 1)[1].split()
        pid = int(raw.split(" (", 1)[0])
        user, system, threads, start = (int(fields[index]) for index in (11, 12, 17, 19))
        ticks = os.sysconf("SC_CLK_TCK")
        if pid != process.pid or min(user, system, start) < 0 or threads <= 0 or ticks <= 0:
            raise ValueError("invalid process identity or counters")
        if fields[0] in ("Z", "X", "x") or process.poll() is not None:
            raise RuntimeError(f"load generator PID {process.pid} exited during CPU endpoint")
    except (IndexError, ValueError) as error:
        raise RuntimeError(f"load generator PID {process.pid} malformed CPU endpoint: {error}") from error
    return dict(pid=pid, start_time_ticks=start, user_ticks=user, system_ticks=system,
                clock_ticks_per_second=ticks, observed_process_threads=threads,
                user_seconds=user / ticks, system_seconds=system / ticks,
                cpu_seconds=(user + system) / ticks, raw_stat=raw,
                read_started_monotonic=read_started, read_finished_monotonic=read_finished)


def generator_cpu_between(before, after, workers, central_start, central_end):
    """Diagnostic percentages use this process's own endpoint interval, never the wider run.

    Samples sit immediately outside the existing central counter window. Retain the
    sampling offsets rather than claiming exact simultaneous endpoints for all PIDs.
    Values are not clamped: coordinator work may put worker-normalized CPU above 100%.
    Neither this value nor a larger worker count certifies generator headroom.
    """
    if any(before[key] != after[key] for key in ("pid", "start_time_ticks", "clock_ticks_per_second")):
        raise RuntimeError("load generator identity or CPU clock changed between endpoints")
    user, system = after["user_ticks"] - before["user_ticks"], after["system_ticks"] - before["system_ticks"]
    if min(user, system) < 0:
        raise RuntimeError("load generator CPU counter reset between endpoints")
    first = (before["read_started_monotonic"] + before["read_finished_monotonic"]) / 2
    last = (after["read_started_monotonic"] + after["read_finished_monotonic"]) / 2
    if (type(workers) is not int or workers <= 0 or last <= first or central_end <= central_start or
            before["read_started_monotonic"] > before["read_finished_monotonic"] or
            after["read_started_monotonic"] > after["read_finished_monotonic"] or
            before["read_finished_monotonic"] > central_start or
            after["read_started_monotonic"] < central_end):
        raise RuntimeError("invalid load generator CPU sampling bounds or worker count")
    ticks, interval = before["clock_ticks_per_second"], last - first
    seconds = (user + system) / ticks
    return dict(status="COMPLETE", user_ticks_delta=user, system_ticks_delta=system,
                user_seconds_delta=user / ticks, system_seconds_delta=system / ticks,
                cpu_seconds_delta=seconds, endpoint_window_seconds=interval,
                cpu_pct_one_core=100 * seconds / interval,
                cpu_pct_per_configured_worker=100 * seconds / (interval * workers),
                before_offset_from_central_start_seconds=first - central_start,
                after_offset_from_central_end_seconds=last - central_end)


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
        self.profile_factory = None  # Diagnostic opt-in only: no default PMCs or profile objects.
        self.worker_affinity_factory = None
        self.load_startup_seconds = 0  # Diagnostic allowance; normal generator lifetime stays 28s.

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

    def memtier(self, layout, *, cell=None):
        argv = ["taskset", "-c", cpu_string(layout["cpus"]), self.args.memtier,
                "-s", "127.0.0.1", "-p", str(self.args.port), "--protocol=redis",
                "-t", str(layout["threads"]), "-c", str(layout["clients"]),
                "--key-minimum=1", f"--key-maximum={KEYS}",
                "-d", "64", "--distinct-client-seed", "--hide-histogram"]
        # memtier rejects its built-in SET:GET pattern whenever --command is
        # present. Those workloads carry a P pattern on EACH command instead;
        # population and built-in GET/SET/MIX retain the original P:P geometry.
        if cell is None or cell.op in ("GET", "SET", "MIX"):
            argv += ["--key-pattern=P:P"]
        return argv

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

    def measure(self, cell, arm, sequence, instances, knobs, *, _calibration=None, _window=None):
        # Calibration reuses this exact load/counter/accounting path. Only its
        # server lifetime and requested window differ; it cannot publish ABBA evidence.
        if _window is not None and (_calibration is None or _window != 10):
            raise ValueError("short windows require the isolated 10-second calibration session")
        window = WINDOW if _window is None else _window
        reused = bool(_calibration)
        if reused and _calibration["cell"] != asdict(cell):
            raise ValueError("calibration session cannot outlive its cell")
        profile = None
        worker_affinity = None
        legacy_control = self.legacy_reorder_control(cell, arm, knobs)
        folder = self.out / cell.id / f"n{instances}-{sequence}-{arm}"
        folder.mkdir(parents=True)
        layout = load_layout(self.load_cpus, instances, cell.conns)
        # Never connect to or terminate an existing listener, even if it speaks TomoKV.
        if not reused:
            require_unbound_port(self.args.port)
        command = ["taskset", "-c", cpu_string(self.server_cpus), self.binaries[arm],
                   "--port", str(self.args.port), "--bind", "127.0.0.1", "--atomic", str(cell.atomic),
                   "--enable-debug-command", "yes", "--save", "", "--appendonly", "no",
                   "--dir", str(folder)]
        # Defaults are made explicit so changes to placement cannot masquerade as code gains.
        ex = 0
        if cell.mode == "2s":
            split_ratio = measured_ratio("abba", len(self.server_cpus))
            ex = int(split_ratio.split(":")[1])
            command += ["--ratio", split_ratio, "--flip-auto", "0"]
        command += ["--shards", str(min(8 * (ex if cell.mode == "2s" else len(self.server_cpus)), 256))]
        for name, value in knobs.items():
            command += [f"--{name}", str(value)]
        started = time.monotonic()
        srv, conn, generators = None, None, []
        log = folder / "server.log"
        result = {"arm": arm, "instances": instances, "complete": False,
                  "server_argv": [str(x) for x in command],
                  "load_layout": layout, "artifacts": str(folder.relative_to(self.out))}
        if _calibration is not None:
            result.update(calibration_only=True, population_reused=reused)
        print(f"  {cell.id} n={instances} {sequence}:{arm} "
              f"{'reuse populated server' if reused else 'boot/populate'}/{window}s", flush=True)
        try:
            if reused:
                srv, conn = _calibration["srv"], _calibration["conn"]
                if srv.poll() is not None:
                    raise RuntimeError("calibration server exited between load rungs")
                result.update(pid=srv.pid, boot_info=_calibration["boot_info"],
                    server_argv=_calibration["server_argv"], population=_calibration["population"],
                    populate_seconds=0.)
            else:
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
                result.update(pid=srv.pid, boot_info=identity)
                # CONFIG GET echoes the requested knob even when the old reference
                # cannot arm it (split, or fused overlap on). INFO SERVER read_local
                # has always reported the effective lane. Check both arms before SET
                # population or measured load, including write cells: arming also
                # changes immutable replacement and retirement obligations.
                effective_read_local = identity.get("read_local")
                if effective_read_local not in ("0", "1"):
                    raise RuntimeError(f"{arm} boot lacks valid effective INFO SERVER read_local: "
                                       f"{effective_read_local!r}")
                if int(effective_read_local) != cell.read_local:
                    reason = (f"{arm} boot effective read_local={effective_read_local} does not match "
                              f"cell read-local={cell.read_local}; CONFIG GET alone cannot prove arming")
                    if arm == "A":
                        raise NotComparable(reason)
                    raise RuntimeError(reason)
                for name, value in {"atomic": cell.atomic, **knobs}.items():
                    actual = conn.must("CONFIG", "GET", name)
                    if actual != [name.encode(), str(value).encode()]:
                        raise RuntimeError(f"boot did not apply {name}={value}: {actual!r}")
                result["population"] = self.populate(cell, arm, conn, folder)
                result["populate_seconds"] = time.monotonic() - started
                if _calibration is not None:
                    _calibration.update(srv=srv, conn=conn, cell=asdict(cell), boot_info=identity,
                        server_argv=result["server_argv"], population=result["population"])
            # Bracket ALL generators, after wire/snapshot population and any service-
            # cost probes. Only the named workload command counters enter accounting;
            # INFO/DEBUG and client protocol setup never become phantom workload ops.
            result["whole_run_commandstats_before"] = info(conn, "commandstats")
            result["whole_run_clients_before"] = info(conn, "clients")
            load_lifetime = self.load_startup_seconds + WARMUP + window + TAIL
            load_launch = time.monotonic() if self.load_startup_seconds else None
            if self.load_startup_seconds:
                result["load_timing"] = dict(startup_allowance_seconds=self.load_startup_seconds,
                    first_launch_monotonic=load_launch, requested_lifetime_seconds=load_lifetime,
                    fresh_warmup_seconds=WARMUP, central_window_seconds=window, tail_seconds=TAIL)
            for i, placement in enumerate(layout):
                argv = self.memtier(placement, cell=cell) + workload_arguments(cell) + [f"--pipeline={cell.depth}",
                        f"--test-time={load_lifetime}",
                        f"--json-out-file={folder / f'load-{i}.json'}"]
                generators.append(self.children.start(argv, folder / f"load-{i}.log", folder))
                result.setdefault("load_argv", []).append(argv)
            if self.worker_affinity_factory is not None:
                worker_affinity = self.worker_affinity_factory(folder)
                result["load_worker_affinity"] = worker_affinity.record
                worker_affinity.begin(generators, layout, load_launch, load_lifetime)
            if self.load_startup_seconds:
                if time.monotonic() >= load_launch + self.load_startup_seconds:
                    raise RuntimeError("diagnostic load startup allowance expired before fresh warmup")
                result["load_timing"]["warmup_started_monotonic"] = time.monotonic()
            # The counter window excludes setup/teardown and is the SAME for all LGs.
            time.sleep(WARMUP)
            if worker_affinity is not None:
                worker_affinity.verify("after-fresh-warmup")
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
            # The added /proc reads lie OUTSIDE the unchanged central stats/timer window.
            # Save each raw endpoint immediately so an exit/reset preserves partial evidence.
            generator_cpu = result["generator_cpu"] = {
                "schema": 1, "decision_input": False, "generator_headroom": "UNPROVEN",
                "scope": "whole-process CPU near central window, normalized by configured workers",
                "status": "INCOMPLETE", "processes": []}
            for index, (process, placement) in enumerate(zip(generators, layout)):
                sample = dict(index=index, pid=process.pid, configured_worker_threads=placement["threads"],
                              assigned_cpus=placement["cpus"], status="INCOMPLETE")
                generator_cpu["processes"].append(sample)
                sample["before"] = generator_cpu_endpoint(process)
            if self.profile_factory is not None:
                profile = self.profile_factory(folder)
                result["cpu_profile"] = profile.record
                profile.begin(srv, generators)
            if worker_affinity is not None:
                worker_affinity.verify("before-central-window")
            before = info(conn, "stats")
            before_cpu, t0 = cpu_seconds(srv.pid), time.monotonic()
            if self.load_startup_seconds:
                # Use the earliest possible generator expiry. Setup consumes its
                # allowance, never the central window or the reserved tail. Both
                # floating and fixed diagnostics use the same requested lifetime.
                remaining = load_launch + load_lifetime - t0
                result["load_timing"].update(central_start_monotonic=t0,
                    minimum_remaining_lifetime_seconds=remaining)
                if remaining < window + TAIL:
                    raise RuntimeError("insufficient diagnostic load lifetime for full central window and tail")
            time.sleep(window)
            after = info(conn, "stats")
            t1, after_cpu = time.monotonic(), cpu_seconds(srv.pid)
            if profile is not None:
                # The same unmodified central command count/window remains the rate.
                # PMCs encompass it; every wider endpoint offset is retained explicitly.
                profile.finish(t0, t1, int(after["total_commands_processed"]) -
                               int(before["total_commands_processed"]) - 1)
            if worker_affinity is not None:
                worker_affinity.finish()
            generator_cpu.update(central_start_monotonic=t0, central_end_monotonic=t1,
                                 central_window_seconds=t1 - t0)
            for sample, process in zip(generator_cpu["processes"], generators):
                sample["after"] = generator_cpu_endpoint(process)
                sample.update(generator_cpu_between(sample["before"], sample["after"],
                    sample["configured_worker_threads"], t0, t1))
            generator_cpu["status"] = "COMPLETE"
            after_lb = lb_snapshot(conn, folder / "lb-after.txt")
            after_lb_at = time.monotonic()
            after_commands = info(conn, "commandstats")
            after_mode = info(conn, "server") if cell.op == "REORDER" else {}
            if any(p.poll() is not None for p in generators):
                raise RuntimeError(f"load generator ended inside the {window}-second window")
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
                          # Preserve legacy busy separately: idle executors are correct
                          # under split read-local, and cannot dilute its IO bottleneck.
                          # Raw same-window deltas are replayed before either arm earns
                          # the floor. No process CPU substitution or missing-data fallback.
                          saturation=bottleneck_saturation(before_lb, after_lb, floor_pct=BUSY_FLOOR),
                          diagnostic_saturation=productive_saturation(
                              before_lb, after_lb, floor_pct=BUSY_FLOOR),
                          cpu_pct=100 * (after_cpu - before_cpu) / ((t1 - t0) * len(self.server_cpus)),
                          info_before=before, info_after=after)
            result["central_saturation"] = require_saturation_window(result["saturation"], result)
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
            if self.load_startup_seconds:
                # HDR includes setup/warmup/tail. Keep each actual memtier runtime
                # (milliseconds in its JSON schema), not a fictional 20s HDR window.
                from abba_worker_affinity import histogram_runtime
                result["full_histogram_runtime"] = histogram_runtime(folder, len(generators), load_lifetime)
                # Pinning pauses generators before their fresh warmup, but their
                # full-run HDR still includes that pause. These diagnostics cannot
                # justify adopting worker pinning for p1 or scored tail latency.
                result["full_histogram_runtime"].update(includes_startup_sigstop=worker_affinity is not None,
                    latency_scoring_eligible=False,
                    limitation="exclude startup pause from scored latency or pin before traffic before any production adoption")
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
                result["histogram_window_seconds"] = load_lifetime
        except BaseException as e:
            if worker_affinity is not None:
                worker_affinity.fail(e)
            if profile is not None and profile.record.get("status") == "INCOMPLETE":
                profile.fail(e)
            if result.get("generator_cpu", {}).get("status") == "INCOMPLETE":
                result["generator_cpu"].update(status="INVALID", error=f"{type(e).__name__}: {e}")
            result["complete"] = False
            result["error"] = f"{type(e).__name__}: {e}"
            raise
        finally:
            try:
                if profile is not None:
                    profile.close()
            except BaseException as error:
                result.update(complete=False, error=f"profile cleanup: {type(error).__name__}: {error}")
                profile.fail(error)
                raise
            finally:
                # A failed counter/artifact close must never strand owned children.
                keep_server = _calibration is not None and result.get("complete") is True
                if conn and not keep_server:
                    conn.close()
                for p in generators:
                    self.children.stop(p)
                if srv and not keep_server:
                    self.children.stop(srv)
                if _calibration is not None and not keep_server:
                    _calibration.clear()
                result["wall_seconds"] = time.monotonic() - started
                (folder / "measurement.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"    {result['rate']/1e6:.5f}M/s legacy-busy={result['busy_pct']:.3f}% "
              f"productive-role={result['central_saturation']['score_pct']:.3f}% "
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
          f"legacy-busy(ABBA)={','.join(f'{x:.3f}' for x in a['busy_pct_abba'])}% "
          f"productive-role(ABBA)={','.join('?' if x is None else f'{x:.3f}' for x in a['saturation_pct_abba'])}% "
          f"instances={a['instances']} {a['verdict']}", flush=True)
    selection = a["load_selection"]
    if selection["status"] not in ("PINNED", "EXEMPT"):
        print(f"  load floor={selection['lowest_tested_qualifying_instances']} "
              f"({selection['status']}, lowest TESTED qualifying rung); "
              f"confirmation={selection['confirmation_instances']} "
              f"numerical peak={selection['numerical_peak_instances']}; generator headroom UNPROVEN", flush=True)
        if selection["selected_index"] is not None:
            chosen = selection["tested_rungs"][selection["selected_index"]]
            print(f"  workers {chosen['worker_threads']} -> {chosen['confirmation_worker_threads']}; "
                  f"per-arm gains={chosen['arm_gains_pct']} "
                  f"repeatability={chosen['arm_repeatability_pct']} "
                  f"shape={chosen['confirmation_shape']}", flush=True)
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
    p.add_argument("--calibrate", action="store_true",
                   help="one-arm 10s load-floor search; boot/populate once per cell; PIN only, never a verdict")
    p.add_argument("--escalate", action="store_true",
                   help="ignore pinned load levels and search the ladder; use this to RE-PIN a cell "
                        "after the gate reports its pinned level no longer saturates")
    p.add_argument("--only", default="", help="comma-separated IDs; partial diagnostic, never a full-tier PASS")
    p.add_argument("--max-instances", type=int, choices=LADDER, default=16,
                   help="load-instance ceiling (default 16); 1 cannot prove unpinned deep-pipeline saturation")
    return p.parse_args()


def main(args, *, diagnostic_monitor=None, diagnostic_profile=0,
         diagnostic_pin_load_workers=0, diagnostic_load_startup_seconds=0):
    if getattr(args, "calibrate", False):
        if diagnostic_monitor is not None or diagnostic_profile or diagnostic_pin_load_workers or diagnostic_load_startup_seconds:
            raise ValueError("calibration cannot use diagnostic measurement overrides")
        from load_calibration import main as calibration_main
        return calibration_main(args)
    if args.list_cells:
        cells = selected_cells(read_cells(args.cells), args.subset, args.only)
        print(json.dumps({"subset": args.subset, **coverage(cells)}, indent=2))
        return 0
    start = time.monotonic()
    out = (args.output or ROOT / "build" / f"abbagate-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}").resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "verdict": "FAIL", "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "window_seconds": WINDOW, "order": list(ORDER), "cells": [], "output": str(out),
              "subset": args.subset, "only": args.only, "escalate": args.escalate,
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
        if diagnostic_profile not in (0, 1) or diagnostic_profile and diagnostic_monitor is None:
            report.update(measurement_valid=False, normal_gate_eligible=False)
            raise ValueError("CPU profiling requires the permanently untrusted diagnostic runner")
        if diagnostic_profile:
            report["cpu_profile_requested"] = True
        if (diagnostic_pin_load_workers not in (0, 1) or diagnostic_load_startup_seconds not in (0, 5) or
                (diagnostic_pin_load_workers or diagnostic_load_startup_seconds) and
                (diagnostic_monitor is None or not args.collect_null) or
                diagnostic_pin_load_workers and diagnostic_load_startup_seconds != 5):
            report.update(measurement_valid=False, normal_gate_eligible=False)
            raise ValueError("worker placement requires diagnostic null arms and explicit 5s startup allowance")
        if diagnostic_load_startup_seconds:
            report.update(pin_load_workers=diagnostic_pin_load_workers,
                          load_startup_seconds=diagnostic_load_startup_seconds)
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
        split_ratio = measured_ratio("abba", len(server_cpus))
        placement = dict(server_physical=server_physical, server_smt=server_smt,
                         load_physical=load_physical, load_smt=load_smt, split_ratio=split_ratio)
        cells = read_cells(args.cells, placement=placement)
        report["measurements"] = load_measurements()
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
        quiet_options = {"own_root_pid": os.getpid(), "window_seconds": WINDOW,
                         "ports": (args.port,), "sample_artifact": out / "quiet-samples.jsonl"}
        quiet = (diagnostic_monitor or QuietMonitor)(server_cpus, load_cpus, **quiet_options)
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
        runner.load_startup_seconds = diagnostic_load_startup_seconds
        if diagnostic_pin_load_workers:
            from abba_worker_affinity import WorkerAffinity
            runner.worker_affinity_factory = WorkerAffinity
        if diagnostic_profile:
            # Dormant imports are included in the instrument fingerprint. The normal
            # path neither imports the helper nor allocates any perf/profile state.
            from abba_profile import WindowProfile
            runner.profile_factory = WindowProfile
        report["environment"] = {"uname": list(os.uname()),
                                 "python_runtime": report["instrument_fingerprint"]["python"],
                                 "server_cpus": server_cpus,
                                 "server_physical": server_physical, "server_smt": server_smt,
                                 "load_physical": load_physical, "load_smt": load_smt,
                                 "load_instance_ceiling": min(args.max_instances, len(load_physical)),
                                 "load_cpus": load_cpus, "port": args.port,
                                 "permitted_ports": permitted_ports, "keys": KEYS,
                                 "data_bytes": 64, "key_pattern": "P:P", "atomic": "per-cell",
                                 "split_ratio": split_ratio,
                                 "split_flip_auto": 0, "memtier_path": args.memtier,
                                 "memtier_sha256": sha256(Path(args.memtier)),
                                 "memtier_version": capture([args.memtier, "--version"]).stdout.strip(),
                                 **runner.population_environment()}
        print(f"GEOMETRY server={args.server_cores} ({len(server_physical)} physical cores) "
              f"server-smt={args.server_smt or '(reserved)'} ({len(server_cpus)} threads) "
              f"load={args.load_cores} load-smt={args.load_smt or '(reserved)'} "
              f"port={args.port} allowed={permitted_ports[0]}-{permitted_ports[1]}; "
              f"Cell connections are TOTAL, shared across load instances. Split uses reviewed ratio {split_ratio}, flip=0.", flush=True)
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
                ladder = ((cell.instances,) if pinned else
                          tuple(sorted(set(LADDER) | ({cell.instances} if args.escalate and cell.instances else set()))))
                row["load_ladder"] = list(ladder)
                # Clearing the assessment pin matters even at --max-instances=1:
                # --escalate must prove its load floor with a higher probe, never borrow
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
                    bounds = NULL_MODE if args.collect_null else resolution_bounds(control, cell.id)
                    row["assessment"] = assess(assessed_cell, row["rounds"], bounds)
                    row["verdict"] = row["assessment"]["verdict"]
                    print_cell(row)
                    if saturation_done(assessed_cell, row["rounds"], bounds):
                        break
                    # For a VERDICT run an unstable block is permanent evidence, never an excuse to
                    # search for a later block that happens to pass -- that is re-rolling until green.
                    # CALIBRATION is the opposite problem. Its low rungs are DELIBERATELY unsaturated,
                    # so they are unstable by construction, and the search exists precisely to walk
                    # past them to the rung where the server saturates. Breaking on the first unstable
                    # block meant the ladder never advanced beyond n=1: on 2026-09-11 a 33-minute
                    # campaign measured 15 cells for one rung each and recorded zero floors.
                    if not row["assessment"]["measurement_valid"]:
                        reasons = row["assessment"].get("reasons", [])
                        incomplete = any("incomplete" in r or "failed ABBA" in r for r in reasons)
                        # "Still climbing" means the fastest arm gained MORE THAN THE NOISE over the
                        # previous rung -- a raw max is not enough, since a 0.5% rise inside a 3%
                        # spread is noise, and treating it as a climb would let an unstable
                        # confirmation probe be skipped. That is the p-hacking case, one rung up.
                        still_climbing = False
                        if len(row["rounds"]) >= 2:
                            last = paired(row["rounds"][-1]["runs"])
                            prev = paired(row["rounds"][-2]["runs"])
                            fast = max(last["reference_mean"], last["candidate_mean"])
                            before = max(prev["reference_mean"], prev["candidate_mean"])
                            gain = 100 * (fast / before - 1) if before > 0 else 0.0
                            noise = max(last["reference_spread_pct"], last["candidate_spread_pct"],
                                        prev["reference_spread_pct"], prev["candidate_spread_pct"])
                            still_climbing = gain > noise
                        # Three cases, and only one may continue:
                        #  * a VERDICT run: any invalid block is permanent (re-rolling is p-hacking);
                        #  * an INCOMPLETE measurement (crash, closed connection): permanent in every
                        #    mode -- a broken measurement says nothing about load, so searching past
                        #    it would pin a floor on evidence that does not exist;
                        #  * an unstable block that is NOT the current peak is the confirmation probe
                        #    above a candidate floor, and cannot be discarded to look for a kinder
                        #    one -- that is the same re-roll, one rung up.
                        # What remains is calibration walking past an unstable rung while the rate is
                        # still climbing: an unsaturated low rung, unstable by construction, and not a
                        # floor candidate at all.
                        if not args.escalate or incomplete or not still_climbing:
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
        if args.escalate:
            # Calibration starts with unpinned INPUTS. Preserve that inventory,
            # but a confirmed live plateau is no longer a pending measurement.
            # Otherwise a successful --collect-null --escalate campaign fails
            # its final evidence check merely because its input was unmeasured.
            report["coverage"]["requested_pending_pins"] = report["coverage"]["pending_pins"][:]
            report["coverage"]["pending_pins"] = [row["cell"]["id"] for row in report["cells"]
                if row["cell"]["depth"] > 1 and row.get("assessment", {}).get(
                    "load_selection", {}).get("status") != "CONFIRMED"]
        report["statistical_verdict"], report["worst_cell"] = overall(report["cells"])
        report["verdict"] = report["statistical_verdict"]
        if report["statistical_verdict"] == "PASS":
            report["verdict"] = "PARTIAL"
            if diagnostic_monitor is not None:
                report["null_control"] = {"verdict": "UNTRUSTED", "reason":
                    "background qualification is diagnostic only; not a standing null or a gate PASS"}
                print("BACKGROUND QUALIFICATION: raw cells pass; instrument remains UNTRUSTED", flush=True)
            elif args.collect_null:
                try:
                    report["null_control"] = null_result(report, now=time.time())
                except ValueError as error:
                    # A favorable code-comparison verdict can still fail the null's
                    # two-sided instrument check. Retain that distinct failure.
                    report["null_control"] = {"verdict": "FAIL", "reason": str(error)}
                    raise
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
    from _abba_test_fixtures import quiet_record, saturation_record
    (ROOT / "build").mkdir(exist_ok=True)

    class ABBA(unittest.TestCase):
        def setUp(self):
            self.cell = Cell("h01", "1s", 1, 0, 0, "GET", 32, 512)
            # Serverless loop tests replace the selected-CPU observer too. Dedicated
            # negative controls below inject failures through the same main path.
            self.quiet = mock.Mock()
            self.quiet.evidence.return_value = quiet_record()
            self.quiet.close.return_value = quiet_record(samples=3)
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

            # Check the complete argv boundary: validating workload_arguments
            # alone missed the conflicting --key-pattern supplied by Runner.
            from types import SimpleNamespace
            runner = Runner(SimpleNamespace(server_cores="0-1", server_smt="", load_cores="2-3",
                            load_smt="", port=9090, memtier="never-executed-memtier"),
                            Path("/unused"), {}, Children())
            layout = {"cpus": [2, 3], "threads": 2, "clients": 2}
            self.assertIn("--key-pattern=P:P", runner.memtier(layout))  # wire population
            for op in ("GET", "SET", "MIX", "MGET", "MSET", "MIX8", "REORDER"):
                cell = replace(self.cell, op=op, mix="7:1")
                argv = runner.memtier(layout, cell=cell) + workload_arguments(cell)
                commands = [arg for arg in argv if arg.startswith("--command=")]
                native_patterns = [arg for arg in argv if arg.startswith("--key-pattern=")]
                command_patterns = [arg for arg in argv if arg.startswith("--command-key-pattern=")]
                with self.subTest(op=op):
                    if commands:
                        self.assertEqual(native_patterns, [])
                        self.assertEqual(command_patterns, ["--command-key-pattern=P"] * len(commands))
                    else:
                        self.assertEqual(native_patterns, ["--key-pattern=P:P"])
                        self.assertEqual(command_patterns, [])

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

        def test_generator_cpu_endpoints_preserve_identity_seconds_and_worker_normalization(self):
            from types import SimpleNamespace
            process = SimpleNamespace(pid=321, poll=lambda: None)
            def stat(user, system):
                fields = ["0"] * 22
                for index, value in ((0, "R"), (11, user), (12, system), (17, 17), (19, 9876)):
                    fields[index] = str(value)
                return "321 (memtier ) name) " + " ".join(fields)
            with (mock.patch.object(Path, "read_text", side_effect=[stat(100, 20), stat(30100, 420)]),
                  mock.patch.object(os, "sysconf", return_value=100),
                  mock.patch.object(time, "monotonic", side_effect=[10, 10.002, 30, 30.002])):
                before, after = generator_cpu_endpoint(process), generator_cpu_endpoint(process)
            self.assertEqual(before["start_time_ticks"], 9876)
            self.assertEqual(before["observed_process_threads"], 17)
            self.assertEqual(before["cpu_seconds"], 1.2)
            self.assertIn("memtier ) name", before["raw_stat"])
            result = generator_cpu_between(before, after, 16, 10.01, 29.99)
            self.assertEqual(result["user_seconds_delta"], 300)
            self.assertEqual(result["system_seconds_delta"], 4)
            self.assertAlmostEqual(result["endpoint_window_seconds"], 20)
            self.assertAlmostEqual(result["cpu_pct_per_configured_worker"], 95)
            self.assertLess(result["before_offset_from_central_start_seconds"], 0)
            self.assertGreater(result["after_offset_from_central_end_seconds"], 0)
            # Zero CPU is valid diagnostic evidence; coordinator overhead is never clamped.
            idle = {**after, "user_ticks": before["user_ticks"], "system_ticks": before["system_ticks"]}
            self.assertEqual(generator_cpu_between(before, idle, 16, 10.01, 29.99)["cpu_seconds_delta"], 0)
            self.assertGreater(generator_cpu_between(before, after, 4, 10.01, 29.99)["cpu_pct_per_configured_worker"], 100)
            for changed in ({"pid": 322}, {"start_time_ticks": 9877}, {"clock_ticks_per_second": 1000},
                            {"user_ticks": 99}, {"system_ticks": 19}):
                with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                    generator_cpu_between(before, {**after, **changed}, 16, 10.01, 29.99)
            with self.assertRaisesRegex(RuntimeError, "sampling bounds"):
                generator_cpu_between(before, after, 16, 10, 31)

        def test_generator_cpu_endpoint_rejects_exit_missing_and_malformed_proc(self):
            from types import SimpleNamespace
            process = SimpleNamespace(pid=321, poll=mock.Mock(return_value=0))
            with mock.patch.object(Path, "read_text") as read, self.assertRaisesRegex(RuntimeError, "exited before"):
                generator_cpu_endpoint(process)
            read.assert_not_called()
            process.poll = mock.Mock(return_value=None)
            with (mock.patch.object(Path, "read_text", side_effect=FileNotFoundError()),
                  self.assertRaisesRegex(RuntimeError, "unavailable")):
                generator_cpu_endpoint(process)
            with (mock.patch.object(Path, "read_text", return_value="missing fields"),
                  self.assertRaisesRegex(RuntimeError, "malformed")):
                generator_cpu_endpoint(process)
            fields = ["0"] * 22
            fields[0], fields[17], fields[19] = "R", "1", "9876"
            raw = "321 (memtier) " + " ".join(fields)
            process.poll = mock.Mock(side_effect=[None, 0])
            with (mock.patch.object(Path, "read_text", return_value=raw),
                  self.assertRaisesRegex(RuntimeError, "exited during")):
                generator_cpu_endpoint(process)

        def test_real_measure_brackets_all_generators_and_rejects_counter_mutants(self):
            from types import SimpleNamespace
            # Exercise Runner.measure itself, including argv production, generator
            # waits, JSON parsing, both endpoint reads and failure artifacts. Only
            # process/network/time boundaries are fake; the accounting is real.
            cases = [(value, 0, 0) for value in (0, -1, 10000, "cpu-reset", "cpu-exit")] + [
                    (0, value, 0) for value in ("on", "begin-fail", "finish-fail", "close-fail")] + [
                    (0, "on", value) for value in ("fixed", "floating", "setup-fail", "warmup-fail",
                                                  "before-fail", "finish-fail", "lifetime-fail")]
            for corruption, profile_mode, affinity_mode in cases:
                with self.subTest(corruption=corruption, profile=profile_mode, affinity=affinity_mode), tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                    directory, events, generators = Path(tmp), [], []
                    stopped, profiles = [], []
                    phase = {"window": 0, "finished": 0}
                    srv = SimpleNamespace(pid=123, poll=lambda: None)
                    def start(argv, log, cwd):
                        if "--protocol=redis" not in argv:
                            return srv
                        events.append("start")
                        path = Path(next(arg.split("=", 1)[1] for arg in argv if arg.startswith("--json-out-file=")))
                        requested = int(next(arg.split("=", 1)[1] for arg in argv if arg.startswith("--test-time=")))
                        self.assertEqual(requested, 33 if affinity_mode else 28)
                        document = self.accounting_document({"GET": 5000})
                        document["ALL STATS"]["Runtime"].update({"Time unit": "MILLISECONDS", "Start time": 1000,
                            "Finish time": 1000 + requested * 1000, "Total duration": requested * 1000})
                        path.write_text(json.dumps(document))
                        def wait(timeout):
                            phase["finished"] += 1
                            events.append("finish")
                            return 0
                        process = SimpleNamespace(pid=124 + len(generators), poll=lambda: None, wait=wait)
                        generators.append(process)
                        return process
                    children = SimpleNamespace(start=start, stop=lambda process: stopped.append(process.pid))
                    conn = SimpleNamespace(must=lambda *args: [args[-1].encode(), b"1"], close=lambda: None)
                    def snapshot(_conn, section):
                        if section == "server":
                            return {"process_id": "123", "read_local": str(cell.read_local)}
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
                    def endpoint(process):
                        events.append(("generator-cpu", phase["window"], process.pid))
                        if corruption == "cpu-exit" and phase["window"] == 2:
                            raise RuntimeError("load generator exited during CPU endpoint")
                        now = time.monotonic()
                        user = (0 if corruption == "cpu-reset" else 200) if phase["window"] == 2 else 100
                        return dict(pid=process.pid, start_time_ticks=1000 + process.pid,
                                    clock_ticks_per_second=100, user_ticks=user, system_ticks=0,
                                    read_started_monotonic=now, read_finished_monotonic=now)
                    def sleep(seconds):
                        self.assertIn(seconds, (WARMUP, WINDOW))
                        phase["window"] += 1
                    cell = replace(self.cell, conns=4)
                    args = SimpleNamespace(server_cores="0-1", server_smt="", load_cores="2-3", load_smt="",
                                           port=9090, memtier="never-executed-memtier")
                    runner = Runner(args, directory, {"A": Path("never-executed-server")}, children)
                    class Affinity:
                        def __init__(inner, folder):
                            inner.record = {"status": "INCOMPLETE", "normal_gate_eligible": False}
                        def begin(inner, loads, layout, first, lifetime):
                            self.assertEqual(loads, generators)
                            self.assertEqual(phase["window"], 0)
                            self.assertEqual(lifetime, 33)
                            self.assertEqual(sum(p["threads"] * p["clients"] for p in layout), 4)
                            events.append("affinity-setup")
                            if affinity_mode == "setup-fail":
                                raise RuntimeError("affinity injected setup failure")
                        def verify(inner, stage):
                            self.assertEqual(phase["window"], 1)
                            events.append(stage)
                            if (affinity_mode == "warmup-fail" and stage == "after-fresh-warmup" or
                                    affinity_mode == "before-fail" and stage == "before-central-window"):
                                raise RuntimeError("affinity injected mask failure")
                            if affinity_mode == "lifetime-fail" and stage == "before-central-window":
                                # Move only the actual central-start observation past its
                                # budget; the real Runner must reject before WINDOW sleep.
                                real_clock = time.monotonic
                                delayed_clock = mock.patch.object(time, "monotonic", side_effect=lambda: real_clock() + 20)
                                delayed_clock.start()
                                self.addCleanup(delayed_clock.stop)
                                inner.delayed_clock = delayed_clock
                        def finish(inner):
                            self.assertEqual(phase["window"], 2)
                            events.append("affinity-finish")
                            if affinity_mode == "finish-fail":
                                raise RuntimeError("affinity injected end failure")
                            inner.record["status"] = "COMPLETE"
                        def fail(inner, error):
                            inner.record.update(status="INVALID", error=str(error))
                            if hasattr(inner, "delayed_clock"):
                                inner.delayed_clock.stop()
                    self.assertIsNone(runner.worker_affinity_factory)
                    self.assertEqual(runner.load_startup_seconds, 0)
                    if affinity_mode:
                        runner.load_startup_seconds = 5
                    if affinity_mode and affinity_mode != "floating":
                        runner.worker_affinity_factory = Affinity
                    class Profile:
                        def __init__(inner, folder):
                            profiles.append(inner)
                            inner.record = {"status": "INCOMPLETE", "normal_gate_eligible": False}
                        def begin(inner, server, loads):
                            self.assertIs(server, srv)
                            self.assertEqual(loads, generators)
                            self.assertEqual(phase["window"], 1)
                            inner.before = time.monotonic()
                            if profile_mode == "begin-fail":
                                raise RuntimeError("profile injected begin failure")
                        def finish(inner, first, last, count):
                            self.assertEqual(phase["window"], 2)
                            self.assertLessEqual(inner.before, first)
                            self.assertLess(first, last)
                            self.assertLessEqual(last, time.monotonic())
                            self.assertEqual(count, 4000)
                            if profile_mode == "finish-fail":
                                raise RuntimeError("profile injected finish failure")
                            inner.record["status"] = "COMPLETE"
                        def fail(inner, error):
                            inner.record.update(status="INVALID", error=str(error))
                        def close(inner):
                            if profile_mode == "close-fail":
                                raise RuntimeError("profile injected close failure")
                    self.assertIsNone(runner.profile_factory)
                    if profile_mode:
                        runner.profile_factory = Profile
                    lb = SimpleNamespace(threads={i: {"role": "fused", "busy": 10, "idle": 0} for i in (0, 1)})
                    with mock.patch.multiple(__name__, require_unbound_port=mock.Mock(), Conn=mock.Mock(return_value=conn),
                            info=mock.Mock(side_effect=snapshot), lb_snapshot=mock.Mock(return_value=lb),
                            cpu_seconds=mock.Mock(return_value=0), generator_cpu_endpoint=mock.Mock(side_effect=endpoint),
                            busy_between=mock.Mock(return_value=(99, {})),
                            busy_deltas=mock.Mock(return_value={}), productive_saturation=mock.Mock(return_value={}),
                            bottleneck_saturation=mock.Mock(return_value=saturation_record(threads=2)),
                            require_saturation_window=mock.Mock(return_value={"score_pct": 99.9})), \
                         mock.patch.object(runner, "populate", return_value=None), \
                         mock.patch.object(time, "sleep", side_effect=sleep), contextlib.redirect_stdout(io.StringIO()):
                        if affinity_mode not in (0, "fixed", "floating"):
                            with self.assertRaisesRegex(RuntimeError, "affinity injected|insufficient diagnostic load lifetime"):
                                runner.measure(cell, "A", 1, 2, {})
                        elif profile_mode not in (0, "on"):
                            with self.assertRaisesRegex(RuntimeError, "profile injected"):
                                runner.measure(cell, "A", 1, 2, {})
                        elif isinstance(corruption, str):
                            with self.assertRaisesRegex(RuntimeError, "CPU counter reset|exited during CPU endpoint"):
                                runner.measure(cell, "A", 1, 2, {})
                        elif corruption:
                            with self.assertRaisesRegex(RuntimeError, "accounting mismatch"):
                                runner.measure(cell, "A", 1, 2, {})
                        else:
                            result = runner.measure(cell, "A", 1, 2, {})
                            self.assertEqual(result["commands"], 4000)
                            self.assertEqual(result["whole_run_accounting"]["commands"]["GET"]["server_calls"], 10000)
                    self.assertEqual(events[0], ("commandstats", 0, 0))
                    retained = json.loads((directory / cell.id / "n2-1-A/measurement.json").read_text())
                    self.assertEqual(retained["complete"], not bool(corruption) and profile_mode in (0, "on") and
                                     affinity_mode in (0, "fixed", "floating"))
                    self.assertEqual(stopped, [124, 125, 123])
                    if affinity_mode:
                        self.assertEqual(retained["load_timing"]["requested_lifetime_seconds"], 33)
                        if affinity_mode in ("fixed", "floating"):
                            self.assertEqual(retained["full_histogram_runtime"]["requested_seconds"], 33)
                            self.assertEqual([row["duration_seconds"] for row in retained["full_histogram_runtime"]["processes"]], [33, 33])
                            self.assertEqual(retained["full_histogram_runtime"]["includes_startup_sigstop"], affinity_mode == "fixed")
                            self.assertFalse(retained["full_histogram_runtime"]["latency_scoring_eligible"])
                        if affinity_mode != "floating":
                            self.assertEqual(retained["load_worker_affinity"]["status"], "COMPLETE" if affinity_mode == "fixed" else "INVALID")
                            if affinity_mode != "setup-fail":
                                self.assertLess(events.index("affinity-setup"), events.index("after-fresh-warmup"))
                        if affinity_mode not in ("fixed", "floating"):
                            self.assertEqual(phase["window"], 2 if affinity_mode == "finish-fail" else 0 if affinity_mode == "setup-fail" else 1)
                            continue
                    if profile_mode:
                        self.assertEqual(len(profiles), 1)
                        self.assertEqual(retained["cpu_profile"]["status"], "COMPLETE" if profile_mode == "on" else "INVALID")
                        self.assertFalse(retained["cpu_profile"]["normal_gate_eligible"])
                    else:
                        self.assertEqual(profiles, [])
                        self.assertNotIn("cpu_profile", retained)
                    if profile_mode not in (0, "on"):
                        continue
                    cpu = retained["generator_cpu"]
                    self.assertFalse(cpu["decision_input"])
                    self.assertEqual(cpu["generator_headroom"], "UNPROVEN")
                    self.assertEqual([row["pid"] for row in cpu["processes"]], [124, 125])
                    self.assertTrue(all("before" in row for row in cpu["processes"]))
                    if isinstance(corruption, str):
                        self.assertEqual(cpu["status"], "INVALID")
                        self.assertTrue(all("cpu_pct_per_configured_worker" not in row for row in cpu["processes"]))
                        self.assertNotIn("whole_run_commandstats_after", retained)
                    else:
                        self.assertEqual(events[-1], ("commandstats", 2, 2))
                        self.assertIn("whole_run_commandstats_after", retained)
                        self.assertEqual(len(retained["memtier"]), 2)
                        self.assertEqual(cpu["status"], "COMPLETE")
                        self.assertEqual([event for event in events if isinstance(event, tuple) and event[0] == "generator-cpu"],
                                         [("generator-cpu", 1, 124), ("generator-cpu", 1, 125),
                                          ("generator-cpu", 2, 124), ("generator-cpu", 2, 125)])
                        for row in cpu["processes"]:
                            self.assertLessEqual(row["before"]["read_finished_monotonic"], cpu["central_start_monotonic"])
                            self.assertGreaterEqual(row["after"]["read_started_monotonic"], cpu["central_end_monotonic"])
                            self.assertEqual(row["cpu_seconds_delta"], 1)

        def test_real_runner_checks_effective_read_local_before_population(self):
            from types import SimpleNamespace
            # The mock CONFIG endpoint faithfully echoes the requested knob,
            # including inert legacy boots. Removing the INFO check reaches the
            # population tripwire and makes every mismatched/missing case fail.
            cases = ((0, "0", True), (1, "1", True), (1, "0", False), (0, "1", False),
                     (0, None, False), (1, None, False), (1, "", False),
                     (1, "01", False), (1, "2", False), (1, 1, False), (1, True, False))
            for mode, op in ((mode, op) for mode in ("1s", "2s") for op in ("GET", "SET", "MSET")):
                for arm in ("A", "B"):
                    for requested, effective, matches in cases:
                        with self.subTest(mode=mode, op=op, arm=arm, requested=requested, effective=effective), \
                             tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
                            folder = Path(tmp)
                            cell = replace(self.cell, mode=mode, op=op, read_local=requested)
                            knobs = {"thread-mode": mode, "read-local": requested}
                            configured = {"atomic": cell.atomic, **knobs}
                            identity = {"process_id": "123"}
                            if effective is not None:
                                identity["read_local"] = effective
                            conn = SimpleNamespace(close=mock.Mock(), must=mock.Mock(
                                side_effect=lambda *args: [args[-1].encode(), str(configured[args[-1]]).encode()]))
                            srv = SimpleNamespace(pid=123, poll=lambda: None)
                            children = SimpleNamespace(start=mock.Mock(return_value=srv), stop=mock.Mock())
                            args = SimpleNamespace(server_cores="0-7", server_smt="", load_cores="8-15",
                                                   load_smt="", port=9090)
                            runner = Runner(args, folder, {arm: Path("never-executed-server")}, children)
                            population = mock.Mock(side_effect=RuntimeError("population boundary reached"))
                            with mock.patch.multiple(__name__, require_unbound_port=mock.Mock(),
                                    Conn=mock.Mock(return_value=conn), info=mock.Mock(return_value=identity),
                                    measured_ratio=mock.Mock(return_value="4:4")), \
                                 mock.patch.object(runner, "populate", population), \
                                 contextlib.redirect_stdout(io.StringIO()):
                                error = ("population boundary reached" if matches else
                                         "effective read_local" if effective in ("0", "1") else
                                         "valid effective INFO SERVER read_local")
                                with self.assertRaisesRegex(RuntimeError, error):
                                    runner.measure(cell, arm, 1, 1, knobs)
                            if matches:
                                population.assert_called_once_with(cell, arm, conn, folder / cell.id / f"n1-1-{arm}")
                            else:
                                population.assert_not_called()
                            children.start.assert_called_once()  # No population or load child.
                            children.stop.assert_called_once_with(srv)
                            conn.close.assert_called_once()
                            retained = json.loads((folder / cell.id / f"n1-1-{arm}/measurement.json").read_text())
                            self.assertEqual(retained["boot_info"], identity)
                            self.assertFalse(retained["complete"])
                            self.assertNotIn("populate_seconds", retained)
                            self.assertNotIn("load_argv", retained)

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

        def round(self, rates, n=1, busy=99.5, latency=None, mode="1s"):
            layout = load_layout(list(range(32, 128)) + list(range(160, 256)), n, self.cell.conns)
            return {"instances": n, "runs": [dict(arm=arm, rate=rate, busy_pct=busy,
                    saturation=saturation_record(mode, busy),
                    window_seconds=20, midpoint_monotonic=11,
                    complete=True, instances=n, load_layout=layout,
                    latency_ms=(latency or [1, 1, 1, 1])[i])
                    for i, (arm, rate) in enumerate(zip(ORDER, rates))]}

        def test_abba_cancels_linear_drift(self):
            p = paired(self.round([100, 100.1, 100.2, 100.3])["runs"])
            self.assertAlmostEqual(p["delta_pct"], 0)
            self.assertAlmostEqual(p["threshold_pct"], 100 * .3 / 100.15)
            self.assertGreater(p["pair_deltas_pct"][0], 0)
            self.assertLess(p["pair_deltas_pct"][1], 0)

        def test_null_resolution_records_equal_gain_and_loss_for_every_scored_metric(self):
            from abba_evidence import null_resolution
            for score, depth, metric in (("rate", 32, "rate"), ("latency", 1, "latency_ms"),
                                         ("p999", 32, "p999_ms"), ("p999", 32, "long_p999_ms")):
                cell = replace(self.cell, score=score, depth=depth,
                               op="REORDER" if score == "p999" else "GET")
                for error in (-1.01, -1., 0., 1., 1.01):
                    with self.subTest(metric=metric, error=error):
                        runs = [dict(arm=arm, rate=100., latency_ms=100., p999_ms=100., long_p999_ms=100.)
                                for arm in ORDER]
                        for run, value in zip(runs, (99.5, 100 + error, 100 + error, 100.5)):
                            run[metric] = value
                        # Cached summaries deliberately claim success and a huge
                        # allowance; only raw observations establish null resolution.
                        report = {"cells": [dict(cell=asdict(cell), assessment={"verdict": "PASS", "threshold_pct": 99},
                                                 rounds=[dict(instances=4, runs=runs)])]}
                        # Owner ruling: a null MEASURES resolution, it does not fail on it. Both signs
                        # are recorded with their spreads; a comparison inherits them as floors.
                        checks = null_resolution(report)
                        scored = next(row for row in checks if row["metric"] == metric)
                        self.assertAlmostEqual(scored["absolute_delta_pct"], abs(error))
                        self.assertEqual(scored["reference_spread_pct"], 1.)
                        self.assertEqual(scored["candidate_spread_pct"], 0.)
                        self.assertEqual(scored["within_reference_spread"], abs(error) <= 1)
                        self.assertEqual(len(checks), 2 if score == "p999" else 1)

        def test_comparison_inherits_the_null_floor_and_can_still_fail(self):
            cell = replace(self.cell, score="rate", instances=4)   # pinned: one rung is a measurement
            control = {"null_control": {"resolution": [dict(cell=cell.id, instances=4, metric="rate",
                delta_pct=-1., absolute_delta_pct=1., reference_spread_pct=.3, candidate_spread_pct=2.5,
                within_reference_spread=False)]}}
            bounds = resolution_bounds(control, cell.id)
            self.assertEqual(bounds, {"rate": {"spread": 2.5, "abs_delta": 1.}})
            # A 0.8% loss against a reference that repeats to 0.3%: the flat rule calls it a
            # regression; the null proved the instrument itself drifts 1.0% on identical bytes.
            drift = [self.round([100.15, 99.2, 99.2, 99.85], n=4)]
            self.assertEqual(assess(cell, drift)["verdict"], "FAIL")
            floored = assess(cell, drift, bounds)
            self.assertEqual(floored["verdict"], "PASS")
            self.assertEqual(floored["threshold_source"], "null-floor")
            # A candidate spread of 2.3% fails the flat 2% bound and passes the null's 2.5%.
            noisy = [self.round([100, 98.85, 101.15, 100], n=4)]
            self.assertIn("stability boundary", " ".join(assess(cell, noisy)["reasons"]))
            self.assertNotIn("stability boundary", " ".join(assess(cell, noisy, bounds)["reasons"]))
            # The floor is a floor, not a pass: a 1.5% loss still fails with it.
            real = [self.round([100.1, 98.5, 98.5, 99.9], n=4)]
            self.assertEqual(assess(cell, real, bounds)["verdict"], "FAIL")
            # NULL_MODE never vetoes on spread or threshold; measurement validity still applies.
            self.assertEqual(assess(cell, real, NULL_MODE)["verdict"], "PASS")
            self.assertIsNone(resolution_bounds(None, cell.id))
            self.assertIsNone(resolution_bounds(control, "other"))
            # The blocker command's p99.9 has its own threshold and must follow the same contract.
            tail = replace(self.cell, score="p999", op="REORDER", instances=4)
            def tail_round(short, long_):
                r = self.round([100] * 4, n=4)
                for run, s_, l_ in zip(r["runs"], short, long_):
                    run.update(p999_ms=s_, long_p999_ms=l_)
                return r
            # long p99.9 drifts +0.8% (B slower) against a reference that repeats to 0.3%.
            drift_tail = [tail_round([1., 1., 1., 1.], [1.0015, 1.0095, 1.0095, .9985])]
            self.assertIn("long-command p99.9", " ".join(assess(tail, drift_tail)["reasons"]))
            tail_control = {"null_control": {"resolution": [
                dict(cell=tail.id, instances=4, metric="p999_ms", delta_pct=0., absolute_delta_pct=0.,
                     reference_spread_pct=.1, candidate_spread_pct=.1, within_reference_spread=True),
                dict(cell=tail.id, instances=4, metric="long_p999_ms", delta_pct=1., absolute_delta_pct=1.,
                     reference_spread_pct=.3, candidate_spread_pct=.3, within_reference_spread=False)]}}
            tb = resolution_bounds(tail_control, tail.id)
            self.assertNotIn("long-command p99.9", " ".join(assess(tail, drift_tail, tb)["reasons"]))
            self.assertNotIn("long-command p99.9", " ".join(assess(tail, drift_tail, NULL_MODE)["reasons"]))

        def test_null_resolution_retains_a_failing_escalation_probe(self):
            from abba_evidence import null_resolution
            cell = replace(self.cell, score="rate")
            good = self.round([100] * 4, n=1)
            bad = self.round([100, 101, 101, 100], n=2)
            report = {"cells": [dict(cell=asdict(cell), rounds=[good, bad],
                                     assessment={"instances": 1, "verdict": "PASS"})]}
            checks = null_resolution(report)
            probe = [row for row in checks if row["instances"] == 2 and row["metric"] == "rate"]
            self.assertEqual(len(probe), 1)
            self.assertFalse(probe[0]["within_reference_spread"])
            self.assertAlmostEqual(probe[0]["absolute_delta_pct"], 1.)

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
            rounds = [self.round([100, 100, 100, 100], 2, mode="2s")]
            self.assertEqual(assess(pinned, rounds)["verdict"], "PASS")

        def test_a_pin_that_stops_saturating_fails_and_says_how_to_fix_it(self):
            # A candidate fast enough to outgrow its pinned load must not be measured in headroom.
            pinned = Cell("hp", "2s", 0, 1, 0, "SET", 32, 512, instances=2)
            rounds = [self.round([100] * 4, 2, busy=BUSY_FLOOR - 5, mode="2s")]
            a = assess(pinned, rounds)
            self.assertEqual(a["verdict"], "FAIL")
            self.assertTrue(any("re-pin" in r for r in a["reasons"]), a["reasons"])

        def test_a_peak_at_the_top_is_not_yet_proven(self):
            # Still climbing: without a higher probe that fails to beat it, the top block might
            # simply be the last one we ran.
            rounds = [self.round([100] * 4, 1), self.round([200] * 4, 2)]
            self.assertIn("n=2: no higher-instance confirmation block", assess(self.cell, rounds)["reasons"])
            self.assertFalse(saturation_done(self.cell, rounds))

        def test_fastest_arm_still_gaining_fails(self):
            rounds = [self.round([100, 110, 110, 100], 1), self.round([100, 120, 120, 100], 2)]
            self.assertFalse(saturation_done(self.cell, rounds))
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")

        def test_one_probe_cannot_prove_plateau(self):
            self.assertEqual(assess(self.cell, [self.round([100] * 4)])["verdict"], "FAIL")

        def test_busy_floor_applies_to_every_run_of_the_judged_block(self):
            # One idle run inside the block being judged is enough to reject it.
            for index in range(4):
                for pinned in (False, True):
                    with self.subTest(index=index, pinned=pinned):
                        cell = replace(self.cell, instances=1 if pinned else 0)
                        rounds = [self.round([100] * 4, n) for n in ((1,) if pinned else (1, 2))]
                        rounds[0]["runs"][index]["saturation"] = saturation_record(score=50)
                        self.assertEqual(assess(cell, rounds)["verdict"], "FAIL")

        def test_split_local_io_can_saturate_with_idle_executors(self):
            cell = replace(self.cell, mode="2s", instances=1)
            block = self.round([100] * 4, mode="2s")
            for run in block["runs"]:
                run["busy_pct"] = 49.5
                run["saturation"] = saturation_record("2s", inactive_roles=("ex",))
            result = assess(cell, [block])
            self.assertEqual(result["verdict"], "PASS", result["reasons"])
            self.assertEqual(result["busy_pct_abba"], [49.5] * 4)
            self.assertTrue(all(value >= BUSY_FLOOR for value in result["saturation_pct_abba"]))
            # The corrected role witness does not remove the higher-capacity probe.
            self.assertEqual(assess(replace(cell, instances=0), [block])["verdict"], "FAIL")

        def test_legacy_busy_cannot_replace_missing_or_forged_saturation_evidence(self):
            for absent in (True, False):
                block = self.round([100] * 4)
                if absent:
                    block["runs"][1].pop("saturation")
                else:
                    block["runs"][1]["saturation"]["threads"][0]["ops_delta"] = 0
                result = assess(replace(self.cell, instances=1), [block])
                self.assertEqual(result["verdict"], "FAIL")
                self.assertFalse(result["measurement_valid"])

        def test_saturation_from_another_window_is_rejected(self):
            block = self.round([100] * 4)
            block["runs"][0]["window_seconds"] = 21
            result = assess(replace(self.cell, instances=1), [block])
            self.assertFalse(result["measurement_valid"])
            self.assertTrue(any("do not span" in reason for reason in result["reasons"]))

        def test_pinned_floor_cannot_borrow_occupancy_outside_central_window(self):
            block = self.round([100] * 4)
            for run in block["runs"]:
                run.update(saturation=saturation_record(score=98, window_seconds=1000),
                           midpoint_monotonic=501)
            result = assess(replace(self.cell, instances=1), [block])
            self.assertEqual(result["verdict"], "FAIL")
            self.assertEqual(result["saturation_pct_abba"], [0] * 4)

        def test_reference_noise_cannot_turn_a_bad_session_green(self):
            rounds = [self.round([100, 99, 99, 103], n) for n in (1, 2)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "FAIL")

        def test_stable_equal_arms_pass(self):
            rounds = [self.round([100, 100, 100, 100], n) for n in (1, 2)]
            self.assertEqual(assess(self.cell, rounds)["verdict"], "PASS")
            self.assertEqual(assess(self.cell, rounds)["threshold_pct"], 0)

        def test_depth_one_uses_latency_and_is_exempt(self):
            c = Cell("p1", "2s", 0, 0, 0, "GET", 1, 512)
            a = assess(c, [self.round([100] * 4, busy=10, latency=[1, 1.1, 1.1, 1.001], mode="2s")])
            self.assertEqual(a["verdict"], "FAIL")
            self.assertTrue(a["saturation_exempt"])
            self.assertGreater(a["loss_pct"], 9)
            self.assertEqual(assess(c, [self.round([100] * 4, busy=10, mode="2s")])["verdict"], "PASS")
            # This escalation repair must not add a throughput score to the p1 latency gate.
            self.assertEqual(assess(c, [self.round([100, 100, 100, 120], busy=10, mode="2s")])["verdict"], "PASS")

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
            for n in sorted(set(LADDER) | {3}):
                layout = load_layout(load, n, 512)
                self.assertEqual(sum(x["threads"] * x["clients"] for x in layout), 512)
                assigned = [c for x in layout for c in x["cpus"]]
                self.assertEqual(sorted(assigned), load)
                self.assertEqual(len(assigned), len(set(assigned)))

        def fake_main(self, *, pin="-", depth=32, escalate=False, busy=99.9,
                      climbing=False, ceiling=16, contend_after=None, reference_error=None, rates=None,
                      run_overrides=None, load_cores="32-127", load_smt="160-255",
                      diagnostic_profile=0,
                      diagnostic_pin_load_workers=0, diagnostic_load_startup_seconds=0):
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
                        "--server-cores", "0-31", "--server-smt", "", "--load-cores", load_cores,
                        "--load-smt", load_smt, "--max-instances", str(ceiling)] + (["--escalate"] if escalate else [])
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
                    value = rates[instances][sequence - 1] if rates else instances * 100 if climbing else 100
                    result = dict(arm=arm, rate=value, complete=True, instances=instances,
                                  load_layout=layouts[-1], busy_pct=busy, latency_ms=1,
                                  window_seconds=20, midpoint_monotonic=11,
                                  saturation=saturation_record(cell.mode, busy))
                    if run_overrides:
                        result.update(run_overrides(instances, sequence))
                    return result

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
                    rc = main(args, diagnostic_profile=diagnostic_profile,
                              diagnostic_pin_load_workers=diagnostic_pin_load_workers,
                              diagnostic_load_startup_seconds=diagnostic_load_startup_seconds)
                return rc, order, layouts, json.loads((output / "results.json").read_text()), stream.getvalue()

        def test_profile_cannot_be_enabled_in_normal_gate_or_cli(self):
            rc, calls, _, report, _ = self.fake_main(pin=4, diagnostic_profile=1)
            self.assertEqual((rc, calls), (1, []))
            self.assertIn("permanently untrusted diagnostic", report["reason"])
            self.assertFalse(report["measurement_valid"])
            with mock.patch.object(sys, "argv", ["abbagate.py", "--profile", "1"]), \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                parse_args()

        def test_worker_pinning_and_startup_allowance_cannot_enter_normal_gate(self):
            for pin, allowance in ((1, 5), (0, 5), (1, 0)):
                rc, calls, _, report, _ = self.fake_main(pin=4,
                    diagnostic_pin_load_workers=pin, diagnostic_load_startup_seconds=allowance)
                self.assertEqual((rc, calls), (1, []))
                self.assertFalse(report["measurement_valid"])
                self.assertFalse(report["normal_gate_eligible"])
            for option, value in (("--pin-load-workers", "1"), ("--load-startup-seconds", "5")):
                with mock.patch.object(sys, "argv", ["abbagate.py", option, value]), \
                     contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    parse_args()

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

        def test_real_loop_passes_selected_port_and_preserves_cpu_samples(self):
            _, measurements, _, report, _ = self.fake_main(pin=4)
            self.assertEqual(len(measurements), 4)
            self.assertEqual(self.quiet_factory.call_args.kwargs["ports"], (8700,))
            self.assertEqual(self.quiet_factory.call_args.kwargs["sample_artifact"].name, "quiet-samples.jsonl")
            self.assertEqual(report["quiet_box"]["policy"], "selected-core-port-budget-v1")

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
            for pin in (3, 4, 12):
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
                    if pin == 12:
                        self.assertEqual(sum(x["threads"] for x in layouts[0]), 192)
                        self.assertEqual(sorted(x["clients"] for x in layouts[0]), [2] * 4 + [3] * 8)

        def test_escalate_ignores_pin_and_drives_full_ladder(self):
            rc, order, _, result, output = self.fake_main(pin=3, escalate=True, climbing=True)
            self.assertEqual(order, [(n, arm) for n in sorted(set(LADDER) | {3}) for arm in ORDER])
            self.assertEqual(len(order), 28)
            self.assertEqual(rc, 1, output)  # Still rising at the ceiling is unproven saturation.
            self.assertIn("ESCALATE ignores pin=3", output)
            self.assertIn("n=16: no higher-instance confirmation block",
                          result["cells"][0]["assessment"]["reasons"])

        def test_real_escalation_accepts_positive_gain_inside_each_arms_repeatability(self):
            rates = {1: [99.9, 99.9, 100.1, 100.1], 2: [100, 100, 100.2, 100.2]}
            rc, order, layouts, result, output = self.fake_main(escalate=True, rates=rates)
            self.assertEqual(rc, 3, output)
            self.assertEqual(order, [(n, arm) for n in (1, 2) for arm in ORDER])
            selection = result["cells"][0]["assessment"]["load_selection"]
            self.assertEqual(selection["lowest_tested_qualifying_instances"], 1)
            self.assertEqual(selection["numerical_peak_instances"], 2)
            self.assertEqual(selection["confirmation_instances"], 2)
            self.assertEqual(selection["generator_headroom"], "UNPROVEN")
            self.assertEqual(selection["tested_rungs"][0]["load_layout"], layouts[0])
            for arm in ("reference", "candidate"):
                self.assertGreater(selection["tested_rungs"][0]["arm_gains_pct"][arm], 0)
                self.assertLess(selection["tested_rungs"][0]["arm_gains_pct"][arm],
                                selection["tested_rungs"][0]["arm_repeatability_pct"][arm])

        def test_real_escalation_cannot_discard_an_unstable_higher_probe(self):
            rates = {1: [100] * 4, 2: [99, 99, 102, 102], 4: [100] * 4}
            rc, order, _, result, output = self.fake_main(escalate=True, rates=rates)
            self.assertEqual(rc, 1, output)
            self.assertEqual(order, [(n, arm) for n in (1, 2) for arm in ORDER])
            row = result["cells"][0]
            self.assertEqual(row["assessment"]["load_selection"]["status"], "INVALID")
            self.assertFalse(row["assessment"]["measurement_valid"])
            self.assertTrue(any("measurement n=2" in reason and "stability boundary" in reason
                                for reason in row["assessment"]["reasons"]))
            self.assertIsNone(row["assessment"]["load_selection"]["lowest_tested_qualifying_instances"])
            self.assertEqual(len(row["rounds"][1]["runs"]), 4)

        def test_real_escalation_retests_existing_non_ladder_pin(self):
            rates = {1: [100] * 4, 2: [200] * 4, 3: [300] * 4, 4: [300] * 4}
            rc, order, layouts, result, output = self.fake_main(pin=3, escalate=True, rates=rates)
            self.assertEqual(rc, 3, output)
            self.assertEqual(order, [(n, arm) for n in (1, 2, 3, 4) for arm in ORDER])
            selection = result["cells"][0]["assessment"]["load_selection"]
            self.assertEqual(selection["lowest_tested_qualifying_instances"], 3)
            self.assertEqual(selection["confirmation_instances"], 4)
            self.assertEqual([r["instances"] for r in selection["lower_rung_rejections"]], [1, 2])
            self.assertTrue(all(r["reasons"] for r in selection["lower_rung_rejections"]))
            self.assertTrue(all(sum(p["threads"] * p["clients"] for p in layout) == 512 for layout in layouts))

        def test_real_escalation_does_not_hide_either_arms_different_knee(self):
            for slower in ("A", "B"):
                with self.subTest(slower=slower):
                    means = {1: (100, 90), 2: (200, 100), 4: (198, 150), 8: (197, 149)}
                    rates = {n: [pair[1 if arm == slower else 0] for arm in ORDER]
                             for n, pair in means.items()}
                    rc, order, _, result, output = self.fake_main(escalate=True, rates=rates)
                    self.assertEqual(order, [(n, arm) for n in (1, 2, 4, 8) for arm in ORDER])
                    self.assertEqual(rc, 1 if slower == "B" else 3, output)
                    selection = result["cells"][0]["assessment"]["load_selection"]
                    self.assertEqual(selection["numerical_peak_instances"], 2)
                    self.assertEqual(selection["lowest_tested_qualifying_instances"], 4)
                    self.assertEqual(selection["confirmation_instances"], 8)
                    reason = ("reference" if slower == "A" else "candidate") + " still gains"
                    self.assertTrue(any(reason in text for text in selection["lower_rung_rejections"][1]["reasons"]))
                    chosen = selection["tested_rungs"][selection["selected_index"]]
                    self.assertEqual(chosen["confirmation_shape"], "congestion")
                    self.assertEqual(selection["generator_headroom"], "UNPROVEN")

        def test_real_escalation_twelve_confirms_eight_with_more_workers(self):
            rates = {n: [min(n, 8) * 100] * 4 for n in LADDER}
            rc, order, layouts, result, output = self.fake_main(escalate=True, rates=rates)
            self.assertEqual(rc, 3, output)
            self.assertEqual(order, [(n, arm) for n in (1, 2, 4, 8, 12) for arm in ORDER])
            self.assertEqual(len(order), 20)
            workers = {n: sum(p["threads"] for p in layout)
                       for (n, _), layout in zip(order, layouts)}
            self.assertEqual(workers, {1: 16, 2: 32, 4: 64, 8: 128, 12: 192})
            for layout in layouts:
                assigned = [cpu for p in layout for cpu in p["cpus"]]
                self.assertEqual(sorted(assigned), list(range(32, 128)) + list(range(160, 256)))
                self.assertEqual(sum(p["threads"] * p["clients"] for p in layout), 512)
            selection = result["cells"][0]["assessment"]["load_selection"]
            self.assertEqual(selection["lowest_tested_qualifying_instances"], 8)
            self.assertEqual(selection["confirmation_instances"], 12)
            self.assertEqual(selection["generator_headroom"], "UNPROVEN")

        def test_real_escalation_sixteen_cannot_credit_fewer_workers_than_twelve(self):
            rates = {n: [min(n, 12) * 100] * 4 for n in LADDER}
            rc, order, layouts, result, output = self.fake_main(escalate=True, rates=rates)
            self.assertEqual(rc, 1, output)
            self.assertEqual(order, [(n, arm) for n in LADDER for arm in ORDER])
            self.assertEqual(len(order), 24)
            workers = {n: sum(p["threads"] for p in layout)
                       for (n, _), layout in zip(order, layouts)}
            self.assertEqual(workers, {1: 16, 2: 32, 4: 64, 8: 128, 12: 192, 16: 128})
            selection = result["cells"][0]["assessment"]["load_selection"]
            self.assertIsNone(selection["lowest_tested_qualifying_instances"])
            self.assertIn("higher instance count did not increase generator worker capacity",
                          selection["tested_rungs"][-2]["rejection_reasons"])

        def test_real_escalation_cannot_call_same_worker_count_more_capacity(self):
            # A smaller explicitly assigned load pool makes n=4 and n=8 both use
            # 64 workers. Even a clean plateau cannot turn more processes into
            # evidence of greater worker capacity; retain every rejected probe.
            rates = {n: [(400 if n == 8 else min(n, 12) * 100)] * 4 for n in LADDER}
            rc, order, layouts, result, output = self.fake_main(
                escalate=True, rates=rates, load_cores="32-79", load_smt="160-207")
            self.assertEqual(rc, 1, output)
            self.assertEqual(order, [(n, arm) for n in LADDER for arm in ORDER])
            self.assertEqual(len(order), 24)
            workers = {n: sum(p["threads"] for p in layout)
                       for (n, _), layout in zip(order, layouts)}
            self.assertEqual(workers[4], 64)
            self.assertEqual(workers[8], workers[4])
            selection = result["cells"][0]["assessment"]["load_selection"]
            self.assertIsNone(selection["lowest_tested_qualifying_instances"])
            rung = next(row for row in selection["tested_rungs"] if row["instances"] == 4)
            self.assertIn("higher instance count did not increase generator worker capacity",
                          rung["rejection_reasons"])

        def test_real_escalation_accepts_twelve_as_cli_ceiling(self):
            rc, order, _, result, output = self.fake_main(escalate=True, climbing=True, ceiling=12)
            self.assertEqual(rc, 1, output)
            self.assertEqual(order, [(n, arm) for n in (1, 2, 4, 8, 12) for arm in ORDER])
            self.assertEqual(len(order), 20)
            self.assertIn("n=12: no higher-instance confirmation block",
                          result["cells"][0]["assessment"]["reasons"])

        def test_real_escalation_incomplete_measurement_is_permanent_failure(self):
            rc, order, _, result, output = self.fake_main(escalate=True,
                run_overrides=lambda n, sequence: {"complete": False} if n == 1 and sequence == 2 else {})
            self.assertEqual(rc, 1, output)
            self.assertEqual(order, [(1, arm) for arm in ORDER])
            self.assertFalse(result["cells"][0]["assessment"]["measurement_valid"])

        def test_escalate_cannot_borrow_pin_for_a_single_block(self):
            rc, order, _, result, _ = self.fake_main(pin=1, escalate=True, ceiling=1)
            self.assertEqual(order, [(1, arm) for arm in ORDER])
            self.assertEqual(rc, 1)
            self.assertIn("n=1: no higher-instance confirmation block",
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
                                    complete=True, instances=instances,
                                    load_layout=load_layout(_self.load_cpus, instances, cell.conns),
                                    busy_pct=99.9, latency_ms=1, saturation=saturation_record(cell.mode),
                                    window_seconds=20, midpoint_monotonic=11)

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

                def run(*, collect=False, subset="full", control=None, candidate_rate=100, only="", escalate=False):
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
                    if escalate:
                        argv += ["--escalate"]
                    with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, {}, clear=True):
                        args = parse_args()
                    calls = []
                    quiet_started = epoch + ticks[0]
                    cpus_ = list(range(128))
                    quiet = mock.Mock()
                    def evidence():
                        count = max(2, int(epoch + ticks[0] - quiet_started))
                        return quiet_record(cpus=cpus_, samples=count, started_at=quiet_started,
                            finished_at=epoch + ticks[0], sample_artifact=out / "quiet-samples.jsonl")
                    quiet.evidence.side_effect = evidence
                    quiet.close.side_effect = evidence
                    def measure(_runner, cell, arm, index, instances, knobs):
                        calls.append((cell.id, instances, arm))
                        ticks[0] += WINDOW + 8
                        return dict(arm=arm, rate=100 if arm == "A" else candidate_rate, busy_pct=99.9,
                            saturation=saturation_record(cell.mode),
                            midpoint_monotonic=11,
                            latency_ms=1, complete=True, commands=2000, pid=123, window_seconds=WINDOW,
                            instances=instances, load_layout=load_layout(_runner.load_cpus, instances, cell.conns),
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
                original_source = source.read_text()
                source.write_text(original_source.replace(" | 4 | ", " | - | "))
                rc, calls, calibrated, _ = run(collect=True, escalate=True)
                source.write_text(original_source)
                self.assertEqual((rc, len(calls), calibrated["null_control"]["verdict"]), (3, 16, "PASS"))
                self.assertEqual(calibrated["coverage"]["requested_pending_pins"], ["n1", "n2"])
                self.assertEqual(calibrated["coverage"]["pending_pins"], [])
                self.assertEqual([row["cell"]["instances"] for row in calibrated["cells"]], [0, 0])
                # A +-1% delta on identical arms is the INSTRUMENT'S error. The null records it as
                # this cell's resolution instead of failing; a later comparison inherits it as a
                # floor on its threshold. Either sign is the same measurement.
                for rate in (99, 101):
                    rc, calls, noisy_null, _ = run(collect=True, candidate_rate=rate)
                    self.assertEqual((rc, len(calls), noisy_null["verdict"]), (3, 8, "PARTIAL"))
                    self.assertEqual(noisy_null["null_control"]["verdict"], "PASS")
                    rows = [r for r in noisy_null["null_control"]["resolution"] if r["metric"] == "rate"]
                    self.assertTrue(rows and all(abs(r["absolute_delta_pct"] - 1.) < 1e-6 for r in rows))
                    self.assertTrue(all(r["within_reference_spread"] is False for r in rows))
                rc, calls, improvement, _ = run(control=control, candidate_rate=101)
                self.assertEqual((rc, len(calls), improvement["verdict"]), (0, 8, "PASS"))
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
                    "favorable null error": lambda c: [run.update(rate=101) for run in c["cells"][0]["rounds"][0]["runs"]
                                                        if run["arm"] == "B"],
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
                # The same 2% on IDENTICAL bytes is not a regression, it is the instrument's error:
                # the null records it as this cell's resolution (and a later comparison would inherit
                # a 2% floor -- honest, since the instrument demonstrably cannot see below that).
                rc, calls, report, _ = run(collect=True, candidate_rate=98)
                self.assertEqual((rc, len(calls), report["statistical_verdict"], report["null_control"]["verdict"]),
                                 (3, 8, "PASS", "PASS"))
                res = [r for r in report["null_control"]["resolution"] if r["metric"] == "rate"]
                self.assertTrue(res and all(abs(r["absolute_delta_pct"] - 2.) < 1e-6 and
                                            r["within_reference_spread"] is False for r in res))
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
                    return dict(arm=arm, rate=instances * 100, busy_pct=99.9, latency_ms=1,
                                saturation=saturation_record(cell.mode, threads=len(_self.server_cpus)),
                                window_seconds=20, midpoint_monotonic=11,
                                complete=True, instances=instances,
                                load_layout=load_layout(_self.load_cpus, instances, cell.conns))

                provenance = dict(source="test", commit="0" * 40, sha256=sha256(binary))
                with mock.patch.object(Runner, "measure", measure), \
                     mock.patch(__name__ + ".measured_ratio", return_value="4:4"), \
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
                self.assertIn("n=8: no higher-instance confirmation block", row["assessment"]["reasons"])

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
    if args.self_test:
        from load_calibration import self_test as calibration_self_test
        sys.exit(max(self_test(), saturation_self_test(), calibration_self_test()))
    sys.exit(main(args))
