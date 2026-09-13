// reorder.h -- oldest observed arrivals first within one gathered executor batch.
// Per-connection order and special-task barriers are absolute. One-client runs retain FIFO.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "genthread_pipeline.h"
#include "thread.h"
#include "orthog.h"
#include "../net/conn.h"
#include "../cmd/command.h"

namespace tomo {

inline constexpr uint32_t kExArrivalMask = 0x7fffffffu;
inline constexpr uint32_t kExArrivalHalf = 0x40000000u;

// Ordinary Tasks resolve their shard through the Op for EVERY negative selector. Use those
// otherwise equivalent values for a parse-batch arrival, keeping -1 as "arrival unavailable".
// This replaces the existing selector store, not Task's enqueue_us_low: that independent word
// arms per-task flip EWMAs when nonzero. Filling it on every request would buy an age policy by
// introducing exactly the per-operation bookkeeping this scheduler must avoid.
//
// The parser calls this once per invocation, using its already-paid monotonic loop clock. All
// commands parsed in that pass tie. This measures observed batch arrival, not kernel receive
// time or time spent waiting for ROB space before parsing. With reorder=0 no clock is loaded
// and the original -1 selector is used. No clock, counter, or extra field is added per request.
inline int32_t ex_schedule_arrival(uint64_t cached_now_us) {
    return static_cast<int32_t>(static_cast<uint32_t>(cached_now_us) | 0x80000000u);
}

// Only the ordinary one-owner path participates. Every existing special mechanism is a hard
// barrier in the gathered sequence: eligible work on either side cannot move across it.
inline bool ex_sched_candidate(const Task& task) {
    if (!task.client || task.scatter) return false;
    const Op& op = task.client->rob().at(task.op_id);
    if (!op.spec || op.has_blocking_state()) return false;
    constexpr uint32_t kSpecial =
        CmdFlags::Admin | CmdFlags::ConnLocal | CmdFlags::AllShards | CmdFlags::RandomShard |
        CmdFlags::CursorShard | CmdFlags::ConfigRoute | CmdFlags::ScriptRoute |
        CmdFlags::PubSub | CmdFlags::Blocking | CmdFlags::Transaction |
        CmdFlags::StreamRoute | CmdFlags::SubcmdRoute | CmdFlags::FlipAsync;
    // MultiShard is deliberately absent: a same-owner MGET/MSET local-fast task is ordinary
    // here. A real scatter has task.scatter set and returned above.
    return !(op.spec->flags & kSpecial);
}

// Shared S3 instrumentation still reports the historical predictor against observed service
// times. Keep its interface, but no cost class or ROB-head rank drives the age scheduler.
inline bool ex_sched_candidate(const Task& task, uint8_t& length) {
    if (!ex_sched_candidate(task)) return false;
    const Op& op = task.client->rob().at(task.op_id);
    length = static_cast<uint8_t>(command_length_class(*op.spec));
    if (length >= static_cast<uint8_t>(CommandLengthClass::Count)) return false;
    if (op.atomic_hazard()) length = static_cast<uint8_t>(CommandLengthClass::Long);
    return true;
}

inline bool ex_age_candidate(const Task& task) {
    // A demoted local read has no original arrival in its compact lane entry; do not invent
    // one at demotion. Such tasks, explicit-shard tasks, and the one reserved timestamp per
    // clock wrap remain FIFO barriers. Forwarding copies an ordinary selector unchanged.
    return task.shard < -1 && ex_sched_candidate(task);
}

template <size_t BatchOps>
uint32_t ex_schedule_run(Task* tasks, uint32_t n) {
    if (n < 2) return 0;
    Client* const only_client = tasks[0].client;
    uint32_t distinct_at = 1;
    while (distinct_at < n && tasks[distinct_at].client == only_client) distinct_at++;
    if (distinct_at == n) return 0;

    int32_t arrivals[BatchOps];
    uint8_t order[BatchOps];
    const uint32_t reference = static_cast<uint32_t>(tasks[0].shard);
    int32_t earliest = 0, latest = 0;
    bool already_ordered = true;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t delta = (static_cast<uint32_t>(tasks[i].shard) - reference) &
                               kExArrivalMask;
        // Signed circular distance in [-2^30, 2^30), without signed overflow or a pairwise
        // modular comparator (which would not be a strict weak order around a clock wrap).
        arrivals[i] = static_cast<int32_t>(delta ^ kExArrivalHalf) -
                      static_cast<int32_t>(kExArrivalHalf);
        earliest = std::min(earliest, arrivals[i]);
        latest = std::max(latest, arrivals[i]);
        if (i) already_ordered &= arrivals[i - 1] <= arrivals[i];
        order[i] = static_cast<uint8_t>(i);
    }
    // Low-31-bit microseconds distinguish arrivals spanning less than 2^30 us (17.9 min).
    // Reject an ambiguous span. Like the existing low-word telemetry, a whole clock-period
    // alias cannot be detected here; it can affect priority, never same-connection safety.
    if (static_cast<uint32_t>(latest - earliest) >= kExArrivalHalf || already_ordered)
        return 1;

    // Sort indices so an identity order or an invalid connection chain never copies Tasks.
    // Original position breaks ties: equal arrivals retain FIFO even when their costs differ.
    // std::sort uses bounded stack scratch, with no allocator or persistent queue state.
    std::sort(order, order + n, [&](uint8_t a, uint8_t b) {
        return arrivals[a] < arrivals[b] || (arrivals[a] == arrivals[b] && a < b);
    });

    // Arrival order normally implies connection order because each connection has one parser
    // and ownership moves only while quiesced. Prove it before copying anyway: bad stamps,
    // clock aliases, or a broken gather must never let a younger own operation jump its elder.
    // Inspect only represented live Tasks; no flush_id sampling or absent ROB-slot reads.
    static constexpr uint32_t kChainSlots = BatchOps * 2;
    static constexpr uint32_t kChainWords = (kChainSlots + 63) / 64;
    Client* chain_client[kChainSlots];
    uint8_t chain_last[kChainSlots];
    uint64_t chain_occupied[kChainWords] = {};
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t index = order[i];
        Client* client = tasks[index].client;
        uint64_t hash = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(client));
        hash ^= hash >> 33;
        hash *= uint64_t{0xff51afd7ed558ccdull};
        hash ^= hash >> 33;
        uint32_t slot = static_cast<uint32_t>(hash) & (kChainSlots - 1);
        uint64_t bit = uint64_t{1} << (slot & 63);
        while ((chain_occupied[slot >> 6] & bit) && chain_client[slot] != client) {
            slot = (slot + 1) & (kChainSlots - 1);
            bit = uint64_t{1} << (slot & 63);
        }
        if (!(chain_occupied[slot >> 6] & bit)) {
            chain_occupied[slot >> 6] |= bit;
            chain_client[slot] = client;
        } else {
            const uint8_t previous = chain_last[slot];
            if (index <= previous || tasks[index].op_id <= tasks[previous].op_id) return 1;
        }
        chain_last[slot] = index;
    }

    Task ordered[BatchOps];
    for (uint32_t i = 0; i < n; i++) ordered[i] = tasks[order[i]];
    for (uint32_t i = 0; i < n; i++) tasks[i] = ordered[i];
    return 2;
}

// The boot-latched enable branch guards this call AND its stack reservation. Arrival priority
// applies only inside this gathered scope. An older long command still runs first; nothing
// preempts it, promotes a younger cheap command, or bounds waits outside the scope.
template <size_t BatchOps>
__attribute__((noinline)) ReorderResult ex_schedule_batch(Task (&tasks)[BatchOps], uint32_t n) {
    static_assert(BatchOps == kGenthreadExBatchOps ||
                  BatchOps == kGenthreadPipelineExBatchOps,
                  "audit new executor geometry before enabling reorder");
    static_assert(BatchOps <= UINT8_MAX, "scheduler indices must fit in a byte");
    static_assert((BatchOps & (BatchOps - 1)) == 0, "connection hash mask needs power of two");
    if (__builtin_expect(n > BatchOps, false)) std::abort();
    ReorderResult result;
    uint32_t begin = 0;
    while (begin < n) {
        if (!ex_age_candidate(tasks[begin])) {
            begin++;
            continue;
        }
        uint32_t end = begin + 1;
        while (end < n && ex_age_candidate(tasks[end])) end++;
        const uint32_t witness = ex_schedule_run<BatchOps>(tasks + begin, end - begin);
        result.multi_client_runs += witness != 0;
        result.permuted_runs += witness == 2;
        begin = end + (end < n);
    }
    return result;
}

}  // namespace tomo
