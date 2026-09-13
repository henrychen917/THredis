// read_local.h — fused read-lane owner-side deferred reclamation.
//
// Readers never enter/leave an epoch. Their entire foreign-pointer lifetime is bounded by one
// coarse fused rotation, whose ThreadCtx tick is published by every FusedExLoop pass (including
// snapshot-driven passes). A store owner unlinks first, then hands the retired allocation to this
// fixed ring; each unsealed FIFO suffix receives one shared post-unlink stamp at its next drain.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <sched.h>
#include <type_traits>
#include "server.h"
#include "../base/alloc.h"
#include "../store/kv_block_cache.h"
#ifdef TOMO_RL_CACHE_DEBUG
#include <cstdio>
#include <unordered_set>
#include <unistd.h>
#include <sys/syscall.h>
#endif
#include "../store/read_local_reclaim.h"
#include "../store/read_local_settax.h"

namespace tomo {

inline bool read_local_command_is_mget(const Op& op) {
    return op.spec && command_is_read_local_mget(*op.spec);
}

// MSET is a blind write to every key named by its regular key range. While its ROB slot is live,
// the armed RYOW ring can retain one descriptor for the command and use these immutable argv slices
// for exact overlap checks. Other multi-key writes remain conservative: source/destination roles,
// implicit writes, or execution-dependent effects make their registry key range insufficient.
// MSET's registry row is the only string-write candidate that steps by two (t_string.cc: first_key
// 1, last_key -1, key_step 2; armed shadow rows copy the row). Testing the already-loaded spec
// first lets every point op prove "not MSET" without loading argv[0] for the string compare.
inline bool read_local_command_is_precise_mset(const Op& op) {
    return op.spec && op.spec->key_step == 2 && op.cmd_name().eq_icase("mset");
}

// Owner-tail ordering is hash-conservative, like the RYOW write ring: an actual hash collision is
// allowed to take the owner path. MGET and precise MSET each keep one ROB slot, so every key in the
// command's immutable argv participates without consuming one ring entry per key.
inline bool read_local_command_touches_hash(const Op& op, uint64_t hash) {
    if (!read_local_command_is_mget(op) && !read_local_command_is_precise_mset(op))
        return op.hash == hash;
    const uint32_t step = static_cast<uint32_t>(op.spec->key_step);
    for (uint32_t arg = static_cast<uint32_t>(op.spec->first_key);
         arg < op.argc(); arg += step)
        if (FlatStore::hash_key(op.arg(arg)) == hash) return true;
    return false;
}

// The same walk, additionally reporting EVERY key hash of `op` into `keys` (no early exit). The
// demotion planner uses it to rebuild a connection's pending-read filter exactly while it is
// walking the pending set anyway. It must never disagree with touches_hash about an op's keys:
// the filter's "miss proves absence" contract rests on that.
inline bool read_local_command_touches_hash_collect(const Op& op, uint64_t hash,
                                                    ReadLocalPendingFilter& keys) {
    if (!read_local_command_is_mget(op) && !read_local_command_is_precise_mset(op)) {
        keys.add(op.hash);
        return op.hash == hash;
    }
    bool touched = false;
    const uint32_t step = static_cast<uint32_t>(op.spec->key_step);
    for (uint32_t arg = static_cast<uint32_t>(op.spec->first_key);
         arg < op.argc(); arg += step) {
        const uint64_t key = FlatStore::hash_key(op.arg(arg));
        keys.add(key);
        touched |= key == hash;
    }
    return touched;
}

// Pre-check for read_local_commands_overlap_precise_keyset below: false PROVES that no pending
// local read can share a key with `command`, because every pending read's keys are in `pending`
// and this walks exactly the keys touches_hash(command, .) would compare against.
inline bool read_local_pending_filter_may_touch_command(const ReadLocalPendingFilter& pending,
                                                        const Op& command) {
    if (!read_local_command_is_mget(command) && !read_local_command_is_precise_mset(command))
        return pending.may_contain(command.hash);
    const uint32_t step = static_cast<uint32_t>(command.spec->key_step);
    for (uint32_t arg = static_cast<uint32_t>(command.spec->first_key);
         arg < command.argc(); arg += step)
        if (pending.may_contain(FlatStore::hash_key(command.arg(arg)))) return true;
    return false;
}

inline bool read_local_command_is_precise_point(const Op& op) {
    if (!op.spec) return false;
    constexpr uint32_t kNonPointRoutes =
        CmdFlags::AllShards | CmdFlags::RandomShard | CmdFlags::CursorShard |
        CmdFlags::ConfigRoute | CmdFlags::MultiShard | CmdFlags::ScriptRoute |
        CmdFlags::Blocking | CmdFlags::Transaction | CmdFlags::StreamRoute |
        CmdFlags::SubcmdRoute;
    return (op.spec->flags & kNonPointRoutes) == 0 && op.spec->first_key > 0 &&
           op.spec->last_key == op.spec->first_key && op.spec->key_step == 1;
}

inline bool read_local_owner_command_is_precise(const Op& op) {
    return read_local_command_is_mget(op) || read_local_command_is_precise_point(op);
}

// A marked broad owner route remains conservative. Marked GETs, MGETs, and ordinary point routes
// carry enough immutable ROB metadata to fence only a later read of the same hash.
inline bool read_local_owner_command_touches_hash(const Op& op, uint64_t hash) {
    if (read_local_command_is_mget(op)) return read_local_command_touches_hash(op, hash);
    if (!read_local_owner_command_is_precise(op)) return true;
    return op.hash == hash;
}

// Callers use this only after proving that `command` mutates no key outside its declared keyset.
// Unlike owner-tail overlap, this deliberately accepts precise MSET as an exact command.
inline bool read_local_commands_overlap_precise_keyset(const Op& read, const Op& command) {
    if (!read_local_command_is_mget(read))
        return read_local_command_touches_hash(command, read.hash);
    for (uint32_t arg = 1; arg < read.argc(); arg++)
        if (read_local_command_touches_hash(
                command, FlatStore::hash_key(read.arg(arg)))) return true;
    return false;
}

// Same predicate, also reporting every key hash of `read` into `keys` (planner filter rebuild).
inline bool read_local_commands_overlap_precise_keyset_collect(
        const Op& read, const Op& command, ReadLocalPendingFilter& keys) {
    if (!read_local_command_is_mget(read)) {
        keys.add(read.hash);
        return read_local_command_touches_hash(command, read.hash);
    }
    bool overlap = false;
    for (uint32_t arg = 1; arg < read.argc(); arg++) {
        const uint64_t key = FlatStore::hash_key(read.arg(arg));
        keys.add(key);
        if (!overlap) overlap = read_local_command_touches_hash(command, key);
    }
    return overlap;
}

inline bool read_local_commands_overlap(const Op& read, const Op& owner) {
    if (!read_local_command_is_mget(read))
        return read_local_owner_command_touches_hash(owner, read.hash);
    for (uint32_t arg = 1; arg < read.argc(); arg++)
        if (read_local_owner_command_touches_hash(
                owner, FlatStore::hash_key(read.arg(arg)))) return true;
    return false;
}

// Index bookkeeping for the owner's deferred-reclaim ring, kept free of Server/ThreadCtx so it can
// be exercised without a booted process (tests/read_local_ring_unit.cc). Entry slots form a FIFO;
// every entry retired since the previous seal shares ONE stamp (the epoch that seal advanced past),
// recorded once per sealed batch rather than once per entry. The drain tests one stamp per batch and
// never scrubs a slot: the owner assigns every field of a slot before it is live again.
struct ReadLocalRetireRing {
    static constexpr uint32_t kCapacity = kReadLocalRetireRingCapacity;
    static_assert((kCapacity & (kCapacity - 1)) == 0);
    // One batch per owner pass that retired anything. When the batch ring is full the next suffix
    // folds into the NEWEST batch under the newer stamp, which can only delay those entries; QSBR
    // always permits reclaiming later, never earlier.
    static constexpr uint32_t kBatchCapacity = 256;
    static_assert((kBatchCapacity & (kBatchCapacity - 1)) == 0);

    struct Batch {
        uint64_t stamp;   // the OLD epoch value returned by the seal's fetch_add
        uint32_t count;   // entries in this batch, contiguous from the drain head
    };

    bool empty() const { return count == 0; }
    bool full() const { return count == kCapacity; }
    bool has_sealed() const { return batch_count != 0; }
    // Stamp of the oldest sealed batch. Requires has_sealed().
    uint64_t head_stamp() const { return batches[batch_head].stamp; }

    // Claims the next slot for the caller to fill. Requires !full().
    uint32_t push() {
        const uint32_t slot = tail;
        tail = (tail + 1) & (kCapacity - 1);
        count++;
        unsealed++;
        return slot;
    }

    // Covers every unsealed entry with `stamp` as one batch. Returns how many were sealed.
    uint32_t seal(uint64_t stamp) {
        const uint32_t sealed = unsealed;
        if (!sealed) return 0;
        if (batch_count == kBatchCapacity) {
            Batch& newest = batches[(batch_head + batch_count - 1) & (kBatchCapacity - 1)];
            newest.stamp = stamp;
            newest.count += sealed;
        } else {
            Batch& batch = batches[(batch_head + batch_count) & (kBatchCapacity - 1)];
            batch.stamp = stamp;
            batch.count = sealed;
            batch_count++;
        }
        unsealed = 0;
        return sealed;
    }

    // Pops every sealed batch whose stamp is below `floor`, oldest first, handing each entry slot
    // to `fn` in retirement order. Batches are FIFO with non-decreasing stamps, so the first batch
    // at or above the floor ends the drain exactly where a per-entry test would. Unsealed entries
    // are never popped here.
    template <typename Fn>
    uint32_t drain_below(uint64_t floor, Fn&& fn) {
        uint32_t drained = 0;
        while (batch_count) {
            const Batch& batch = batches[batch_head];
            if (batch.stamp >= floor) break;
            for (uint32_t remaining = batch.count; remaining; remaining--) {
                fn(head);
                head = (head + 1) & (kCapacity - 1);
            }
            count -= batch.count;
            drained += batch.count;
            batch_head = (batch_head + 1) & (kBatchCapacity - 1);
            batch_count--;
        }
        return drained;
    }

    // Pops everything, sealed or not. Only valid once no reader can retain a pointer (shutdown).
    template <typename Fn>
    uint32_t drain_all(Fn&& fn) {
        const uint32_t drained = count;
        while (count) {
            fn(head);
            head = (head + 1) & (kCapacity - 1);
            count--;
        }
        unsealed = 0;
        batch_head = 0;
        batch_count = 0;
        return drained;
    }

    Batch batches[kBatchCapacity] = {};
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t count = 0;
    uint32_t unsealed = 0;      // entries at the tail not yet covered by a batch
    uint32_t batch_head = 0;
    uint32_t batch_count = 0;
};

class ReadLocalDeferredQueue {
public:
    static constexpr uint32_t kCapacity = ReadLocalRetireRing::kCapacity;

    // The owner's block cache dies with the owner; nothing can still reference its blocks by then
    // (drain_shutdown ran, and a cached block is by construction referenced by nobody).
    ~ReadLocalDeferredQueue() { block_cache_.release_all(); }

    bool init(Server* server, ThreadCtx* owner) {
        server_ = server;
        owner_ = owner;
        entries_.reset(new (std::nothrow) Entry[kCapacity]);
        if (!entries_) return false;
        sink_.context = this;
        sink_.defer = &ReadLocalDeferredQueue::defer_thunk;
        // Every store this owner drives shares ONE cache: a single head array that stays hot in
        // L1 for every armed write, whatever shard the key lands on. See kv_block_cache.h.
        sink_.block_cache = &block_cache_;
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        sink_.bind_settax_stats(&owner_->read_local_stats().settax);
#endif
        return true;
    }

    ReadLocalRetireSink* sink() { return entries_ ? &sink_ : nullptr; }
    bool empty() const { return ring_.empty(); }
    uint32_t size() const { return ring_.count; }

    // Called only by this owner thread, after the object/table is no longer store-reachable.
    // Arguments are not re-validated: the sink is handed out only once entries_ exists (sink()),
    // `reclaim` is always one of FlatStore's three static callbacks, and every producer tests its
    // payload before retiring it. A null in any of them would fault at reclaim in the same pass.
    void defer(void* reclaim_owner, void* payload, size_t auxiliary,
               ReadLocalRetireSink::ReclaimFn reclaim) {
#ifdef TOMO_RL_CACHE_DEBUG
        // A payload may be in this ring at most once. Retiring one object twice puts it into the
        // owner's block cache twice, which splices a CYCLE into that class's free list; the block
        // is then handed out while still linked, its header overwrites the `next` word, and the
        // next take() either aborts on the allocation check or dereferences a wild head. That is
        // the corruption this P0 is chasing, so name it at the double retire instead.
        dbg_check_owner("defer");
        if (!dbg_pending_.insert(payload).second) {
            std::fprintf(stderr,
                "\nRLRING-VIOLATION double-retire: payload %p aux %zu reclaim %p (queue %p, "
                "ring count %u)\n", payload, auxiliary,
                reinterpret_cast<void*>(reclaim), static_cast<void*>(this), ring_.count);
            std::fflush(stderr);
            std::abort();
        }
#endif
        if (ring_.full()) force_oldest_grace();
        Entry& entry = entries_[ring_.push()];
        entry.owner = reclaim_owner;
        entry.payload = payload;
        entry.auxiliary = auxiliary;
        entry.reclaim = reclaim;
        // The next drain (or rare capacity seal) performs the global epoch RMW after every unlink
        // in this suffix. Sharing that stamp avoids one globally contended RMW per retired object.
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        ReadLocalSetTaxStats& stats = settax_stats();
        stats.qsbr_deferrals++;
        if (auxiliary) stats.qsbr_object_deferrals++;
        else stats.qsbr_table_deferrals++;
        stats.qsbr_depth = ring_.count;
        stats.qsbr_max_owner_depth = std::max<uint64_t>(
            stats.qsbr_max_owner_depth, ring_.count);
        stats.qsbr_depth_samples++;
        stats.qsbr_depth_sum += ring_.count;
#endif
    }

    uint32_t drain_ready() {
        if (ring_.empty()) return 0;
        seal_pending();
        // One participant scan per owner pass, not per retired allocation. Stamps are FIFO and
        // strictly below the returned floor only after every active tick has crossed them; parked
        // participants contribute infinity because they hold no foreign pointer.
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        ReadLocalSetTaxStats& stats = settax_stats();
        stats.qsbr_grace_scans++;
        stats.qsbr_participant_loads += server_->nthreads();
#endif
        // Ask only whether the OLDEST sealed stamp has been crossed. Nothing is releasable until it
        // has, so the scan may stop at the first participant still below it (and re-test that one
        // first next pass) instead of loading every participant's tick line each pass. The ring is
        // non-empty here and seal_pending() just ran, so a sealed batch exists.
        const uint64_t grace_floor =
            server_->read_local_grace_floor(ring_.head_stamp(), grace_hint_);
        const uint32_t drained = ring_.drain_below(
            grace_floor, [this](uint32_t slot) { reclaim_entry(entries_[slot]); });
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        stats.qsbr_reclaims += drained;
        if (!drained) stats.qsbr_zero_progress_scans++;
        stats.qsbr_depth = ring_.count;
#endif
        return drained;
    }

    // All fused threads have joined; no epoch test is needed and no reader can retain a pointer.
    uint32_t drain_shutdown() {
#ifdef TOMO_RL_CACHE_DEBUG
        // genthread.cc drains every owner's queue from the MAIN thread after pool.join(). That
        // crossing is single-threaded by construction; adopt the caller so the owner assertions
        // below police only live operation, which is where a crossing would be the defect.
        dbg_shutdown_ = true;
        dbg_owner_tid_ = static_cast<long>(::syscall(SYS_gettid));
        block_cache_.dbg_owner_tid = dbg_owner_tid_;
#endif
        const uint32_t drained = ring_.drain_all(
            [this](uint32_t slot) { reclaim_entry(entries_[slot]); });
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        ReadLocalSetTaxStats& stats = settax_stats();
        stats.qsbr_reclaims += drained;
        stats.qsbr_depth = 0;
#endif
        // Shutdown returns the cache too: the store destructors that follow free the LIVE objects,
        // and these blocks are live to nobody.
        block_cache_.release_all();
        return drained;
    }

private:
    // 32 bytes: two entries per cache line. No stamp here -- it belongs to the ring's sealed batch.
    struct Entry {
        void* owner = nullptr;
        void* payload = nullptr;
        size_t auxiliary = 0;
        ReadLocalRetireSink::ReclaimFn reclaim = nullptr;
    };
    static_assert(sizeof(Entry) == 32);

    static void defer_thunk(void* context, void* owner, void* payload,
                            size_t auxiliary, ReadLocalRetireSink::ReclaimFn reclaim) {
        static_cast<ReadLocalDeferredQueue*>(context)->defer(
            owner, payload, auxiliary, reclaim);
    }

    // A reclaimed slot is not scrubbed: defer() assigns every field before the slot is live again,
    // and nothing reads a slot outside [head_, tail_).
    void reclaim_entry(Entry& entry) {
#ifdef TOMO_RL_CACHE_DEBUG
        dbg_check_owner("reclaim");
        if (dbg_pending_.erase(entry.payload) != 1) {
            std::fprintf(stderr,
                "\nRLRING-VIOLATION reclaim-of-unpending: payload %p (queue %p)\n",
                entry.payload, static_cast<void*>(this));
            std::fflush(stderr);
            std::abort();
        }
#endif
        entry.reclaim(sink_, entry.owner, entry.payload, entry.auxiliary);
    }

    void seal_pending() {
        if (!ring_.unsealed) return;
        // Every object/table in the suffix was unlinked before this seq-cst RMW. The returned old
        // epoch is therefore a valid common retirement stamp for the entire owner-pass batch --
        // recorded once, not once per entry.
        [[maybe_unused]] const uint32_t sealed =
            ring_.seal(server_->advance_read_local_epoch());
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        ReadLocalSetTaxStats& stats = settax_stats();
        stats.qsbr_seals++;
        stats.qsbr_sealed_entries += sealed;
#endif
    }

    void force_oldest_grace() {
        // The freeing thread is also a QSBR participant. It has finished this rotation's local-read
        // copy before owner mutation can reach here, so publishing its current epoch is the safe
        // point that prevents a full list from waiting on itself.
        seal_pending();
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        settax_stats().qsbr_forced_graces++;
#endif
        while (ring_.full()) {
            // Preserve a permanent/parked publication if shutdown cleanup reaches this path.
            owner_->refresh_read_local_quiescence(server_->read_local_epoch());
            if (!drain_ready()) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                settax_stats().qsbr_forced_yields++;
#endif
                __builtin_ia32_pause();
                ::sched_yield();
            }
        }
    }

#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
    ReadLocalSetTaxStats& settax_stats() {
        ReadLocalSetTaxStats* stats = sink_.diagnostics();
        if (!stats) std::abort();
        return *stats;
    }
#endif

#ifdef TOMO_RL_CACHE_DEBUG
    // The retire ring is the owner's private QSBR list. Both laws -- one thread, each payload
    // resident at most once -- are stated here as assertions in the debug build only.
    long dbg_owner_tid_ = 0;
    bool dbg_shutdown_ = false;
    std::unordered_set<void*> dbg_pending_;
    void dbg_check_owner(const char* op) {
        const long tid = static_cast<long>(::syscall(SYS_gettid));
        if (!dbg_owner_tid_) { dbg_owner_tid_ = tid; return; }
        if (dbg_owner_tid_ != tid && !dbg_shutdown_) {
            std::fprintf(stderr,
                "\nRLRING-VIOLATION owner: %s on queue %p from tid %ld, owner tid %ld\n",
                op, static_cast<void*>(this), tid, dbg_owner_tid_);
            std::fflush(stderr);
            std::abort();
        }
    }
#endif

    KvBlockCache block_cache_{};
    Server* server_ = nullptr;
    ThreadCtx* owner_ = nullptr;
    std::unique_ptr<Entry[]> entries_;
    ReadLocalRetireSink sink_;
    ReadLocalRetireRing ring_;
    uint32_t grace_hint_ = UINT32_MAX;   // last participant seen blocking the oldest batch
};

}  // namespace tomo
