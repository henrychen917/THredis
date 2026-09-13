// reorder.h -- latency scheduling across connections within one gathered executor batch.
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

// Only a non-degenerate multi-client run reaches dependency widening and bucket scatter.
// Keeping this frame out of line also keeps its hash table and Task copies off identity paths.
template <size_t BatchOps>
__attribute__((noinline))
uint32_t ex_schedule_ranked_run(Task* tasks, ExScheduleKey* keys, uint32_t n,
                                uint8_t min_rank, uint8_t max_rank) {
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
    bool one_length = true;
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

// Visit each represented Op once to collect eligibility, static cost and ROB rank together.
// The gather contract is increasing op_id per connection. Sampling NEWEST to OLDEST is required:
// flush_id can advance on IO during this scan, but an older task must still get a lower rank.
// Adjacent tasks from a connection share that snapshot. Never read an absent predecessor's slot;
// the ranked slow path retains the existing conservative Long widening for missing heads/gaps.
//
// Runs are visited backwards solely to combine the metadata and rank passes. Permutations stay
// inside their original barrier-delimited run, so visiting independent runs in reverse cannot
// move a Task across a barrier or change the stable (rank, effective class) policy.
template <size_t BatchOps>
__attribute__((noinline)) ReorderResult ex_schedule_candidates(Task (&tasks)[BatchOps], uint32_t n) {
    static_assert(BatchOps == kGenthreadExBatchOps ||
                  BatchOps == kGenthreadPipelineExBatchOps,
                  "audit new executor geometry before enabling reorder");
    static_assert(BatchOps <= UINT8_MAX, "scheduler indices/counts must fit in a byte");
    static_assert((BatchOps & (BatchOps - 1)) == 0, "connection hash mask needs power of two");
    // A future broken gather must fail loudly even in release builds, never schedule a prefix.
    if (__builtin_expect(n > BatchOps, false)) std::abort();
    ReorderResult result;
    ExScheduleKey keys[BatchOps];
    uint32_t end = n;
    while (end) {
        uint32_t begin = end;
        Client* first_client = nullptr;
        Client* sampled_client = nullptr;
        uint64_t sampled_head = 0;
        uint8_t min_rank = UINT8_MAX, max_rank = 0, first_length = 0;
        bool multi_client = false, one_length = true, valid_rank = true;
        while (begin) {
            const Task& task = tasks[begin - 1];
            uint8_t length;
            if (!ex_sched_candidate(task, length)) break;
            --begin;
            if (begin == end - 1) {
                first_client = task.client;
                first_length = length;
            } else {
                multi_client |= task.client != first_client;
                one_length &= length == first_length;
            }
            if (task.client != sampled_client) {
                sampled_client = task.client;
                sampled_head = sampled_client->rob().flush_id();
            }
            const uint64_t distance = task.op_id - sampled_head;
            // An unfinished task belongs to the 64-slot live window. An invalid sample makes
            // the WHOLE run FIFO; do not clamp it into a bucket and accidentally invent order.
            valid_rank &= distance < kRobWindow;
            keys[begin] = {static_cast<uint8_t>(distance), length};
            min_rank = std::min(min_rank, keys[begin].rank);
            max_rank = std::max(max_rank, keys[begin].rank);
        }
        if (multi_client) {
            result.multi_client_runs++;
            // Preserve the existing escape BEFORE dependency widening. In particular, this
            // is not R2's uniform-class trigger: a wider all-GET rank span still gets scheduled.
            if (valid_rank && !(one_length && max_rank - min_rank <= 1))
                result.permuted_runs += ex_schedule_ranked_run<BatchOps>(
                    tasks + begin, keys + begin, end - begin, min_rank, max_rank) == 2;
        }
        // The failed candidate just before begin is a known barrier: consume it once.
        end = begin - (begin != 0);
    }
    return result;
}

// A single connection has no legal promotion, regardless of command classes, hazards, gaps,
// or barriers. Check the gathered Task pointers before touching Op chunks or IO's flush line.
// Keep the metadata/sort frame in its own callee so a FIFO batch reserves none of that scratch.
// This is a connection-order proof, not a cost-class trigger: a multi-client all-GET batch still
// promotes live heads across deeper requests when the existing rank policy calls for it.
template <size_t BatchOps>
__attribute__((noinline)) ReorderResult ex_schedule_batch(Task (&tasks)[BatchOps], uint32_t n) {
    static_assert(BatchOps == kGenthreadExBatchOps ||
                  BatchOps == kGenthreadPipelineExBatchOps,
                  "audit new executor geometry before enabling reorder");
    if (__builtin_expect(n > BatchOps, false)) std::abort();
    if (n < 2) return {};
    Client* const first = tasks[0].client;
    uint32_t i = 1;
    while (i < n && tasks[i].client == first) i++;
    if (i == n) return {};
    return ex_schedule_candidates(tasks, n);
}

}  // namespace tomo
