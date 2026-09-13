// Orthogonal schedule witnesses. Optional storage, no request-level hooks when disabled.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "overlap_cache.h"

namespace tomo {

struct ReorderResult {
    uint32_t multi_client_runs = 0;
    uint32_t permuted_runs = 0;
};

enum class OverlapSchedule : uint32_t { None, SplitIo, Fused };

struct alignas(64) ModeScheduleStats {
    std::atomic<uint64_t> overlap_passes{0};
    std::atomic<uint64_t> overlap_interleaved_passes{0};
    std::atomic<uint64_t> reorder_batches{0};
    std::atomic<uint64_t> reorder_multi_client_runs{0};
    std::atomic<uint64_t> reorder_permuted_runs{0};
    std::atomic<uint32_t> reorder_max_batch{0};
    std::atomic<OverlapSchedule> overlap_schedule{OverlapSchedule::None};
    OverlapCache overlap_cache;

    // Each element has one physical-thread writer for its entire lifetime, including FLIP.
    // INFO reads atomically; no locked RMW and no changes to the shared ThreadCtx cache lines.
    static void add(std::atomic<uint64_t>& counter, uint64_t n = 1) {
        counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    void note_overlap(OverlapSchedule schedule, bool interleaved) {
        overlap_schedule.store(schedule, std::memory_order_relaxed);
        add(overlap_passes);
        if (interleaved) add(overlap_interleaved_passes);
    }
    void note_reorder(uint32_t n, ReorderResult result) {
        add(reorder_batches);
        add(reorder_multi_client_runs, result.multi_client_runs);
        add(reorder_permuted_runs, result.permuted_runs);
        reorder_max_batch.store(std::max(n, reorder_max_batch.load(std::memory_order_relaxed)),
                                std::memory_order_relaxed);
    }
};
static_assert(sizeof(ModeScheduleStats) == 64);

__attribute__((noinline, cold))
inline void append_mode_schedule_info(std::string& body, const ModeScheduleStats* stats,
                                     uint32_t nthreads, bool overlap_enabled) {
    uint64_t passes = 0, interleaved = 0, batches = 0, multi = 0, permutations = 0;
    uint32_t max_batch = 0, schedules = 0;
    if (stats) for (uint32_t tid = 0; tid < nthreads; tid++) {
        const auto& s = stats[tid];
        passes += s.overlap_passes.load(std::memory_order_relaxed);
        interleaved += s.overlap_interleaved_passes.load(std::memory_order_relaxed);
        batches += s.reorder_batches.load(std::memory_order_relaxed);
        multi += s.reorder_multi_client_runs.load(std::memory_order_relaxed);
        permutations += s.reorder_permuted_runs.load(std::memory_order_relaxed);
        max_batch = std::max(max_batch, s.reorder_max_batch.load(std::memory_order_relaxed));
        schedules |= 1u << static_cast<uint32_t>(s.overlap_schedule.load(std::memory_order_relaxed));
    }
    schedules &= ~1u;
    const char* schedule = schedules == 2 ? "split-io-overlap"
        : schedules == 4 ? "fused-overlap" : schedules == 0 ? "plain" : "mixed";
    char row[512];
    const int n = std::snprintf(row, sizeof(row),
        "schedule_stats_threads:%u\r\noverlap_schedule:%s\r\noverlap_passes:%llu\r\n"
        "overlap_interleaved_passes:%llu\r\nreorder_batches:%llu\r\n"
        "reorder_multi_client_runs:%llu\r\nreorder_permuted_runs:%llu\r\nreorder_max_batch:%u\r\n",
        stats ? nthreads : 0, schedule,
        static_cast<unsigned long long>(passes), static_cast<unsigned long long>(interleaved),
        static_cast<unsigned long long>(batches), static_cast<unsigned long long>(multi),
        static_cast<unsigned long long>(permutations), max_batch);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
    body.append(row, static_cast<size_t>(n));
    if (overlap_enabled && stats) for (uint32_t tid = 0; tid < nthreads; tid++) {
        const OverlapCache& cache = stats[tid].overlap_cache;
        if (!cache.line_bytes) continue;
        const int bytes = std::snprintf(row, sizeof(row),
            "overlap_cache_thread_%u:l1d=%u,l2=%u,line=%u,stages=%u,ex_ops=%u,wb_ops=%u\r\n",
            tid, cache.l1d_bytes, cache.l2_bytes, cache.line_bytes, cache.stages,
            cache.ex_ops, cache.wb_ops);
        if (bytes < 0 || static_cast<size_t>(bytes) >= sizeof(row)) std::abort();
        body.append(row, static_cast<size_t>(bytes));
    }
}

} // namespace tomo
