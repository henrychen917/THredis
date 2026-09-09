// ex_loop.h — executes ops against owned shards. The connection's IO owner retires and sends
// their replies, using the same Channel signalling and LoopSignals units.
//
//   in   task_in from IO threads         a parsed op to execute
//   out  ready-mask bit / client_in      completed ops for the connection's IO owner
//
// The executor's Ring receives wakes and submits its snapshot/AOF persistence work.
//
// WAITING IS THE INTERESTING PART. A worker with an empty inbox must not spin a core at 100% — that
// is a real cost at 64 workers and it distorts every utilisation reading a controller might use. It
// also must not sleep so eagerly that it pays a wakeup per op under load. So: spin briefly, then arm
// the blocked flag, re-check, and block. Producers only pay a wake syscall while that flag is armed.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <type_traits>
#include "server.h"
#include "signal.h"
#include "genthread_pipeline.h"
#include "read_local.h"
#include "../net/conn.h"
#include "../net/resp.h"
#include "../net/uring.h"
#include "../net/wb.h"
#include "../cmd/command.h"
#include "../cmd/blocking.h"
#include "../cmd/multi.h"
#include "../cmd/notify.h"
#include "../cmd/slowlog.h"
#include "../cmd/xshard.h"
#include "../persist/aof.h"

namespace tomo {

// "Long enough to cover an inter-arrival gap, short enough not to burn a core." A starting point to
// measure, not a result.
inline constexpr uint32_t kExSpinBudget = 2048;

// How many ops are gathered before executing, so their storage prefetches can overlap. Large enough
// that the prefetches have time to land, small enough that the batch stays in L1.
inline constexpr uint32_t kExecBatch = kGenthreadExBatchOps;
inline constexpr uint32_t kReadLocalPrefetchKeys = 32;
inline constexpr uint32_t kActiveExpireChecks = 20;
// The armed coarse rotation alternates these two equal quanta. One owner quantum per captured
// producer bounds the stretch before WB by active producers rather than by queued depth, while the
// local quantum matches the most one parse invocation can append for a connection.
inline constexpr uint32_t kReadLocalDrainChunkOps = kGenthreadIfidBatchOps;
inline constexpr uint32_t kReadLocalOwnerTaskChunkOps = kGenthreadExBatchOps;
inline constexpr uint32_t kReadLocalMaxChunksBetweenOwnerBatches = 1;
// Parser-side demotion may reserve one additional point command behind the entire local run.
// Leave that credit outside the pending-read fanout budget so the combined reservation can always
// fit an empty producer lane and therefore cannot retry forever.
inline constexpr uint32_t kReadLocalDemotionBudget = kInboxSlots - 1;
// Lane ADMISSION pressure window (P128.md). A LANE-FULL deferral arms this many fused rotations
// during which the parser bounds every connection to kInboxSlots / active connections in flight.
// It decays by one per rotation and is re-armed only by the next lane-full event -- never by the
// quota's own deferrals (the actuator must not police itself) -- so under sustained oversubscription
// one rotation in (this + 1) lets the lane prove the pressure again, and once aggregate demand has
// stayed under the lane for this many rotations the bound is UINT32_MAX and the steady state pays
// one predicted byte test per parse pass and nothing per op.
inline constexpr uint8_t kReadLocalLanePressureRotations = 8;
inline constexpr uint32_t kExSchedClasses =
    static_cast<uint32_t>(CommandLengthClass::Count);
inline constexpr uint32_t kExSchedBuckets = kRobWindow * kExSchedClasses;
inline constexpr uint32_t kExSchedBucketWords = (kExSchedBuckets + 63) / 64;
static_assert(kExecBatch <= UINT8_MAX);
static_assert(kExecBatch <= 32);
static_assert(kGenthreadIfidBatchOps <= kExecBatch);
static_assert(kReadLocalDrainChunkOps == kExecBatch);
static_assert(kReadLocalOwnerTaskChunkOps == kExecBatch);
static_assert(kReadLocalMaxChunksBetweenOwnerBatches > 0);
static_assert((kExecBatch & (kExecBatch - 1)) == 0);
static_assert(kExSchedBuckets == 192);

// Constructed only for an armed declared-key-precise write whose owner has since enabled eviction.
// Keeping the existing maxmemory admission active but forcing NoEviction makes the IO-side promise
// self-fulfilling: this operation can update its declared point/keyset or fail, but cannot delete a
// different key behind a younger local read. The owner restores its live policy before the next
// task.
class ReadLocalPreciseWriteGuard {
public:
    explicit ReadLocalPreciseWriteGuard(FlatStore& store) : store_(&store) {
        previous_ = store_->script_suspend_eviction();
    }
    ~ReadLocalPreciseWriteGuard() { store_->script_restore_eviction(previous_); }
    ReadLocalPreciseWriteGuard(const ReadLocalPreciseWriteGuard&) = delete;
    ReadLocalPreciseWriteGuard& operator=(const ReadLocalPreciseWriteGuard&) = delete;

private:
    FlatStore* store_ = nullptr;
    MaxmemoryPolicy previous_ = MaxmemoryPolicy::NoEviction;
};

template <bool Enabled>
struct ReadLocalExState;

template <>
struct ReadLocalExState<false> {};

template <>
struct ReadLocalExState<true> {
    struct Impl {
        using DemoteFn = bool (*)(void*, Client*, const uint64_t*,
                                  const ReadLocalFallbackReason*, uint32_t, uint32_t&);

        // A lane entry is the ROB handle only. The full 32-byte Task carried a shard and a
        // scatter pointer that were always -1/nullptr here; at kInboxSlots entries that doubled
        // the lane to the size of the whole L1D. Demotion builds its owner Tasks from the id.
        struct LaneEntry {
            Client* client;
            uint64_t op_id;
        };
        static_assert(sizeof(LaneEntry) == 16);

        std::unique_ptr<LaneEntry[]> lane;
        // A failed probe can sit behind an earlier bounded demotion wave. Preserve its original
        // reason until that exact ROB op is lowered instead of reclassifying the suffix as Context.
        std::unique_ptr<ReadLocalFallbackReason[]> lane_fallbacks;
        uint32_t lane_head = 0;
        uint32_t lane_tail = 0;
        uint32_t lane_count = 0;
        // Upper-bound owner-task demand for every still-local lane entry. GET contributes one;
        // MGET contributes min(key count, shard count), which bounds its touched-shard fanout.
        // Keeping the sum within the demotion budget makes every task-capacity plan reservable
        // (snapshot pressure may still split it into waves).
        uint32_t lane_demotion_demand = 0;
        // Rotations left in the lane-admission pressure window; see kReadLocalLanePressureRotations.
        uint8_t lane_pressure = 0;
        bool lane_has_tombstones = false;
        bool point_writes_precise = true;
        bool keymiss_notify_armed = false;
        uint8_t reserved_schedule[2] = {}; // preserve the demotion-state offsets
        // EFFECTIVE lane capacity for ADMISSION. The physical ring is always kInboxSlots entries
        // and its index arithmetic still masks with kInboxSlots - 1; this only decides when the
        // parser stops admitting, so lowering it can never overrun the ring. It derives to
        // kInboxSlots and stays there unless DEBUG READ-LOCAL-LANE-CAP sets a test value, which
        // fused_pass_impl copies here once per rotation. It occupies the two padding bytes that
        // already sat at offset 38 between reserved_schedule and demote_context, on the same first
        // cache line as lane_head/lane_tail/lane_count that every admission test already reads, so
        // the struct does not grow, nothing moves, and reading it costs no line the caller did not
        // already own. See P128.md section 8.
        uint16_t lane_admit_cap = static_cast<uint16_t>(kInboxSlots);
        void* demote_context = nullptr;
        DemoteFn demote = nullptr;
        ReadLocalDeferredQueue deferred;

        void rebind_owned_shards(ThreadCtx& owner) {
            ReadLocalRetireSink* sink = deferred.sink();
            if (!sink) std::abort();
            for (Shard* shard : owner.shards())
                shard->store().rebind_read_local_retire_sink(*sink);
        }
    };

    std::unique_ptr<Impl> impl;
};

using ReadLocalExImpl = ReadLocalExState<true>::Impl;
static_assert(std::is_empty_v<ReadLocalExState<false>>);

template <uint32_t Capacity>
struct ReadLocalCaptureBuffer {
    FlatStore::ReadLocalPrefetchCapture entries[Capacity];
};

template <bool Fused>
class ExLoopT {
public:
    using FusedCompletionFn = void (*)(void*, Client*);

    bool init(Server* srv, ThreadCtx* self, bool dormant = false) {
        srv_ = srv; self_ = self;
        aof_manager_ = srv->aof().configured() ? &srv->aof() : nullptr;
        foreign_touch_random_ ^= (static_cast<uint64_t>(self->id()) + 1) * 0x9e3779b97f4a7c15ULL;
        lb_sample_rate_ = srv->lb_machinery_enabled() ? srv->lb_sample_rate() : 0;
        lb_sample_countdown_ = lb_sample_rate_;
        lb_controller_armed_ = srv->lb_machinery_enabled();
        age_sample_rate_cached_ = srv->effective_age_sample_rate();
        ex_sched_enabled_ = srv->cfg().ex_sched != 0;
        pipeline_batches_ = Fused && srv->cfg().overlap != 0;
        // Both interwoven schedules use the iofused fixed producer lanes.
        iofused_ = Fused && srv->cfg().overlap != 0;
        if constexpr (Fused) {
            if (srv->read_local_enabled()) {
                std::unique_ptr<ReadLocalExImpl> impl(new (std::nothrow) ReadLocalExImpl);
                if (!impl) return false;
                impl->lane.reset(new (std::nothrow) ReadLocalExImpl::LaneEntry[kInboxSlots]);
                impl->lane_fallbacks.reset(
                    new (std::nothrow) ReadLocalFallbackReason[kInboxSlots]);
                if (!impl->lane || !impl->lane_fallbacks ||
                    !impl->deferred.init(srv, self)) return false;
                impl->point_writes_precise =
                    srv->cfg().maxmemory == 0 ||
                    srv->cfg().maxmemory_policy == MaxmemoryPolicy::NoEviction;
                read_local_.impl = std::move(impl);
            }
        }
        if (!ring_.init(1024)) return false;
        fused_handoff_ring_ = &ring_;
        initialized_ = true;
        if (!dormant) activate();
        return true;
    }

    void activate() {
        if (!initialized_) std::abort();
        self_->set_ring(&ring_);
        self_->set_wb_engine(nullptr);
        blocking_bind_executor(srv_, self_, &ring_);
        for (Shard* shard : self_->shards())
            shard->bind_notify_pending(&notify_keyless_pending_);
    }

    // Fused tenure shares the physical thread with IoLoop. IoLoop remains the published wake
    // endpoint because it owns the only blocking wait; this ring still carries persistence CQEs.
    void activate_fused(Ring* handoff_ring) {
        static_assert(Fused);
        if (!initialized_) std::abort();
        // Interwoven schedules put executor-originated task/client handoffs on the network ring.
        // That leaves this private ring with control/persistence SQEs; both iofused-family arms
        // amortize at their shared N2 boundary. Pipeline 0 retains its existing ring ownership.
        if (srv_->cfg().overlap != 0 && handoff_ring)
            fused_handoff_ring_ = handoff_ring;
        blocking_bind_executor(srv_, self_, &ring_);
        if (read_local_enabled())
            self_->bind_read_local_retire_sink(*read_local_impl().deferred.sink());
        for (Shard* shard : self_->shards()) {
            shard->bind_notify_pending(&notify_keyless_pending_);
            if (read_local_enabled())
                shard->store().configure_read_local(
                    true, *read_local_impl().deferred.sink());
        }
    }

    void bind_fused_completion(void* context, FusedCompletionFn completion) {
        static_assert(Fused);
        fused_io_context_ = context;
        fused_completion_ = completion;
    }

    void bind_read_local_demotion(void* context, ReadLocalExImpl::DemoteFn demote) {
        static_assert(Fused);
        if (!read_local_enabled() || !context || !demote) std::abort();
        read_local_impl().demote_context = context;
        read_local_impl().demote = demote;
    }

    // The armed parser is the only producer and calls this immediately after its own
    // local_read_lane_has_room(demand) verdict on the same op, with nothing that can consume lane
    // room in between (only demand releases can run there). It already holds the Op, its MGET
    // classification and its demotion demand, so this is the four lane stores and nothing else.
    __attribute__((always_inline)) void enqueue_local_read(Client* client, uint64_t op_id,
                                                           uint32_t demand) {
        static_assert(Fused);
        auto& state = *read_local_.impl;
        state.lane[state.lane_tail] = {client, op_id};
        state.lane_fallbacks[state.lane_tail] = ReadLocalFallbackReason::None;
        state.lane_tail = (state.lane_tail + 1) & (kInboxSlots - 1);
        state.lane_count++;
        state.lane_demotion_demand += demand;
    }

    bool local_read_lane_has_room(uint32_t demotion_demand = 1) const {
        static_assert(Fused);
        if (!read_local_enabled()) return false;
        const auto& state = read_local_impl();
        // `<` rather than `!=` because lane_admit_cap may be below the ring size under a test
        // cap; with the production value (kInboxSlots) the two are identical, since lane_count is
        // never allowed past it.
        return state.lane_count < state.lane_admit_cap &&
               demotion_demand <=
                   kReadLocalDemotionBudget - state.lane_demotion_demand;
    }

    // LANE ADMISSION (P128.md). The parser asks once per parse pass how many lane slots (pending
    // local reads) one connection may hold. Unbounded while the lane is not under pressure -- below
    // kInboxSlots of aggregate demand per rotation. Under pressure it is the lane divided among the
    // connections competing for it, which by construction keeps the sum of admitted local reads
    // within the lane, so the lane-full deferral -- whose victims are whichever connections the
    // rotation parsed last -- stops firing and every connection progresses every rotation. Both
    // terms self-derive: live connections from the active set, observed depth from the lane-full
    // events that arm the window. No knob.
    uint32_t read_local_lane_quota(size_t live_connections) const {
        static_assert(Fused);
        // Same disabled-state shape as local_read_lane_has_room(): no impl means no lane and no
        // bound (the parser only asks when the server's read-local is on, which is what creates
        // the impl at init; this is the one test that call already paid, not a second one).
        if (!read_local_enabled()) return UINT32_MAX;
        const auto& state = *read_local_.impl;
        if (__builtin_expect(state.lane_pressure == 0, true)) return UINT32_MAX;
        return std::max<uint32_t>(
            1, state.lane_admit_cap /
                   static_cast<uint32_t>(std::max<size_t>(1, live_connections)));
    }

    // Is the lane-admission pressure window armed? ONE BYTE on the Impl's first cache line -- the
    // same line (lane_head/lane_tail/lane_count/lane_demotion_demand at offsets 16..31, and
    // lane_pressure at 32) that local_read_lane_has_room() already reads for every eligible read of
    // the same pass -- so asking costs no cache line the pass did not already own.
    //
    // The parser MUST ask this before it evaluates read_local_lane_quota's argument. C++ evaluates
    // arguments eagerly, so `read_local_lane_quota(active_.size())` loaded active_ on EVERY parse
    // pass even though the callee returns UINT32_MAX immediately when the window is not armed --
    // and IoLoop::active_ sits at offset 7728, on a line (7680-7743, shared only with unix_rr_,
    // random_state_ and aof_gate_target_) that the parse pass never otherwise touches, while the
    // IoLoop fields it does read live on two other lines entirely (notify_armed_ 7864,
    // proto_max_bulk_len_ 7872). That made always-on machinery cost one cold-line demand fill per
    // parse pass while every counter it owns still read zero -- measured on the owner's 32-core box
    // at 512 connections as +11% DRAM and +3.4% same-CCX fills per op with instructions flat
    // (P128.md 9.5). A hint must never be load-bearing, and machinery that has not fired must be
    // free: gate the argument, not just the answer.
    bool read_local_lane_under_pressure() const {
        static_assert(Fused);
        if (!read_local_enabled()) return false;
        return read_local_.impl->lane_pressure != 0;
    }

    // A lane-full deferral arms the pressure window; fused_pass_impl decays it once per rotation.
    void note_local_read_lane_full() {
        static_assert(Fused);
        if (!read_local_enabled()) return;
        read_local_.impl->lane_pressure = kReadLocalLanePressureRotations;
    }

    void note_local_read_demoted(const Op& op) {
        static_assert(Fused);
        if (!read_local_enabled()) std::abort();
        release_local_read_demotion_demand(op);
        read_local_impl().lane_has_tombstones = true;
    }

    void preserve_local_read_fallback(Client* client, uint64_t op_id,
                                      ReadLocalFallbackReason reason) {
        static_assert(Fused);
        if (!read_local_enabled() || !client ||
            reason == ReadLocalFallbackReason::None) std::abort();
        auto& lane = read_local_impl();
        for (uint32_t offset = 0; offset < lane.lane_count; offset++) {
            const uint32_t index =
                (lane.lane_head + offset) & (kInboxSlots - 1);
            const auto& task = lane.lane[index];
            if (task.client != client || task.op_id != op_id) continue;
            if (!client->rob().pending_read_local(op_id)) std::abort();
            if (lane.lane_fallbacks[index] == ReadLocalFallbackReason::None)
                lane.lane_fallbacks[index] = reason;
            return;
        }
        std::abort();
    }

    void set_read_local_keymiss_notify(bool armed) {
        static_assert(Fused);
        if (!read_local_enabled()) std::abort();
        read_local_impl().keymiss_notify_armed = armed;
    }

    bool read_local_keymiss_notify_armed() const {
        static_assert(Fused);
        return read_local_enabled() && read_local_impl().keymiss_notify_armed;
    }

    void read_local_shutdown_drain() {
        static_assert(Fused);
        if (read_local_enabled()) (void)read_local_impl().deferred.drain_shutdown();
    }

    // One non-blocking executor batch in the coarse fused rotation. The network loop owns park;
    // this pass is the split executor body without its role loop or independent wait.
    uint32_t fused_pass() {
        static_assert(Fused);
        if (!pipeline_batches_)
            return fused_pass_impl<kGenthreadExBatchOps, true, false>();
        return iofused_
            ? fused_pass_impl<kGenthreadPipelineExBatchOps, true, true, true>()
            : fused_pass_impl<kGenthreadPipelineExBatchOps, true, false>();
    }

    uint32_t fused_baseline_pass() {
        static_assert(Fused);
        if (read_local_enabled())
            return fused_pass_impl<kGenthreadExBatchOps, true, false, false, true>();
        return fused_pass_impl<kGenthreadExBatchOps, true, false>();
    }

    // Private-lane whole-batch turn shared by source iofused, overlap-2's thin path, idle repair,
    // and blocking snapshot progress. It has no streams pipeline-state probes.
    uint32_t fused_coarse_pass() {
        static_assert(Fused);
        return fused_pass_impl<kGenthreadPipelineExBatchOps, true, true, true>();
    }

    // Three-way overlap is the ordinary iofused executor envelope with one piece of independent
    // CPU work inserted into the first fresh task batch's existing prefetch gap.  The batch remains
    // the stack-local whole batch used by drain_tasks(); nothing is staged across this call.
    template <typename Filler>
    uint32_t fused_three_way_pass(Filler&& filler) {
        static_assert(Fused);
        using Fn = std::remove_reference_t<Filler>;
        return fused_pass_impl<kGenthreadPipelineExBatchOps, true, true, true, false, Fn>(
            &filler);
    }

    // Legacy streams entry retained for branch archaeology; overlap-2 dispatch has no caller.
    uint32_t fused_streams_pass() {
        static_assert(Fused);
        return fused_pass_impl<kGenthreadPipelineExBatchOps, true, false>();
    }

    // Buffered schedules keep control/persistence work in the executor owner but let the fused
    // loop own task gather/prefetch/execute. This has no internal park and never consumes a Task.
    uint32_t fused_pipeline_control() {
        static_assert(Fused);
        return fused_pass_impl<kGenthreadPipelineExBatchOps, false, false>();
    }

    template <uint32_t BatchOps, bool ConsumeTasks, bool CoalesceSubmit,
              bool IofusedPrivateQueue = false, bool InterleaveLocalReads = false,
              typename Filler = void>
    uint32_t fused_pass_impl(Filler* filler = nullptr) {
        constexpr bool HasFiller = !std::is_void_v<Filler>;
        [[maybe_unused]] bool filler_used = false;
        auto finish_filler = [&] {
            if constexpr (HasFiller) {
                if (!filler) std::abort();
                if (!filler_used) {
                    (*filler)();
                    filler_used = true;
                }
            }
        };
        struct RotationBoundary {
            bool enabled;
            ThreadCtx* self;
            Server* server;
            ~RotationBoundary() {
                if (enabled) self->publish_read_local_tick(server->read_local_epoch());
            }
        } rotation_boundary{read_local_enabled(), self_, srv_};
        cached_now_ms_ = realtime_ms();
        // A completed LB stage may have changed this owner's shard vector. Armed retirement sinks
        // must follow ownership before any work; the literal baseline notification block remains in
        // lb_control_pass below. Since the ownership edge itself now rebinds
        // (Server::adopt_read_local_retire_sink) this is a no-op standing check, kept because a
        // no-op is the cheapest proof that the eager rebind ran.
        if (lb_rebind_pending_ && srv_->lb_stage() != LbStage::ExDrain)
            read_local_rebind_owned_shards_after_lb();
#ifdef TOMO_RL_CACHE_DEBUG
        if constexpr (Fused)
            if (read_local_enabled())
                srv_->debug_assert_read_local_sinks_follow_ownership(self_->id());
#endif
        const bool lb_frozen = lb_controller_armed_ && srv_->lb_dispatch_paused();
        if (!lb_frozen) refresh_live_config();
        if (maxmemory_enabled_)
            cached_lru_clock_ = static_cast<uint8_t>(
                (static_cast<uint64_t>(cached_now_ms_ / 1000) >> kLruClockShift) & 0x1f);

        // The WB batch captured its AOF gate before entering this call. Only a clean fresh-task
        // turn may put that WB work into an EX prefetch gap: executing older retry/deferred debt
        // first could make a newly Done prefix eligible for the already-gated batch. Exceptional
        // control state therefore consumes WB immediately and continues in the ordinary coarse EX
        // order. The saturated steady path has all five queues empty and reaches the split below.
        if constexpr (HasFiller) {
            if (lb_frozen || snapshot_owner_state_ != SnapshotOwnerState::None ||
                !stale_tasks_.empty() || !multi_retries_.empty() ||
                !atomic_deferred_.empty() || !xshard_retries_.empty() ||
                !ordered_deferred_.empty())
                finish_filler();
        }

        [[maybe_unused]] bool owner_work_remains = false;
        bool fairlane_turn = false;
        if constexpr (InterleaveLocalReads) {
            static_assert(Fused && ConsumeTasks);
            static_assert(BatchOps == kReadLocalOwnerTaskChunkOps);
            if (!read_local_enabled()) std::abort();
            // Exceptional debt keeps its established total order. The ordinary saturated turn is
            // the only place that caps fresh owner work before WB.
            fairlane_turn = !lb_frozen && !fairlane_owner_debt_pending();
            if (fairlane_turn) {
                compact_local_read_tombstones();
                fairlane_turn = read_local_impl().lane_count != 0;
            }
        }
        uint32_t did = 0;
        if (fairlane_turn) {
            // EARLY: reads parsed by the preceding IFID phase get the first execution/reply slots.
            did += drain_local_reads_bounded(kReadLocalDrainChunkOps);
        } else {
            did += drain_local_reads();
        }
        if (lb_frozen) {
            if (!srv_->lb_acked(self_->id())) {
                did += service_stale_forwards<BatchOps, IofusedPrivateQueue>();
                did += drain_releases(true);
                did += service_multi_retries<IofusedPrivateQueue>();
                did += service_atomic_deferred<IofusedPrivateQueue>();
                did += service_xshard_retries<IofusedPrivateQueue>();
                if (xshard_retries_.empty())
                    did += service_ordered_deferred<BatchOps, IofusedPrivateQueue>();
                if constexpr (ConsumeTasks)
                    if (xshard_retries_.empty() && ordered_deferred_.empty()) {
                        if constexpr (HasFiller)
                            did += drain_tasks_with_filler<BatchOps, IofusedPrivateQueue>(
                                true, *filler, filler_used);
                        else
                            did += drain_tasks<BatchOps, IofusedPrivateQueue>(true);
                    }
                flush_xshard_commits();
                did += aof_flush_pass();
                did += drain_notify_keyless(self_->sig());
            }
            finish_filler();
            did += ring_.for_each_cqe([&](io_uring_cqe* cqe) { on_cqe(cqe); });
            did += lb_control_pass();
        } else {
            did += snapshot_control_pass<BatchOps, IofusedPrivateQueue>();
            did += service_stale_forwards<BatchOps, IofusedPrivateQueue>();
            did += drain_releases();
            if (!snapshot_blocks_tasks()) {
                did += service_multi_retries<IofusedPrivateQueue>();
                did += service_atomic_deferred<IofusedPrivateQueue>();
                did += service_xshard_retries<IofusedPrivateQueue>();
                if (xshard_retries_.empty())
                    did += service_ordered_deferred<BatchOps, IofusedPrivateQueue>();
                if constexpr (ConsumeTasks)
                    if (xshard_retries_.empty() && ordered_deferred_.empty()) {
                        if (snapshot_owner_state_ == SnapshotOwnerState::None) {
                            if constexpr (HasFiller)
                                did += drain_tasks_with_filler<BatchOps, IofusedPrivateQueue>(
                                    false, *filler, filler_used);
                            else {
                                if (fairlane_turn)
                                    did += drain_tasks_read_local_interleaved<
                                        IofusedPrivateQueue>(false, owner_work_remains);
                                else
                                    did += drain_tasks<BatchOps, IofusedPrivateQueue>();
                            }
                        } else {
                            did += drain_tasks_snapshot<BatchOps, IofusedPrivateQueue>();
                        }
                    }
            }
            finish_filler();
            flush_xshard_commits();
            if (__builtin_expect(srv_->blocking_waiters() != 0, false) &&
                cached_now_ms_ >= blocking_beat_ms_) {
                did += blocking_owner_cycle(*srv_, *self_, ring_, cached_now_ms_, true);
                blocking_beat_ms_ = cached_now_ms_ + 10;
            }
            did += aof_flush_pass();
            did += ring_.for_each_cqe([&](io_uring_cqe* cqe) { on_cqe(cqe); });
            did += lb_control_pass();
            lb_bucket_bytes_pass();
        }
        if (fairlane_turn) {
            owner_work_remains |= fairlane_owner_debt_pending();
            if (!owner_work_remains) did += drain_local_read_tail();
            // Parser-created tombstones must not cross the WB corpse-grace boundary.
            compact_local_read_tombstones();
        }
        if (read_local_enabled()) {
            did += read_local_impl().deferred.drain_ready();
            // One rotation of the lane-admission pressure window has elapsed (P128.md).
            if (__builtin_expect(read_local_impl().lane_pressure != 0, false))
                read_local_impl().lane_pressure--;
            // Adopt the test lane cap, if one is set. ONE relaxed load per ROTATION of a
            // read-mostly word that production never writes (so it stays shared-clean in every
            // core's L1), against ~1000 ops per rotation, and the store happens only when the
            // value actually changes -- the admission path itself keeps reading a plain uint16 on
            // a line it already owns. 0 means derive, which is what production always sees.
            const uint32_t cap = srv_->debug_read_local_lane_cap();
            const uint16_t want = cap == 0 ? static_cast<uint16_t>(kInboxSlots)
                                           : static_cast<uint16_t>(std::min<uint32_t>(
                                                 cap, kInboxSlots));
            if (__builtin_expect(read_local_impl().lane_admit_cap != want, false))
                read_local_impl().lane_admit_cap = want;
        }
        if (did) {
            did += drain_notify_keyless(self_->sig());
            fused_submit_boundary<CoalesceSubmit>();
            fused_idle_spins_ = 0;
            return did;
        }
        if constexpr (!ConsumeTasks) return 0;
        if (lb_frozen) return 0;
        if (++fused_idle_spins_ < kExSpinBudget) return 0;
        fused_idle_spins_ = 0;
        if constexpr (InterleaveLocalReads) {
            did = fairlane_turn
                ? sweep<BatchOps, ConsumeTasks, IofusedPrivateQueue, true>()
                : sweep<BatchOps, ConsumeTasks, IofusedPrivateQueue>();
        } else {
            did = sweep<BatchOps, ConsumeTasks, IofusedPrivateQueue>();
        }
        if (did) fused_submit_boundary<CoalesceSubmit>();
        return did;
    }

    uint32_t fused_sweep(bool consume_tasks = true) {
        static_assert(Fused);
        if (!consume_tasks) {
            if (lb_controller_armed_ && srv_->lb_dispatch_paused())
                return iofused_
                    ? fused_pass_impl<kGenthreadPipelineExBatchOps, false, true, true>()
                    : fused_pass_impl<kGenthreadPipelineExBatchOps, false, false>();
            if (!pipeline_batches_)
                return fused_sweep_impl<kGenthreadExBatchOps, false, false>();
            return iofused_
                ? fused_sweep_impl<kGenthreadPipelineExBatchOps, false, true, true>()
                : fused_sweep_impl<kGenthreadPipelineExBatchOps, false, false>();
        }
        if (!pipeline_batches_)
            return fused_sweep_impl<kGenthreadExBatchOps, true, false>();
        return iofused_
            ? fused_sweep_impl<kGenthreadPipelineExBatchOps, true, true, true>()
            : fused_sweep_impl<kGenthreadPipelineExBatchOps, true, false>();
    }

    uint32_t fused_baseline_sweep() {
        static_assert(Fused);
        if (read_local_enabled())
            return fused_sweep_impl<kGenthreadExBatchOps, true, false, false, true>();
        return fused_sweep_impl<kGenthreadExBatchOps, true, false>();
    }

    uint32_t fused_coarse_sweep() {
        static_assert(Fused);
        return fused_sweep_impl<kGenthreadPipelineExBatchOps, true, true, true>();
    }

    uint32_t fused_pipeline_control_sweep() {
        static_assert(Fused);
        return fused_sweep_impl<kGenthreadPipelineExBatchOps, false, false>();
    }

    template <uint32_t BatchOps, bool ConsumeTasks, bool CoalesceSubmit,
              bool IofusedPrivateQueue = false, bool InterleaveLocalReads = false>
    uint32_t fused_sweep_impl() {
        if (lb_controller_armed_ && srv_->lb_dispatch_paused())
            return fused_pass_impl<BatchOps, ConsumeTasks, CoalesceSubmit,
                                   IofusedPrivateQueue>();
        struct RotationBoundary {
            bool enabled;
            ThreadCtx* self;
            Server* server;
            ~RotationBoundary() {
                if (enabled) self->publish_read_local_tick(server->read_local_epoch());
            }
        } rotation_boundary{read_local_enabled(), self_, srv_};
        cached_now_ms_ = realtime_ms();
        if (lb_rebind_pending_) read_local_rebind_owned_shards_after_lb();
        uint32_t did = 0;
        if constexpr (InterleaveLocalReads) {
            if (!read_local_enabled()) std::abort();
            if (fairlane_owner_debt_pending()) {
                did += drain_local_reads() +
                    sweep<BatchOps, ConsumeTasks, IofusedPrivateQueue>();
            } else {
                compact_local_read_tombstones();
                if (read_local_impl().lane_count) {
                    did += drain_local_reads_bounded(kReadLocalDrainChunkOps);
                    did += sweep<BatchOps, ConsumeTasks, IofusedPrivateQueue, true>();
                } else {
                    did += sweep<BatchOps, ConsumeTasks, IofusedPrivateQueue>();
                }
            }
        } else {
            did += drain_local_reads() +
                sweep<BatchOps, ConsumeTasks, IofusedPrivateQueue>();
        }
        if (read_local_enabled()) did += read_local_impl().deferred.drain_ready();
        if (did) fused_submit_boundary<CoalesceSubmit>();
        return did;
    }

    void fused_snapshot_start(SnapshotManager* manager) {
        static_assert(Fused);
        begin_snapshot(manager);
    }

    Ring& ring() { return ring_; }

    void run() {
        LoopSignals& sig = self_->sig();
        uint32_t idle_spins = 0;

        while (!self_->stop_flag().load(std::memory_order_relaxed) &&
               self_->role() == Role::Ex) {
            cached_now_ms_ = realtime_ms();
            const bool flip_frozen = srv_->flip_stage() >= FlipStage::ExDrain;
            const bool lb_frozen = lb_controller_armed_ && srv_->lb_dispatch_paused();
            const bool placement_frozen = flip_frozen || lb_frozen;
            // refresh_live_config() walks this owner's shard vector. The coordinator may rewrite
            // those vectors after ExDrain acknowledgement, so a frozen executor must not even run
            // the otherwise-cold configuration refresh path.
            if (!placement_frozen) refresh_live_config();
            if (maxmemory_enabled_)
                cached_lru_clock_ = static_cast<uint8_t>(
                    (static_cast<uint64_t>(cached_now_ms_ / 1000) >> kLruClockShift) & 0x1f);
            sig.iterations++;

            uint32_t did = 0;
            uint64_t pass_ns = 0;
            {
                Span busy(pass_ns);
                if (self_->sample_depth(busy.start_ns() / 1000)) {
                    const uint32_t age_rate = srv_->effective_age_sample_rate();
                    if (age_rate != age_sample_rate_cached_) {
                        age_sample_rate_cached_ = age_rate;
                        self_->sig().configure_age_sampling(age_rate);
                    }
                }
                if (placement_frozen) {
                    // Once ExDrain is acknowledged this loop is a hard safe point: no expiry,
                    // cleanup, waiter walk, or task can reacquire a moved FlatStore before FLIP
                    // publishes ExInstall. The coordinator may rewrite owner entries immediately
                    // after observing the acknowledgement.
                    const bool acknowledged = flip_frozen
                        ? srv_->flip_acked(self_->id(), FlipStage::ExDrain)
                        : srv_->lb_acked(self_->id());
                    if (!acknowledged) {
                        did += service_stale_forwards();
                        did += drain_releases(true);
                        did += service_multi_retries();
                        did += service_atomic_deferred();
                        did += service_xshard_retries();
                        if (xshard_retries_.empty()) did += service_ordered_deferred();
                        if (xshard_retries_.empty() && ordered_deferred_.empty())
                            did += drain_tasks(true);
                        flush_xshard_commits();
                        did += aof_flush_pass();
                        did += drain_notify_keyless(sig);
                    }
                    did += ring_.for_each_cqe([&](io_uring_cqe* cqe) { on_cqe(cqe); });
                    did += flip_control_pass();
                    did += lb_control_pass();
                } else {
                    did += snapshot_control_pass();
                    did += service_stale_forwards();
                    did += drain_releases();
                    if (!snapshot_blocks_tasks()) {
                        did += service_multi_retries();
                        did += service_atomic_deferred();
                        did += service_xshard_retries();
                        if (xshard_retries_.empty()) did += service_ordered_deferred();
                        if (xshard_retries_.empty() && ordered_deferred_.empty())
                            did += snapshot_owner_state_ == SnapshotOwnerState::None
                                       ? drain_tasks() : drain_tasks_snapshot();
                    }
                    flush_xshard_commits();
                    if (__builtin_expect(srv_->blocking_waiters() != 0, false) &&
                        cached_now_ms_ >= blocking_beat_ms_) {
                        did += blocking_owner_cycle(
                            *srv_, *self_, ring_, cached_now_ms_, true);
                        blocking_beat_ms_ = cached_now_ms_ + 10;
                    }
                    did += aof_flush_pass();
                    did += ring_.for_each_cqe([&](io_uring_cqe* cqe) { on_cqe(cqe); });
                    did += flip_control_pass();
                    did += lb_control_pass();
                    lb_bucket_bytes_pass();
                }
            }
            // A pass that found nothing -- every drain and control pass came back empty -- is
            // polling, not work. Book it as idle so busy_ns means WORK: the FLIP placement model
            // reads the roles' busy shares, and an executor spinning its 2048-pass budget between
            // task batches would otherwise report the polling as demand (measured on 8-key
            // MGET/MSET at 2:2 of 4: ex 97% "busy", three quarters of its passes empty; the model
            // read io = 0.73 of a 0.47 workload). One local and one branch per pass.
            if (did) sig.busy_ns += pass_ns; else sig.idle_ns += pass_ns;
            sig.cpu_ns = thread_cpu_ns();

            // Flush prepared SQEs before looping. Recv re-arms and cross-ring wakes are
            // PREPARED during the work section but only reach the kernel on submit; taking
            // the busy path without submitting strands them in the SQ forever, and the peer
            // that is waiting on that wake never runs.
            if (did) {
                did += drain_notify_keyless(sig);
                ring_.submit_and_reap(); idle_spins = 0; continue;
            }

            // A frozen executor must never fall through to sweep(): sweep owns expiry, MVCC
            // cleanup and blocking-waiter walks, any of which can touch a shard after ExDrain's
            // acknowledgement. Before the acknowledgement keep polling internal retry debt; after
            // it, sleep only on the ring the coordinator wakes at every stage publication.
            if (placement_frozen) {
                const bool acknowledged = flip_frozen
                    ? srv_->flip_acked(self_->id(), FlipStage::ExDrain)
                    : srv_->lb_acked(self_->id());
                if (!acknowledged) {
                    __builtin_ia32_pause();
                    continue;
                }
                Span idle(sig.idle_ns);
                self_->arm_blocked();
                ring_.submit_and_wait(1);
                self_->clear_blocked();
                continue;
            }

            if (++idle_spins < kExSpinBudget) { sig.spins++; __builtin_ia32_pause(); continue; }
            idle_spins = 0;

            // Mask-independent sweep before parking. The mask is a hint for the hot path; it must
            // not be the only thing that can find queued work, or one lost bit wedges a connection
            // forever. Runs only when this thread has already concluded it has nothing to do.
            if (sweep()) { ring_.submit_and_reap(); continue; }

            Span idle(sig.idle_ns);
            self_->arm_blocked();
            if (!self_->any_ex_inbound()) ring_.submit_and_wait(1);
            else                       ring_.submit_and_reap();
            self_->clear_blocked();
        }
    }

private:
    friend class IoLoop;

    bool read_local_enabled() const {
        if constexpr (Fused) return read_local_.impl != nullptr;
        return false;
    }

    bool fairlane_owner_debt_pending() const {
        return srv_->flip_dispatch_paused() ||
            (lb_controller_armed_ && srv_->lb_dispatch_paused()) ||
            snapshot_owner_state_ != SnapshotOwnerState::None ||
            !stale_tasks_.empty() || !stale_releases_.empty() ||
            !multi_retries_.empty() || !atomic_deferred_.empty() ||
            !xshard_retries_.empty() || !ordered_deferred_.empty();
    }

    ReadLocalExImpl& read_local_impl() {
        static_assert(Fused);
        if (!read_local_.impl) std::abort();
        return *read_local_.impl;
    }

    const ReadLocalExImpl& read_local_impl() const {
        static_assert(Fused);
        if (!read_local_.impl) std::abort();
        return *read_local_.impl;
    }

    bool read_local_point_writes_precise() const {
        return read_local_impl().point_writes_precise;
    }

    void set_read_local_point_writes_precise(bool precise) {
        read_local_impl().point_writes_precise = precise;
    }

    void read_local_rebind_owned_shards_after_lb() {
        if constexpr (Fused) {
            if (read_local_.impl) read_local_.impl->rebind_owned_shards(*self_);
        }
    }

    template <bool CoalesceSubmit>
    void fused_submit_boundary() {
        if constexpr (!CoalesceSubmit) {
            ring_.submit_and_reap();
        } else {
            if (ring_.take_sq_full_submit()) fused_non_submit_rotations_ = 0;
            if (++fused_non_submit_rotations_ >= kGenthreadIoFusedCoalesceRotations) {
                ring_.submit_and_reap();
                fused_non_submit_rotations_ = 0;
            }
        }
    }

    bool pipeline_tasks_allowed() const {
        if (snapshot_blocks_tasks()) return false;
        return !(lb_controller_armed_ && srv_->lb_dispatch_paused() &&
                 srv_->lb_acked(self_->id()));
    }

    Ring& handoff_ring() { return *fused_handoff_ring_; }

    void read_local_clear_reply(Op& op) {
        op.clear_reply();       // bytes AND the reply code
        op.zc_ptr = nullptr;
        op.zc_len = 0;
        op.zc_shard = -1;
    }

    void compact_local_read_tombstones() {
        if constexpr (!Fused) return;
        auto& state = read_local_impl();
        if (!state.lane_has_tombstones) return;
        uint32_t kept = 0;
        const uint32_t old_count = state.lane_count;
        for (uint32_t offset = 0; offset < old_count; offset++) {
            const uint32_t from =
                (state.lane_head + offset) & (kInboxSlots - 1);
            const ReadLocalExImpl::LaneEntry task = state.lane[from];
            // Test the full generation before touching its Op slot. The fused pass sweeps any
            // parser-created tombstone before WB's corpse-grace reaper can free its Client.
            if (!task.client || !task.client->rob().pending_read_local(task.op_id)) continue;
            const uint32_t to = (state.lane_head + kept) & (kInboxSlots - 1);
            state.lane[to] = task;
            state.lane_fallbacks[to] = state.lane_fallbacks[from];
            kept++;
        }
        state.lane_count = kept;
        state.lane_tail = (state.lane_head + kept) & (kInboxSlots - 1);
        state.lane_has_tombstones = false;
    }

    bool demote_local_read_batch(Client* client, const uint64_t* probed,
                                 const ReadLocalFallbackReason* fallbacks,
                                 uint32_t probed_count, uint32_t& demoted) {
        static_assert(Fused);
        auto& lane = read_local_impl();
        if (!lane.demote || !lane.demote_context) std::abort();
        return lane.demote(
            lane.demote_context, client, probed, fallbacks, probed_count, demoted);
    }

    struct PreparedLocalRead {
        ReadLocalFallbackReason fallback = ReadLocalFallbackReason::None;
        uint32_t keyspace_hits = 0;
        uint32_t keyspace_misses = 0;
    };

    static bool read_local_mget(const Op& op) {
        return op.spec && command_is_read_local_mget(*op.spec);
    }

    static bool read_local_reply_string(Op& op, const KvObj* object, uint8_t stable_flags) {
        const Enc encoding = object->encoding();
        if constexpr (kReadLocalSetTaxAtomicRaw) {
            if (encoding == Enc::Raw) {
                [[maybe_unused]] uint32_t sequence = 0;
                if constexpr (kReadLocalSetTaxVariant ==
                              ReadLocalSetTaxVariant::ObjectSequenceOverwrite) {
                    sequence = object->raw_sequence_acquire();
                }
                // Selector 3 may copy after observing odd: both the bounded length and fixed cells
                // are atomic, and both old/new lengths fit this allocation class. Combining odd
                // with the final mismatch therefore leaves the stable GET path one branch.
                const uint32_t length = kvobj_read_local_raw_length(object);
                auto sink = op.sink();
                char* frame = sink.reserve(24 + static_cast<size_t>(length) + 2);
                char* payload = frame;
                *payload++ = '$';
                payload += u64_to_dec(payload, length);
                *payload++ = '\r';
                *payload++ = '\n';
                kvobj_read_local_copy_raw(object, stable_flags, length, payload);
                payload += length;
                *payload++ = '\r';
                *payload++ = '\n';
                if constexpr (kReadLocalSetTaxVariant ==
                              ReadLocalSetTaxVariant::ObjectSequenceOverwrite) {
                    // Keep every payload load before the confirming sequence load. Saturating the
                    // writer sequence makes equality an ABA-free validation even across preemption.
                    std::atomic_thread_fence(std::memory_order_acquire);
                    const uint32_t confirmed = object->raw_sequence_relaxed();
                    if (((confirmed ^ sequence) | (sequence & 1u)) != 0) return false;
                }
                sink.advance(static_cast<size_t>(payload - frame));
                return true;
            }
        }
        reply_bulk(op.sink(), object->read_local_str_value(stable_flags));
        return true;
    }

    uint32_t read_local_task_demotion_demand(const Op& op) const {
        if (!op.read_local()) std::abort();
        return read_local_mget(op)
            ? std::min<uint32_t>(op.argc() - 1, srv_->nshards()) : 1;
    }

    void release_local_read_demotion_demand(const Op& op) {
        auto& lane = read_local_impl();
        const uint32_t demand = read_local_task_demotion_demand(op);
        if (demand > lane.lane_demotion_demand) std::abort();
        lane.lane_demotion_demand -= demand;
    }

    // Command-wide window validator for a local MGET. With the per-key filter armed, every queried
    // cell is snapshotted before the first value load and re-read exactly once after the complete
    // reply is private. Atomic exchanges still bracket the table word for point GET, but an exchange
    // of an unrelated key cannot move this key's epoch. Each probe retains its stable-topology
    // handshake; immutable replacement plus rotation QSBR then makes a second per-key table-word
    // validation redundant. Above the bounded route cache,
    // every shard keeps the legacy generation rule and per-key validation.
    struct LocalMgetWindow {
        static constexpr uint32_t kMaxShards = 256;
        static constexpr uint32_t kMaxEpochKeys = 128;
        uint64_t states[kMaxShards];
        uint32_t epochs[kMaxEpochKeys];
        bool use_epochs = false;
    };

    static bool local_mget_touched(const uint64_t* touched, uint32_t sid) {
        return (touched[sid >> 6] & (uint64_t{1} << (sid & 63))) != 0;
    }

    // All window loads precede every value load. Returns None when the window is open.
    ReadLocalFallbackReason local_mget_window_open(
            LocalMgetWindow& window, const uint64_t* touched, const uint64_t* hashes,
            const int32_t* shards, uint32_t key_count, bool cached_routes) const {
        window.use_epochs = cached_routes;
        for (uint32_t sid = 0; sid < srv_->nshards(); sid++) {
            if (!local_mget_touched(touched, sid)) continue;
            const uint64_t state = srv_->shard(static_cast<int32_t>(sid))
                                       .store()
                                       .read_local_state_acquire();
            if (FlatStore::read_local_generation_poisoned(state))
                return ReadLocalFallbackReason::Generation;
            window.states[sid] = state;
            // The filter-on path lets each probe perform the topology handshake. The generation
            // control must be even now so an equal even word later proves no bracket ran between.
            if (!window.use_epochs && FlatStore::read_local_table_mutating(state))
                return ReadLocalFallbackReason::SeqChurn;
        }
        if (!window.use_epochs) return ReadLocalFallbackReason::None;
        for (uint32_t key = 0; key < key_count; key++) {
            window.epochs[key] = srv_->shard(shards[key]).store().foreign_read_cell_epoch(
                hashes[key]);
        }
        return ReadLocalFallbackReason::None;
    }

    // All window loads follow every value load. Returns None when every participant is unchanged.
    ReadLocalFallbackReason local_mget_window_close(
            const LocalMgetWindow& window, const uint64_t* touched, const uint64_t* hashes,
            const int32_t* shards, uint32_t key_count) const {
        // Formal seqlock hygiene: keep the value loads above on the near side of the re-reads
        // below. Free on x86.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!window.use_epochs) {
            for (uint32_t sid = 0; sid < srv_->nshards(); sid++) {
                if (!local_mget_touched(touched, sid)) continue;
                const uint64_t after = srv_->shard(static_cast<int32_t>(sid))
                                           .store()
                                           .read_local_state_acquire();
                if (!FlatStore::read_local_state_eligible(after))
                    return ReadLocalFallbackReason::SeqChurn;
                // A group that published its filter but installed nothing leaves the generation
                // equal and only raises the pending bit; the command linearizes before it.
                if (!FlatStore::read_local_generation_equal(window.states[sid], after))
                    return ReadLocalFallbackReason::Generation;
            }
            return ReadLocalFallbackReason::None;
        }
        for (uint32_t key = 0; key < key_count; key++) {
            if (srv_->shard(shards[key]).store().foreign_read_cell_epoch(hashes[key]) !=
                window.epochs[key])
                return ReadLocalFallbackReason::Generation;
        }
        return ReadLocalFallbackReason::None;
    }

    // After the bounded attempts, prefer the actionable atomic reason if one queried key won the
    // final race; otherwise report the last transient reason observed.
    ReadLocalFallbackReason local_mget_final_reason(
            const Op& op, const uint64_t* hashes, const int32_t* shards, uint32_t key_count,
            bool cached_routes, ReadLocalFallbackReason transient) const {
        for (uint32_t key = 0; key < key_count; key++) {
            const uint64_t hash = cached_routes
                ? hashes[key] : FlatStore::hash_key(op.arg(key + 1));
            const int32_t shard_id = cached_routes
                ? shards[key] : srv_->router().shard_of(hash);
            const FlatStore& store = srv_->shard(shard_id).store();
            const uint64_t state = store.read_local_state_acquire();
            if (store.foreign_read_key_unsafe(state, hash))
                return ReadLocalFallbackReason::AtomicPending;
        }
        return transient;
    }

    // What note_local_read_access() records for a lane-served read. Not the MaxmemoryPolicy enum:
    // every policy that keys on nothing a read changes collapses to None here, so the cold helper
    // dispatches on one byte instead of re-deriving the classification per key.
    static constexpr uint8_t kForeignTouchNone = 0;
    static constexpr uint8_t kForeignTouchLru  = 1;
    static constexpr uint8_t kForeignTouchLfu  = 2;

    // Eviction accounting for one key served on the read-local lane, and the CLIENT NO-TOUCH
    // answer for it. The owner does both for every key it serves (FlatStore::find -> touch(), and
    // the per-task climon_note_no_touch() in execute_task); a lane-served read that did neither
    // made the two paths disagree about what a read MEANS:
    //   * LFU/LRU: a key kept hot purely by lane-served reads was never counted as accessed, so
    //     the victim chooser saw it as the coldest thing in the shard and evicted it FIRST -- the
    //     policy inverted (tests/evict_battery.py lfu, lruclock).
    //   * CLIENT NO-TOUCH: the flag was never consulted here, so "suppressed" and "never looked"
    //     were indistinguishable on the wire (tests/climon2.py, client_no_touch_ops).
    // The call sites gate on maxmemory_enabled_ -- the SAME per-pass byte the owner path tests --
    // so the default posture pays one predicted-not-taken branch per key and never loads the
    // policy, the no-touch bit, or the LRU clock. Out of line and cold: reaching it at all means
    // maxmemory is armed, and then this is a cache-cold decision on a path whose whole point is to
    // stay inside this thread's own lines.
    //
    // The arithmetic below is FlatStore::touch()'s, with the two differences foreignness forces:
    // the logarithmic-increment dice are this thread's own (the store's PRNG belongs to its owner
    // and is not safe to advance from here), and the meta write is one unretried CAS
    // (KvObj::touch_eviction_meta_foreign) rather than the owner's load/store pair, so a lost race
    // costs one approximate touch -- which LRU and LFU tolerate by construction -- and can never
    // corrupt a layout bit. Store-rare by construction: LRU writes only when the key's bucket is
    // stale (once per 256 seconds per key) and LFU only when the
    // increment actually fires, so the foreign RFO into the owner's line is the exception rather
    // than the per-read rule.
    __attribute__((noinline, cold))
    void note_local_read_access(const Op& op, const KvObj* object, uint8_t flags) {
        if (op.no_touch()) { srv_->climon_note_no_touch(); return; }
        KvObj* touched = const_cast<KvObj*>(object);
        const uint8_t meta = static_cast<uint8_t>(flags >> 3);
        if (foreign_touch_policy_ == kForeignTouchLru) {
            if (meta != cached_lru_clock_)
                touched->touch_eviction_meta_foreign(flags, cached_lru_clock_);
            return;
        }
        // Random/TTL/noeviction key on nothing a read changes, so there is nothing to record --
        // but the NO-TOUCH count above still has to happen, exactly as it does on the owner.
        if (foreign_touch_policy_ != kForeignTouchLfu) return;
        uint8_t count = meta ? meta : 5;
        const uint32_t base = count > 5 ? static_cast<uint32_t>(count - 5) : 0;
        const uint32_t denominator = base * 10 + 1;
        uint64_t x = foreign_touch_random_;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        foreign_touch_random_ = x;
        if (count < 31 && (x * 2685821657736338717ULL) % denominator == 0) count++;
        if (count != meta) touched->touch_eviction_meta_foreign(flags, count);
    }

    // TEST HOOK (DEBUG ATOMIC-FANOUT-DEFER), the read-local half. A fused read-local MGET never
    // enters the scatter engine, so the fan-out park there widens nothing for it and the one
    // expiry-cut invariant expwide.py S1 polices went unexercised on the path that serves every
    // clean MGET in the armed posture. This is the same window on that path: the cut
    // (command_now_ms) is already pinned, no value has been loaded, and the hook holds the lane
    // open between the two. It STALLS rather than parks because the local lane is synchronous --
    // there is no fragment to re-queue and nothing else can run on this thread meanwhile -- and the
    // stall is the point: every key is then read after the wall clock has passed a deadline the
    // pinned cut still says is ahead, which is exactly the tear a per-key clock would produce.
    // Cold and out of line; the caller's test is one thread-private word latched per pass, so an
    // unarmed MGET pays a predicted-not-taken branch and no atomic load (see LiveConfigSnapshot).
    __attribute__((noinline, cold)) void debug_fanout_stall_local() {
        Server::debug_stall_us(debug_fanout_defer_us_);
    }

    PreparedLocalRead prepare_captured_local_mget(Op& op) {
        static constexpr uint32_t kAttempts = 2;
        const uint32_t key_count = op.argc() - 1;
        if (!key_count || srv_->nshards() > LocalMgetWindow::kMaxShards) std::abort();

        uint64_t route_hashes[LocalMgetWindow::kMaxEpochKeys];
        int32_t route_shards[LocalMgetWindow::kMaxEpochKeys];
        const bool cached_routes = key_count <= LocalMgetWindow::kMaxEpochKeys;
        uint64_t touched[LocalMgetWindow::kMaxShards / 64] = {};
        for (uint32_t key = 0; key < key_count; key++) {
            const uint64_t hash = FlatStore::hash_key(op.arg(key + 1));
            const int32_t shard_id = srv_->router().shard_of(hash);
            touched[static_cast<uint32_t>(shard_id) >> 6] |=
                uint64_t{1} << (static_cast<uint32_t>(shard_id) & 63);
            if (cached_routes) {
                route_hashes[key] = hash;
                route_shards[key] = shard_id;
            }
        }
        const int64_t command_now_ms = cached_now_ms_;
        if (__builtin_expect(debug_fanout_defer_us_ != 0, false)) debug_fanout_stall_local();

        ReadLocalFallbackReason transient = ReadLocalFallbackReason::Generation;
        for (uint32_t attempt = 0; attempt < kAttempts; attempt++) {
            LocalMgetWindow window;
            PreparedLocalRead prepared;
            read_local_clear_reply(op);

            transient = local_mget_window_open(
                window, touched, route_hashes, route_shards, key_count, cached_routes);
            bool retry = transient != ReadLocalFallbackReason::None;
            if (!retry) reply_array_header(op.sink(), key_count);

            for (uint32_t first = 0; first < key_count && !retry;
                 first += kReadLocalPrefetchKeys) {
                const uint32_t count = std::min<uint32_t>(
                    key_count - first, kReadLocalPrefetchKeys);
                uint64_t hashes[kReadLocalPrefetchKeys];
                int32_t shards[kReadLocalPrefetchKeys];
                ReadLocalCaptureBuffer<kReadLocalPrefetchKeys> captures;

                // I0 warms every home word in this bounded window. C0 then performs the complete
                // key-verified walk and prefetches the exact object's value before E0 copies it.
                for (uint32_t offset = 0; offset < count; offset++) {
                    if (cached_routes) {
                        hashes[offset] = route_hashes[first + offset];
                        shards[offset] = route_shards[first + offset];
                    } else {
                        hashes[offset] = FlatStore::hash_key(op.arg(first + offset + 1));
                        shards[offset] = srv_->router().shard_of(hashes[offset]);
                    }
                    srv_->shard(shards[offset]).store().read_local_prefetch(hashes[offset]);
                }
                for (uint32_t offset = 0; offset < count; offset++) {
                    captures.entries[offset] =
                        srv_->shard(shards[offset]).store().read_local_prefetch_capture(
                            hashes[offset], op.arg(first + offset + 1));
                }

                for (uint32_t offset = 0; offset < count && !retry; offset++) {
                    const int32_t shard_id = shards[offset];
                    FlatStore& store = srv_->shard(shard_id).store();
                    const FlatStore::ReadLocalPrefetchCapture& capture =
                        captures.entries[offset];
                    if (capture.result == FlatStore::ReadLocalProbeResult::AtomicPending) {
                        read_local_clear_reply(op);
                        return {ReadLocalFallbackReason::AtomicPending};
                    }
                    if (capture.result == FlatStore::ReadLocalProbeResult::Churn) {
                        transient = ReadLocalFallbackReason::SeqChurn;
                        retry = true;
                        break;
                    }
                    if (capture.result == FlatStore::ReadLocalProbeResult::Missing) {
                        reply_null(op.sink(), op.resp3());
                        prepared.keyspace_misses++;
                        if (!window.use_epochs && !store.read_local_validate(capture.state)) {
                            transient = ReadLocalFallbackReason::SeqChurn;
                            retry = true;
                        }
                        continue;
                    }

                    const KvObj* object = capture.object;
                    if (!capture.slot || !object) std::abort();
                    const uint8_t flags = object->read_local_flags();
                    if (static_cast<Type>(object->type) != Type::String) {
                        read_local_clear_reply(op);
                        return {ReadLocalFallbackReason::Typed};
                    }
                    if (flags & KvObjFlags::HasTtl) {
                        const int64_t deadline = object->read_local_expire_at_ms(flags);
                        if (deadline >= 0 && deadline <= command_now_ms) {
                            read_local_clear_reply(op);
                            return {ReadLocalFallbackReason::Expired};
                        }
                    }

                    const Enc encoding = object->encoding();
                    if (encoding == Enc::Int) {
                        char text[24];
                        const uint32_t length = i64_to_dec(
                            text, object->read_local_int_value(flags));
                        reply_bulk(op.sink(), Slice(text, length));
                    } else if (encoding == Enc::Raw || encoding == Enc::Extern) {
                        if (!read_local_reply_string(op, object, flags)) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                            self_->read_local_stats().settax.object_sequence_retries++;
#endif
                            transient = ReadLocalFallbackReason::SeqChurn;
                            retry = true;
                            break;
                        }
                    } else {
                        read_local_clear_reply(op);
                        return {ReadLocalFallbackReason::Typed};
                    }
                    prepared.keyspace_hits++;
                    // Account only for a key this pass accepted, before any retry or demotion.
                    if (!window.use_epochs && !store.read_local_validate(capture.state)) {
                        transient = ReadLocalFallbackReason::SeqChurn;
                        retry = true;
                    } else if (__builtin_expect(maxmemory_enabled_, false)) {
                        note_local_read_access(op, object, flags);
                    }
                }
            }

            // C0 performs each probe's topology validation. This one outer close then validates
            // every queried cell across the complete multi-shard capture/copy interval.
            if (!retry) {
                transient = local_mget_window_close(
                    window, touched, route_hashes, route_shards, key_count);
                retry = transient != ReadLocalFallbackReason::None;
            }
            if (!retry) return prepared;
            read_local_clear_reply(op);
            if (attempt + 1 < kAttempts)
                self_->read_local_stats().mget_generation_retries++;
        }
        return {local_mget_final_reason(
            op, route_hashes, route_shards, key_count, cached_routes, transient)};
    }

    // Build one local reply but do not publish Done. `fallback == None` means the bytes are
    // complete and the reply retains no FlatStore pointer. A caller-owned capture may remain on
    // this rotation's stack until drain_local_reads_bounded_impl returns. Selective commit below
    // keeps an overlapping younger read behind any operation that needs the owner path.
    // `op`, `store` and `mget` were resolved once by the chunk gather; a point read passes its
    // home store, an MGET passes null and takes its own multi-store path.
    PreparedLocalRead prepare_local_read(
            Op& op, FlatStore* home, bool mget,
            const FlatStore::ReadLocalPrefetchCapture* captured = nullptr) {
        if (mget) {
            if (captured) std::abort();
            return prepare_captured_local_mget(op);
        }
        FlatStore& store = *home;
        static constexpr uint32_t kRetries = 3;
        ReadLocalCaptureBuffer<1> local_capture;
        // A mixed GET/MGET chunk executes in program order. Its point reads capture here so a
        // later GET can never retain a version older than the preceding MGET returned.
        if (!captured) {
            store.read_local_prefetch(op.hash);
            local_capture.entries[0] = store.read_local_prefetch_capture(op.hash, op.key());
            captured = &local_capture.entries[0];
        }

        for (uint32_t attempt = 0; attempt < kRetries; attempt++) {
            const FlatStore::ReadLocalProbeResult result = captured->result;
            const KvObj* object = captured->object;
            const uint64_t probe_state = captured->state;
            if (result == FlatStore::ReadLocalProbeResult::AtomicPending) {
                read_local_clear_reply(op);
                return {ReadLocalFallbackReason::AtomicPending};
            }
            if (result == FlatStore::ReadLocalProbeResult::Missing) {
                read_local_clear_reply(op);
                return {ReadLocalFallbackReason::Missing};
            }
            if (result == FlatStore::ReadLocalProbeResult::Churn) break;

            if (!object) std::abort();
            // Keep the observed word's address as part of the snapshot, but consume only the
            // decoded immutable object. Loading through slot here would chase a newer version.
            if (!captured->slot) std::abort();
            const uint8_t flags = object->read_local_flags();
            if (static_cast<Type>(object->type) != Type::String) {
                read_local_clear_reply(op);
                return {ReadLocalFallbackReason::Typed};
            }
            if (flags & KvObjFlags::HasTtl) {
                const int64_t deadline = object->read_local_expire_at_ms(flags);
                if (deadline >= 0 && deadline <= cached_now_ms_) {
                    read_local_clear_reply(op);
                    return {ReadLocalFallbackReason::Expired};
                }
            }

            read_local_clear_reply(op);
            const Enc encoding = object->encoding();
            if (encoding == Enc::Int) {
                char text[24];
                const uint32_t length = i64_to_dec(
                    text, object->read_local_int_value(flags));
                reply_bulk(op.sink(), Slice(text, length));
            } else if (encoding == Enc::Raw || encoding == Enc::Extern) {
                if (!read_local_reply_string(op, object, flags)) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                    self_->read_local_stats().settax.object_sequence_retries++;
#endif
                    read_local_clear_reply(op);
                    continue;
                }
            } else {
                read_local_clear_reply(op);
                return {ReadLocalFallbackReason::Typed};
            }

            if (!store.read_local_validate(probe_state)) {
                read_local_clear_reply(op);
                break;
            }
            // One predicted-not-taken test on the same per-pass byte the owner path tests, after
            // the validate that makes this read final. See note_local_read_access().
            if (__builtin_expect(maxmemory_enabled_, false))
                note_local_read_access(op, object, flags);
            return {ReadLocalFallbackReason::None, 1, 0};
        }

        read_local_clear_reply(op);
        const uint64_t state = store.read_local_state_acquire();
        if (store.foreign_read_key_unsafe(state, op.hash))
            return {ReadLocalFallbackReason::AtomicPending};
        return {ReadLocalFallbackReason::SeqChurn};
    }

    // Consume at most `op_budget` lane entries in same-client gather quanta. The fair-lane
    // scheduler owns tombstone compaction around the complete rotation so an interleave does not
    // turn each 32-operation chunk into a scan of the 1024-entry lane.
    template <bool YieldToOwner = false>
    uint32_t drain_local_reads_bounded(uint32_t op_budget) {
        if constexpr (!Fused) {
            return 0;
        } else {
            if (!read_local_enabled() || !op_budget) return 0;
            return drain_local_reads_bounded_impl<YieldToOwner>(op_budget);
        }
    }

    template <bool YieldToOwner>
    uint32_t drain_local_reads_bounded_impl(uint32_t op_budget) {
        static_assert(Fused);
        auto& lane = read_local_impl();
        if (!lane.lane_count) return 0;
        // One record per chunk entry. Gather resolves the Op, its home store and its MGET class
        // exactly once; every later pass reads them from here instead of walking the ROB again.
        // Stack-local to this bounded drain, so nothing here outlives the rotation's QSBR tick.
        struct LocalReadChunk {
            Op* ops[kReadLocalDrainChunkOps];
            FlatStore* stores[kReadLocalDrainChunkOps];   // null for an MGET (multi-store)
            uint64_t op_ids[kReadLocalDrainChunkOps];
            ReadLocalFallbackReason fallbacks[kReadLocalDrainChunkOps];
        } chunk;
        static_assert(kReadLocalDrainChunkOps <= 32, "mget_mask is one 32-bit word");
        ReadLocalCaptureBuffer<kReadLocalDrainChunkOps> captures;
        const uint32_t nshards = srv_->nshards();
        ReadLocalStats& stats = self_->read_local_stats();
        uint32_t work = 0;
        uint32_t consumed = 0;
        auto discard_tombstone_heads = [&] {
            while (lane.lane_count) {
                const auto& head = lane.lane[lane.lane_head];
                if (head.client && head.client->rob().pending_read_local(head.op_id)) break;
                lane.lane_head = (lane.lane_head + 1) & (kInboxSlots - 1);
                lane.lane_count--;
            }
        };
        auto owner_turn_pending = [&] {
            if constexpr (YieldToOwner) {
                if (!consumed || consumed % kReadLocalDrainChunkOps) return false;
                return fairlane_owner_debt_pending() ||
                    self_->notified_task_depth_capped(1) != 0;
            }
            return false;
        };
        // Work through each client's lane run in gather/prefetch-sized chunks. A successful chunk
        // can commit immediately. At a fallback, only the intrinsically failing entries and their
        // transitive same-key set move in a ROB-ordered owner wave; unrelated reads remain local.
        discard_tombstone_heads();
        while (lane.lane_count && consumed < op_budget) {
            const auto& head = lane.lane[lane.lane_head];
            Client* client = head.client;
            auto& rob = client->rob();
            uint32_t count = 0;
            uint32_t capacity = std::min<uint32_t>(
                kReadLocalDrainChunkOps, op_budget - consumed);
            if constexpr (YieldToOwner)
                capacity = std::min<uint32_t>(
                    capacity, kReadLocalDrainChunkOps -
                        consumed % kReadLocalDrainChunkOps);
            // Gather: one ROB frontier snapshot for the whole same-client run (see
            // Rob::pending_read_local(id, dispatch, flush)), and each entry's Op, home store and
            // MGET class resolved here once. bit i of mget_mask marks an MGET entry.
            const uint64_t dispatch = rob.dispatch_id();
            const uint64_t flush = rob.flush_id();
            uint32_t mget_mask = 0;
            while (count < lane.lane_count && count < capacity) {
                const uint32_t index =
                    (lane.lane_head + count) & (kInboxSlots - 1);
                const auto& task = lane.lane[index];
                if (task.client != client ||
                    !rob.pending_read_local(task.op_id, dispatch, flush))
                    break;
                Op& op = rob.at(task.op_id);
                const bool mget = read_local_mget(op);
                if (!mget && op.shard < 0) std::abort();
                chunk.ops[count] = &op;
                chunk.stores[count] = mget ? nullptr : &srv_->shard(op.shard).store();
                chunk.op_ids[count] = task.op_id;
                chunk.fallbacks[count] = lane.lane_fallbacks[index];
                mget_mask |= uint32_t{mget} << count;
                count++;
            }
            if (!count) std::abort();
            consumed += count;

            // Pure point chunks retain the widest I0/C0/E0 overlap. A mixed chunk captures and
            // consumes each command in program order below: MGET may retry and recapture, so
            // pre-capturing a following GET could otherwise let that later command regress.
            const bool point_capture_batch = mget_mask == 0;
            if (point_capture_batch) {
                // I0 retains the old whole-batch home-slot overlap. C0 consumes those warm
                // words, records their decoded objects, and hints object/value bytes for E0.
                for (uint32_t i = 0; i < count; i++)
                    chunk.stores[i]->read_local_prefetch(chunk.ops[i]->hash);
                for (uint32_t i = 0; i < count; i++) {
                    const Op& op = *chunk.ops[i];
                    captures.entries[i] =
                        chunk.stores[i]->read_local_prefetch_capture(op.hash, op.key());
                }
            }

            // E0. The owner-map recheck is decided once per chunk: only this thread's parser and
            // demotion add owner slots, and neither runs inside a chunk, so an empty map stays
            // empty for every entry. Prefix keyspace counters accumulate only while no fallback
            // has been seen, which is exactly the prefix complete_local_prefix publishes.
            const bool owner_fence = rob.has_read_local_owner();
            uint32_t first_fallback = count;
            uint32_t prefix_keyspace_hits = 0;
            uint32_t prefix_keyspace_misses = 0;
            uint32_t prefix_mget_hits = 0;
            uint32_t prefix_demand = 0;
            uint64_t prefix_bits = 0;
            for (uint32_t i = 0; i < count; i++) {
                Op& op = *chunk.ops[i];
                const bool mget = ((mget_mask >> i) & 1u) != 0;
                if (owner_fence && chunk.fallbacks[i] == ReadLocalFallbackReason::None) {
                    bool broad_owner = false;
                    const bool owner_conflict =
                        rob.read_local_owner_conflicts_before(
                            chunk.op_ids[i], [&](const Op& owner) {
                                if (!read_local_commands_overlap(op, owner)) return false;
                                broad_owner |= !read_local_owner_command_is_precise(owner);
                                return true;
                            });
                    if (owner_conflict) {
                        chunk.fallbacks[i] = broad_owner
                            ? ReadLocalFallbackReason::ContextRoute
                            : ReadLocalFallbackReason::ContextOwnerKey;
                    }
                }
                if (chunk.fallbacks[i] == ReadLocalFallbackReason::None) {
                    PreparedLocalRead prepared;
                    if (point_capture_batch) {
                        prepared = prepare_local_read(
                            op, chunk.stores[i], false, &captures.entries[i]);
                    } else {
                        prepared = prepare_local_read(op, chunk.stores[i], mget);
                    }
                    chunk.fallbacks[i] = prepared.fallback;
                    if (first_fallback == count &&
                        prepared.fallback == ReadLocalFallbackReason::None) {
                        prefix_keyspace_hits += prepared.keyspace_hits;
                        prefix_keyspace_misses += prepared.keyspace_misses;
                        prefix_mget_hits += mget;
                        prefix_demand += mget
                            ? std::min<uint32_t>(op.argc() - 1, nshards) : 1;
                        prefix_bits |= Rob<kRobWindow>::read_local_slot_bit(chunk.op_ids[i]);
                    }
                }
                if (chunk.fallbacks[i] != ReadLocalFallbackReason::None &&
                    first_fallback == count) first_fallback = i;
            }

            // Completion is chunk-aggregated: one pending-mask clear, one demand release and one
            // add per statistic instead of six read-modify-writes per op; the prefix accumulators
            // above describe exactly the entries `completed` names. The pending bits are cleared
            // before any Done publish, as the per-op order did. note_command and Done stay per op.
            auto complete_local_prefix = [&](uint32_t completed) {
                if (!completed) return;
                rob.complete_pending_read_local_mask(prefix_bits);
                if (prefix_demand > lane.lane_demotion_demand) std::abort();
                lane.lane_demotion_demand -= prefix_demand;
                stats.hits += completed;
                stats.keyspace_hits += prefix_keyspace_hits;
                stats.keyspace_misses += prefix_keyspace_misses;
                stats.mget_local_hits += prefix_mget_hits;
                for (uint32_t i = 0; i < completed; i++) {
                    Op& op = *chunk.ops[i];
                    self_->note_command(op.spec->id);
                    op.state.store(OpState::Done, std::memory_order_release);
                }
                notify_sender(client);
                lane.lane_head = (lane.lane_head + completed) & (kInboxSlots - 1);
                lane.lane_count -= completed;
                work += completed;
            };

            if (first_fallback != count) {
                // Publish the independent older prefix locally. Only the failing command and the
                // transitive same-key set move owner-side; ROB retirement holds every younger Done
                // reply without imposing a connection-wide execution venue.
                complete_local_prefix(first_fallback);
                for (uint32_t i = first_fallback; i < count; i++)
                    read_local_clear_reply(*chunk.ops[i]);
                lane.lane_fallbacks[lane.lane_head] = chunk.fallbacks[first_fallback];
                uint32_t demoted = 0;
                if (!demote_local_read_batch(
                        client, chunk.op_ids + first_fallback,
                        chunk.fallbacks + first_fallback,
                        count - first_fallback, demoted))
                    break;
                work += demoted;
                discard_tombstone_heads();
                if (owner_turn_pending()) break;
                continue;
            }

            complete_local_prefix(count);
            discard_tombstone_heads();
            if (owner_turn_pending()) break;
        }
        self_->sig().ops += work;
        return work;
    }

    uint32_t drain_local_reads() {
        if constexpr (!Fused) return 0;
        if (!read_local_enabled()) return 0;
        compact_local_read_tombstones();
        const uint32_t work = drain_local_reads_bounded(UINT32_MAX);
        compact_local_read_tombstones();
        return work;
    }

    bool flip_quiesced() const {
        if constexpr (Fused) {
            if (read_local_enabled() &&
                (read_local_impl().lane_count != 0 || !read_local_impl().deferred.empty()))
                return false;
        }
        if (snapshot_owner_state_ != SnapshotOwnerState::None ||
            !self_->ex_inbound_quiesced() || !stale_tasks_.empty() ||
            !stale_releases_.empty() || !atomic_deferred_.empty() ||
            !multi_retries_.empty() || !xshard_retries_.empty() ||
            !ordered_deferred_.empty() || xshard_commit_pending_ ||
            notify_keyless_pending_ ||
            srv_->atomic_inflight() != 0 || srv_->atomic_apply_inflight() != 0) return false;
        for (const auto& queue : snapshot_backlogs_) if (!queue.empty()) return false;
        for (Shard* shard : self_->shards()) {
            if (shard->notify_output_pending()) return false;
            if (aof_manager_ && aof_manager_->recording() && shard->store().aof().has_pending())
                return false;
        }
        return true;
    }

    uint32_t flip_control_pass() {
        const FlipStage stage = srv_->flip_stage();
        if (stage == FlipStage::IoPrepare &&
            srv_->flip_candidate_target(self_->id()) == Role::Ifid &&
            !srv_->flip_acked(self_->id(), stage)) {
            if (!self_->prepare_io_role())
                srv_->flip_note_failure("ERR FLIP could not prepare a new IO listener");
            else if (!self_->prepare_client_capacity(srv_->flip_incoming_clients(self_->id())))
                srv_->flip_note_failure("ERR FLIP could not reserve destination connection state");
            srv_->flip_ack(self_->id(), stage);
            return 1;
        }
        if (stage == FlipStage::ExDrain && !srv_->flip_acked(self_->id(), stage) &&
            flip_quiesced()) {
            srv_->flip_ack(self_->id(), stage);
            return 1;
        }
        if (stage == FlipStage::ExInstall && !srv_->flip_acked(self_->id(), stage)) {
            // ExDrain retired every old lane and IO dispatch remains parked until all ExInstall
            // acknowledgements. Reassign blocks in the existing consumer-local allocation now;
            // no topology object is rebuilt and no producer can observe a half-installed mask.
            if (!self_->remask_task_inbox_quiesced(srv_->placement().ifid_threads(),
                                                    srv_->placement().ex_threads()))
                std::abort();
            for (Shard* shard : self_->shards())
                shard->bind_notify_pending(&notify_keyless_pending_);
            srv_->flip_ack(self_->id(), stage);
            return 1;
        }
        if (stage == FlipStage::Rollback && !srv_->flip_acked(self_->id(), stage)) {
            if (srv_->flip_candidate_target(self_->id()) == Role::Ifid)
                self_->cancel_prepared_io_role();
            srv_->flip_ack(self_->id(), stage);
            return 1;
        }
        return 0;
    }

    uint32_t lb_control_pass() {
        if (!lb_controller_armed_) return 0;
        if (srv_->lb_stage() != LbStage::ExDrain) {
            lb_ack_wake_pending_ = false;
            // A completed mover stage may have changed this loop's shard set. Every owned shard's
            // keyless-notify pending pointer must aim at THIS loop, exactly as FlipStage::ExInstall
            // rebinds after a flip -- the mover's lighter stage protocol shipped without this step,
            // and a moved shard's expired-key events then pinged the OLD owner, whose drain walks
            // only its own shards: active-expiry notifications stranded forever (notify battery,
            // key-lb half, 2/6). Rebinding is a handful of pointer stores and runs only on the
            // first pass after a stage ends.
            if (lb_rebind_pending_) {
                lb_rebind_pending_ = false;
                for (Shard* shard : self_->shards())
                    shard->bind_notify_pending(&notify_keyless_pending_);
                notify_keyless_pending_ = true;   // force one state-checked drain after adoption
            }
            return 0;
        }
        auto wake_coordinator = [&]() {
            Ring* coordinator = srv_->thread(srv_->lb_coordinator()).ring();
            if (coordinator && ring_.msg_to(*coordinator, ur_tag(UrKind::Wake, nullptr))) {
                self_->sig().wakes_sent++;
                lb_ack_wake_pending_ = false;
            } else {
                self_->sig().sqe_starved++;
            }
            return 1u; // retry after submit if the wake SQE was unavailable
        };
        if (lb_ack_wake_pending_) return wake_coordinator();
        if (srv_->lb_acked(self_->id())) return 0;
        if (!flip_quiesced()) return 0;
        srv_->lb_ack(self_->id());
        lb_ack_wake_pending_ = true;
        lb_rebind_pending_ = true;   // membership may change before the stage ends; rebind after
        return wake_coordinator();
    }

    void refresh_live_config() {
        LiveConfigSnapshot snapshot;
        if (!srv_->live_config_snapshot_if_changed(live_config_version_, snapshot)) return;
        const bool enabled = snapshot.maxmemory != 0;
        const uint64_t shard_limit = snapshot.maxmemory / srv_->nshards();
        for (Shard* sh : self_->shards()) {
            sh->configure_maxmemory(enabled, shard_limit, snapshot.policy, snapshot.samples);
            // CLIENT TRACKING and periodic SAVE need the same per-write observation points as
            // keyspace notifications, so they ride the shard mask as synthetic observer bits.
            // notify_record expands those observers over NOTIFY_ALL without adding those class
            // bits here: the operator's configured pub/sub classes therefore remain independent.
            // NOTIFY_NEW and NOTIFY_KEY_MISS stay outside the observer surface: `new` would count
            // or invalidate a mutation twice, and a key miss is not a value change.
            sh->set_notify_mask(snapshot.notify_events |
                                (snapshot.tracking_armed ? NOTIFY_TRACKING : 0u) |
                                (snapshot.save_armed ? NOTIFY_SAVE : 0u));
        }
        maxmemory_enabled_ = enabled;
        foreign_touch_policy_ =
            !enabled                                    ? kForeignTouchNone
            : maxmemory_policy_is_lru(snapshot.policy)  ? kForeignTouchLru
            : maxmemory_policy_is_lfu(snapshot.policy)  ? kForeignTouchLfu
                                                        : kForeignTouchNone;
        slowlog_arm_.slowlog_us = snapshot.slowlog_log_slower_than;
        slowlog_arm_.latency_ms = snapshot.latency_monitor_threshold;
        slowlog_armed_ = slowlog_arm_.armed();
        debug_fanout_defer_us_ = snapshot.debug_fanout_defer_us;
        live_config_version_ = snapshot.version;
    }

    // The pending flag is a HINT, and a hint must never be the only looker: a shard whose binding
    // went stale (any ownership move a future path forgets to rebind) would strand its events
    // behind a flag it can no longer set. The busy path keeps the cheap flag gate; sweep() forces
    // the state walk, so stranded events survive at most until the owner's next idle pass.
    uint32_t drain_notify_keyless(LoopSignals& signals, bool force = false) {
        if (__builtin_expect(!notify_keyless_pending_, true) && !force) return 0;
        notify_keyless_pending_ = false;
        uint32_t work = 0;
        for (Shard* shard : self_->shards()) {
            work += notify_ex_pass_entry(
                *srv_, *shard, self_->id(), *self_, ring_, signals);
            notify_keyless_pending_ |= shard->notify_output_pending();
        }
        return work;
    }

    // Visits only the IO threads that actually have work for us, via the notify mask, rather than
    // polling every possible producer. retire() happens inside the helper, AFTER execution — see
    // exqueue.h on why the retired frontier is separate from head.
    // Mask-independent: the state backstop behind the notify hint, run only when this thread has
    // already concluded it has nothing to do.
    template <uint32_t BatchOps = kGenthreadExBatchOps, bool ConsumeTasks = true,
              bool IofusedPrivateQueue = false, bool InterleaveLocalReads = false>
    uint32_t sweep() {
        [[maybe_unused]] bool owner_work_remains = false;
        uint32_t n = snapshot_control_pass<BatchOps, IofusedPrivateQueue>() +
                     service_stale_forwards<BatchOps, IofusedPrivateQueue>() +
                     drain_releases(true);
        if (!snapshot_blocks_tasks()) {
            n += service_multi_retries<IofusedPrivateQueue>();
            n += service_atomic_deferred<IofusedPrivateQueue>();
            n += service_xshard_retries<IofusedPrivateQueue>();
            if (xshard_retries_.empty())
                n += service_ordered_deferred<BatchOps, IofusedPrivateQueue>();
            if constexpr (ConsumeTasks)
                if (xshard_retries_.empty() && ordered_deferred_.empty()) {
                    if (snapshot_owner_state_ == SnapshotOwnerState::None) {
                        if constexpr (InterleaveLocalReads)
                            n += drain_tasks_read_local_interleaved<IofusedPrivateQueue>(
                                true, owner_work_remains);
                        else
                            n += drain_tasks<BatchOps, IofusedPrivateQueue>(true);
                    } else {
                        n += drain_tasks_snapshot<BatchOps, IofusedPrivateQueue>(true);
                    }
                }
        }
        if constexpr (InterleaveLocalReads) {
            owner_work_remains |= fairlane_owner_debt_pending();
            if (!owner_work_remains) n += drain_local_read_tail();
            compact_local_read_tombstones();
        }
        flush_xshard_commits();
        n += active_expire_cycle() + atomic_cleanup_cycle(64);
        n += drain_notify_keyless(self_->sig(), /*force=*/true);
        if (__builtin_expect(srv_->blocking_waiters() != 0, false))
            n += blocking_owner_cycle(*srv_, *self_, ring_, cached_now_ms_, true);
        n += aof_flush_pass();
        return n;
    }

    uint32_t aof_flush_pass() {
        if (__builtin_expect(aof_manager_ == nullptr || !aof_manager_->recording(), true)) return 0;
        AofOwnerContext context{self_->id(), &ring_, &self_->sig()};
        uint32_t work = 0;
        for (Shard* shard : self_->shards()) {
            AofProducer& producer = shard->store().aof();
            if (!producer.has_pending()) continue;
            if (producer.flush(context)) work++;
        }
        return work;
    }

    static int64_t realtime_ms() {
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

    uint32_t active_expire_cycle() {
        if (!srv_->active_expire_enabled()) return 0;
        if (snapshot_blocks_tasks()) return 0;
        auto& shards = self_->shards();
        if (shards.empty()) return 0;
        uint32_t removed = 0;
        const uint32_t visits = static_cast<uint32_t>(
            std::min<size_t>(shards.size(), kActiveExpireChecks));
        const uint32_t base = kActiveExpireChecks / visits;
        const uint32_t extra = kActiveExpireChecks % visits;
        for (uint32_t i = 0; i < visits; i++) {
            if (expire_shard_cursor_ >= shards.size()) expire_shard_cursor_ = 0;
            Shard* sh = shards[expire_shard_cursor_++];
            sh->set_cached_now_ms(cached_now_ms_, cached_lru_clock_);
            const uint32_t n = sh->active_expire(base + (i < extra ? 1 : 0));
            if (n) sh->publish_size();
            removed += n;
        }
        return removed;
    }

    uint32_t atomic_cleanup_cycle(uint32_t budget) {
        auto& shards = self_->shards();
        if (shards.empty() || !budget) return 0;
        // Preserve the cutoff-before-floor handshake once for this owner pass, then reuse the exact
        // pair for every owned shard. A stale-low floor is safe; reversing these loads is not.
        const uint64_t cleanup_cutoff = srv_->atomic_snapshot();
        const uint64_t floor = srv_->atomic_read_floor();
        uint32_t work = 0;
        for (size_t visited = 0; visited < shards.size(); visited++) {
            if (atomic_cleanup_cursor_ >= shards.size()) atomic_cleanup_cursor_ = 0;
            Shard* shard = shards[atomic_cleanup_cursor_++];
            if (!shard->store().atomic_has_records()) continue;
            shard->set_cached_now_ms(cached_now_ms_, cached_lru_clock_);
            work += xshard_cleanup_shard_at(*shard, floor, cleanup_cutoff, budget - work);
            if (work >= budget) break;
        }
        return work;
    }

    uint32_t drain_releases(bool unmasked = false) {
        auto take = [&](const BorrowRelease& r) {
            // The IO producer can have sampled the source just before the bucket ownership edge.
            // Recheck before the first FlatStore touch and forward the stale release intact.
            if (forward_stale_release(r)) return;
            srv_->shard(r.shard).store().unborrow(r.ptr);
        };
        return unmasked ? self_->drain_releases_unmasked(take) : self_->drain_releases(take);
    }

    // The armed coarse scheduler gives every captured producer one owner-task quantum. This keeps
    // WB distance independent of a producer's queued depth and gives a continuously busy remote IO
    // lane service every rotation. ThreadCtx preserves recv -> callback -> retire for each Task and
    // invokes the local hook only after the Task completing a full execution batch is retired.
    template <bool IofusedPrivateQueue = false>
    uint32_t drain_tasks_read_local_interleaved(bool unmasked,
                                                bool& owner_work_remains) {
        Task batch[kReadLocalOwnerTaskChunkOps];
        uint32_t held = 0;
        uint32_t local_work = 0;
        auto take = [&](const Task& task) {
            batch[held++] = task;
            if (held != kReadLocalOwnerTaskChunkOps) return false;
            exec_batch<IofusedPrivateQueue>(batch, held);
            held = 0;
            return true;
        };
        auto local_turn = [&] {
            // A local read must not run between a last-owner install and this batch's epoch
            // publication. This boundary still batches every group in the preceding owner chunk.
            flush_xshard_commits();
            for (uint32_t chunk = 0;
                 chunk < kReadLocalMaxChunksBetweenOwnerBatches; chunk++)
                local_work += drain_local_reads_bounded(kReadLocalDrainChunkOps);
        };
        const uint32_t n = self_->drain_task_producer_chunks<IofusedPrivateQueue>(
            kReadLocalOwnerTaskChunkOps, take, local_turn, unmasked);
        if (held) {
            exec_batch<IofusedPrivateQueue>(batch, held);
            local_turn();
        }
        owner_work_remains = self_->notified_task_depth_capped(1) != 0 ||
            fairlane_owner_debt_pending();
        self_->sig().ops += n;
        return n + local_work;
    }

    // Once the captured owner set is empty, retain the old pure-read capacity by consuming further
    // bounded chunks. Recheck the ordinary owner hint at every boundary so a newly visible writer
    // normally yields the tail after at most one chunk; the existing unmasked idle audit remains
    // the correctness backstop for a lost or racing hint.
    uint32_t drain_local_read_tail() {
        if (fairlane_owner_debt_pending() ||
            self_->notified_task_depth_capped(1)) return 0;
        // Keep one scratch batch live across the pure-read tail. The bounded drain rechecks owner
        // work after every scheduling chunk, so the fast path does not pay stack setup per chunk.
        return drain_local_reads_bounded<true>(UINT32_MAX);
    }

    template <uint32_t BatchOps = kGenthreadExBatchOps,
              bool IofusedPrivateQueue = false>
    uint32_t drain_tasks(bool unmasked = false) {
        Task batch[BatchOps];
        uint32_t held = 0;
        auto take = [&](const Task& t) {
            batch[held++] = t;
            if (held == BatchOps) {
                exec_batch<IofusedPrivateQueue>(batch, held);
                held = 0;
            }
        };
        const uint32_t n = unmasked
            ? self_->drain_tasks_unmasked<IofusedPrivateQueue>(take)
            : self_->drain_tasks<IofusedPrivateQueue>(take);
        if (held) exec_batch<IofusedPrivateQueue>(batch, held);
        self_->sig().ops += n;
        return n;
    }

    // Identical ready-mask drain and stack batch as the iofused coarse path.  Only the first batch
    // is split at its existing load-to-use seam; the caller's WB work runs synchronously there and
    // every later batch remains the ordinary prefetch+execute unit.  ThreadCtx still owns the exact
    // same pop/retire sequence, so no streams lane list or delayed-retirement context is involved.
    template <uint32_t BatchOps, bool IofusedPrivateQueue, typename Filler>
    uint32_t drain_tasks_with_filler(bool unmasked, Filler& filler, bool& filler_used) {
        Task batch[BatchOps];
        uint32_t held = 0;
        auto execute_batch = [&] {
            if (!held) return;
            if (!filler_used && xshard_retries_.empty()) {
                if (__builtin_expect(ex_sched_enabled_, false))
                    ex_schedule_batch(batch, held);
                prefetch_exec_batch(batch, held);
                filler();
                filler_used = true;
                exec_batch_prefetched<IofusedPrivateQueue>(batch, held);
            } else {
                exec_batch<IofusedPrivateQueue>(batch, held);
            }
            held = 0;
        };
        auto take = [&](const Task& task) {
            batch[held++] = task;
            if (held == BatchOps) execute_batch();
        };
        const uint32_t n = unmasked
            ? self_->drain_tasks_unmasked<IofusedPrivateQueue>(take)
            : self_->drain_tasks<IofusedPrivateQueue>(take);
        execute_batch();
        self_->sig().ops += n;
        return n;
    }

    // Only the ordinary one-owner path participates. Every existing special mechanism is a hard
    // barrier in the gathered sequence: eligible work on either side cannot move across it.
    bool ex_sched_candidate(const Task& task, uint8_t& length) const {
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

    void ex_schedule_run(Task* tasks, const uint8_t* base_lengths, uint32_t n) {
        if (n < 2) return;
        Client* const only_client = tasks[0].client;
        uint32_t distinct_at = 1;
        while (distinct_at < n && tasks[distinct_at].client == only_client) distinct_at++;
        // Absolute per-connection order leaves no legal permutation in a one-client run.
        if (distinct_at == n) return;

        ExScheduleKey keys[kExecBatch];
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
            if (__builtin_expect(distance >= kRobWindow, false)) return;
            keys[i] = ExScheduleKey{static_cast<uint8_t>(distance), base_lengths[i]};
            min_rank = std::min(min_rank, keys[i].rank);
            max_rank = std::max(max_rank, keys[i].rank);
            if (i == n - 1) first_length = keys[i].length;
            else one_length &= keys[i].length == first_length;
        }

        // The measured-law escape is defined on the directly available gathered classes and runs
        // before conservative widening for an invisible predecessor. This is what keeps homogeneous
        // rank-adjacent traffic off the dependency and bucket paths.
        if (one_length && max_rank - min_rank <= 1) return;

        // Effective class is the prefix maximum for each connection in this gathered run. A rank
        // before the first represented task, or a gap in its ids, means an unrepresented blocker;
        // its slot cannot safely be read while IO may recycle it, so Long is the no-state upper
        // bound. Open addressing is at most half full and is reached only after degeneration.
        static constexpr uint32_t kChainSlots = kExecBatch * 2;
        Client* chain_client[kChainSlots];
        uint8_t chain_last[kChainSlots];
        uint64_t chain_occupied = 0;
        for (uint32_t i = 0; i < n; i++) {
            Client* client = tasks[i].client;
            uint64_t hash = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(client));
            hash ^= hash >> 33;
            hash *= uint64_t{0xff51afd7ed558ccdull};
            hash ^= hash >> 33;
            uint32_t slot = static_cast<uint32_t>(hash) & (kChainSlots - 1);
            uint64_t bit = uint64_t{1} << slot;
            while ((chain_occupied & bit) && chain_client[slot] != client) {
                slot = (slot + 1) & (kChainSlots - 1);
                bit = uint64_t{1} << slot;
            }
            if (!(chain_occupied & bit)) {
                chain_occupied |= bit;
                chain_client[slot] = client;
                if (keys[i].rank != 0)
                    keys[i].length = static_cast<uint8_t>(CommandLengthClass::Long);
            } else {
                const uint8_t previous = chain_last[slot];
                // Preserve the existing FIFO if a producer-lane bug ever violates the gather
                // contract. The scheduler must never create a same-connection inversion.
                if (tasks[i].op_id <= tasks[previous].op_id) return;
                keys[i].length = std::max(keys[i].length, keys[previous].length);
                if (tasks[i].op_id != tasks[previous].op_id + 1)
                    keys[i].length = static_cast<uint8_t>(CommandLengthClass::Long);
            }
            chain_last[slot] = static_cast<uint8_t>(i);
        }
        one_length = true;
        for (uint32_t i = 1; i < n; i++) one_length &= keys[i].length == keys[0].length;
        if (one_length && max_rank - min_rank <= 1) return;

        // Stable gather order often already matches the selected bucket order. Avoid scratch
        // setup and two Task copies when the policy would be an identity permutation.
        bool already_ordered = true;
        uint32_t previous_bucket = keys[0].rank * kExSchedClasses + keys[0].length;
        for (uint32_t i = 1; i < n; i++) {
            const uint32_t bucket = keys[i].rank * kExSchedClasses + keys[i].length;
            already_ordered &= bucket >= previous_bucket;
            previous_bucket = bucket;
        }
        if (already_ordered) return;

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

        Task ordered[kExecBatch];
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t bucket = keys[i].rank * kExSchedClasses + keys[i].length;
            ordered[cursors[bucket]++] = tasks[i];
        }
        for (uint32_t i = 0; i < n; i++) tasks[i] = ordered[i];
    }

    void ex_schedule_batch(Task* tasks, uint32_t n) {
        uint8_t base_lengths[kExecBatch];
        uint32_t begin = 0;
        while (begin < n) {
            if (!ex_sched_candidate(tasks[begin], base_lengths[begin])) {
                begin++;
                continue;
            }
            uint32_t end = begin + 1;
            while (end < n && ex_sched_candidate(tasks[end], base_lengths[end])) end++;
            ex_schedule_run(tasks + begin, base_lengths + begin, end - begin);
            // The failed candidate at end is a known barrier; consume it without reading its Op a
            // second time, then find the next eligible run.
            begin = end + (end < n);
        }
    }

    enum class SnapshotOwnerState : uint8_t {
        None, Preparing, Prepared, Frozen, Marked, Capturing, Draining
    };

    bool snapshot_blocks_tasks() const {
        if (snapshot_owner_state_ == SnapshotOwnerState::Frozen ||
            snapshot_owner_state_ == SnapshotOwnerState::Marked) return true;
        return snapshot_owner_state_ == SnapshotOwnerState::Capturing &&
               snapshot_manager_ && snapshot_manager_->blocking();
    }

    void begin_snapshot(SnapshotManager* manager) {
        if (!manager || snapshot_owner_state_ != SnapshotOwnerState::None) return;
        snapshot_manager_ = manager;
        snapshot_epoch_ = manager->epoch();
        snapshot_was_cancelled_ = false;
        snapshot_owner_state_ = SnapshotOwnerState::Preparing;
        snapshot_prepare_cursor_ = 0;
        snapshot_progress_cursor_ = 0;
        snapshot_done_shards_.assign(srv_->nshards(), 0);
        snapshot_pending_chunks_.clear();
        snapshot_pending_chunks_.resize(srv_->nshards());
        snapshot_backlogs_.clear();
        snapshot_backlogs_.resize(srv_->nshards());
    }

    template <uint32_t BatchOps = kGenthreadExBatchOps,
              bool IofusedPrivateQueue = false>
    uint32_t snapshot_control_pass() {
        if (snapshot_owner_state_ == SnapshotOwnerState::None) return 0;
        SnapshotManager::Phase phase = snapshot_manager_->phase();
        if (phase == SnapshotManager::Phase::Failed &&
            snapshot_owner_state_ != SnapshotOwnerState::Draining) {
            for (Shard* shard : self_->shards()) shard->store().snapshot_cancel();
            snapshot_pending_chunks_.clear();
            snapshot_was_cancelled_ = true;
            snapshot_owner_state_ = SnapshotOwnerState::Draining;
            return 1;
        }

        if (snapshot_owner_state_ == SnapshotOwnerState::Preparing) {
            auto& shards = self_->shards();
            if (snapshot_prepare_cursor_ < shards.size()) {
                FlatStore::SnapshotWriteResult result = shards[snapshot_prepare_cursor_]
                    ->store().snapshot_prepare(snapshot_epoch_, 0);
                if (result == FlatStore::SnapshotWriteResult::Error) {
                    snapshot_manager_->fail(snapshot_epoch_, "could not prepare shard snapshot");
                    return 1;
                }
                if (result == FlatStore::SnapshotWriteResult::Ready) snapshot_prepare_cursor_++;
                return 1;
            }
            snapshot_owner_state_ = SnapshotOwnerState::Prepared;
            snapshot_manager_->owner_ready(snapshot_epoch_);
            return 1;
        }

        if (snapshot_owner_state_ == SnapshotOwnerState::Prepared &&
            phase == SnapshotManager::Phase::Freeze) {
            // The Mark-time AOF segment switch is positional. Publish every pre-cut owner buffer
            // before acknowledging Freeze so the writer can drain a complete old segment.
            aof_flush_pass();
            snapshot_owner_state_ = SnapshotOwnerState::Frozen;
            snapshot_manager_->owner_frozen(snapshot_epoch_);
            return 1;
        }
        if (snapshot_owner_state_ == SnapshotOwnerState::Frozen &&
            phase == SnapshotManager::Phase::Mark) {
            for (Shard* shard : self_->shards()) {
                if (!shard->store().snapshot_mark(shard->id(), snapshot_manager_->cut_ms())) {
                    snapshot_manager_->fail(snapshot_epoch_, "could not mark shard snapshot epoch");
                    return 1;
                }
            }
            snapshot_owner_state_ = SnapshotOwnerState::Marked;
            snapshot_manager_->owner_marked(snapshot_epoch_);
            return 1;
        }
        if (snapshot_owner_state_ == SnapshotOwnerState::Marked &&
            phase == SnapshotManager::Phase::Capture) {
            snapshot_owner_state_ = SnapshotOwnerState::Capturing;
            return 1;
        }
        if (snapshot_owner_state_ == SnapshotOwnerState::Capturing &&
            phase == SnapshotManager::Phase::Capture) {
            return progress_snapshot_capture();
        }
        if (snapshot_owner_state_ == SnapshotOwnerState::Draining) {
            const uint32_t n = service_snapshot_backlogs<IofusedPrivateQueue>(
                BatchOps, false);
            if (snapshot_backlogs_empty()) {
                if (snapshot_was_cancelled_) snapshot_manager_->owner_cancelled(snapshot_epoch_);
                else                         snapshot_manager_->owner_finished(snapshot_epoch_);
                snapshot_owner_state_ = SnapshotOwnerState::None;
                snapshot_manager_ = nullptr;
            }
            return n;
        }
        return 0;
    }

    uint32_t progress_snapshot_capture() {
        auto& shards = self_->shards();
        if (shards.empty()) {
            snapshot_owner_state_ = SnapshotOwnerState::Draining;
            return 1;
        }
        uint32_t work = 0;
        const uint32_t wanted_sid = snapshot_manager_->save_current_shard();
        for (size_t visits = 0; visits < shards.size(); visits++) {
            if (snapshot_progress_cursor_ >= shards.size()) snapshot_progress_cursor_ = 0;
            Shard* shard = shards[snapshot_progress_cursor_++];
            const uint32_t sid = static_cast<uint32_t>(shard->id());
            if (snapshot_done_shards_[sid]) continue;
            if (snapshot_manager_->blocking() && sid != wanted_sid) continue;

            auto& pending = snapshot_pending_chunks_[sid];
            if (!pending) pending = shard->store().snapshot_take_chunk();
            if (pending) {
                const bool end = (pending->flags & SnapshotFrameEnd) != 0;
                if (!snapshot_manager_->post_chunk(self_->id(), pending, ring_, self_->sig())) return work;
                work++;
                if (end) {
                    shard->store().snapshot_handoff_complete();
                    snapshot_done_shards_[sid] = 1;
                }
            }
            if (!snapshot_done_shards_[sid]) {
                work += shard->store().snapshot_progress(kSnapshotChunkBytes, 256);
                if (shard->store().snapshot_failed()) {
                    snapshot_manager_->fail(snapshot_epoch_, "snapshot type serialization failed");
                    return work + 1;
                }
                if (!pending) pending = shard->store().snapshot_take_chunk();
                if (pending) {
                    const bool end = (pending->flags & SnapshotFrameEnd) != 0;
                    if (!snapshot_manager_->post_chunk(self_->id(), pending, ring_, self_->sig()))
                        return work;
                    work++;
                    if (end) {
                        shard->store().snapshot_handoff_complete();
                        snapshot_done_shards_[sid] = 1;
                    }
                }
            }
            break;  // one shard, one bounded byte/slot budget per executor pass
        }
        bool all_done = true;
        for (Shard* shard : shards)
            all_done &= snapshot_done_shards_[static_cast<uint32_t>(shard->id())] != 0;
        if (all_done) {
            snapshot_owner_state_ = SnapshotOwnerState::Draining;
        }
        return work;
    }

    bool snapshot_backlogs_empty() const {
        for (const auto& queue : snapshot_backlogs_) if (!queue.empty()) return false;
        return true;
    }

    FlatStore::SnapshotWriteResult snapshot_prepare_plain_write(Op& op, Shard& shard) {
        if (!(op.spec->flags & CmdFlags::SubcmdRoute))
            return shard.store().snapshot_prepare_write(op.hash, op.key());

        // A routed container's key position belongs to its selected child, not argv[1].  The
        // registered position is still the common keyed-child position; a short keyless child
        // such as XGROUP HELP has no pre-image to capture.
        const int16_t first_key = op.spec->first_key;
        if (first_key <= 0 || static_cast<uint32_t>(first_key) >= op.argc())
            return FlatStore::SnapshotWriteResult::Ready;
        return shard.store().snapshot_prepare_write(op.hash, op.arg(first_key));
    }

    template <bool IofusedPrivateQueue = false>
    bool execute_snapshot_task(const Task& task, bool capture_writes) {
        // MULTI's tagged task owns its command images outside the public ROB and performs the
        // snapshot pre-image gate per installed transaction key.  Never decode it as a normal op.
        if (multi_task_tagged(task)) return execute<IofusedPrivateQueue>(task);
        if (!task.client) return execute<IofusedPrivateQueue>(task);
        Op& op = task.client->rob().at(task.op_id);
        const int32_t sid = task.shard >= 0 ? task.shard : op.shard;
        Shard& shard = srv_->shard(sid);
        if (op.has_blocking_state()) {
            if (capture_writes) {
                const BlockingSnapshotPrepare result =
                    blocking_snapshot_prepare(task, shard, op);
                if (result == BlockingSnapshotPrepare::Pending) return false;
                if (result == BlockingSnapshotPrepare::Error) {
                    snapshot_manager_->fail(
                        snapshot_epoch_, "snapshot pre-image serialization failed");
                    return false;
                }
            }
            return execute<IofusedPrivateQueue>(task);
        }
        if (capture_writes &&
            (op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) {
            const FlatStore::SnapshotWriteResult result = task.scatter
                ? xshard_snapshot_prepare(task, shard)
                : xshard_is_local(op)
                    ? xshard_local_snapshot_prepare(op, shard)
                    : snapshot_prepare_plain_write(op, shard);
            if (result == FlatStore::SnapshotWriteResult::Pending) return false;
            if (result == FlatStore::SnapshotWriteResult::Error) {
                snapshot_manager_->fail(snapshot_epoch_, "snapshot pre-image serialization failed");
                return false;
            }
        }
        if (!execute<IofusedPrivateQueue>(task)) return false;
        shard.publish_size();
        return true;
    }

    template <bool IofusedPrivateQueue = false>
    void schedule_snapshot_task(const Task& task) {
        if (multi_task_tagged(task)) {
            execute<IofusedPrivateQueue>(task);
            return;
        }
        if (!task.client) {
            execute<IofusedPrivateQueue>(task);
            return;
        }
        // A bounded scatter continuation is still the oldest task on this owner.  Holding later
        // work here preserves the queue-order RYOW argument without installing a conn barrier for
        // KEYS.  Snapshot-gated scatter writes use the same ordering hold while Pending.
        if (!xshard_retries_.empty()) { ordered_deferred_.push_back(task); return; }
        if (task.scatter) {
            if (!execute_snapshot_task<IofusedPrivateQueue>(task, true))
                xshard_retries_.push_back(task);
            return;
        }
        const Op& op = task.client->rob().at(task.op_id);
        const int32_t sid = task.shard >= 0 ? task.shard : op.shard;
        auto& queue = snapshot_backlogs_[static_cast<uint32_t>(sid)];
        if (!queue.empty() || !execute_snapshot_task<IofusedPrivateQueue>(task, true))
            queue.push_back(task);
    }

    template <bool IofusedPrivateQueue = false>
    uint32_t service_snapshot_backlogs(uint32_t budget, bool capture_writes = true) {
        uint32_t n = 0;
        for (Shard* shard : self_->shards()) {
            auto& queue = snapshot_backlogs_[static_cast<uint32_t>(shard->id())];
            while (budget && !queue.empty()) {
                if (!execute_snapshot_task<IofusedPrivateQueue>(
                        queue.front(), capture_writes)) break;
                queue.pop_front(); budget--; n++;
            }
            if (!budget) break;
        }
        return n;
    }

    template <uint32_t BatchOps = kGenthreadExBatchOps,
              bool IofusedPrivateQueue = false>
    uint32_t drain_tasks_snapshot(bool unmasked = false) {
        auto take = [&](const Task& task) {
            schedule_snapshot_task<IofusedPrivateQueue>(task);
        };
        const uint32_t n = unmasked
            ? self_->drain_tasks_unmasked<IofusedPrivateQueue>(take)
            : self_->drain_tasks<IofusedPrivateQueue>(take);
        self_->sig().ops += n;
        // Retire at least as many deferred Tasks as this drain can add, plus one batch of old debt;
        // otherwise a saturated post-capture owner could remain in Draining forever.
        return n + service_snapshot_backlogs<IofusedPrivateQueue>(BatchOps + n);
    }

    // The armed slow-log arm. Out of line and cold-marked so its register pressure and its clock
    // calls never reach the disabled batch loop above.
    //
    // TWO MODES. Normal mode brackets the whole batch with two now_ns() reads. A batch of ONE op
    // gives that op's exact duration, which is every command on an unpipelined connection. A batch
    // of many that overruns cannot be attributed retroactively -- the ops already ran -- so it
    // arms per-op timing for the next kSlowlogEscalateBatches batches instead, and the recurrence
    // is timed exactly. This is the documented divergence from redis, which times every command.
    //
    template <bool IofusedPrivateQueue = false>
    __attribute__((noinline, cold))
    void exec_batch_timed(const Task* batch, uint32_t n) {
        const SlowlogArm arm = slowlog_arm_;
        const int64_t now_ms = cached_now_ms_;
        slowlog_note_batch_timed();
        // Earlier service queues in this outer pass are not part of this batch's timing sample.
        flush_xshard_commits();

        if (slowlog_state_.escalate_batches || n == 1) {
            if (slowlog_state_.escalate_batches) slowlog_state_.escalate_batches--;
            for (uint32_t i = 0; i < n; i++) {
                // Snapshot argv BEFORE execution. execute() publishes Done, after which the owning
                // IO thread may retire the op and compact the read buffer the Slices point into.
                Client* client = batch[i].client;
                if (client)
                    slowlog_capture(client->rob().at(batch[i].op_id), slowlog_state_.capture);
                const uint64_t started = now_ns();
                const bool ok = execute<IofusedPrivateQueue>(batch[i]);
                // Done is deliberately delayed with the commit. Armed per-op timing must include
                // that commit and publish it before the Done-based one-command attribution test.
                flush_xshard_commits();
                const uint64_t elapsed = now_ns() - started;
                // ONE ENTRY PER COMMAND, not per participating shard. A cross-shard op is handed
                // to every owner it touches; all but the last return with the op still Issued.
                // Recording only the owner that published Done means a scatter is attributed to
                // the slice that actually computed the answer -- documented in NOTES-SERVERTAIL.md.
                if (client &&
                    client->rob().at(batch[i].op_id).state.load(std::memory_order_relaxed) ==
                        OpState::Done)
                    slowlog_record_captured(self_->id(), client->id(), slowlog_state_.capture,
                                            elapsed, now_ms, arm);
                if (ok) continue;
                xshard_retries_.push_back(batch[i]);
                for (uint32_t j = i + 1; j < n; j++) ordered_deferred_.push_back(batch[j]);
                return;
            }
            return;
        }

        const uint64_t started = now_ns();
        uint32_t executed = n;
        for (uint32_t i = 0; i < n; i++) {
            if (execute<IofusedPrivateQueue>(batch[i])) continue;
            xshard_retries_.push_back(batch[i]);
            for (uint32_t j = i + 1; j < n; j++) ordered_deferred_.push_back(batch[j]);
            executed = i;
            break;
        }
        flush_xshard_commits();
        const uint64_t elapsed = now_ns() - started;
        // The screen is per-op-average, not whole-batch: a full batch of ordinary commands must
        // not look like one slow command just because there were 32 of them.
        const uint64_t threshold_ns = arm.slowlog_us >= 0
            ? static_cast<uint64_t>(arm.slowlog_us) * 1000
            : UINT64_MAX;
        const uint64_t latency_ns = arm.latency_ms
            ? static_cast<uint64_t>(arm.latency_ms) * 1000000 : UINT64_MAX;
        const uint64_t bar = std::min(threshold_ns, latency_ns);
        if (executed && bar != UINT64_MAX && elapsed / executed >= bar) {
            slowlog_state_.escalate_batches = kSlowlogEscalateBatches;
            slowlog_note_escalation();
        }
    }

    // Prefetch the whole batch's slots, THEN execute. Issuing the loads up front lets their DRAM
    // round trips overlap instead of each op stalling on its own miss in turn.
    void prefetch_exec_batch(const Task* batch, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) {
            if (!batch[i].client) continue;
            const Op& op = batch[i].client->rob().at(batch[i].op_id);
            const int32_t shard = batch[i].shard >= 0 ? batch[i].shard : op.shard;
            // The shipped coarse batch must obey the same resolve -> verify-owner -> store-touch
            // order as pipelined E1. A route can go stale after enqueue; even a prefetch through
            // the old FlatStore is formally an ownership violation under TSAN's model.
            if (shard >= 0 && !batch[i].scatter &&
                srv_->worker_of_shard(shard) == self_->id() &&
                !(op.spec->flags & (CmdFlags::CursorShard | CmdFlags::RandomShard)))
                srv_->shard(shard).store().prefetch(op.hash);
        }
    }

    // Consume a bucket-prefetched homogeneous batch. The interwoven schedule calls this
    // immediately after the prefetch loop; an interleaved schedule reaches it after independent-
    // stream filler.
    template <bool IofusedPrivateQueue = false>
    void exec_batch_prefetched(const Task* batch, uint32_t n) {
        if (!xshard_retries_.empty()) {
            for (uint32_t i = 0; i < n; i++) ordered_deferred_.push_back(batch[i]);
            return;
        }
        {
            // Completion notification is coalesced over this loop and flushed when the scope
            // closes -- before the publish and reclamation work below, so the io is not made to
            // wait on either.
            NotifyBatchScope notify_batch(this);
            // THE ENTIRE DISABLED-FEATURE COST OF SLOWLOG/LATENCY: one predicted-false branch
            // here, once per batch of up to kExecBatch ops. No clock is read and the recorder is
            // not linked into this loop at all. The armed body is out of line in
            // exec_batch_timed().
            if (__builtin_expect(slowlog_armed_, false)) {
                exec_batch_timed<IofusedPrivateQueue>(batch, n);
            } else {
                for (uint32_t i = 0; i < n; i++) {
                    if (execute<IofusedPrivateQueue>(batch[i])) continue;
                    xshard_retries_.push_back(batch[i]);
                    for (uint32_t j = i + 1; j < n; j++) ordered_deferred_.push_back(batch[j]);
                    break;
                }
            }
        }
        // One publish per batch, covering every shard this batch touched. Cheaper than tracking
        // which ones changed, and this thread owns all of them.
        for (Shard* sh : self_->shards()) sh->publish_size();
        // Reclamation is owner-batched rather than one posted task per completed group. Visit the
        // owned shards after each atomic batch so their pending-entry lists stay short.
        if (xshard_retries_.empty() && srv_->atomic_work_active()) {
            atomic_cleanup_cycle(256);
        }
    }

    // Run mutation-capable reclamation only after every buffered E2 in the pass. In particular, an
    // A/D sequence must not put cleanup between E1(D) and E2(D), and consecutive modulo chunks use
    // the same rule. The caller still holds every gathered source prefix unretired here.
    void finish_buffered_exec_pass(uint32_t executable_count) {
        flush_xshard_commits();
        if (xshard_retries_.empty() && srv_->atomic_work_active() && executable_count) {
            // The legacy entry supplied 256 cleanup records of service for every batch of at most
            // 128 tasks. Preserve that capacity while paying the owned-shard walk only once.
            atomic_cleanup_cycle(std::max<uint32_t>(256, executable_count * 2));
        }
    }

    // Buffered E2 batches can be much smaller than the shipped coarse drain. Their caller tracks
    // owner-verified shards while E1 already has the route in hand, then publishes that dense set
    // once after the complete multi-chunk EX pass. Keep the ordinary coarse/iofused entry above --
    // including its historical all-owned-shards publication -- unchanged.
    void exec_batch_prefetched_buffered(const Task* batch, uint32_t n) {
        if (!xshard_retries_.empty()) {
            for (uint32_t i = 0; i < n; i++) ordered_deferred_.push_back(batch[i]);
            return;
        }
        NotifyBatchScope notify_batch(this);
        if (__builtin_expect(slowlog_armed_, false)) {
            exec_batch_timed(batch, n);
        } else {
            for (uint32_t i = 0; i < n; i++) {
                if (execute(batch[i])) continue;
                xshard_retries_.push_back(batch[i]);
                for (uint32_t j = i + 1; j < n; j++) ordered_deferred_.push_back(batch[j]);
                break;
            }
        }
    }

    // Coarse compatibility: prefetch the whole batch and consume it without an intervening
    // micro-stage.
    template <bool IofusedPrivateQueue = false>
    void exec_batch(Task* batch, uint32_t n) {
        // Deferral first (skip wasted prefetch on the rare retry path), then the opt-in
        // reorder BEFORE prefetch so prefetch order matches execution order.
        if (!xshard_retries_.empty()) {
            for (uint32_t i = 0; i < n; i++) ordered_deferred_.push_back(batch[i]);
            return;
        }
        if (__builtin_expect(ex_sched_enabled_, false)) ex_schedule_batch(batch, n);
        prefetch_exec_batch(batch, n);
        exec_batch_prefetched<IofusedPrivateQueue>(batch, n);
    }

    template <bool IofusedPrivateQueue = false, bool ReadLocalNoEvict = false>
    bool execute(const Task& t) {
        // Forwarding, rather than a request epoch, resolves the route-read/enqueue race.  This check
        // must precede every shard dereference, including tagged MULTI and ownerless cleanup tasks.
        if (forward_stale_task<IofusedPrivateQueue>(t)) return true;
        if (multi_task_tagged(t)) {
            Shard& shard = srv_->shard(t.shard);
            shard.set_cached_now_ms(cached_now_ms_, cached_lru_clock_);
            // PROGRAM ORDER: a transaction fragment must not run past an older same-connection
            // task parked in atomic_deferred_/xshard_retries_ on this shard. This branch used to
            // skip the check entirely, so a pipelined EXEC's child executed before the
            // connection's earlier cross-shard DEL fragment that was deferred here, and the
            // transaction read the pre-DEL world (the seed-19 differ divergence). An ownerless
            // teardown fragment (null client, the WATCH-disconnect path) belongs to no
            // connection and carries no program order -- and has no ROB to consult.
            if (t.client) {
                Op& carrier = t.client->rob().at(t.op_id);
                if (has_parked_predecessor(t, carrier, t.shard)) {
                    atomic_deferred_.push_back(t);
                    return true;
                }
                // PROGRAM ORDER, THE OTHER HALF: an older same-connection cross-shard unit that
                // has already INSTALLED here but has not yet DECIDED. Nothing is parked, so the
                // check above cannot see it, yet its physically installed candidate may still be
                // withdrawn -- MSETNX under --atomic 1 installs first and only then discovers an
                // existing key on another owner, and any group can still fail on OOM/maxmemory.
                // A transaction fragment reads through the store's own-connection RYOW overlay
                // (atomic_resolve_internal's own_private arm), which is keyed on the CONNECTION
                // and therefore also exposes that other unit's undecided candidate. Running here
                // let an EXEC clone a value that was never committed and install it as its own
                // candidate, which then committed with a real ticket: a withdrawn MSETNX value
                // laundered into an acknowledged transaction write, visible to every connection
                // and permanent (tests/multirace.py).
                // Deferring here rather than in prepare_write_key() is what makes it safe. This
                // fragment has installed nothing yet on this owner, so parking it cannot hold a
                // half-applied command; and the predecessor can never wait on it, because on
                // every shard the older unit's fragment is drained first and, if it is itself
                // parked, parked_predecessor_in() holds this younger fragment behind it. That is
                // the acyclicity NOTES-MULTIRES.md §5(a) found missing when the same hold was
                // attempted mid-command against an order-blind key probe.
                if (__builtin_expect(shard.store().atomic_has_records(), false) &&
                    shard.store().atomic_has_foreign_unit_undecided(
                        t.client->id(),
                        [&t](const void* epoch) { return multi_task_owns_epoch(t, epoch); })) {
                    shard.stats().atomic_exec_order_holds++;
                    atomic_deferred_.push_back(t);
                    return true;
                }
            }
            // The no-touch answer is PER TASK; a MULTI body inherits the transaction owner's.
            if (__builtin_expect(maxmemory_enabled_, false)) {
                const Op& carrier = t.client->rob().at(t.op_id);
                const bool no_touch = carrier.no_touch();
                shard.set_no_touch(no_touch);
                if (no_touch) srv_->climon_note_no_touch();
            }
            AofOwnerContext aof_context{self_->id(), &ring_, &self_->sig()};
            const MultiTaskResult result =
                multi_execute_task(*srv_, t, shard, self_->id(), self_->domain(),
                                   aof_manager_ ? &aof_context : nullptr);
            shard.publish_size();
            if (result == MultiTaskResult::Retry) {
                multi_retries_.push_back(t);
                return true;
            }
            if (result == MultiTaskResult::Final) {
                Op& public_op = t.client->rob().at(t.op_id);
                public_op.state.store(OpState::Done, std::memory_order_release);
                notify_sender(t.client);
            }
            return true;
        }
        if (!t.client) {
            Shard& cleanup = srv_->shard(t.shard);
            cleanup.set_cached_now_ms(cached_now_ms_, cached_lru_clock_);
            // An ownerless cleanup pass belongs to no connection: clear the per-task no-touch
            // answer so a NO-TOUCH client cannot leave it armed for whatever runs next.
            if (__builtin_expect(maxmemory_enabled_, false)) cleanup.set_no_touch(false);
            xshard_cleanup_shard(*srv_, cleanup, 32);
            cleanup.publish_size();
            return true;
        }
        Op& op = t.client->rob().at(t.op_id);
        const int32_t shard_id = t.shard >= 0 ? t.shard : op.shard;
        Shard& sh = srv_->shard(shard_id);
        if constexpr (Fused && !ReadLocalNoEvict) {
            if (__builtin_expect(
                    maxmemory_enabled_ && read_local_enabled() &&
                    op.read_local_precise_write() &&
                    sh.store().maxmemory_policy() != MaxmemoryPolicy::NoEviction,
                    false)) {
                ReadLocalPreciseWriteGuard no_evict(sh.store());
                return execute<IofusedPrivateQueue, true>(t);
            }
        }
        sh.set_cached_now_ms(cached_now_ms_, cached_lru_clock_);
        // CLIENT NO-TOUCH. maxmemory_enabled_ is this loop's own per-pass value, so with the
        // default (maxmemory off) this is one predicted-not-taken test on a register -- and the
        // read path in FlatStore never loads the byte at all, because its && short-circuits.
        if (__builtin_expect(maxmemory_enabled_, false)) {
            const bool no_touch = op.no_touch();
            sh.set_no_touch(no_touch);
            if (no_touch) srv_->climon_note_no_touch();
        }
        if (op.has_blocking_state()) {
            sh.note_execution(self_->domain());
            note_lb_hash(sh, op.hash);
            if constexpr (IofusedPrivateQueue) {
                static_assert(Fused);
                return blocking_execute_iofused(*srv_, *self_, ring_, t, sh, op);
            } else {
                return blocking_execute(*srv_, *self_, ring_, t, sh, op);
            }
        }
        // #77 TRIPWIRE A samples before any scheduler park. The first answer is retained across
        // retries; the debug registry qualifies begin-plain transitions when execution arrives.
        if (__builtin_expect(atomic_tripwire_enabled(), false))
            xshard_tripwire_fragment_begin(t, sh, op);
        if (has_parked_predecessor(t, op, shard_id) ||
            xshard_task_should_defer(*srv_, sh, t, op)) {
            atomic_deferred_.push_back(t);
            return true;
        }
        if (!t.scatter && (op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)) &&
            __builtin_expect(sh.has_watches(), false) && !multi_plain_write_ready(sh, op))
            return false;
        // Records the op AND whether it was executed from this shard's home L3 domain. One compare
        // and one increment, no atomics — the shard has a single owner.
        sh.note_execution(self_->domain());

        if (!t.scatter) self_->note_command(op.spec->id);

        if (t.scatter) {
            const ScatterTaskResult result = xshard_execute(t, sh, op, self_->id());
            xshard_watch_finish(t, sh, op, result);
            if (result == ScatterTaskResult::Retry) return false;
            if (lb_sample_rate_) {
                struct LbVisit { ExLoopT* loop; Shard* shard; } visit{this, &sh};
                xshard_visit_task_hashes(t, &visit, [](void* context, uint64_t hash) {
                    auto* visit = static_cast<LbVisit*>(context);
                    visit->loop->note_lb_hash(*visit->shard, hash);
                });
            }
            if (__builtin_expect(sh.has_blocking_waiters(), false))
                blocking_scatter_mutation_published(t, sh, op);
            if (__builtin_expect(aof_manager_ != nullptr, false)) {
                AofOwnerContext context{self_->id(), &ring_, &self_->sig()};
                xshard_aof_emit(t, sh, op, context);
            }
            sh.publish_size();
            const ScatterFinish finished = [&] {
                if constexpr (IofusedPrivateQueue) {
                    static_assert(Fused);
                    return xshard_complete_iofused(*srv_, *self_, ring_, t, op);
                } else {
                    return xshard_complete(*srv_, *self_, ring_, t, op);
                }
            }();
            if (finished == ScatterFinish::Waiting) return true;
            if (finished == ScatterFinish::Retry) return false;
            if (finished == ScatterFinish::CommitQueued) {
                xshard_queue_commit(*srv_, *self_, t);
                xshard_commit_pending_ = true;
                return true;
            }
        } else if (__builtin_expect(op.spec->flags & CmdFlags::DenyOom, false) &&
                   !sh.store().budget_admit(op.arg(static_cast<uint32_t>(op.spec->first_key)))) {
            // Growth gate (wrinkle fix 2026-08-25): collection growth mutates behind a stable
            // KvObj and never crosses insert-admission, so an HSET-only workload could blow
            // through maxmemory unbounded. One predicted-false flag test per op when disabled.
            // Scatter tasks bypass it -- their writes replace whole objects through insert-level
            // admission in their owner pass.
            reply_maxmemory_oom(op);
        } else {
            // A null pending-entry list is the sole common-path branch. With no cross-shard window,
            // handlers take their original path with no epoch loads, allocations, or cleanup work.
            const bool scoped_foreign_write = read_local_enabled() &&
                (op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)) &&
                (op.local_xshard() || (op.spec->flags & CmdFlags::ScriptRoute));
            if (__builtin_expect(
                    sh.store().atomic_has_records() || scoped_foreign_write, false)) {
                PlainForeignReadScope foreign_scope;
                const bool execute_handler = xshard_plain_prepare(
                    *srv_, sh, op, t.client->id(), foreign_scope);
                const bool defer_blocking =
                    __builtin_expect(sh.has_blocking_waiters(), false);
                if (defer_blocking) blocking_defer_plain_publication(true);
                if (execute_handler) op.spec->handler(sh, op);
                xshard_plain_finish(sh, foreign_scope);
                if (defer_blocking) {
                    blocking_defer_plain_publication(false);
                    if (execute_handler) blocking_plain_mutation_published(sh, op);
                }
            } else {
                op.spec->handler(sh, op);
            }
        }
        if (!t.scatter) note_lb_hash(sh, op.hash);
        if (!t.scatter && (op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)) &&
            __builtin_expect(sh.has_watches(), false))
            multi_plain_write_committed(sh, op);
        if (!t.scatter && __builtin_expect(aof_manager_ != nullptr, false)) {
            AofOwnerContext context{self_->id(), &ring_, &self_->sig()};
            if (op.local_xshard()) xshard_aof_emit_local(sh, op, context);
            else                   aof_record_local_op(sh, op, context);
        }

        // Release pairs with the IO thread's acquire on Done: everything the handler wrote into
        // op.reply becomes visible through this one store.
        op.state.store(OpState::Done, std::memory_order_release);

        // Notify the connection's io thread; the claim flag dedupes a burst into one post.
        // EXECUTOR-ISSUED SENDS ARE A CLOSED DOOR: the exwb mode (executor
        // sends its own completed prefix) was built, measured #1 in zero cells across every size
        // and pipe depth, and deleted 2026-08-24 -- send work rides the scarce ex role, nearly
        // free at p1 and ruinous at p32. Flush-at-head-from-ex was separately built twice and
        // reverted twice before that (fixed sender 8.1-8.5M get_p32 vs 4.1-4.7M with wedges).
        notify_sender(t.client);
        return true;
    }

    template <bool IofusedPrivateQueue = false>
    uint32_t service_xshard_retries() {
        if (xshard_retries_.empty()) return 0;
        const Task task = xshard_retries_.front();
        xshard_retries_.pop_front();
        const bool complete = snapshot_owner_state_ == SnapshotOwnerState::None
            ? execute<IofusedPrivateQueue>(task)
            : execute_snapshot_task<IofusedPrivateQueue>(task, true);
        if (!complete) xshard_retries_.push_back(task);
        return 1;  // one bounded KEYS pass (or one snapshot-gate attempt) per executor iteration
    }

    void note_lb_hash(Shard& shard, uint64_t hash) {
        if (!lb_sample_rate_) return;
        if (--lb_sample_countdown_ != 0) return;
        lb_sample_countdown_ = lb_sample_rate_;
        shard.note_lb_sample(hash, lb_sample_rate_);
    }

    void lb_bucket_bytes_pass() {
        if (!lb_sample_rate_ || cached_now_ms_ < lb_bytes_next_ms_) return;
        lb_bytes_next_ms_ = cached_now_ms_ + 10;
        const uint32_t rate = srv_->lb_sample_rate();
        if (rate != lb_sample_rate_) {
            lb_sample_rate_ = rate;
            lb_sample_countdown_ = rate;
        }
        auto& owned = self_->shards();
        if (owned.empty()) return;
        if (lb_bytes_shard_cursor_ >= owned.size()) lb_bytes_shard_cursor_ = 0;
        (void)owned[lb_bytes_shard_cursor_++]->lb_scan_bucket_bytes(64);
    }

    template <bool IofusedPrivateQueue = false>
    uint32_t service_multi_retries() {
        if (multi_retries_.empty()) return 0;
        const Task task = multi_retries_.front();
        multi_retries_.pop_front();
        // execute() requeues an unfinished transaction task on multi_retries_ itself.  Returning
        // true here is work/progress, while ordinary inbox draining remains enabled so every shard
        // participant gets its first turn at the command barrier.
        execute<IofusedPrivateQueue>(task);
        return 1;
    }

    template <bool IofusedPrivateQueue = false>
    uint32_t service_atomic_deferred() {
        if (atomic_deferred_.empty()) return 0;
        const Task task = atomic_deferred_.front();
        atomic_deferred_.pop_front();
        Op& op = task.client->rob().at(task.op_id);
        const int32_t shard_id = task.shard >= 0 ? task.shard : op.shard;
        Shard& shard = srv_->shard(shard_id);
        shard.set_cached_now_ms(cached_now_ms_, cached_lru_clock_);
        if (has_parked_predecessor(task, op, shard_id) ||
            xshard_task_should_defer(*srv_, shard, task, op)) {
            atomic_deferred_.push_back(task);
            // Keep polling the decision without preventing normal inbox drains. At low traffic this
            // also prevents an owner sleeping after another worker publishes the dependency.
            return 1;
        }
        const bool complete = snapshot_owner_state_ == SnapshotOwnerState::None
            ? execute<IofusedPrivateQueue>(task)
            : execute_snapshot_task<IofusedPrivateQueue>(task, true);
        if (!complete) xshard_retries_.push_back(task);
        return 1;
    }

    // PROGRAM ORDER AGAINST EVERY PARKED PREDECESSOR, NOT JUST THE DEFERRED ONES.
    // Both queues hold tasks that have been taken off the inbox but have not finished, and both are
    // serviced ahead of a fresh drain, so a younger task from the same connection can reach a shard
    // before an older one that is sitting in either. xshard_retries_ matters as much as
    // atomic_deferred_ because a bounded KEYS walk lives there between passes: service_atomic_
    // deferred() runs first in the loop, so a younger write parked earlier could install a key
    // behind the walker's cursor and be reported by a listing that must not see it yet.
    // Both deques are empty on any path with no cross-shard atomic window open.
    bool has_parked_predecessor(const Task& task, Op& op, int32_t shard_id) {
        if (__builtin_expect(atomic_deferred_.empty() && xshard_retries_.empty(), true))
            return false;
        return parked_predecessor_in(atomic_deferred_, task, op, shard_id) ||
               parked_predecessor_in(xshard_retries_, task, op, shard_id);
    }

    __attribute__((noinline)) bool parked_predecessor_in(const std::deque<Task>& parked,
                                                         const Task& task, Op& op,
                                                         int32_t shard_id) {
        for (const Task& older : parked) {
            if (older.client != task.client || older.op_id >= task.op_id) continue;
            Op& older_op = older.client->rob().at(older.op_id);
            if (!xshard_tasks_share_key(older, older_op, task, op, shard_id)) continue;
            // Either direction is the scan-ordering hold: a walker held behind an older
            // write, or a younger write held behind a walk that is already in flight.
            if (xshard_task_is_whole_owner(older) || xshard_task_is_whole_owner(task))
                self_->note_atomic_scan_hold();
            return true;
        }
        return false;
    }

    template <uint32_t BatchOps = kGenthreadExBatchOps,
              bool IofusedPrivateQueue = false>
    uint32_t service_ordered_deferred() {
        uint32_t work = 0;
        while (work < BatchOps && !ordered_deferred_.empty() && xshard_retries_.empty()) {
            const Task task = ordered_deferred_.front();
            ordered_deferred_.pop_front();
            if (snapshot_owner_state_ == SnapshotOwnerState::None) {
                if (!execute<IofusedPrivateQueue>(task)) xshard_retries_.push_back(task);
            } else {
                schedule_snapshot_task<IofusedPrivateQueue>(task);
            }
            work++;
        }
        return work;
    }

    int32_t task_shard(const Task& task) const {
        if (task.shard >= 0) return task.shard;
        if (!task.client) return -1;
        return task.client->rob().at(task.op_id).shard;
    }

    template <bool IofusedPrivateQueue = false>
    bool post_forwarded_task(const Task& task, uint32_t target) {
        ThreadCtx& destination = srv_->thread(target);
        if constexpr (IofusedPrivateQueue) {
            static_assert(Fused);
            return destination.post_iofused_task(
                self_->id(), task, handoff_ring(), self_->sig());
        } else {
            return destination.post_task(
                self_->id(), task, handoff_ring(), self_->sig());
        }
    }

    template <bool IofusedPrivateQueue = false>
    bool forward_stale_task(const Task& task) {
        const int32_t sid = task_shard(task);
        if (sid < 0) return false;
        const uint32_t target = srv_->worker_of_shard(sid);
        if (target == self_->id()) return false;
        if (!post_forwarded_task<IofusedPrivateQueue>(task, target))
            stale_tasks_.push_back(task);
        return true;
    }

    bool post_forwarded_release(const BorrowRelease& release, uint32_t target) {
        ThreadCtx& destination = srv_->thread(target);
        return destination.post_release(self_->id(), release, handoff_ring(), self_->sig());
    }

    bool forward_stale_release(const BorrowRelease& release) {
        if (release.shard < 0) std::abort();
        const uint32_t target = srv_->worker_of_shard(release.shard);
        if (target == self_->id()) return false;
        if (!post_forwarded_release(release, target)) stale_releases_.push_back(release);
        return true;
    }

    template <uint32_t BatchOps = kGenthreadExBatchOps,
              bool IofusedPrivateQueue = false>
    uint32_t service_stale_forwards() {
        uint32_t work = 0;
        while (!stale_tasks_.empty() && work < BatchOps) {
            const Task task = stale_tasks_.front();
            const int32_t sid = task_shard(task);
            if (sid < 0) std::abort();
            const uint32_t target = srv_->worker_of_shard(sid);
            if (target == self_->id()) {
                if constexpr (Fused) {
                    // A read-local fallback reached this FIFO after its own or an older group
                    // member's SPSC refusal. Executing it directly would jump the older queued
                    // tasks. Retry the tail publication and leave it stale until room exists.
                    if (read_local_enabled() && task.client &&
                        task.client->rob().at(task.op_id).read_local()) {
                        if (!post_forwarded_task(task, target)) break;
                        stale_tasks_.pop_front();
                        work++;
                        continue;
                    }
                }
                stale_tasks_.pop_front();
                if (!execute<IofusedPrivateQueue>(task)) xshard_retries_.push_back(task);
                work++;
                continue;
            }
            if (!post_forwarded_task<IofusedPrivateQueue>(task, target)) break;
            stale_tasks_.pop_front();
            work++;
        }
        while (!stale_releases_.empty() && work < BatchOps) {
            const BorrowRelease release = stale_releases_.front();
            const uint32_t target = srv_->worker_of_shard(release.shard);
            if (target == self_->id()) {
                stale_releases_.pop_front();
                srv_->shard(release.shard).store().unborrow(release.ptr);
                work++;
                continue;
            }
            if (!post_forwarded_release(release, target)) break;
            stale_releases_.pop_front();
            work++;
        }
        return work;
    }
    void flush_xshard_commits() {
        if (__builtin_expect(!xshard_commit_pending_, true)) return;
        xshard_flush_commits(
            *srv_, *self_, ring_, this,
            [](void* context, Client* client) {
                static_cast<ExLoopT*>(context)->notify_sender(client);
            });
        xshard_commit_pending_ = false;
    }

    // Tell this client's io thread it has completed ops -- it retires the ROB and writes, so it is
    // what needs waking. Deduplicated: a pipelined burst of N completions on one client must not
    // enqueue it N times.
    void notify_sender(Client* c) {
        const uint32_t target = c->ifid_thread();
        if constexpr (Fused) {
            if (target == self_->id()) {
                // Self-owned work was consumed from the same SPSC lane as remote work. Schedule
                // local retirement without a redundant cross-thread post or wake.
                if (!fused_completion_) std::abort();
                fused_completion_(fused_io_context_, c);
                return;
            }
        }
        // THE READY-MASK PATH (#19/#20 ported): once the sender has assigned this connection a
        // slot, completion signalling is one idempotent bit -- no claim, no channel entry, no
        // pointer in flight. The empty->flagged RMW is the fence; whoever performs it owes the
        // park-wake.
        const uint32_t slot = c->wb_slot();
        if (slot != Client::kNoWbSlot) {
            // INSIDE AN EXECUTOR BATCH: record and return. The fence, the read-first set and the
            // wake decision are paid once per batch in flush_notify_batch() (DESIGN-NOTIFY.md
            // §2, §4). A batch of one op degrades to exactly the sequence below it.
            if (__builtin_expect(notify_batch_open_, true)) {
                if (__builtin_expect(notify_batch_n_ == kNotifyBatchMax, false))
                    flush_notify_batch();
                notify_batch_[notify_batch_n_++] = NotifyEntry{target, slot};
                return;
            }
            ThreadCtx& snd = srv_->thread(target);
            // FENCE BEFORE THE READ-FIRST CHECK -- defect 5's exact shape, third appearance. Our
            // caller stored Done; ReadyMask::set() begins with a relaxed LOAD of the word, and TSO
            // lets that load run ahead of the store draining. Unfenced, it can read a stale 1 from
            // a signal the sender is consuming RIGHT NOW: we skip our set, the sender's drain reads
            // our op before the Done lands, and nobody ever signals again. Twelve connections were
            // stranded exactly this way at 4 nodes, where cross-CCD coherence stretches the window.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (snd.ready().set(slot))
                snd.wake_if_parked(handoff_ring(), self_->sig());
            return;
        }
        // No ready slot: use the claimed channel post. This remains the completion route
        // for a connection while the sender's ready-slot table is full.
        notify_sender_to(c, target);
    }

    void notify_sender_to(Client* c, uint32_t target) {
        bool expected = false;
        if (!c->retire_queued().compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            TOMO_FORENSIC(c->n_defers.fetch_add(1, std::memory_order_relaxed));
            return;
        }
        TOMO_FORENSIC(c->n_claims.fetch_add(1, std::memory_order_relaxed));
        ThreadCtx& snd = srv_->thread(target);
        if (!snd.post_client(self_->id(), c, handoff_ring(), self_->sig())) {
            self_->sig().notify_drop++;
            c->retire_queued().store(false, std::memory_order_release);   // retry on a later pass
        }
    }

    // Batch end (or a full record): ONE fence covering every Done store of the batch, then the
    // same read-first set per recorded client run that the per-op path performs, then one wake
    // decision per io that saw an empty->flagged edge. Deciding the wake once per io AFTER all of
    // its RMWs is equivalent to deciding it per edge: every RMW is a full barrier ahead of the
    // parked_ load, so the Dekker pair with ThreadCtx::arm_blocked closes for each edge on its
    // own (DESIGN-NOTIFY.md §4.4). The wake is never memoised across flushes -- a cached "already
    // woke this io" could straddle a park/unpark cycle of the consumer and miss its second park.
    void flush_notify_batch() {
        const uint32_t n = notify_batch_n_;
        if (!n) return;
        notify_batch_n_ = 0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        constexpr uint32_t kEdgeWords = (kMaxThreads + 63) / 64;
        uint64_t edged[kEdgeWords] = {};
        uint32_t prev_io = UINT32_MAX;
        uint32_t prev_slot = UINT32_MAX;
        for (uint32_t i = 0; i < n; i++) {
            const NotifyEntry e = notify_batch_[i];
            // A connection's completions arrive in runs, so the adjacent skip removes nearly
            // every repeat; a non-adjacent repeat costs one L1-hit load and is idempotent.
            if (e.io == prev_io && e.slot == prev_slot) continue;
            prev_io = e.io;
            prev_slot = e.slot;
            if (srv_->thread(e.io).ready().set(e.slot))
                edged[e.io >> 6] |= uint64_t{1} << (e.io & 63);
        }
        for (uint32_t w = 0; w < kEdgeWords; w++) {
            uint64_t bits = edged[w];
            while (bits) {
                const uint32_t b = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                srv_->thread(w * 64 + b).wake_if_parked(handoff_ring(), self_->sig());
            }
        }
    }

    // Opens the record for one executor batch and flushes it at scope exit. Nest-safe: an inner
    // scope restores the outer state and flushes early, which is always allowed.
    struct NotifyBatchScope {
        ExLoopT* loop;
        bool was_open;
        explicit NotifyBatchScope(ExLoopT* l) : loop(l), was_open(l->notify_batch_open_) {
            loop->notify_batch_open_ = true;
        }
        ~NotifyBatchScope() {
            loop->notify_batch_open_ = was_open;
            loop->flush_notify_batch();
        }
        NotifyBatchScope(const NotifyBatchScope&) = delete;
        NotifyBatchScope& operator=(const NotifyBatchScope&) = delete;
    };

    void on_cqe(io_uring_cqe* cqe) {
        // Executors issue no sends; the ring exists for wakes.
        if (ur_kind(cqe->user_data) == UrKind::Wake) self_->sig().wakes_recv++;
        else if (ur_kind(cqe->user_data) == UrKind::SnapshotStart)
            begin_snapshot(ur_ptr<SnapshotManager>(cqe->user_data));
        else if (ur_kind(cqe->user_data) == UrKind::Shutdown) return;
    }

    Server*    srv_  = nullptr;
    ThreadCtx* self_ = nullptr;
    Ring       ring_;
    int64_t    cached_now_ms_ = 0;
    int64_t    blocking_beat_ms_ = 0;
    size_t     expire_shard_cursor_ = 0;
    size_t     atomic_cleanup_cursor_ = 0;
    uint64_t   live_config_version_ = UINT64_MAX;
    AofManager* aof_manager_ = nullptr;
    bool       maxmemory_enabled_ = false;
    bool       ex_sched_enabled_ = false;
    uint8_t    cached_lru_clock_ = 0;
    uint32_t   lb_sample_rate_ = 0;
    uint32_t   lb_sample_countdown_ = 0;
    uint32_t   age_sample_rate_cached_ = 0;
    bool       lb_controller_armed_ = false;
    bool       lb_ack_wake_pending_ = false;
    bool       lb_rebind_pending_ = false;   // set at ExDrain ack; rebind owned shards after stage
    // The next two live in the five bytes of padding this run already had between the bools above
    // and the 8-aligned int64 below, so neither costs the object anything. Both are latched per
    // pass with the rest of the live-config snapshot and read ONLY from cold code -- the hot path
    // gates on maxmemory_enabled_ / a zero test and never loads either.
    uint8_t    foreign_touch_policy_ = kForeignTouchNone;   // see note_local_read_access()
    // TEST HOOK (DEBUG ATOMIC-FANOUT-DEFER), read-local half; production 0. See
    // debug_fanout_stall_local().
    uint32_t   debug_fanout_defer_us_ = 0;
    int64_t    lb_bytes_next_ms_ = 0;
    size_t     lb_bytes_shard_cursor_ = 0;
    // LFU logarithmic-increment dice for a lane-served read, per thread: the store's PRNG belongs
    // to its owner and must not be advanced from here. The only field in this class the read-local
    // eviction accounting actually adds (see the sizeof lock at the bottom of the file).
    uint64_t   foreign_touch_random_ = 0x9e3779b97f4a7c15ULL;
    SnapshotManager* snapshot_manager_ = nullptr;
    SnapshotOwnerState snapshot_owner_state_ = SnapshotOwnerState::None;
    uint64_t snapshot_epoch_ = 0;
    bool snapshot_was_cancelled_ = false;
    size_t snapshot_prepare_cursor_ = 0;
    size_t snapshot_progress_cursor_ = 0;
    std::vector<uint8_t> snapshot_done_shards_;
    std::vector<std::unique_ptr<SnapshotChunk>> snapshot_pending_chunks_;
    std::vector<std::deque<Task>> snapshot_backlogs_;
    std::deque<Task> atomic_deferred_;
    std::deque<Task> multi_retries_;
    std::deque<Task> xshard_retries_;
    std::deque<Task> ordered_deferred_;
    // Backpressure cannot turn a stale route into a dropped request/release. These queues contain
    // objects for shards this loop no longer owns and are serviced before ordinary inbox work.
    std::deque<Task> stale_tasks_;
    std::deque<BorrowRelease> stale_releases_;
    bool       notify_keyless_pending_ = false;
    bool       initialized_ = false;
    // Slow-log state at the true cold tail: nothing above it moves. `slowlog_armed_` is the single
    // predicted-false branch exec_batch pays when the feature is off.
    bool         slowlog_armed_ = false;
    SlowlogArm   slowlog_arm_{};
    SlowlogExState slowlog_state_{};
    // Fused-only state stays at the true tail so every split ExLoop field keeps its offset.
    uint32_t fused_idle_spins_ = 0;
    uint32_t fused_non_submit_rotations_ = 0;
    void* fused_io_context_ = nullptr;
    FusedCompletionFn fused_completion_ = nullptr;
    Ring* fused_handoff_ring_ = nullptr;
    // Both interwoven schedules share the measured 128-task geometry and coalesced N2 boundary.
    bool pipeline_batches_ = false;
    bool iofused_ = false;
    // Consumes the bool padding this class already carried before notify_batch_n_. Ordinary
    // commands pay one predicted-true test at an executor boundary and never touch the TLS
    // commit list.
    bool xshard_commit_pending_ = false;
    // Per-batch completion notification (DESIGN-NOTIFY.md §2). While a batch is open, the slot
    // path of notify_sender records (io, slot) here instead of fencing and setting per op; the
    // batch end pays ONE seq_cst fence, one ReadyMask::set per recorded client run and one wake
    // decision per io that saw an empty->flagged edge. Sized to this loop's largest batch; the
    // record never holds more than one entry per executed task, and a full record flushes in
    // place regardless. (io, slot), never Client*: the flush may run a whole batch after the Done
    // store, and a connection is only guaranteed allocated until the io's second reap prologue
    // after close.
    struct NotifyEntry { uint32_t io; uint32_t slot; };
    static constexpr uint32_t kNotifyBatchMax =
        Fused ? kGenthreadPipelineExBatchOps : kGenthreadExBatchOps;
    bool notify_batch_open_ = false;
    uint32_t notify_batch_n_ = 0;
    NotifyEntry notify_batch_[kNotifyBatchMax] = {};
    [[no_unique_address]] ReadLocalExState<Fused> read_local_;
};

using ExLoop = ExLoopT<false>;
using FusedExLoop = ExLoopT<true>;

// Disabled split executors retain the exact pre-read-local allocation stride plus the 264-byte
// per-batch notification record (DESIGN-NOTIFY.md §2): 5848 + 8 + 32 * sizeof(NotifyEntry).
// 6104 -> 6112: read-local eviction accounting adds exactly ONE word, the per-thread LFU dice
// (foreign_touch_random_). Its two companions -- the latched policy byte and the fan-out defer
// hook -- went into padding the lb bool run already carried and cost nothing. This is a per-
// EXECUTOR object, one per thread, not a per-op or per-connection footprint: Op, Client,
// ThreadCtx, Shard and Config are the locks that may not move, and none of them did.
// VESTCUT: removing the never-serving 256-byte WbEngine reduces 6112 to 5856 bytes.
static_assert(sizeof(ExLoop) == 5856);

}  // namespace tomo
