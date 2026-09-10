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
    return {"keys": LONG_KEYS, "bytes_each": LONG_BYTES, "short_bytes": 64,
            "sampled_handler_usec": cost}


def command_stat(data, name):
    value = data.get("cmdstat_" + name.lower(), "")
    fields = dict(part.split("=", 1) for part in value.split(",") if "=" in part)
    return int(fields.get("calls", 0)), float(fields.get("usec", 0))


def require_workload_witness(cell, before, after, mode_before, mode_after):
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
            raise RuntimeError("reorder engagement counter unavailable")
        permutations = int(mode_after[field]) - int(mode_before[field])
        if permutations < 0 or (cell.reorder and permutations == 0) or (not cell.reorder and permutations):
            raise RuntimeError(f"reorder={cell.reorder} permutation witness failed: delta={permutations}")
        evidence["reorder_permuted_runs"] = permutations
    return evidence


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


def command_histogram(data, command):
    stats = data["ALL STATS"]
    matches = [value for name, value in stats.items() if name.upper() in (command, command + "S")]
    if len(matches) != 1:
        raise ValueError(f"expected one {command} command histogram")
    row = matches[0]
    histogram = decode_histogram(row["Percentile Latencies"]["Histogram log format"]["Compressed Histogram"])
    # The producer's completed Count and HDR observations are different counters:
    # saved 2026-09-10 memtier output has Count=165918660 and HDR=165919453, with
    # exactly matching producer/decoded percentiles. Preserve HDR counts for merging
    # and require at least the completed count; never scale bins to force equality.
    if sum(histogram.values()) < row["Count"] or row["Count"] <= 0:
        raise ValueError(f"{command} histogram omits completed commands")
    # Cross-check our decoder against the producer's own percentile. A 0.001 ms
    # output rounding unit is the only permitted difference, not a measurement
    # tolerance. This catches units/geometry/format drift before it changes a verdict.
    if abs(percentile(histogram, 99.9) - row["Percentile Latencies"]["p99.90"]) > 0.001001:
        raise ValueError(f"{command} HDR decoding disagrees with memtier's p99.9")
    return histogram


def merged_tail(documents):
    short, long = Counter(), Counter()
    for document in documents:
        short.update(command_histogram(document, "GET"))
        long.update(command_histogram(document, "BITCOUNT"))
    if min(sum(short.values()), sum(long.values())) < 1000:
        raise ValueError("fewer than 1000 observations in a latency class; p99.9 not resolved")
    return {"p999_ms": percentile(short, 99.9), "long_p999_ms": percentile(long, 99.9),
            "combined_p999_ms": percentile(short + long, 99.9),
            "short_count": sum(short.values()), "long_count": sum(long.values())}
