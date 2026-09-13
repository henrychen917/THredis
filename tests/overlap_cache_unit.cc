// Serverless correctness checks for the production cache-window primitive and ROB retirement.
// No clock, listener, worker, load generator, or perf event is started. The maintainer runs this
// alongside the existing overlap/feature/atomic batteries; it does not add a gate row.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "src/core/overlap_cache.h"
#include "src/core/orthog.h"
#include "src/core/thread.h"
#include "src/net/conn.h"

namespace {
using namespace tomo;

void require(bool condition, const char* why) {
    if (condition) return;
    std::fprintf(stderr, "overlap cache: FAIL: %s\n", why);
    std::abort();
}

void geometry() {
    // Synthetic measurements, not production defaults. Changing either cache, its line size,
    // the physical sharing, or the stage count must change the capacity-derived bound.
    for (uint32_t line : {32u, 64u, 128u}) {
        for (uint32_t l1 : {16u * 1024, 32u * 1024, 64u * 1024}) {
            for (uint32_t l2 : {8u * 1024, 256u * 1024, 1024u * 1024}) {
                uint16_t previous_ex = UINT16_MAX, previous_wb = UINT16_MAX;
                for (uint32_t stages : {1u, 2u, 3u}) {
                    OverlapCache cache{};
                    cache.l1d_bytes = l1;
                    cache.l2_bytes = l2;
                    cache.line_bytes = static_cast<uint16_t>(line);
                    const uint64_t fixed = sizeof(ThreadCtx) + sizeof(Client);
                    cache.derive(stages, fixed, sizeof(Task), 512, 128, 64);
                    const uint64_t reserve = (fixed + line - 1) / line * line;
                    const uint64_t op = (sizeof(Op) + line - 1) / line * line;
                    const uint64_t task = (sizeof(Task) + line - 1) / line * line;
                    const uint64_t ex_live = reserve + 2 * stages * cache.ex_ops *
                        (op + task + 2 * line);
                    const uint64_t wb_live = reserve + 2 * stages * cache.wb_ops * (op + 512);
                    require(ex_live <= l1 && ex_live <= l2, "executor exceeds a measured cache");
                    require(wb_live <= l1 && wb_live <= l2, "retirement exceeds a measured cache");
                    require(cache.ex_ops <= previous_ex && cache.wb_ops <= previous_wb,
                            "more simultaneous stages increased a window");
                    previous_ex = cache.ex_ops;
                    previous_wb = cache.wb_ops;
                }
            }
        }
    }
    OverlapCache absent{};
    absent.derive(3, sizeof(ThreadCtx) + sizeof(Client), sizeof(Task), 512, 128, 64);
    require(!absent.ex_ops && !absent.wb_ops, "missing measurement invented a cache size");
    OverlapCache tiny{};
    tiny.l1d_bytes = tiny.l2_bytes = 1024;
    tiny.line_bytes = 64;
    tiny.derive(3, sizeof(ThreadCtx) + sizeof(Client), sizeof(Task), 512, 128, 64);
    require(!tiny.ex_ops && !tiny.wb_ops, "tiny cache forced a window that cannot fit");
}

template <bool FirstPrefetched>
void windows() {
    uint32_t witnessed = 0;
    for (uint32_t n = 0; n <= 128; n++) {
        for (uint32_t width : {1u, 3u, 5u, 8u, 12u, 32u, 64u, 128u}) {
            for (uint32_t stop = 0; stop <= n; stop++) {
                std::vector<uint32_t> warms(n), consumed;
                if constexpr (FirstPrefetched)
                    for (uint32_t i = 0; i < std::min(n, width); i++) warms[i]++;
                uint32_t warm_frontier = std::min(n, FirstPrefetched ? width : 0u);
                auto prefetch = [&](uint32_t first, uint32_t count) {
                    require(count && count <= width && first + count <= n,
                            "prefetch outside a nonempty bounded window");
                    require(first == warm_frontier, "prefetch duplicated or omitted a window");
                    warm_frontier += count;
                    require(warm_frontier - consumed.size() <= 2 * width,
                            "prefetched more than current and next");
                    for (uint32_t i = first; i < first + count; i++) warms[i]++;
                };
                auto consume = [&](uint32_t first, uint32_t count) {
                    require(first == consumed.size(), "window moved an operation");
                    const uint32_t end = first + count;
                    require(warm_frontier == std::min(n, end + width),
                            "next window was not staged before consuming current");
                    witnessed += end < n;
                    for (uint32_t i = first; i < end; i++) {
                        require(warms[i] == 1, "op was not prefetched exactly once");
                        if (i == stop) return false;
                        consumed.push_back(i);
                    }
                    return true;
                };
                OverlapCache::windows<FirstPrefetched>(n, width, prefetch, consume);
                require(consumed.size() == stop, "consumed past a barrier or lost a suffix");
            }
        }
    }
    require(witnessed > 0, "window never opened");
    // A throwaway PF-all/consume-all replacement must fail the bounded-width and ahead-of-use
    // witnesses above, even though it returns every answer in the same order.
}

void retirement() {
    std::array<char, 512> borrowed{};
    for (uint16_t width : {uint16_t{0}, uint16_t{1}, uint16_t{3}, uint16_t{5}, uint16_t{8},
                           uint16_t{64}}) {
        OverlapCache cache{};
        cache.wb_ops = width;
        cache.line_bytes = 64;
        Rob<64> rob;
        // Each fresh generation exercises every possible hole, including window/chunk boundaries
        // and wraparound. Younger Done operations cannot retire through the Issued hole.
        for (uint32_t hole = 0; hole <= 64; hole++) {
            const uint64_t first = rob.dispatch_id();
            for (uint32_t i = 0; i < 64; i++) {
                Op* op = rob.acquire();
                require(op, "fixture failed to populate all real ROB slots");
                op->hash = first + i;
                op->zc_ptr = i % 2 ? borrowed.data() : reinterpret_cast<const char*>(uintptr_t{1});
                op->zc_len = static_cast<uint32_t>(borrowed.size());
                op->zc_shard = i % 2 ? 0 : -1;
                op->state.store(i == hole ? OpState::Issued : OpState::Done,
                                std::memory_order_release);
                rob.publish();
            }
            uint64_t expect = first;
            auto sink = [&](Op& op) {
                require(rob.flush_id() == first, "published a flush frontier between windows");
                require(op.hash == expect++, "retired replies out of order");
                require(op.state.load(std::memory_order_acquire) == OpState::Done,
                        "retired an unpublished reply");
            };
            require(OverlapCache::drain<true>(rob, sink, &cache, 512) == hole,
                    "retirement crossed the first unfinished command");
            require(rob.flush_id() == first + hole, "wrong final flush frontier");
            if (hole != 64) {
                require(rob.at(first + hole).state.load() == OpState::Issued,
                        "retirement changed the blocker");
                rob.at(first + hole).state.store(OpState::Done, std::memory_order_release);
                const uint64_t resumed = rob.flush_id();
                auto tail = [&](Op& op) {
                    require(rob.flush_id() == resumed, "tail published a partial flush frontier");
                    require(op.hash == expect++, "lost or duplicated a resumed reply");
                };
                require(OverlapCache::drain<true>(rob, tail, &cache, 512) == 64 - hole,
                        "finished blocker did not release the complete suffix");
            }
            require(rob.quiesced() && expect == first + 64, "ROB did not drain completely");
        }
    }
    // The false specialization must not dereference even an invalid optional cache address.
    Rob<64> off;
    Op* op = off.acquire();
    require(op, "off fixture did not arm");
    op->state.store(OpState::Done, std::memory_order_release);
    off.publish();
    require(OverlapCache::drain<false>(off, [](Op&) {},
        reinterpret_cast<const OverlapCache*>(uintptr_t{1}), 512) == 1,
        "off arm changed plain retirement");
}

void reporting() {
    ModeScheduleStats stats;
    // Only reorder is on. Its witnesses must survive, and INFO must not inspect the overlap
    // bytes which this boot posture deliberately leaves uninitialized. Poison them as a witness.
    std::memset(&stats.overlap_cache, 0xa5, sizeof(stats.overlap_cache));
    stats.note_reorder(32, ReorderResult{1, 1});
    std::string info;
    append_mode_schedule_info(info, &stats, 1, false);
    require(info.find("reorder_batches:1\r\n") != std::string::npos,
            "overlap off hid reorder telemetry");
    require(info.find("overlap_cache_thread_") == std::string::npos,
            "overlap off inspected its uninitialized cache bytes");
    stats.overlap_cache = {};
    stats.overlap_cache.l1d_bytes = 32 * 1024;
    stats.overlap_cache.l2_bytes = 1024 * 1024;
    stats.overlap_cache.line_bytes = 64;
    stats.overlap_cache.derive(3, sizeof(ThreadCtx) + sizeof(Client), sizeof(Task), 512, 128, 64);
    info.clear();
    append_mode_schedule_info(info, &stats, 1, true);
    require(info.find("overlap_cache_thread_0:l1d=32768,l2=1048576,line=64,stages=3,ex_ops=8,wb_ops=5\r\n")
                != std::string::npos, "armed INFO did not report the derived worker geometry");
}
} // namespace

int main() {
    geometry();
    windows<false>();
    windows<true>();
    retirement();
    reporting();
    std::puts("overlap cache: PASS (geometry, two-window witness, barriers, ROB wrap/flush, off)");
}
