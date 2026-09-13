// reorder.h -- latency scheduling across connections within one gathered executor batch.
// Per-connection order and special-task barriers are absolute. One-client runs retain FIFO.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "genthread_pipeline.h"
#include "thread.h"
#include "../net/conn.h"
#include "../cmd/command.h"

namespace tomo {

inline constexpr uint32_t kExSchedClasses = 3;
inline constexpr uint8_t kExSchedUnknown = kExSchedClasses - 1;
inline constexpr uint32_t kExSchedBuckets = kRobWindow * kExSchedClasses;
inline constexpr uint32_t kExSchedBucketWords = (kExSchedBuckets + 63) / 64;
static_assert(kExSchedBuckets == 192);

struct ReorderResult {
    uint32_t multi_client_runs = 0;
    uint32_t permuted_runs = 0;
};

// R4 learns executor service, not queueing time. The table belongs to a physical worker, never
// to a shard: moving shards does not transfer ownership of any entry or name another worker's
// storage. Registry shadow rows share the same dense id. Local-lane reads, scatter fragments,
// parked tasks and stale forwards are deliberately not training observations for this venue.
//
// Only the existing reorder=1 schedule sidecar owns this object. Ordinary scheduling reads one
// cached class per candidate; EWMA updates, scans and clocks happen in one selected window.
class ExReorderCosts {
public:
    static constexpr uint32_t kCommands = 256; // registry reserves at most 255 ACL command ids
    static constexpr uint32_t kWindowMs = 10; // same observation cadence as LB's byte scan
    static constexpr uint32_t kEwmaWeight = 8;

    explicit ExReorderCosts(uint32_t worker = 0)
        : next_id_(worker % kCommands), random_(worker + 1) {
        classes_.fill(kExSchedUnknown);
    }

    uint8_t class_for(uint16_t id) const { return classes_[id]; }
    uint64_t estimate_ns(uint16_t id) const { return service_ns_[id]; }

    void observe(uint16_t id, uint64_t elapsed_ns) {
        if (id >= kCommands || !elapsed_ns) return;
        uint64_t& mean = service_ns_[id];
        // Difference form cannot overflow even for UINT64_MAX samples; round towards the
        // observation so a small, persistent change does not get stuck below one EWMA step.
        if (!mean) mean = elapsed_ns;
        else if (elapsed_ns > mean) {
            const uint64_t difference = elapsed_ns - mean;
            mean += difference / kEwmaWeight + (difference % kEwmaWeight != 0);
        } else {
            const uint64_t difference = mean - elapsed_ns;
            mean -= difference / kEwmaWeight + (difference % kEwmaWeight != 0);
        }

        // Equal logarithmic bands over the observed range derive both boundaries from this
        // worker's costs. No verb list, fixed nanosecond threshold or armed-write promotion.
        // A factor-of-two bin absorbs sub-bin variation; one observed bin is homogeneous.
        uint32_t low = 64, high = 0;
        for (uint64_t ns : service_ns_) if (ns) {
            const uint32_t bin = std::bit_width(ns) - 1;
            low = std::min(low, bin);
            high = std::max(high, bin);
        }
        const uint32_t span = high - low + 1;
        for (uint32_t command = 0; command < kCommands; command++) {
            const uint64_t ns = service_ns_[command];
            classes_[command] = ns
                ? (std::bit_width(ns) - 1 - low) * kExSchedClasses / span
                : kExSchedUnknown;
        }
    }

    // This gate uses the pass's already-read realtime milliseconds. Unsigned subtraction
    // tolerates wrap/backward adjustments, and reserving the window even on an empty/barrier
    // batch bounds failed sampling attempts too. There is no per-operation countdown.
    bool open_window(uint32_t now_ms) {
        if (window_seen_ && uint32_t(now_ms - last_window_ms_) < kWindowMs) return false;
        window_seen_ = true;
        last_window_ms_ = now_ms;
        return true;
    }

    uint32_t sample_index(const Task* tasks, uint32_t n, uint32_t now_ms);

private:
    std::array<uint8_t, kCommands> classes_;
    std::array<uint64_t, kCommands> service_ns_{};
    uint32_t last_window_ms_ = 0;
    uint32_t next_id_ = 0;
    uint32_t random_;
    bool window_seen_ = false;
};

// Only the ordinary one-owner path participates. Every existing special mechanism is a hard
// barrier in the gathered sequence: eligible work on either side cannot move across it.
inline bool ex_sched_candidate(const Task& task, uint8_t& length,
                               const ExReorderCosts& costs) {
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
    if (op.spec->flags & kSpecial) return false;
    if (__builtin_expect(op.spec->id >= ExReorderCosts::kCommands, false)) return false;
    length = costs.class_for(op.spec->id);
    // There is no O(1) class pointer from an op to an exact parked atomic predecessor. The
    // immutable publish-time hazard bit says an older own atomic group existed; Long is the
    // safe upper bound without a deque scan or persistent scheduler state.
    if (op.atomic_hazard()) length = kExSchedUnknown;
    return true;
}

inline uint32_t ExReorderCosts::sample_index(const Task* tasks, uint32_t n, uint32_t now_ms) {
    if (!open_window(now_ms) || !n) return n;
    // Round-robin over command ids represented in THIS batch, not over operations. Frequent
    // GETs must not starve a rare expensive verb of samples. Randomise the starting position
    // once per window so recurring pipelines do not always train on the same argument shape.
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    const uint32_t start = random_ % n;
    uint32_t chosen = n, best_distance = kCommands;
    for (uint32_t offset = 0; offset < n; offset++) {
        const uint32_t i = (start + offset) % n;
        uint8_t length;
        if (!ex_sched_candidate(tasks[i], length, *this)) continue;
        const Op& op = tasks[i].client->rob().at(tasks[i].op_id);
        if (op.atomic_hazard()) continue;
        const uint32_t distance = (op.spec->id + kCommands - next_id_) % kCommands;
        if (distance < best_distance) { best_distance = distance; chosen = i; }
    }
    if (chosen != n)
        next_id_ = (tasks[chosen].client->rob().at(tasks[chosen].op_id).spec->id + 1) % kCommands;
    return chosen;
}

struct ExScheduleKey {
    uint8_t rank = 0;
    uint8_t length = 0;
};

template <size_t BatchOps>
uint32_t ex_schedule_run(Task* tasks, const uint8_t* base_lengths, uint32_t n) {
    if (n < 2) return 0;
    Client* const only_client = tasks[0].client;
    uint32_t distinct_at = 1;
    while (distinct_at < n && tasks[distinct_at].client == only_client) distinct_at++;
    // Absolute per-connection order leaves no legal permutation in a one-client run.
    if (distinct_at == n) return 0;

    ExScheduleKey keys[BatchOps];
    uint8_t min_rank = UINT8_MAX;
    uint8_t max_rank = 0;
    uint8_t first_length = 0;
    bool one_length = true;

    // The gather contract presents each connection's Tasks in increasing op_id order. Sample
    // newest-to-oldest: flush_id only advances, so an older task still receives a strictly
    // lower rank even if IO retires a completed prefix between samples. Adjacent tasks from
    // one connection reuse the head load without any per-connection table. The slow path
    // verifies the contract before reordering; every earlier exit retains FIFO.
    Client* sampled_client = nullptr;
    uint64_t sampled_head = 0;
    for (uint32_t i = n; i-- > 0;) {
        const Task& task = tasks[i];
        if (task.client != sampled_client) {
            sampled_client = task.client;
            sampled_head = sampled_client->rob().flush_id();
        }
        const uint64_t distance = task.op_id - sampled_head;
        // A fresh unfinished task is always in the 64-slot live ROB window. If that invariant
        // is ever broken, preserve today's FIFO instead of collapsing ranks and risking order.
        if (__builtin_expect(distance >= kRobWindow, false)) return 1;
        keys[i] = ExScheduleKey{static_cast<uint8_t>(distance), base_lengths[i]};
        min_rank = std::min(min_rank, keys[i].rank);
        max_rank = std::max(max_rank, keys[i].rank);
        if (i == n - 1) first_length = keys[i].length;
        else one_length &= keys[i].length == first_length;
    }

    // The measured-law escape is defined on the directly available gathered classes and runs
    // before conservative widening for an invisible predecessor. This is what keeps homogeneous
    // rank-adjacent traffic off the dependency and bucket paths.
    if (one_length && max_rank - min_rank <= 1) return 1;

    // Effective class is the prefix maximum for each connection in this gathered run. A rank
    // before the first represented task, or a gap in its ids, means an unrepresented blocker;
    // its slot cannot safely be read while IO may recycle it, so Long is the no-state upper
    // bound. Open addressing is at most half full and is reached only after degeneration.
    static constexpr uint32_t kChainSlots = BatchOps * 2;
    static constexpr uint32_t kChainWords = (kChainSlots + 63) / 64;
    Client* chain_client[kChainSlots];
    uint8_t chain_last[kChainSlots];
    uint64_t chain_occupied[kChainWords] = {};
    for (uint32_t i = 0; i < n; i++) {
        Client* client = tasks[i].client;
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
            if (keys[i].rank != 0)
                keys[i].length = kExSchedUnknown;
        } else {
            const uint8_t previous = chain_last[slot];
            // Preserve the existing FIFO if a producer-lane bug ever violates the gather
            // contract. The scheduler must never create a same-connection inversion.
            if (tasks[i].op_id <= tasks[previous].op_id) return 1;
            keys[i].length = std::max(keys[i].length, keys[previous].length);
            if (tasks[i].op_id != tasks[previous].op_id + 1)
                keys[i].length = kExSchedUnknown;
        }
        chain_last[slot] = static_cast<uint8_t>(i);
    }
    one_length = true;
    for (uint32_t i = 1; i < n; i++) one_length &= keys[i].length == keys[0].length;
    if (one_length && max_rank - min_rank <= 1) return 1;

    // Stable gather order often already matches the selected bucket order. Avoid scratch
    // setup and two Task copies when the policy would be an identity permutation.
    bool already_ordered = true;
    uint32_t previous_bucket = keys[0].rank * kExSchedClasses + keys[0].length;
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t bucket = keys[i].rank * kExSchedClasses + keys[i].length;
        already_ordered &= bucket >= previous_bucket;
        previous_bucket = bucket;
    }
    if (already_ordered) return 1;

    uint8_t counts[kExSchedBuckets];
    uint8_t cursors[kExSchedBuckets];
    uint64_t occupied[kExSchedBucketWords] = {};
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t bucket = keys[i].rank * kExSchedClasses + keys[i].length;
        const uint32_t word = bucket >> 6;
        const uint64_t bit = uint64_t{1} << (bucket & 63);
        if (!(occupied[word] & bit)) {
            occupied[word] |= bit;
            counts[bucket] = 0;
        }
        counts[bucket]++;
    }

    uint8_t out = 0;
    for (uint32_t word = 0; word < kExSchedBucketWords; word++) {
        uint64_t bits = occupied[word];
        while (bits) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const uint32_t bucket = word * 64 + bit;
            cursors[bucket] = out;
            out = static_cast<uint8_t>(out + counts[bucket]);
        }
    }

    Task ordered[BatchOps];
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t bucket = keys[i].rank * kExSchedClasses + keys[i].length;
        ordered[cursors[bucket]++] = tasks[i];
    }
    for (uint32_t i = 0; i < n; i++) tasks[i] = ordered[i];
    return 2;
}

// Keep scratch behind the caller's boot-latched enable branch, including stack reservation.
// Deduce capacity from the gathered array: exec_batch must not decay it to a Task pointer.
// Scratch never allocates. The worker's cost table changes only at observation windows.
template <size_t BatchOps>
__attribute__((noinline)) ReorderResult ex_schedule_batch(Task (&tasks)[BatchOps], uint32_t n,
                                                       const ExReorderCosts& costs) {
    static_assert(BatchOps == kGenthreadExBatchOps ||
                  BatchOps == kGenthreadPipelineExBatchOps,
                  "audit new executor geometry before enabling reorder");
    static_assert(BatchOps <= UINT8_MAX, "scheduler indices/counts must fit in a byte");
    static_assert((BatchOps & (BatchOps - 1)) == 0, "connection hash mask needs power of two");
    // A future broken gather must fail loudly even in release builds, never schedule a prefix.
    if (__builtin_expect(n > BatchOps, false)) std::abort();
    ReorderResult result;
    uint8_t base_lengths[BatchOps];
    uint32_t begin = 0;
    while (begin < n) {
        if (!ex_sched_candidate(tasks[begin], base_lengths[begin], costs)) {
            begin++;
            continue;
        }
        uint32_t end = begin + 1;
        while (end < n && ex_sched_candidate(tasks[end], base_lengths[end], costs)) end++;
        const uint32_t witness = ex_schedule_run<BatchOps>(
            tasks + begin, base_lengths + begin, end - begin);
        result.multi_client_runs += witness != 0;
        result.permuted_runs += witness == 2;
        // The failed candidate at end is a known barrier; consume it without reading its Op a
        // second time, then find the next eligible run.
        begin = end + (end < n);
    }
    return result;
}

}  // namespace tomo
