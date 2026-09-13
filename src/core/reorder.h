// reorder.h -- R6: order IO admission before releasing the producer tail to the owner.
// Per-connection order and special-task barriers are absolute. One-client runs retain FIFO.
// The transport keeps its original slots and consumer frontier. IO replaces the per-task
// release store with a private frontier store, orders bounded chunks, then publishes them.
// Thus EX never sorts, and no operation acquires a new counter, pointer or timestamp.
//
// The scope is one IO read pass, one producer/owner lane and one 128-task chunk. Other IO
// producers, already-published work, atomic retries and the read-local lane are outside it.
// There is no wait to fill a chunk. Every pass, special command, demotion, blocking resume and
// close publishes its pending prefix before entering the existing mechanism. In 1s these
// instructions compete with execution on the SAME core; moving them buys no spare IO core.
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

// Scope guards live here so every entry and every exceptional publication uses the same fence.
// Empty/off guards neither allocate nor enter the producer-bank or scheduler code.
template <typename Loop>
class IoAdmissionBatch {
public:
    explicit IoAdmissionBatch(Loop& loop) : loop_(loop) {
        if (loop_.reorder_enabled_) loop_.begin_admission();
    }
    ~IoAdmissionBatch() { finish(); }
    void finish() { if (loop_.admission_active_) loop_.finish_admission(); }
    IoAdmissionBatch(const IoAdmissionBatch&) = delete;
    IoAdmissionBatch& operator=(const IoAdmissionBatch&) = delete;
private:
    Loop& loop_;
};

template <typename Loop>
class IoAdmissionFence {
public:
    explicit IoAdmissionFence(Loop& loop) : loop_(loop), resume_(loop.admission_active_) {
        if (resume_) loop_.finish_admission();
    }
    ~IoAdmissionFence() {
        if (!resume_) return;
        // A cold subsystem may have posted ordinary/reserved tasks while admission was paused.
        // Publish their notification and sample fresh tails before accepting a younger task.
        loop_.flush_ifid_posts();
        loop_.begin_admission(true);
    }
    IoAdmissionFence(const IoAdmissionFence&) = delete;
    IoAdmissionFence& operator=(const IoAdmissionFence&) = delete;
private:
    Loop& loop_;
    bool resume_;
};

inline constexpr uint32_t kExSchedClasses =
    static_cast<uint32_t>(CommandLengthClass::Count);
inline constexpr uint32_t kExSchedBuckets = kRobWindow * kExSchedClasses;
inline constexpr uint32_t kExSchedBucketWords = (kExSchedBuckets + 63) / 64;
static_assert(kExSchedBuckets == 192);

// Only the ordinary one-owner path participates. Every existing special mechanism is a hard
// barrier in the gathered sequence: eligible work on either side cannot move across it.
inline bool ex_sched_candidate(const Task& task, uint8_t& length) {
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
    length = static_cast<uint8_t>(command_length_class(*op.spec));
    if (__builtin_expect(length >= kExSchedClasses, false)) return false;
    // There is no O(1) class pointer from an op to an exact parked atomic predecessor. The
    // immutable publish-time hazard bit says an older own atomic group existed; Long is the
    // safe upper bound without a deque scan or persistent scheduler state.
    if (op.atomic_hazard()) length = static_cast<uint8_t>(CommandLengthClass::Long);
    return true;
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
                keys[i].length = static_cast<uint8_t>(CommandLengthClass::Long);
        } else {
            const uint8_t previous = chain_last[slot];
            // Preserve the existing FIFO if a producer-lane bug ever violates the gather
            // contract. The scheduler must never create a same-connection inversion.
            if (tasks[i].op_id <= tasks[previous].op_id) return 1;
            keys[i].length = std::max(keys[i].length, keys[previous].length);
            if (tasks[i].op_id != tasks[previous].op_id + 1)
                keys[i].length = static_cast<uint8_t>(CommandLengthClass::Long);
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
// Deduce capacity from the admission array: the producer must not decay it to a Task pointer.
// No heap allocation, persistent state, truncated suffix, or change to the scheduling policy.
template <size_t BatchOps>
__attribute__((noinline)) ReorderResult ex_schedule_batch(Task (&tasks)[BatchOps], uint32_t n) {
    static_assert(BatchOps == kGenthreadExBatchOps ||
                  BatchOps == kGenthreadPipelineExBatchOps,
                  "audit new admission geometry before enabling reorder");
    static_assert(BatchOps <= UINT8_MAX, "scheduler indices/counts must fit in a byte");
    static_assert((BatchOps & (BatchOps - 1)) == 0, "connection hash mask needs power of two");
    // A future broken gather must fail loudly even in release builds, never schedule a prefix.
    if (__builtin_expect(n > BatchOps, false)) std::abort();
    ReorderResult result;
    uint8_t base_lengths[BatchOps];
    uint32_t begin = 0;
    while (begin < n) {
        if (!ex_sched_candidate(tasks[begin], base_lengths[begin])) {
            begin++;
            continue;
        }
        uint32_t end = begin + 1;
        while (end < n && ex_sched_candidate(tasks[end], base_lengths[end])) end++;
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
