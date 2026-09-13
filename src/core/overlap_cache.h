// Cache-budgeted overlap. All scheduling state is a batch-local range; an Op never acquires a
// stage tag, counter, pointer, or retry. The ordinary 32-task executor burst is deliberately left
// alone: a sliding window at that geometry already measured null. The unexplored burst is 128.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sched.h>
#include <type_traits>
#include "../exec/op.h"

namespace tomo {

template <uint32_t Capacity> class Rob;

struct OverlapCache {
    // Immutable after Server::init, before workers/listeners start. These sixteen bytes occupy
    // ModeScheduleStats' existing padding; overlap=0 has no discovery or additional allocation.
    // Leave that padding uninitialized when only reorder is on: even boot-time initialization of
    // overlap metadata belongs behind overlap's latch. Standalone values use {} before discovery.
    uint32_t l1d_bytes;  // this worker's share, not the capacity duplicated for each SMT sibling
    uint32_t l2_bytes;
    uint16_t line_bytes;
    uint16_t ex_ops;
    uint16_t wb_ops;
    uint16_t stages;

    static uint64_t round_lines(uint64_t bytes, uint32_t line) {
        return (bytes + line - 1) / line * line;
    }

    // Capacity is measured, footprint is a conservative source-derived model, residency is a
    // hypothesis to test. Reserve the thread/client headers, then divide the remaining cache among
    // simultaneously live stages and TWO windows (current and next). Count complete Ops even
    // though a prefetch walk touches fewer lines. EX includes the Task and both possible hash-table
    // lines during rehash; WB includes the existing bounded borrowed-payload hint. Large commands
    // can touch more memory than this model; no claim about their full working set follows from it.
    void derive(uint32_t live_stages, uint64_t fixed_bytes, uint32_t task_bytes,
                uint32_t borrow_bytes, uint32_t ex_cap, uint32_t wb_cap) {
        stages = static_cast<uint16_t>(live_stages);
        ex_ops = wb_ops = 0;
        if (!line_bytes || !live_stages) return;
        const uint64_t capacity = std::min(l1d_bytes, l2_bytes);
        const uint64_t fixed = round_lines(fixed_bytes, line_bytes);
        if (capacity <= fixed) return;
        const uint64_t budget = (capacity - fixed) / (2 * live_stages);
        const uint64_t op = round_lines(sizeof(Op), line_bytes);
        ex_ops = static_cast<uint16_t>(std::min<uint64_t>(
            ex_cap, budget / (op + round_lines(task_bytes, line_bytes) + 2 * line_bytes)));
        wb_ops = static_cast<uint16_t>(std::min<uint64_t>(
            wb_cap, budget / (op + round_lines(borrow_bytes, line_bytes))));
    }

    // Called only behind the boot overlap latch. Linux enumerates caches by index, not by level;
    // never assume that index0/index2 identify L1d/L2. Account for every selected logical CPU that
    // shares each cache. Unpinned workers conservatively pass the entire allowed affinity mask.
    static bool discover(int cpu, const cpu_set_t& workers, OverlapCache& out) {
        out = {};
        char root[160];
        std::snprintf(root, sizeof(root), "/sys/devices/system/cpu/cpu%d/cache", cpu);
        DIR* dir = ::opendir(root);
        if (!dir) return false;
        uint32_t l1_line = 0, l2_line = 0;
        while (dirent* entry = ::readdir(dir)) {
            if (std::strncmp(entry->d_name, "index", 5) != 0) continue;
            char path[512], type[32], shared[4096];
            auto read = [&](const char* field, char* buffer, size_t size) {
                std::snprintf(path, sizeof(path), "%s/%s/%s", root, entry->d_name, field);
                FILE* f = std::fopen(path, "r");
                if (!f) return false;
                const bool ok = std::fgets(buffer, static_cast<int>(size), f) != nullptr;
                std::fclose(f);
                return ok;
            };
            char level_text[32], size_text[64], line_text[32];
            if (!read("level", level_text, sizeof(level_text)) ||
                !read("type", type, sizeof(type))) continue;
            const unsigned level = static_cast<unsigned>(std::strtoul(level_text, nullptr, 10));
            if ((level != 1 && level != 2) ||
                (std::strcmp(type, "Data\n") && std::strcmp(type, "Unified\n"))) continue;
            if (!read("size", size_text, sizeof(size_text)) ||
                !read("coherency_line_size", line_text, sizeof(line_text)) ||
                !read("shared_cpu_list", shared, sizeof(shared))) continue;
            char* end = nullptr;
            uint64_t bytes = std::strtoull(size_text, &end, 10);
            if (*end == 'K') bytes *= 1024;
            else if (*end == 'M') bytes *= 1024 * 1024;
            else if (*end != '\n' && *end != '\0') continue;
            const uint64_t line = std::strtoull(line_text, nullptr, 10);
            uint32_t peers = 0;
            const char* p = shared;
            bool valid = true, contains_cpu = false;
            while (*p && *p != '\n') {
                const long first = std::strtol(p, &end, 10);
                if (end == p) { valid = false; break; }
                long last = first;
                if (*end == '-') {
                    p = end + 1;
                    last = std::strtol(p, &end, 10);
                    if (end == p) { valid = false; break; }
                }
                if (first < 0 || last < first || last >= CPU_SETSIZE ||
                    (*end && *end != '\n' && *end != ',')) { valid = false; break; }
                for (long peer = first; peer <= last; peer++) {
                    peers += CPU_ISSET(peer, &workers) != 0;
                    contains_cpu |= peer == cpu;
                }
                p = *end == ',' ? end + 1 : end;
            }
            if (!valid || !contains_cpu || !peers || !bytes || bytes > UINT32_MAX ||
                !line || line > UINT16_MAX || (line & (line - 1)) || bytes % line) continue;
            const uint32_t share = static_cast<uint32_t>(bytes / peers / line * line);
            if (level == 1) { out.l1d_bytes = share; l1_line = static_cast<uint32_t>(line); }
            else { out.l2_bytes = share; l2_line = static_cast<uint32_t>(line); }
        }
        ::closedir(dir);
        if (!out.l1d_bytes || !out.l2_bytes || !l1_line || l1_line != l2_line) return false;
        out.line_bytes = static_cast<uint16_t>(l1_line);
        return true;
    }

    // One range decision per window. Prefetch next before consuming current; no distance test,
    // modulo, bookkeeping store, or indirect call is inserted in the operation loop. Consume can
    // stop at an existing ordering barrier; neither a later window nor filler executes past it.
    template <bool FirstPrefetched = false, typename Prefetch, typename Consume>
    static void windows(uint32_t n, uint32_t width, Prefetch&& prefetch, Consume&& consume) {
        if (!n) return;
        if (!width) width = n;
        uint32_t begin = 0;
        uint32_t end = std::min(n, width);
        if constexpr (!FirstPrefetched) prefetch(begin, end);
        for (;;) {
            const uint32_t next = end + std::min(n - end, width);
            if (next != end) prefetch(end, next - end);
            // One copy of the consumer body, including the short final window. Duplicating it for
            // a separate epilogue inflated each armed WB instantiation with another retire loop.
            if (!consume(begin, end - begin) || end == n) return;
            begin = end;
            end = next;
        }
    }

    template <uint32_t Capacity>
    static void prefetch_wb(Rob<Capacity>& rob, uint64_t first, uint32_t n,
                            uint32_t line, uint32_t borrow_bytes) {
        for (uint32_t off = 0; off < n; off++)
            __builtin_prefetch(&rob.at(first + off).state, 0, 3);
        for (uint32_t off = 0; off < n; off++) {
            Op& op = rob.at(first + off);
            if (op.state.load(std::memory_order_acquire) != OpState::Done) break;
            // Negative zc_shard tags name command/notification state, not a borrowed payload.
            // Acquire Done before reading any reply descriptor; retain no payload pointer.
            if (!op.zc_ptr || op.zc_shard < 0 || !op.zc_len) continue;
            const uint32_t bytes = std::min(op.zc_len, borrow_bytes);
            for (uint32_t pos = 0; pos < bytes; pos += line)
                __builtin_prefetch(op.zc_ptr + pos, 0, 1);
        }
    }

    // This is Rob::drain's lifetime, including ONE flush publication at the end. Splitting it into
    // repeated public drain calls would publish/reclaim between windows and change OOB frontiers.
    // Only the loop limit changes. A not-Done head stops retirement exactly as in the plain drain.
    // Off specializations inline to the original drain and never inspect cache state.
    template <bool Armed, uint32_t Capacity, typename Sink>
    static uint32_t drain(Rob<Capacity>& rob, Sink&& sink, const OverlapCache* cache,
                          uint32_t borrow_bytes) {
        if constexpr (!Armed) {
            return rob.drain(sink);
        } else {
            if (!cache || !cache->wb_ops) return rob.drain(sink);
            const uint32_t width = cache->wb_ops;
            const uint32_t line = cache->line_bytes;
            const uint64_t d = rob.dispatch_.load(std::memory_order_acquire);
            const uint64_t first = rob.flush_.load(std::memory_order_relaxed);
            if (d - first <= width) return rob.drain(sink);
            uint64_t f = first;
            auto prefetch = [&](uint32_t begin, uint32_t count) {
                prefetch_wb(rob, first + begin, count, line, borrow_bytes);
            };
            auto consume = [&](uint32_t begin, uint32_t count) {
                const uint64_t end = first + begin + count;
                while (f != end) {
                    Op& op = rob.at(f);
                    if (op.state.load(std::memory_order_acquire) != OpState::Done) return false;
                    sink(op);
                    if (op.oversized()) op.shrink();
                    op.state.store(OpState::Free, std::memory_order_relaxed);
                    f++;
                }
                return true;
            };
            windows(static_cast<uint32_t>(d - first), width, prefetch, consume);
            if (f != first) rob.flush_.store(f, std::memory_order_release);
            return static_cast<uint32_t>(f - first);
        }
    }
};
static_assert(sizeof(OverlapCache) == 16);
static_assert(std::is_trivially_default_constructible_v<OverlapCache>);

} // namespace tomo
