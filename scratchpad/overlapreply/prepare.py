#!/usr/bin/env python3
"""Prepare O7's measurement handoff offline; never launch a measured executable.

The owner requires an armed stall profile BEFORE a reply-side implementation.
Keep that prerequisite separate from subsequent PRE/POST judging: identical PRE
arms can establish a control, but cannot be relabelled as a POST result. Reuse
the gate's cell parser and workload definitions rather than inventing a driver.
Generated requests, cell lists and measurements are lane artifacts, not source.
"""

from dataclasses import replace
import argparse
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
import abbagate as abba


def shape(cell):
    return (cell.mode, cell.read_local, cell.overlap, cell.reorder, cell.op,
            cell.depth, cell.conns, cell.atomic, cell.score, cell.mix)


def select_cells(source):
    """Preserve smoke controls; add the armed matrix that smoke does not cover."""
    by_shape = {}
    for cell in source:
        if shape(cell) in by_shape:
            raise ValueError(f"ambiguous headline shape: {cell.id}")
        by_shape[shape(cell)] = cell

    def lookup(mode, op, depth, overlap, *, conns=512):
        wanted = (mode, 1, overlap, 1, op, depth, conns, 1,
                  "latency" if depth == 1 else "rate", "-")
        if wanted not in by_shape:
            raise ValueError(f"missing required headline shape: {wanted}")
        return by_shape[wanted]

    # Eight current cells decide whether O7 has a target. GET may run entirely
    # on the local lane; SET and per-role/local-hit witnesses distinguish that
    # from an EX-to-IO crossing. Overlap 0 and 1 are separate baseline postures.
    profile = [lookup(mode, op, 32, overlap)
               for mode in ("1s", "2s") for op in ("GET", "SET")
               for overlap in (0, 1)]

    smoke = [cell for cell in source if cell.smoke]
    if len(smoke) != 17:
        raise ValueError(f"competition expects 17 smoke cells; source has {len(smoke)}")
    if not {f"t{number:02}" for number in range(1, 7)} <= {c.id for c in smoke}:
        raise ValueError("competition tail controls t01-t06 are missing")

    # Disarmed/armed pairs are necessary for S4; an all-on result cannot show
    # that the new mechanism has zero fixed cost when overlap is disabled.
    matrix = [lookup(mode, op, depth, overlap)
              for mode in ("1s", "2s") for op in ("GET", "SET", "MGET", "MSET")
              for depth in (1, 32) for overlap in (0, 1)]
    shallow = [replace(lookup(mode, op, 32, overlap),
                       id=f"o7-{mode}-{op.lower()}-p8-ov{overlap}", depth=8, smoke=False)
               for mode in ("1s", "2s") for op in ("GET", "SET")
               for overlap in (0, 1)]
    connections = [lookup(mode, "GET", 32, 1, conns=2048)
                   for mode in ("1s", "2s")]
    # t01-t04 intentionally disarm the local lane to expose the owner reorder
    # scope. Keep those controls AND supply the armed split counterparts of
    # t05/t06; do not mistake the control posture for C2 coverage in both modes.
    tails = [replace(next(c for c in smoke if c.id == ident),
                     id=f"o7-2s-tail-ro{reorder}", mode="2s", smoke=False)
             for ident, reorder in (("t05", 1), ("t06", 0))]
    judging = list({shape(c): c for c in smoke + matrix + shallow + connections + tails}.values())

    # The history explicitly leaves the beyond-p32 ceiling open. This extension
    # is requested after attribution, not substituted for the mandatory cells.
    deep = [replace(lookup(mode, "GET", 32, overlap),
                    id=f"o7-{mode}-get-p{depth}-ov{overlap}", depth=depth, smoke=False)
            for mode in ("1s", "2s") for depth in (64, 128)
            for overlap in (0, 1)]
    return {"profile": profile, "judging": judging, "deep": deep}


def render(cells):
    # Private IDs prevent an old load-floor pin from being inherited by a new
    # NIC/CPU geometry. The maintainer calibrates these with the gate instrument.
    # Preserve commands, key counts, mixtures and scoring via its normal parser.
    return "".join(
        f"{c.id if c.id.startswith('o7-') else 'o7-' + c.id} | {c.mode} | rl={c.read_local} | ov={c.overlap} | ro={c.reorder} | "
        f"{c.op} | p{c.depth} | {c.conns} | - | - | - | atomic={c.atomic} | "
        f"score={c.score} | mix={c.mix} | smoke={int(c.smoke)}\n" for c in cells)


def prepare(pre, output, request):
    pre, output, request = pre.resolve(), output.resolve(), request.resolve()
    for path in (pre, output, request):
        if not path.is_relative_to(ROOT):
            raise ValueError(f"lane artifacts must stay in this worktree: {path}")
    with pre.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            raise ValueError("PRE is not an ELF executable; build it before preparing the request")
    digest = abba.sha256(pre)
    groups = select_cells(abba.read_cells(ROOT / "tests/headline_cells.txt"))
    if request.exists():
        raise FileExistsError(f"preserve the existing measurement request: {request}")
    output.mkdir(parents=True, exist_ok=False)
    artifacts = {}
    for name, cells in groups.items():
        path = output / f"{name}-cells.txt"
        path.write_text(render(cells))
        parsed = abba.read_cells(path)
        if [shape(c) for c in parsed] != [shape(c) for c in cells] or any(c.instances for c in parsed):
            raise ValueError(f"generated cells changed shape or inherited a load pin: {path}")
        artifacts[name] = {"path": str(path), "count": len(parsed),
                           "ids": [c.id for c in parsed]}

    document = {
        "lane": "overlap-O7",
        "status": "awaiting prerequisite stall profile; no O7 candidate",
        "NEEDS-BOX": "Maintainer to schedule armed baseline attribution, one measuring lane at a time.",
        "pre": {"path": str(pre), "sha256": digest,
                "build_log": str(ROOT / "build/overlap-O7/build-pre.log")},
        "post": None,
        "pad": None,
        "cells": artifacts,
        "geometry": {
            "assignment": "proposed only; maintainer assigns the quiet slot and port",
            "server_cores": "0-31", "server_smt": "", "load_cores": "32-111", "load_smt": "",
            "split_ratio": abba.measured_ratio("abba", 32),
            "shards": "gate ABBA rule: min(8 * owners, 256); record actual argv",
            "transport": "25GbE serverns/clientns rig; tests/niclib.sh",
            "correctness_geometry": "16 shards; GATE_RATIO/GATE_CORES, default 6:2 on 0-7",
            "restriction": "No process-name killing. Stop only the exact PIDs the maintainer's driver started.",
        },
        "fixed_settings": {
            "key-lb": 1, "client-lb": 1, "atomic": 1,
            "read-local": "1 in every profile/matrix cell; original smoke controls retained separately",
            "overlap": "0/1 in separate cells; identical setting in PRE and POST for each cell",
            "reorder": "1 except the explicitly retained smoke/tail off controls",
            "flip-auto": 0,
            "witness": "Record CONFIG GET and effective INFO SERVER read_local, worker roles and local-hit deltas.",
        },
        "workload": {
            "populated_keys": abba.KEYS, "value_bytes": 64,
            "central_window_seconds": abba.WINDOW, "warmup_seconds": abba.WARMUP,
            "tail_seconds": abba.TAIL, "connections": "from each cell, normally 512",
            "multi_key_width": 8, "operation_unit": "commands, not individual keys",
            "generator_instances": "Use the gate's load ladder and retain every attempt; calibrate at the assigned geometry.",
        },
        "phase_1": {
            "cells": "profile",
            "arms": "PRE only, overlap 0/1 separately; repeat controls on identical PRE bytes",
            "instrument": "gate-owned command/count windows and PMCs; separate sampled diagnostic captures",
            "note": "tests/abbagate.py's stock Runner uses loopback. It has no NIC or stall-sampler CLI; "
                    "the maintainer must use the NIC rig with the same gate accounting. "
                    "tests/abba_profile.py WindowProfile provides owned-TID PMCs and schedstat, not stall sampling.",
            "samples": [
                "cycles with symbol/source/callchain attribution for every server worker, plus user/kernel scope",
                "IBS demand-load latency/source/IP for reply/ROB/queue versus storage; "
                "reuse cx-cacheutil/scratchpad/cacheutil/ibs_account.py and ibs_record_account.py",
                "Keep invalid, unknown, off-target, lost and throttled samples explicit; "
                "do not scale sample shares into an additive wall-stall decomposition.",
            ],
            "boundaries": {
                "reply": "wb_retire_prepare, wb_serve_natural, WbEngine::serve_impl, ROB Done loads and reply copies",
                "crossing": "task publication versus notification/wake, Done-to-ready fence, queue/ROB pressure",
                "store": "owner execute/prefetch, bucket/record demand accesses, and actual local-read execution",
                "send": "io_uring/kernel, SEND/command, commands/SEND, bytes/command and backpressure",
            },
            "decision": "Return source-attributed evidence identifying the p32 limitation before O7 implementation. "
                        "A rate plateau, IPC alone, or GET that bypasses EX is insufficient evidence of a reply crossing bottleneck.",
        },
        "phase_2_after_profile_and_candidate": {
            "cells": "judging; then deep for the ceiling/break-even extension",
            "arms": "PRE/POST in A-B-B-A order at the same offered load; PAD twin if a layout changes",
            "columns": ["rate_commands_per_second", "IPC", "instructions_per_command", "cycles_per_command"],
            "verdict": "cycles/command = instructions/command / IPC; matched-load rate alongside; "
                       "zero regression for GET/SET/MGET/MSET p1/p32 in both armed modes",
            "decomposition": "Each added mechanism alone, then bundled; include disarmed PRE/POST controls.",
            "amortization": "Count armed instruction delta, measure disarmed fixed cost and target cycles payoff; "
                            "numeric break-even IPC ratio = POST instructions / PRE instructions. "
                            "Find the first measured depth/load/mix with lower POST cycles. "
                            "An unmeasured transition is a bracket, not an invented exact threshold.",
            "collapse": "No development knobs exist yet. Hardcode measured winners or delete losers before merge; "
                        "retain only --overlap 0|1 and --reorder 0|1.",
        },
        "return": "Append raw artifact paths, exact executable digests/argv/CPU assignments, the stall attribution, "
                  "and measured rate/IPC/instructions/command/cycles/command tables to root MEASURE-RESULT. "
                  "Missing evidence is unavailable, never a passing or zero-cost result.",
    }
    with request.open("x") as stream:
        json.dump(document, stream, indent=2)
        stream.write("\n")
    return document


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pre", type=Path, default=ROOT / "build/tomokv-O7-PRE")
    parser.add_argument("--output", type=Path, default=ROOT / "build/overlap-O7/handoff")
    parser.add_argument("--request", type=Path, default=ROOT / "MEASURE-REQUEST")
    args = parser.parse_args()
    document = prepare(args.pre, args.output, args.request)
    print(json.dumps({"request": str(args.request), "pre": document["pre"],
                      "cell_counts": {name: row["count"] for name, row in document["cells"].items()}}, indent=2))
