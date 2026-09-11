#!/usr/bin/env python3
"""Workload and histogram boundaries for the ABBA regression cells.

Multi-key cells issue eight independent generated keys, as in tests/matrix.sh.
Reorder cells use ordinary one-owner BITCOUNT and GET tasks: scatter MGET and
blocking commands are barriers in reorder.h and cannot establish this mechanism.
"""

import base64
from collections import Counter
import math
import struct
import statistics
import time
import zlib


MULTI_KEYS = 8
LONG_KEYS = 2048
LONG_BYTES = 256 * 1024


def workload_command_names(cell):
    return {"MIX": ("GET", "SET"), "MIX8": ("MGET", "MSET"),
            "REORDER": ("GET", "BITCOUNT")}.get(cell.op, (cell.op,))


def workload_arguments(cell):
    if cell.op in ("GET", "SET"):
        return ["--ratio=" + ("0:1" if cell.op == "GET" else "1:0")]
    if cell.op == "MIX":
        reads, writes = cell.mix.split(":")
        return [f"--ratio={writes}:{reads}"]  # cells describe READ:WRITE; memtier wants SET:GET.
    mget = "MGET " + " ".join(["__key__"] * MULTI_KEYS)
    mset = "MSET " + " ".join(["__key__ __data__"] * MULTI_KEYS)
    if cell.op in ("MGET", "MSET"):
        commands = [(mget if cell.op == "MGET" else mset, 1)]
    elif cell.op == "MIX8":
        reads, writes = map(int, cell.mix.split(":"))
        commands = [(mget, reads), (mset, writes)]
    elif cell.op == "REORDER":
        short, long = map(int, cell.mix.split(":"))
        commands = [("GET __key__", short), ("BITCOUNT blocker:__key__", long)]
    else:
        raise ValueError(f"unsupported workload {cell.op}")
    argv = []
    for command, ratio in commands:
        argv += ["--command=" + command, f"--command-ratio={ratio}", "--command-key-pattern=P"]
    if cell.op == "REORDER":
        # The short keys already exist in the two-million-key population. Only this
        # extra 512 MiB is long; populating two million long values would change the
        # experiment into a capacity test. Keep more keys than connections even at
        # the first one-instance probe so memtier's parallel key ranges are nonempty.
        argv += [f"--key-maximum={LONG_KEYS}"]
    return argv


def prepare_long_keys(conn):
    value = b"\xff" * LONG_BYTES
    for number in range(1, LONG_KEYS + 1):
        key = f"blocker:memtier-{number}"
        if conn.must("SET", key, value) != b"OK":
            raise RuntimeError("long-blocker population failed")
    for number in (1, LONG_KEYS):
        if conn.must("BITCOUNT", f"blocker:memtier-{number}") != LONG_BYTES * 8:
            raise RuntimeError("long-blocker data does not have the requested service cost")
    return {"keys": LONG_KEYS, "bytes_each": LONG_BYTES, "short_bytes": 64,
            "sampled_handler_usec": sample_long_cost(conn)}


def sample_long_cost(conn):
    # INFO COMMANDSTATS in this server counts calls but does not expose handler
    # time. Sample SLOWLOG before the benchmark, then restore its original setting
    # and clear the sample. No per-operation sampling is added to the scored run.
    original = conn.must("CONFIG", "GET", "slowlog-log-slower-than")[1]
    try:
        conn.must("CONFIG", "SET", "slowlog-log-slower-than", "0")
        time.sleep(0.2)  # live config is observed at owner-loop boundaries
        conn.must("SLOWLOG", "RESET")
        for _ in range(16):
            conn.must("GET", "memtier-1")
            conn.must("BITCOUNT", "blocker:memtier-1")
        rows = conn.must("SLOWLOG", "GET", "128")
        samples = {name: [row[2] for row in rows if len(row) >= 4 and row[3]
                         and row[3][0].upper() == name.encode()] for name in ("GET", "BITCOUNT")}
        if min(map(len, samples.values())) < 16:
            raise RuntimeError("long-blocker service-cost sample was not recorded")
        cost = {name: statistics.median(values) for name, values in samples.items()}
        if cost["BITCOUNT"] <= cost["GET"]:
            raise RuntimeError(f"BITCOUNT is not slower than GET: sampled handler microseconds {cost}")
    finally:
        conn.must("CONFIG", "SET", "slowlog-log-slower-than", original)
        conn.must("SLOWLOG", "RESET")
        time.sleep(0.2)
    return cost


def command_stat(data, name):
    value = data.get("cmdstat_" + name.lower(), "")
    fields = dict(part.split("=", 1) for part in value.split(",") if "=" in part)
    return int(fields.get("calls", 0)), float(fields.get("usec", 0))


def require_workload_witness(cell, before, after, mode_before, mode_after, legacy_control=None):
    evidence = {}
    for name in workload_command_names(cell):
        bc, bt = command_stat(before, name)
        ac, at = command_stat(after, name)
        if ac <= bc or at < bt:
            raise RuntimeError(f"{name} did not execute during the measured window")
        evidence[name] = {"calls": ac - bc}
    if cell.op == "REORDER":
        field = "reorder_permuted_runs"
        if field not in mode_before or field not in mode_after:
            # The unchanged pushed reference predates this telemetry. Its fallback must be a
            # live OFF/ON execution-order control on those exact bytes, run in this session.
            # Both workload commands still MUST progress in the scored interval above. State
            # the narrower engagement observation honestly: no inferred or synthetic counter.
            if (field in mode_before or field in mode_after or not legacy_control or
                    legacy_control.get('verdict') != 'PASS' or
                    legacy_control.get('mode') != cell.mode or
                    legacy_control.get('controls') != [0, 1]):
                raise RuntimeError("reorder engagement counter unavailable; live legacy OFF/ON control required")
            evidence['legacy_reorder_control'] = legacy_control
            return evidence
        permutations = int(mode_after[field]) - int(mode_before[field])
        if permutations < 0 or (cell.reorder and permutations == 0) or (not cell.reorder and permutations):
            raise RuntimeError(f"reorder={cell.reorder} permutation witness failed: delta={permutations}")
        evidence["reorder_permuted_runs"] = permutations
    return evidence


def memtier_workload_counts(cell, data, connections):
    """Keep reported command counts and the independent, fully drained HDR count."""
    if type(connections) is not int or connections <= 0 or cell.depth <= 0:
        raise RuntimeError("invalid generator geometry for finite outstanding-request bound")
    stats = data["ALL STATS"]
    interrupted = stats["Runtime"]["Interrupted"]
    if interrupted is not False and interrupted != "false":
        raise RuntimeError("memtier run was interrupted; no completed-work accounting")
    names = workload_command_names(cell)
    counts, observations = {}, {}
    for name in names:
        rows = [row for key, row in stats.items() if key.upper() in (name, name + "S")]
        if len(rows) != 1:
            raise RuntimeError(f"expected one memtier {name} command Count")
        row = rows[0]
        count = row["Count"]
        if type(count) is not int or count <= 0:
            raise RuntimeError(f"invalid memtier {name} command Count: {count!r}")
        counts[name] = count
        histogram = decode_histogram(row["Percentile Latencies"]["Histogram log format"]["Compressed Histogram"])
        observations[name] = sum(histogram.values())
        if observations[name] <= 0:
            raise RuntimeError(f"empty memtier {name} completed-response histogram")
    expected_rows = {name + "S" for name in names} | set(names)
    for name, row in stats.items():
        if name != "Totals" and isinstance(row, dict) and "Count" in row:
            if name.upper() not in expected_rows and row["Count"] != 0:
                raise RuntimeError(f"unexpected memtier workload command: {name}")
    if type(stats["Totals"]["Count"]) is not int or stats["Totals"]["Count"] != sum(counts.values()):
        raise RuntimeError("memtier Totals Count differs from logical workload command Counts")

    # Audited memtier 2.5.1, upstream 5f634d171b83efca9640c5a87606c47b34d3d330:
    # client::finished uses m_cur_stats.m_second. On the first callback in the final
    # second, shard_connection::process_response/fill_pipeline stop generating and
    # client::set_end_time snapshots m_cur_stats once. run_stats::merge copies those
    # snapshots, but not the later current bucket; final drain replies can be absent
    # from JSON Count. Crossing another second while draining can instead duplicate
    # that first callback's bucket. The first callback plus outstanding replies are
    # at most pipeline per connection (fill_pipeline's queue bound). Thus the SUM of
    # absolute per-command errors is bounded by connections * pipeline, not an error
    # percentage. Normal disconnect drains every reply; per-command HDR is updated
    # on each response and merged after pthread_join, so it supplies the exact count.
    # No retries, reconnection, cluster routing, transactions or staircase are enabled
    # by Runner.memtier/workload_arguments. Unknown schemas fail rather than guessing.
    bound = connections * cell.depth
    error = sum(abs(counts[name] - observations[name]) for name in names)
    if error > bound:
        raise RuntimeError(f"memtier Count/HDR discrepancy {error} exceeds finite outstanding bound {bound}")
    return {"connections": connections, "pipeline": cell.depth, "outstanding_bound": bound,
            "reported_counts": counts, "completed_hdr_counts": observations,
            "count_hdr_absolute_difference": error, "runtime": stats["Runtime"]}


def require_workload_accounting(cell, before, after, generators):
    """Compare the entire generator lifetime; observer/setup commands are excluded."""
    if sum(row["connections"] for row in generators) != cell.conns:
        raise RuntimeError("accounted generator connections differ from requested cell geometry")
    evidence = {}
    for name in workload_command_names(cell):
        start, _ = command_stat(before, name)
        finish, _ = command_stat(after, name)
        reported = sum(row["reported_counts"][name] for row in generators)
        completed = sum(row["completed_hdr_counts"][name] for row in generators)
        calls = finish - start
        evidence[name] = {"server_calls": calls, "memtier_count": reported,
                          "completed_hdr_count": completed, "server_minus_count": calls - reported}
        if calls != completed or completed <= 0:
            raise RuntimeError(f"{name} whole-run accounting mismatch: server={calls}, "
                               f"completed HDR={completed}, memtier Count={reported}")
    return {"commands": evidence, "count_outstanding_bound": sum(row["outstanding_bound"] for row in generators),
            "scope": "whole generator run, including warmup and tail; logical commands, not keys",
            "hdr_accounting": "exact; no discrepancy allowance"}


def decode_histogram(encoded):
    """Decode memtier's HDR v2 integer histogram into (upper microseconds,count).

    The wire structure is HDR Histogram v2: a big-endian compression header, zlib,
    a 40-byte geometry header, then zigzag varints with negative zero-run lengths.
    Bucket boundaries follow that public format; no percentiles are averaged.
    Unsupported encodings/normalization are rejected rather than approximated.
    """
    raw = base64.b64decode(encoded, validate=True)
    if len(raw) < 8:
        raise ValueError("truncated HDR compression header")
    cookie, length = struct.unpack(">II", raw[:8])
    if cookie & ~0xf0 != 0x1c849304 or length != len(raw) - 8:
        raise ValueError("invalid HDR v2 compression header")
    decoder = zlib.decompressobj()
    data = decoder.decompress(raw[8:], 16 * 1024 * 1024)
    if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail or len(data) < 40:
        raise ValueError("invalid or excessive HDR payload")
    cookie, size, offset, precision, low, high, conversion = struct.unpack(">IIiiQQd", data[:40])
    if (cookie & ~0xf0 != 0x1c849303 or size != len(data) - 40 or offset != 0
            or not 1 <= precision <= 5 or not 1 <= low <= high or conversion != 1.0):
        raise ValueError("unsupported HDR geometry")
    unit = low.bit_length() - 1
    half_bits = (2 * 10 ** precision - 1).bit_length() - 1
    half = 1 << half_bits
    counts, index, position = {}, 0, 40
    while position < len(data):
        unsigned = 0
        for part in range(9):
            if position == len(data):
                raise ValueError("truncated HDR count")
            byte = data[position]
            position += 1
            unsigned |= (byte if part == 8 else byte & 0x7f) << (7 * part)
            if part == 8 or byte < 128:
                break
        count = (unsigned >> 1) ^ -(unsigned & 1)
        if count < 0:
            index += -count
        else:
            bucket = (index >> half_bits) - 1
            sub = (index & (half - 1)) + half
            if bucket < 0:
                sub -= half
                bucket = 0
            upper = ((sub + 1) << (bucket + unit)) - 1
            if upper > high * 2:
                raise ValueError("HDR index exceeds histogram range")
            if count:
                counts[upper] = count
            index += 1
        if index > 10_000_000:
            raise ValueError("excessive HDR zero run")
    return counts


def percentile(histogram, percent):
    total = sum(histogram.values())
    if not total or not 0 < percent <= 100:
        raise ValueError("empty histogram or invalid percentile")
    target = max(1, math.ceil(total * percent / 100))
    running = 0
    for value, count in sorted(histogram.items()):
        running += count
        if running >= target:
            return value / 1000.0
    raise AssertionError("unreachable histogram rank")


def command_histogram(data, command, *, count_bound=0):
    stats = data["ALL STATS"]
    matches = [value for name, value in stats.items() if name.upper() in (command, command + "S")]
    if len(matches) != 1:
        raise ValueError(f"expected one {command} command histogram")
    row = matches[0]
    histogram = decode_histogram(row["Percentile Latencies"]["Histogram log format"]["Compressed Histogram"])
    # The finite Count/HDR difference is explained in memtier_workload_counts.
    # Runner supplies its audited connections*pipeline bound only AFTER exact
    # whole-run server/HDR accounting passed. Standalone callers default to exact
    # equality; neither direction gets an unbounded allowance and bins never scale.
    if count_bound < 0 or abs(sum(histogram.values()) - row["Count"]) > count_bound or row["Count"] <= 0:
        raise ValueError(f"{command} Count/HDR discrepancy exceeds outstanding bound {count_bound}")
    # Cross-check our decoder against the producer's own percentile. A 0.001 ms
    # output rounding unit is the only permitted difference, not a measurement
    # tolerance. This catches units/geometry/format drift before it changes a verdict.
    # Tolerance is ONE BUCKET WIDTH at the p99.9 value, not a fixed 0.001 ms. Two correct HDR
    # implementations can legitimately pick adjacent buckets when the 99.9th-percentile RANK lands
    # exactly on a bucket edge -- ours uses running >= target, HdrHistogram's tie-break differs.
    # Measured 2026-09-11: 3 of 328 tail-cell load files disagreed, every one by exactly +0.016 ms,
    # the bucket width at ~3.5 ms. A units or format defect would be off by orders of magnitude and
    # is still caught; a one-bucket tie-break is not drift and must not fail a cell.
    ours = percentile(histogram, 99.9)
    theirs = row["Percentile Latencies"]["p99.90"]
    edges = sorted(histogram)
    at = min((v for v in edges if v / 1000.0 >= ours), default=edges[-1])
    below = max((v for v in edges if v < at), default=0)
    bucket_width_ms = (at - below) / 1000.0
    if abs(ours - theirs) > max(0.001001, bucket_width_ms + 0.000001):
        raise ValueError(f"{command} HDR decoding disagrees with memtier's p99.9 by "
                         f"{abs(ours - theirs):.4f} ms (> one bucket, {bucket_width_ms:.4f} ms)")
    return histogram


def merged_tail(documents, *, count_bounds=None):
    short, long = Counter(), Counter()
    if count_bounds is None:
        count_bounds = [0] * len(documents)
    if len(count_bounds) != len(documents):
        raise ValueError("missing per-generator Count/HDR bound")
    for document, bound in zip(documents, count_bounds):
        short.update(command_histogram(document, "GET", count_bound=bound))
        long.update(command_histogram(document, "BITCOUNT", count_bound=bound))
    if min(sum(short.values()), sum(long.values())) < 1000:
        raise ValueError("fewer than 1000 observations in a latency class; p99.9 not resolved")
    return {"p999_ms": percentile(short, 99.9), "long_p999_ms": percentile(long, 99.9),
            "combined_p999_ms": percentile(short + long, 99.9),
            "short_count": sum(short.values()), "long_count": sum(long.values())}
