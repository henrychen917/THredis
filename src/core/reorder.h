// reorder.h -- continuous owner-local short/long FIFOs, shared by both thread modes.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include "genthread_pipeline.h"
#include "thread.h"
#include "orthog.h"
#include "../net/conn.h"
#include "../cmd/command.h"

namespace tomo {

inline constexpr uint32_t kExSchedClasses =
    static_cast<uint32_t>(CommandLengthClass::Count);

// Only the ordinary one-owner path participates. Every existing special mechanism is a hard
// barrier in the incoming sequence: both queues drain before it may execute.
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
    // safe upper bound without a deque scan or a per-connection scheduler table.
    if (op.atomic_hazard()) length = static_cast<uint8_t>(CommandLengthClass::Long);
    return true;
}

// R7's candidate service ratio matches the deciding 8:2 short/long mix. This is a
// candidate constant, NOT a measured optimum. Only --reorder 0|1 is exposed.
inline constexpr uint32_t kExReorderShortQuota = 4;

// These queues live for ONE inbox drain, across its gathered batches. finish() is mandatory
// before local-read service, snapshot/control work, LB acknowledgement, or returning to IO.
// Thus no queued Task survives a shard/role ownership edge, and no new migration sidecar exists.
// At most one batch of debt survives an admission; a second batch supplies the lookahead.
//
// A short entry must be a live ROB head. Any invisible predecessor (including an earlier
// batch, a gap, or an undecided atomic group) conservatively makes the entry long. Long FIFO
// entries also remember the short-tail frontier at admission: service MUST consume that prefix
// before selecting the long entry, even when its ratio turn is due. This is necessary because
// a long successor may follow its own short head. Both rules together preserve connection
// order without consulting recycled predecessors or maintaining a per-connection hash table.
// The frontier can postpone a ratio turn, but newly admitted shorts cannot extend its debt.
//
// Queue cursors, the service quota and this dependency frontier are the mechanism's bookkeeping;
// there are no per-op clocks, atomic updates, allocations, telemetry counters or Client/Op fields.
// Costs and the conservative non-head classification still require S4 measurement.
template <size_t BatchOps>
class ExReorderQueues {
    static_assert(BatchOps == kGenthreadExBatchOps ||
                  BatchOps == kGenthreadPipelineExBatchOps);
    static constexpr uint32_t Capacity = 2 * BatchOps;
    static_assert((Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_trivially_destructible_v<Task>);

    // Default-constructing Task arrays would clear every unused slot at every drain. Activate
    // only the occupied union member; Task has no destructor work when a ring slot is reused.
    union Slot {
        Task task;
        Slot() {}
        ~Slot() {}
    };
    Slot short_tasks_[Capacity];
    Slot long_tasks_[Capacity];
    uint64_t long_frontier_[Capacity];
    uint64_t short_head_ = 0, short_tail_ = 0;
    uint64_t long_head_ = 0, long_tail_ = 0;
    uint32_t short_quota_ = kExReorderShortQuota;

    static uint32_t slot(uint64_t cursor) { return cursor & (Capacity - 1); }

    void append(const Task& task, uint8_t length) {
        if (__builtin_expect(size() == Capacity, false)) std::abort();
        const bool is_short = length < static_cast<uint8_t>(CommandLengthClass::Long) &&
                              task.op_id == task.client->rob().flush_id();
        if (is_short) {
            new (&short_tasks_[slot(short_tail_)].task) Task(task);
            short_tail_++;
        } else {
            const uint32_t at = slot(long_tail_);
            new (&long_tasks_[at].task) Task(task);
            long_frontier_[at] = short_tail_;
            long_tail_++;
        }
    }

    ReorderResult take(Task* output, uint32_t n) {
        if (__builtin_expect(n > size(), false)) std::abort();
        ReorderResult result;
        for (uint32_t i = 0; i < n; i++) {
            const bool have_short = short_head_ != short_tail_;
            const bool have_long = long_head_ != long_tail_;
            const uint32_t at = slot(long_head_);
            if (have_short && have_long && !result.multi_client_runs)
                result.multi_client_runs =
                    short_tasks_[slot(short_head_)].task.client != long_tasks_[at].task.client;
            if (have_short && (!have_long || short_quota_ ||
                               short_head_ < long_frontier_[at])) {
                // This exact queue pick passes a still-pending earlier long entry. A coarse
                // class mix alone is not a permutation witness (quota/dependency can keep FIFO).
                if (have_long && short_head_ >= long_frontier_[at])
                    result.permuted_runs = 1;
                output[i] = short_tasks_[slot(short_head_++)].task;
                if (short_quota_) short_quota_--;
            } else {
                output[i] = long_tasks_[at].task;
                long_head_++;
                short_quota_ = kExReorderShortQuota;
            }
        }
        return result;
    }

    template <typename Emit>
    void emit_queued(uint32_t n, Emit& emit) {
        Task output[BatchOps];
        const ReorderResult result = take(output, n);
        // The callback can publish Done and let IO recycle/free completed slots and Clients.
        // All classification and witness reads precede it; afterward only cursors remain live.
        emit(output, n, result);
    }

public:
    ExReorderQueues() = default;
    ExReorderQueues(const ExReorderQueues&) = delete;
    ExReorderQueues& operator=(const ExReorderQueues&) = delete;
    ~ExReorderQueues() { if (size()) std::abort(); }

    uint32_t size() const {
        return static_cast<uint32_t>((short_tail_ - short_head_) + (long_tail_ - long_head_));
    }

    template <typename Emit>
    void finish(Emit& emit) {
        while (size()) emit_queued(std::min<uint32_t>(BatchOps, size()), emit);
        short_quota_ = kExReorderShortQuota;
    }

    // Admission is a batch decision. Empty-queue one-client and homogeneous coarse-class runs
    // retain their original Task span: no queue entries, head loads or ratio picks. When debt
    // exists, the same input must join the queues so it cannot jump a queued own predecessor.
    template <typename Emit>
    void submit(const Task* tasks, uint32_t n, Emit& emit) {
        if (__builtin_expect(n > BatchOps, false)) std::abort();
        if (!n) return;
        if (!size()) {
            uint32_t same_client = 1;
            while (same_client < n && tasks[same_client].client == tasks[0].client)
                same_client++;
            if (same_client == n) {
                emit(tasks, n, ReorderResult{});
                return;
            }
        }
        uint8_t lengths[BatchOps];
        uint32_t begin = 0;
        while (begin < n) {
            if (!ex_sched_candidate(tasks[begin], lengths[begin])) {
                finish(emit);
                emit(tasks + begin, 1, ReorderResult{});
                begin++;
                continue;
            }
            uint32_t end = begin + 1;
            bool one_client = true, one_class = true;
            const bool first_long = lengths[begin] ==
                static_cast<uint8_t>(CommandLengthClass::Long);
            while (end < n && ex_sched_candidate(tasks[end], lengths[end])) {
                one_client &= tasks[end].client == tasks[begin].client;
                one_class &= (lengths[end] == static_cast<uint8_t>(CommandLengthClass::Long)) ==
                             first_long;
                end++;
            }
            if (!size() && (one_client || one_class)) {
                emit(tasks + begin, end - begin,
                     ReorderResult{static_cast<uint32_t>(!one_client), 0});
            } else {
                for (uint32_t i = begin; i < end; i++) append(tasks[i], lengths[i]);
                if (size() > BatchOps) emit_queued(size() - BatchOps, emit);
            }
            // The first failed candidate is a barrier. Drain before it without inspecting its
            // Op again; never retain a borrowed Op pointer across a callback publishing Done.
            if (end < n) {
                finish(emit);
                emit(tasks + end, 1, ReorderResult{});
            }
            begin = end + (end < n);
        }
    }
};

// Bounded contexts (read-local fairness quanta and existing deferred-work batches) must finish
// before their caller services the next phase. They use the same queues and policy, just with
// one admission. Keep every byte of scratch behind the boot-latched enable branch when off.
template <size_t BatchOps>
__attribute__((noinline)) ReorderResult ex_schedule_batch(Task (&tasks)[BatchOps], uint32_t n) {
    if (__builtin_expect(n > BatchOps, false)) std::abort();
    ExReorderQueues<BatchOps> queues;
    uint32_t count = 0;
    ReorderResult result;
    auto emit = [&](const Task* selected, uint32_t size, ReorderResult witness) {
        result.multi_client_runs += witness.multi_client_runs;
        result.permuted_runs += witness.permuted_runs;
        if (__builtin_expect(size > n - count, false)) std::abort();
        // submit() has admitted every source entry in this output prefix before calling us.
        // A queued emission can therefore replace it now without touching an unscanned suffix
        // or barrier. Bypassed spans already occupy their final position and need no Task copy.
        if (selected != tasks + count)
            for (uint32_t i = 0; i < size; i++) tasks[count + i] = selected[i];
        count += size;
    };
    queues.submit(tasks, n, emit);
    queues.finish(emit);
    if (count != n) std::abort();
    return result;
}

}  // namespace tomo
