#!/usr/bin/env python3
"""Prepare an unscored S3 instrument; never build, boot, or drive a server.

The generated copy times execution attempts by their original gathered position,
including barriers and work outside the reorder point. Local E0 attempts have a
separate population: their time excludes gather/prefetch and reply publication.
Neither population is handler-only time or end-to-end latency. Retries can name
one logical command more than once. No key/value contents are recorded.

All hooks live in a disposable build/ copy, including on reorder=0. Production
has no probe switch, storage, clocks, or per-operation observer. Use the same
instrument for both reorder forks and both modes; instrumented rates are invalid
performance evidence. The capture marker excludes setup without changing config.
"""

import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import time


ROOT = Path(__file__).resolve().parents[1]
DEFAULT = ROOT / "build" / "reorder-scope"

PROBE = r'''#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <sys/syscall.h>
#include <unistd.h>
#include "reorder.h"
#include "config.h"

namespace tomo::reorder_scope {
inline constexpr const char* directory = @DIRECTORY@;
inline uint64_t clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline unsigned log_bin(uint64_t value) {
    return value ? 63u - static_cast<unsigned>(__builtin_clzll(value)) : 0;
}
struct Sample { uint64_t count = 0, ns = 0, max_ns = 0; };
struct Recorder {
    uint64_t window = 0;
    std::map<std::string, Sample> rows;
    std::set<uint64_t> captures;
    std::string prefix() const {
        return std::string(directory) + "/observations/" +
            std::to_string(getpid()) + "-" + std::to_string(syscall(SYS_gettid));
    }
    void mark(const char* suffix, uint64_t capture) {
        FILE* out = std::fopen((prefix() + suffix).c_str(), "a");
        if (!out) std::abort();
        if (std::fprintf(out, "%llu\n", static_cast<unsigned long long>(capture)) < 0)
            std::abort();
        if (std::fclose(out)) std::abort();
    }
    void flush() {
        if (rows.empty()) return;
        const auto pid = static_cast<unsigned long>(getpid());
        const auto tid = static_cast<unsigned long>(syscall(SYS_gettid));
        const std::string path = prefix() + ".tsv";
        FILE* out = std::fopen(path.c_str(), "a");
        if (!out) std::abort();
        if (std::ftell(out) == 0)
            std::fputs("window\tpid\ttid\tmode\trl\tatomic\tkey_lb\tclient_lb\treorder\toverlap\t"
                       "path\tcommand\tpredicted\teffective\trank\tposition\tbatch\t"
                       "later_short\targc_log2\targv_bytes_log2\tlocal_fallback\t"
                       "ns_log2\tcount\tns_total\tns_max\n", out);
        for (const auto& [key, sample] : rows)
            if (std::fprintf(out, "%llu\t%lu\t%lu\t%s\t%llu\t%llu\t%llu\n",
                    static_cast<unsigned long long>(window), pid, tid, key.c_str(),
                    static_cast<unsigned long long>(sample.count),
                    static_cast<unsigned long long>(sample.ns),
                    static_cast<unsigned long long>(sample.max_ns)) < 0) std::abort();
        if (std::fclose(out)) std::abort();
        rows.clear();
    }
    void checkpoint() {
        const std::string marker = std::string(directory) + "/capture";
        FILE* in = std::fopen(marker.c_str(), "r");
        unsigned long long next = 0;
        if (in) {
            if (std::fscanf(in, "%llu", &next) != 1 || !next) std::abort();
            std::fclose(in);
        }
        if (window != next) {
            flush(); window = next;
            if (window && captures.insert(window).second) mark(".opened", window);
        }
    }
    ~Recorder() {
        flush();
        for (uint64_t capture : captures) mark(".closed", capture);
    }
};
inline thread_local Recorder recorder;
inline thread_local unsigned execute_depth = 0;
inline thread_local const char* outside_path = "outside";
struct Outside {
    const char* previous = outside_path;
    explicit Outside(const char* name) { recorder.checkpoint(); outside_path = name; }
    ~Outside() { outside_path = previous; }
};
struct Entry {
    Task task;
    int rank = -1;
    uint8_t length = 0;
    bool eligible = false, later_short = false;
};
struct Batch;
inline thread_local Batch* current_batch = nullptr;
struct Batch {
    Batch* previous = current_batch;
    Entry entries[kGenthreadPipelineExBatchOps];
    uint32_t count = 0;
    Batch(const Task* tasks, uint32_t n) {
        recorder.checkpoint();
        current_batch = this;
        if (!recorder.window) return;
        if (n > kGenthreadPipelineExBatchOps) std::abort();
        count = n;
        // Capture only represented live slots BEFORE any Done publication. In particular,
        // never dereference an absent predecessor or consult these slots at timer teardown.
        for (uint32_t i = n; i-- > 0;) {
            auto& e = entries[i];
            e.task = tasks[i];
            e.eligible = ex_sched_candidate(e.task, e.length);
            if (e.task.client) {
                const auto rank = e.task.op_id - e.task.client->rob().flush_id();
                e.rank = rank < kRobWindow ? static_cast<int>(rank) : -1;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            auto& e = entries[i];
            if (!e.eligible) continue;
            for (uint32_t j = i + 1; j < n && entries[j].eligible; j++)
                e.later_short |= entries[j].task.client != e.task.client &&
                                 entries[j].length < e.length;
        }
    }
    ~Batch() { current_batch = previous; }
};
inline std::string metadata(const Config& cfg, const Op* op, const char* path,
                            int effective, int rank, int position, int batch, bool later) {
    uint64_t bytes = 0;
    if (op) for (uint32_t i = 0; i < op->argc(); i++) bytes += op->arg(i).n;
    char row[384];
    const int n = std::snprintf(row, sizeof(row),
        "%s\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%u\t%u",
        cfg.thread_mode == ThreadMode::Fused ? "1s" : "2s", cfg.read_local,
        cfg.atomic, cfg.key_lb, cfg.client_lb, cfg.reorder, cfg.overlap, path,
        op && op->spec ? op->spec->name : "<internal>",
        op && op->spec ? static_cast<int>(op->spec->length_class) : -1,
        effective, rank, position, batch, later,
        log_bin(op ? op->argc() : 0), log_bin(bytes));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
    return std::string(row, static_cast<size_t>(n));
}
struct Timer {
    std::string key;
    uint64_t started = 0;
    void start(std::string value) { key = std::move(value); started = clock_ns(); }
    void finish(int fallback = -1) {
        if (!started) return;
        const uint64_t elapsed = clock_ns() - started;
        key += '\t' + std::to_string(fallback) + '\t' + std::to_string(log_bin(elapsed));
        auto& sample = recorder.rows[key];
        sample.count++;
        sample.ns += elapsed;
        sample.max_ns = std::max(sample.max_ns, elapsed);
    }
};
struct Execution : Timer {
    Execution(const Config& cfg, const Task& task) {
        if (execute_depth++) return; // The no-eviction recursion is the same attempt.
        if (!current_batch) recorder.checkpoint();
        if (!recorder.window) return;
        const Entry* found = nullptr;
        int position = -1;
        if (current_batch) for (uint32_t i = 0; i < current_batch->count; i++) {
            const Entry& e = current_batch->entries[i];
            if (e.task.client == task.client && e.task.op_id == task.op_id &&
                e.task.scatter == task.scatter && e.task.shard == task.shard) {
                found = &e; position = static_cast<int>(i); break;
            }
        }
        const Op* op = task.client ? &task.client->rob().at(task.op_id) : nullptr;
        const char* path = found ? (found->eligible ? "eligible" : "barrier") : outside_path;
        start(metadata(cfg, op, path, found && found->eligible ? found->length : -1,
                       found ? found->rank : -1, position,
                       found ? current_batch->count : 0, found && found->later_short));
    }
    ~Execution() { finish(); execute_depth--; }
};
struct Local : Timer {
    const ReadLocalFallbackReason& fallback;
    Local(const Config& cfg, const Op& op, const ReadLocalFallbackReason& reason)
        : fallback(reason) {
        if (recorder.window) start(metadata(cfg, &op, "local-e0", -1, -1, -1, 0, false));
    }
    ~Local() { finish(static_cast<int>(fallback)); }
};
} // namespace tomo::reorder_scope
'''


def replace_once(source, anchor, replacement):
    if source.count(anchor) != 1:
        raise ValueError(f"scope anchor changed ({source.count(anchor)} matches): {anchor!r}")
    return source.replace(anchor, replacement, 1)


def instrument(source):
    source = replace_once(source, '#include "reorder.h"',
                          '#include "reorder.h"\n#include "reorder_scope_probe.h"')
    for anchor, hook in (
        ("    void exec_batch(Task (&batch)[BatchOps], uint32_t n) {",
         "        reorder_scope::Batch scope_batch(batch, n);"),
        ("            if (!filler_used && xshard_retries_.empty()) {",
         "                reorder_scope::Batch scope_batch(batch, held);"),
        ("    bool execute(const Task& t) {",
         "        reorder_scope::Execution scope_execute(srv_->cfg(), t);"),
        ("            if (!count) std::abort();\n            consumed += count;",
         "            reorder_scope::recorder.checkpoint();"),
        ("                const bool mget = ((mget_mask >> i) & 1u) != 0;",
         "                reorder_scope::Local scope_local(srv_->cfg(), op, chunk.fallbacks[i]);"),
    ):
        source = replace_once(source, anchor, anchor + "\n" + hook)
    for name in ("xshard_retries", "multi_retries", "atomic_deferred", "ordered_deferred", "stale_forwards"):
        anchor = f"    uint32_t service_{name}() {{"
        source = replace_once(source, anchor, anchor +
                              f'\n        reorder_scope::Outside scope_path("{name}");')
    return source


def destination_path(path):
    path = path.resolve()
    if not path.is_relative_to(ROOT / "build") or path == ROOT / "build":
        raise ValueError("diagnostic copies must be beneath this worktree's build/ directory")
    return path


def prepare(path, revision=None):
    path = destination_path(path)
    if path.exists():
        raise ValueError(f"destination already exists: {path}")
    # Validate the injection sites before writing a partial copy.
    if revision:
        revision = subprocess.check_output(
            ["git", "rev-parse", "--verify", revision + "^{commit}"], cwd=ROOT, text=True).strip()
        original = subprocess.check_output(
            ["git", "show", revision + ":src/core/ex_loop.h"], cwd=ROOT, text=True)
    else:
        original = (ROOT / "src/core/ex_loop.h").read_text()
        if "    uint32_t drain_tasks_reordered(" in original:
            raise ValueError("R7 queues span gathers: this shared probe measures the PRE gather "
                             "scope; prepare it with --ref before drawing a scope conclusion")
    patched = instrument(original)
    path.mkdir(parents=True)
    if revision:
        archive = subprocess.check_output(
            ["git", "archive", revision, "src", "third_party", "Makefile"], cwd=ROOT)
        with tarfile.open(fileobj=io.BytesIO(archive)) as entries:
            entries.extractall(path, filter="data")
    else:
        for name in ("src", "third_party"):
            shutil.copytree(ROOT / name, path / name)
        shutil.copy2(ROOT / "Makefile", path / "Makefile")
    digest = hashlib.sha256()
    for item in sorted(path.rglob("*")):
        if item.is_file():
            digest.update(str(item.relative_to(path)).encode() + b"\0" + item.read_bytes())
    (path / "src/core/ex_loop.h").write_text(patched)
    (path / "src/core/reorder_scope_probe.h").write_text(
        PROBE.replace("@DIRECTORY@", json.dumps(str(path))))
    (path / "observations").mkdir()
    manifest = {"source_ref": revision, "source_sha256": digest.hexdigest(),
                "instrument_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "scored": False, "directory": str(path)}
    (path / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.copy2(Path(__file__), path / "instrument.py")
    return manifest


def start(path):
    path = destination_path(path)
    if not (path / "manifest.json").is_file() or (path / "capture").exists():
        raise ValueError("prepare a diagnostic copy and close any previous capture first")
    window = time.time_ns()
    staging = path / "capture.next"
    staging.write_text(str(window) + "\n")
    staging.replace(path / "capture")
    return window


def read_rows(path, window):
    rows = []
    opened = []
    for name in sorted((path / "observations").glob("*.opened")):
        if str(window) not in name.read_text().splitlines():
            continue
        closed = name.with_suffix(".closed")
        if not closed.is_file() or str(window) not in closed.read_text().splitlines():
            raise ValueError(f"capture thread did not close cleanly: {name.stem}")
        opened.append(name.stem)
    for name in sorted((path / "observations").glob("*.tsv")):
        with name.open() as handle:
            for row in csv.DictReader(handle, delimiter="\t"):
                if int(row["window"]) != window:
                    continue
                if name.stem not in opened:
                    raise ValueError(f"observation without a completed capture: {name.stem}")
                for key in row.keys() - {"path", "command", "mode"}:
                    row[key] = int(row[key])
                if row["count"] <= 0 or row["ns_total"] < 0 or row["ns_max"] < 0:
                    raise ValueError("invalid scope observation")
                rows.append(row)
    if not rows:
        raise ValueError("capture has no observations; stop the capture and gracefully stop its server")
    if len({row["pid"] for row in rows}) != 1:
        raise ValueError("multiple server processes entered one capture; use separate diagnostic copies")
    return rows


def summarize(rows):
    # Derive a descriptive long-attempt threshold separately for each venue. The
    # upper endpoint of GET's median log2 bucket includes clock overhead; 8x is
    # a reported classification convention, never a new scheduling predictor.
    groups = {}
    for row in rows:
        identity = tuple(row[k] for k in ("mode", "rl", "atomic", "key_lb", "client_lb", "reorder", "overlap"))
        groups.setdefault(identity, []).append(row)
    result = []
    for identity, population in sorted(groups.items()):
        for local in (False, True):
            venue = [row for row in population if (row["path"] == "local-e0") == local]
            if not venue:
                continue
            gets = sorted((r for r in venue if r["command"] == "GET" and
                           (not local or r["local_fallback"] == 0)), key=lambda r: r["ns_log2"])
            total = sum(r["count"] for r in gets)
            threshold, cumulative = None, 0
            for row in gets:
                cumulative += row["count"]
                if cumulative * 2 >= total:
                    threshold = 1 << (row["ns_log2"] + 4)
                    break
            summary = {}
            for row in venue:
                key = (row["path"], row["command"], row["predicted"])
                item = summary.setdefault(key, dict(path=key[0], command=key[1], predicted=key[2],
                    attempts=0, ns=0, long_attempts=0, later_short=0, non_head=0))
                item["attempts"] += row["count"]
                item["ns"] += row["ns_total"]
                if threshold is not None and (1 << row["ns_log2"]) >= threshold:
                    item["long_attempts"] += row["count"]
                    item["later_short"] += row["count"] if row["later_short"] else 0
                    item["non_head"] += row["count"] if row["rank"] > 0 else 0
            if threshold is None:
                for item in summary.values():
                    for key in ("long_attempts", "later_short", "non_head"):
                        item[key] = None
            result.append(dict(geometry=identity, venue="local-e0" if local else "executor",
                long_threshold_ns=threshold, short_reference_attempts=total,
                classification="unavailable without same-venue GET samples" if threshold is None else
                    "8x upper endpoint of median GET bucket; elapsed attempt time, not handler time",
                rows=list(summary.values())))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "start", "stop", "report"))
    parser.add_argument("--directory", type=Path, default=DEFAULT)
    parser.add_argument("--ref", help="prepare the exact committed PRE source instead of current files")
    parser.add_argument("--window", type=int, help="capture id printed by start; required for report")
    args = parser.parse_args()
    path = destination_path(args.directory)
    if args.action == "prepare":
        result = prepare(path, args.ref)
    elif args.action == "start":
        result = {"window": start(path)}
    elif args.action == "stop":
        (path / "capture").unlink()
        result = {"capture": "closed; gracefully stop the server to flush idle threads"}
    else:
        if not args.window or (path / "capture").exists():
            parser.error("report requires --window and a closed capture")
        rows = read_rows(path, args.window)
        result = {"window": args.window, "scored": False, "summary": summarize(rows), "rows": rows}
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
