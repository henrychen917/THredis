// shard.h — the unit of OWNERSHIP and of MIGRATION.
//
// A shard owns a contiguous range of the 16,384 routing buckets and every key hashing into it, plus
// its own FlatStore. Exactly one thread touches a given shard at a time, which is the invariant the
// whole design rests on. The boot-disabled fused read-local lane is the narrow exception: foreign
// threads perform read-only probes under a per-store table generation and rotation QSBR, while
// every mutation remains on this single owner. With that lane off, DEL retains the immediate-free
// path unless the allocation is explicitly borrowed by the wire send path.
//
// NO NODE LAYER. The keyspace is one flat set of shards over the whole server; there is no NUMA or
// L3 partitioning of it. That is a deliberate simplification and it gives up a measured gain — on
// this box, partitioning at one L3 domain per node was worth +22.3% on set_p16 and +14.2% on mget8,
// with the optimum landing exactly ON the cache boundary. The structures below stay locality-AWARE
// so a later flip/LB controller can recover that gain by PLACEMENT rather than by partitioning.
//
// SHARDS ARE DECOUPLED FROM THREADS ON PURPOSE. A worker executes one or more shards, and which
// shards it executes can change. Ownership therefore moves by reassigning a shard, never by copying
// a key — which is also what makes resharding O(1).
//
// MIGRATION IS NOT FREE, and this is the part that is easy to get wrong. An L3 domain is filled by
// access, not allocated into. Moving a shard from a worker in domain A to one in domain B does not
// move its memory; it invalidates its residency, and the new worker must re-pull the working set
// through the fabric. On this box a CCX-to-fabric link saturates near 51 GB/s while a single core
// can already pull 50 — so a migration is a real cost, and an LB that prices it at zero will thrash.
// home_domain() and store().resident_estimate() exist so it can be priced instead of guessed.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../base/topology.h"
#include "../store/flatstore.h"
#include "../cmd/notify.h"

namespace tomo {

class Client;
class Op;
class Server;
struct ShardLayoutLock;

// 16,384 buckets. Chosen so changing the shard count reassigns bucket RANGES rather than rehashing
// keys: a key's bucket never changes, only which shard owns that bucket.
inline constexpr uint32_t kNumBuckets = 16384;
inline constexpr uint32_t kBucketMask = kNumBuckets - 1;

// The router takes the LOW bits. FlatStore mixes before indexing so its slot choice stays
// independent of these — see the clustering trap in flatstore.h.
inline uint32_t bucket_of(uint64_t hash) { return static_cast<uint32_t>(hash) & kBucketMask; }

class Shard {
public:
    Shard() = default;
    ~Shard();
    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;

    void init(Server* server, int32_t id, uint32_t bucket_begin, uint32_t bucket_end, uint32_t zc_min,
              const TypeLimits& type_limits, const StreamLimits& stream_limits) {
        server_ = server;
        id_ = id;
        bucket_begin_ = bucket_begin;
        bucket_end_   = bucket_end;
        zc_min_ = zc_min;
        type_limits_ = type_limits;
        stream_limits_ = stream_limits;
        store_.bind_expired_counter(&stats_.expired);
        store_.bind_evicted_counter(&stats_.evicted);
        store_.bind_rehash_counter(&stats_.rehashes);
        flat_notify_sink_.context = this;
        flat_notify_sink_.enabled = notify_flat_enabled;
        flat_notify_sink_.emit = notify_flat_emit;
        notify_bind_flat_store(&store_, &flat_notify_sink_);
    }

    // A cross-owner script executes against a private, executor-thread-local workbench. It owns
    // every routing bucket logically, borrows nothing, and deliberately does not join the global
    // notification binding list or bind AOF/MVCC/maxmemory state. Nested notify-aware handlers pass
    // this sink explicitly, so the normal event builder still sees their exact command sequence.
    void init_private(Server* server, int32_t id, const TypeLimits& type_limits,
                      const StreamLimits& stream_limits) {
        server_ = server;
        id_ = id;
        bucket_begin_ = 0;
        bucket_end_ = kNumBuckets;
        zc_min_ = UINT32_MAX;
        type_limits_ = type_limits;
        stream_limits_ = stream_limits;
        store_.bind_expired_counter(&stats_.expired);
        store_.bind_evicted_counter(&stats_.evicted);
        flat_notify_sink_.context = this;
        flat_notify_sink_.enabled = notify_flat_enabled;
        flat_notify_sink_.emit = notify_flat_emit;
    }

    int32_t  id() const { return id_; }
    bool     owns(uint32_t bucket) const { return bucket >= bucket_begin_ && bucket < bucket_end_; }
    uint32_t bucket_begin() const { return bucket_begin_; }
    uint32_t bucket_end()   const { return bucket_end_; }
    uint32_t zc_min()       const { return zc_min_; }
    void set_zc_min(uint32_t value) { zc_min_ = value; }
    int64_t  now_ms()       const { return now_ms_; }
    const TypeLimits& type_limits() const { return type_limits_; }
    void set_type_limits(const TypeLimits& value) { type_limits_ = value; }
    const StreamLimits& stream_limits() const { return stream_limits_; }
    void set_stream_limits(const StreamLimits& value) { stream_limits_ = value; }

    void set_cached_now_ms(int64_t now_ms, uint8_t lru_clock = 0) {
        now_ms_ = now_ms;
        store_.set_cached_now_ms(now_ms);
        store_.set_cached_lru_clock(lru_clock);
    }
    // Install an OPERATION-WIDE expiry cut for the duration of one fan-out fragment, leaving the
    // LRU clock exactly as the executor published it for this pass. Separate from
    // set_cached_now_ms because that one's lru_clock parameter defaults to 0, and a fragment that
    // reset the LRU clock to zero would corrupt eviction sampling on every cross-shard command.
    void pin_now_ms(int64_t now_ms) {
        now_ms_ = now_ms;
        store_.set_cached_now_ms(now_ms);
    }
    void set_no_touch(bool value) { store_.set_no_touch(value); }
    void configure_maxmemory(bool enabled, uint64_t shard_limit, MaxmemoryPolicy policy,
                             uint32_t samples) {
        store_.configure_maxmemory(enabled, shard_limit, policy, samples);
    }
    void bind_atomic_state(FlatStore::AtomicTicketFn ticket_fn, void* ticket_ctx,
                           std::atomic<uint64_t>* activity,
                           std::atomic<uint64_t>* script_intents) {
        store_.bind_atomic_state(ticket_fn, ticket_ctx, activity, script_intents,
                                 &stats_.atomic_predecessor_reads,
                                 &stats_.atomic_chain_max,
                                 &stats_.atomic_promotions,
                                 &stats_.atomic_records_freed,
                                 &stats_.atomic_entries,
                                 &stats_.atomic_gauge_underflows);
    }
    uint32_t active_expire(uint32_t budget) {
        const uint32_t work = store_.active_expire(budget);
        if (work) publish_active_expire_reap_lag();
        return work;
    }

    // Refreshed once per executor pass through the existing live-config seqlock.  A null store
    // sink is the complete off state: no notification allocation or callback is reachable.
    void set_notify_mask(uint32_t mask) {
        notify_mask_ = mask;
    }
    uint32_t notify_mask() const { return notify_mask_; }
    void note_save_change(uint64_t count = 1) {
        save_changes_.fetch_add(count, std::memory_order_relaxed);
    }
    uint64_t save_changes() const { return save_changes_.load(std::memory_order_relaxed); }
    void set_notify_context(Op* carrier, Op* source, uint32_t order_base) {
        notify_carrier_ = carrier;
        notify_source_ = source;
        notify_order_base_ = order_base;
    }
    Op* notify_carrier() const { return notify_carrier_; }
    Op* notify_source() const { return notify_source_; }
    uint32_t notify_order_base() const { return notify_order_base_; }
    std::unique_ptr<NotifyShardState>& notify_state_slot() { return notify_state_; }
    void bind_notify_pending(bool* pending) { notify_pending_ = pending; }
    void notify_output_created() {
        if (notify_pending_) *notify_pending_ = true;
    }
    bool notify_output_pending() const {
        return notify_state_ && !notify_state_->keyless.empty();
    }
    Server* server() const { return server_; }

    // The registry pointer and its map are owner-only.  Cross-thread INFO and atomic publication
    // touch only these two atomics; ordinary shards retain one predicted-false waiter check.
    bool has_blocking_waiters() const {
        return blocking_waiters_.load(std::memory_order_relaxed) != 0;
    }
    uint64_t blocking_waiters() const {
        return blocking_waiters_.load(std::memory_order_relaxed);
    }
    void set_blocking_dirty() { blocking_dirty_.store(true, std::memory_order_release); }
    bool take_blocking_dirty() { return blocking_dirty_.exchange(false, std::memory_order_acquire); }
    void*& blocking_registry_slot() { return blocking_registry_; }
    void blocking_waiter_added() { blocking_waiters_.fetch_add(1, std::memory_order_relaxed); }
    void blocking_waiter_removed() { blocking_waiters_.fetch_sub(1, std::memory_order_relaxed); }

    FlatStore&       store()       { return store_; }
    const FlatStore& store() const { return store_; }

    template <bool kNotify>
    KvObj* store_find(uint64_t hash, Slice key) {
        if constexpr (kNotify) return store_.find_notify(hash, key, &flat_notify_sink_);
        return store_.find(hash, key);
    }
    // Redis attaches keyspace hit/miss accounting to lookupKeyRead*, not to commands. Keeping the
    // distinction at TomoKV's equivalent lookup boundary makes multi-key reads count per key while
    // write lookups remain invisible even when the command returns the old value.
    void note_keyspace_read(const KvObj* object) {
        if (object) stats_.hits++;
        else stats_.misses++;
    }
    template <bool kNotify>
    KvObj* store_find_read(uint64_t hash, Slice key) {
        KvObj* object = store_find<kNotify>(hash, key);
        note_keyspace_read(object);
        return object;
    }
    KvObj* store_find_read_no_touch(uint64_t hash, Slice key) {
        KvObj* object = store_.find_no_touch(hash, key);
        note_keyspace_read(object);
        return object;
    }
    template <bool kNotify>
    FlatStore::InsertResult store_insert(uint64_t hash, KvObj* object) {
        if constexpr (kNotify) return store_.insert_notify(hash, object, &flat_notify_sink_);
        return store_.insert(hash, object);
    }
    template <bool kNotify>
    bool store_erase(uint64_t hash, Slice key,
                     FlatStore::EraseEvent event = FlatStore::EraseEvent::Del) {
        if constexpr (kNotify) return store_.erase_notify(hash, key, &flat_notify_sink_, event);
        return store_.erase(hash, key);
    }
    template <bool kNotify>
    FlatStore::OverwriteResult store_try_overwrite(uint64_t hash, Slice key, Slice value) {
        if constexpr (kNotify)
            return store_.try_overwrite_notify(hash, key, value, &flat_notify_sink_);
        return store_.try_overwrite(hash, key, value);
    }
    template <bool kNotify>
    FlatStore::TtlResult store_set_expire(uint64_t hash, Slice key, int64_t at) {
        if constexpr (kNotify) return store_.set_expire_notify(hash, key, at, &flat_notify_sink_);
        return store_.set_expire(hash, key, at);
    }
    template <bool kNotify>
    FlatStore::TtlResult store_persist(uint64_t hash, Slice key) {
        if constexpr (kNotify) return store_.persist_notify(hash, key, &flat_notify_sink_);
        return store_.persist(hash, key);
    }

    // WATCH state is touched only by this shard's executor.  The maps remain empty until the first
    // WATCH, so ordinary reads and writes allocate nothing and never enter registry code.
    struct WatchEntry {
        Client* client = nullptr;
        uint64_t generation = 0;
        // Captured while WATCH is armed, and only then. Expiry is an owner-side mutation with no
        // command behind it, so nothing calls watch_write_committed() for it; recognising that one
        // transition needs the deadline the key carried when the client armed the WATCH.
        int64_t expire_at_ms = -1;
    };
    struct WatchReservation {
        const void* token = nullptr;
        std::atomic<uint64_t>* epoch = nullptr;
        std::atomic<bool>* aborted = nullptr;
        std::atomic<uint32_t>* refs = nullptr;
        Client* writer = nullptr;
        uint64_t writer_generation = 0;
        bool mutates = false;
        // Does this reservation make another unit WAIT?
        //
        // A reservation from the EXEC validate path carries an ORDERING claim: the transaction has
        // validated its WATCH on this key, so no foreign write may commit ahead of its decision.
        // A writer therefore has to wait for it, and that wait is bounded because the transaction
        // is already fanned out and decides on its own.
        //
        // A reservation from the atomic-group WRITE path carries no such claim. It exists only to
        // defer THIS group's watcher-dirtying to its own commit decision. Making it exclusive --
        // one slot per key, everybody else waits -- turned it into a cross-shard lock taken in
        // arbitrary order, and two groups that each hold a key the other needs is a wait-for cycle
        // with no timeout and no detector: tests/watchlive.py wedged 6/6 on it. Group reservations
        // now coexist on a key (the map holds a list) and block nobody.
        bool blocking = false;
    };
    bool has_watches() const { return !watchers_.empty() || !watch_reservations_.empty(); }
    bool watch_add(Slice key, Client* client, uint64_t generation);
    void watch_remove(Slice key, Client* client, uint64_t generation);
    bool watch_validate_and_reserve(Slice key, Client* client, uint64_t generation,
                                    const void* token, std::atomic<uint64_t>* epoch,
                                    std::atomic<bool>* aborted,
                                    std::atomic<uint32_t>* refs, bool mutates);
    bool watch_write_ready(Slice key, const void* token = nullptr);
    void watch_reserve_write(Slice key, Client* writer, uint64_t writer_generation,
                             const void* token, std::atomic<uint64_t>* epoch,
                             std::atomic<bool>* aborted, std::atomic<uint32_t>* refs);
    void watch_write_committed(Slice key, Client* writer = nullptr,
                               uint64_t writer_generation = 0);
    bool watch_all_write_ready();
    void watch_all_write_committed();
    bool watch_finalize_reservation(const std::string& key);
    bool watch_append_reservation(const std::string& key, const WatchReservation& reservation);
    void watch_prune_stale(const std::string& key);

    // ---- locality --------------------------------------------------------------------------------
    // The L3 domain this shard's working set is currently resident in — i.e. the domain of the
    // worker that has been executing it. Set on first execution and on migration.
    uint32_t home_domain() const { return home_domain_; }

    // What moving this shard would cost: bytes the new domain must re-pull through the fabric.
    size_t migration_cost_bytes() const { return store_.resident_estimate(); }

    // Published for cross-shard readers (DBSIZE, INFO). Updated once per executed batch rather than
    // per op: a per-op store to a line that other threads poll is exactly the shared-line write the
    // design avoids everywhere else. Slightly stale by construction, which is correct for a stat.
    void publish_size() {
        published_size_.store(store_.size(), std::memory_order_relaxed);
        published_obj_bytes_.store(store_.object_bytes(), std::memory_order_relaxed);
        published_expires_.store(store_.expire_count(), std::memory_order_relaxed);
        published_evicted_.store(stats_.evicted, std::memory_order_relaxed);
    }
    uint32_t published_size() const { return published_size_.load(std::memory_order_relaxed); }
    uint64_t published_obj_bytes() const {
        return published_obj_bytes_.load(std::memory_order_relaxed);
    }
    uint32_t published_expires() const {
        return published_expires_.load(std::memory_order_relaxed);
    }
    uint32_t published_active_expire_reap_lag_ms_max() const {
        return published_active_expire_reap_lag_ms_max_.load(std::memory_order_relaxed);
    }
    uint64_t published_evicted() const {
        return published_evicted_.load(std::memory_order_relaxed);
    }

    // Weighted placement is boot-latched. Keeping all three arrays on the immutable physical
    // shard makes the signal follow the BUCKET, not its current executor: changing ownership never
    // resets or transfers controller history. Only the current shard owner writes these arrays.
    bool enable_lb_signals() {
        const uint32_t n = bucket_end_ - bucket_begin_;
        try {
            lb_bucket_samples_ = std::make_unique<uint32_t[]>(n);
            lb_bucket_bytes_ = std::make_unique<uint64_t[]>(n);
            lb_bucket_bytes_staging_ = std::make_unique<uint64_t[]>(n);
            return true;
        } catch (const std::bad_alloc&) {
            lb_bucket_samples_.reset();
            lb_bucket_bytes_.reset();
            lb_bucket_bytes_staging_.reset();
            return false;
        }
    }
    void note_lb_sample(uint64_t hash, uint32_t rate) {
        const uint32_t bucket = bucket_of(hash);
        if (!lb_bucket_samples_ || !owns(bucket)) return;
        lb_bucket_samples_[bucket - bucket_begin_] += rate;
    }
    uint32_t lb_bucket_samples(uint32_t bucket) const {
        if (!lb_bucket_samples_ || !owns(bucket)) return 0;
        return __atomic_load_n(&lb_bucket_samples_[bucket - bucket_begin_], __ATOMIC_RELAXED);
    }
    uint64_t lb_bucket_bytes(uint32_t bucket) const {
        if (!lb_bucket_bytes_ || !owns(bucket)) return 0;
        return __atomic_load_n(&lb_bucket_bytes_[bucket - bucket_begin_], __ATOMIC_RELAXED);
    }

    // A rolling owner-only census keeps memory fairness separate from request demand without a
    // random counter write on every SET/DEL. One bounded scan slice runs at a time; a complete
    // pass publishes the bucket vector atomically entry-by-entry, then starts a fresh census.
    bool lb_scan_bucket_bytes(uint32_t homes) {
        if (!lb_bucket_bytes_staging_ || !homes) return false;
        // expire_on_visit=false: this census runs from the executor loop at its own cadence, not
        // inside any logical operation. Expiring here deletes keys OUTSIDE every in-flight
        // operation's pinned cut and tears cross-shard reads (the expwide S1 failure). Counting a
        // dead-but-unreaped key's bytes is a rounding error; deleting it here is a torn read.
        lb_bytes_cursor_ = store_.scan(lb_bytes_cursor_, homes, [&](KvObj* object) {
            const uint64_t hash = FlatStore::hash_key(object->key());
            const uint32_t bucket = bucket_of(hash);
            if (owns(bucket)) lb_bucket_bytes_staging_[bucket - bucket_begin_] += kvobj_size(object);
        }, /*expire_on_visit=*/false);
        if (lb_bytes_cursor_ != 0) return false;
        const uint32_t n = bucket_end_ - bucket_begin_;
        for (uint32_t i = 0; i < n; i++) {
            __atomic_store_n(&lb_bucket_bytes_[i], lb_bucket_bytes_staging_[i], __ATOMIC_RELAXED);
            lb_bucket_bytes_staging_[i] = 0;
        }
        return true;
    }

    // Called by the executing worker on every op. `worker_domain` is that thread's L3 domain.
    // Cheap by construction: one compare and one increment, no atomics — the shard is single-owner.
    void note_execution(uint32_t worker_domain) {
        stats_.ops++;
        if (home_domain_ == kNoDomain) { home_domain_ = worker_domain; return; }
        if (worker_domain != home_domain_) stats_.foreign_ops++;
    }

    // Records that ownership moved to a worker in `new_domain`. Residency is now stale.
    void note_migration(uint32_t new_domain) {
        if (new_domain != home_domain_) {
            stats_.migrations++;
            stats_.migrated_bytes += migration_cost_bytes();
            home_domain_ = new_domain;
        }
    }

    // Per-shard counters, single-writer so no two threads share a line. INFO sums them; a global
    // counter incremented per command would be a shared-line write on the hot path.
    struct Stats {
        uint64_t ops           = 0;
        uint64_t hits          = 0;
        uint64_t misses        = 0;
        uint64_t expired       = 0;
        uint64_t evicted       = 0;
        // THE actionable locality signal: ops executed by a worker in a different L3 domain than the
        // one holding this shard's working set. A high ratio means placement is wrong, and it is the
        // number a flip/LB controller should act on — not a guess from thread ids or core counts.
        uint64_t foreign_ops   = 0;
        uint64_t migrations    = 0;
        uint64_t migrated_bytes = 0;
        uint64_t atomic_predecessor_reads = 0;
        uint64_t atomic_chain_max = 0;
        uint64_t atomic_promotions = 0;
        uint64_t atomic_records_freed = 0;
        uint64_t atomic_entries = 0;
        // Version-bytes gauge decrements that would have taken the owner-local gauge below zero.
        // It replaced a std::abort(): an over-decrement means the store returned more version bytes
        // than it charged, which is a real accounting fault worth a test assertion but not worth
        // killing the process for. Must read 0; tests/execfix.py asserts it in both atomic modes.
        uint64_t atomic_gauge_underflows = 0;
        // Times a transaction met an OLDER, still-UNDECIDED unit of the SAME connection on this
        // owner -- the window in which those two units' commit tickets can invert AND in which
        // the older unit's installed-but-withdrawable candidate is exposed to the transaction
        // through the store's connection-scoped RYOW overlay. ONE site raises it: ExLoop::execute(),
        // where the transaction FRAGMENT is parked behind such a unit before it installs anything
        // here. It is a real hold, and it is what stops an aborted MSETNX's candidate from being
        // cloned into an acknowledged transaction write (tests/multirace.py). Cold, and on a path
        // that already walks the owner's pending list.
        // It must be able to read zero -- a transaction with no such predecessor never touches it
        // -- so a non-zero reading is proof the window opened rather than proof the test ran.
        // The question "did the park fire EARLY ENOUGH" is a different one and deliberately does
        // NOT share this counter: multi.inc's prepare_write_key() re-asks it at install time and
        // reports violations through INFO atomic_exec_order_late (multi_exec_order_late()), which
        // must read zero. Summing the two would hide a violation inside a number that is non-zero
        // by design, leaving the park's safety argument with no falsifier (NOTES-MULTIRACE.md §5).
        uint64_t atomic_exec_order_holds = 0;
        // Times watch_finalize_reservation() answered "not ready" because the reservation's epoch
        // was still 0, i.e. a unit was turned into a Retry by an undecided WATCH reservation. It
        // is the ONLY direct evidence that the WATCH-liveness path was entered at all, so
        // tests/watchlive.py asserts it advanced before believing a clean run. Reached only from
        // the reservation registry, which stays empty until the first WATCH: no WATCH, no cost.
        uint64_t watch_reservation_waits = 0;
        // Times an atomic-group write recorded its reservation on a key that ALREADY carried a
        // foreign undecided one. That is exactly the configuration that used to answer "not ready"
        // and re-queue the writer, and two of them crossed over two shards is the wait-for cycle
        // tests/watchlive.py locks. It must be able to read zero (no WATCH, or no overlap), so a
        // non-zero reading is proof the cycle's precondition really occurred and was survived --
        // which is why the gate refuses to pass an armed row that never moved it.
        uint64_t watch_reservation_coexist = 0;
        // Times a transaction's WATCH validation took the serialization in which a foreign
        // atomic-group write to that watched key lands FIRST, and therefore aborted instead of
        // waiting for that group's decision. Counting it keeps the semantic choice visible.
        uint64_t watch_reservation_precommit_aborts = 0;
        // Cold tail, deliberately last: keyspace table rebuilds started by this shard, reported as
        // INFO keyspace_rehashes so a scan-under-resize test can assert the hazard it guards
        // actually fired. Written once per resize and read only by INFO, so it must not push any
        // field the per-op path touches onto a different cache line.
        uint64_t rehashes      = 0;
    };
    Stats& stats() { return stats_; }
    const Stats& stats() const { return stats_; }

    double foreign_ratio() const {
        return stats_.ops ? static_cast<double>(stats_.foreign_ops) / static_cast<double>(stats_.ops) : 0.0;
    }

private:
    void publish_active_expire_reap_lag() {
        const uint32_t value = store_.active_expire_reap_lag_ms_max();
        if (value > published_active_expire_reap_lag_ms_max_.load(std::memory_order_relaxed))
            published_active_expire_reap_lag_ms_max_.store(value, std::memory_order_relaxed);
    }

    friend struct ShardLayoutLock;
    int32_t   id_ = -1;
    uint32_t  bucket_begin_ = 0;
    uint32_t  bucket_end_   = 0;
    uint32_t  zc_min_       = 0;
    int64_t   now_ms_       = 0;
    uint32_t  home_domain_  = kNoDomain;
    std::atomic<uint32_t> published_size_{0};
    std::atomic<uint64_t> published_obj_bytes_{0};
    std::atomic<uint32_t> published_expires_{0};
    // Fills the existing four-byte alignment hole before published_evicted_.
    std::atomic<uint32_t> published_active_expire_reap_lag_ms_max_{0};
    std::atomic<uint64_t> published_evicted_{0};
    FlatStore store_;
    Stats     stats_;
    TypeLimits type_limits_;
    StreamLimits stream_limits_;
    void* blocking_registry_ = nullptr;
    std::atomic<uint64_t> blocking_waiters_{0};
    std::atomic<bool> blocking_dirty_{false};
    std::unordered_map<std::string, std::vector<WatchEntry>> watchers_;
    // One key can carry several undecided reservations at once -- see WatchReservation::blocking.
    std::unordered_map<std::string, std::vector<WatchReservation>> watch_reservations_;
    // Notification state is deliberately appended after every pre-existing field. In particular,
    // server_ must never return to offset 8: doing so shifted the entire hot shard header in v1.
    Server* server_ = nullptr;
    FlatNotifySink flat_notify_sink_{};
    uint32_t notify_mask_ = 0;
    uint32_t notify_order_base_ = 0;
    Op* notify_carrier_ = nullptr;
    Op* notify_source_ = nullptr;
    bool* notify_pending_ = nullptr;
    std::unique_ptr<NotifyShardState> notify_state_;
    // Allocated only when lb is enabled. Appended in the cold tail so the hot shard
    // header and FlatStore offsets remain unchanged when weighted placement is compiled in.
    std::unique_ptr<uint32_t[]> lb_bucket_samples_;
    std::unique_ptr<uint64_t[]> lb_bucket_bytes_;
    std::unique_ptr<uint64_t[]> lb_bucket_bytes_staging_;
    uint64_t lb_bytes_cursor_ = 0;
    // Cold save-policy tail. Only this shard's owner increments it; the designated IO cron owner
    // samples it once per second. Atomicity makes that cross-thread sample data-race-free.
    std::atomic<uint64_t> save_changes_{0};
};

struct ShardLayoutLock {
    static constexpr size_t store_offset = offsetof(Shard, store_);
    static constexpr size_t stats_offset = offsetof(Shard, stats_);
};

// atomic_torn's gate geometry depends on the pre-read-local Shard stride and hot stats position.
static_assert(sizeof(Shard) == 1440);
static_assert(ShardLayoutLock::store_offset == 56);
static_assert(ShardLayoutLock::stats_offset == 1000);

// Installs one logical operation's expiry cut on an owner for the length of ONE fragment, and puts
// the executor's own per-pass clock back afterwards. Every exit restores -- Complete, Retry, or an
// exception -- so nothing that runs later on this owner inherits another operation's instant.
// A zero cut disarms the guard rather than pinning: an unstamped state must not be read as 1970,
// which would hide the entire keyspace.
class PinnedNowMs {
public:
    PinnedNowMs(Shard& shard, int64_t cut_ms)
        : shard_(shard), saved_(shard.now_ms()), armed_(cut_ms != 0) {
        if (__builtin_expect(armed_, true)) shard_.pin_now_ms(cut_ms);
    }
    ~PinnedNowMs() { if (__builtin_expect(armed_, true)) shard_.pin_now_ms(saved_); }
    PinnedNowMs(const PinnedNowMs&) = delete;
    PinnedNowMs& operator=(const PinnedNowMs&) = delete;

private:
    Shard& shard_;
    const int64_t saved_;
    const bool armed_;
};

// The one authoritative bucket map.  Each entry carries two independent facts:
//
//   * the immutable physical Shard/FlatStore containing the bucket's keys;
//   * the mutable EX thread which alone may touch that store.
//
// Keeping both in one atomic word preserves the historical one-indexed-load shard route while
// making the ownership flip itself an array write.  A transfer changes only the EX bits; the shard
// bits, keys, and table never move.
//
// Runtime readers never consult the transfer descriptor.  PREPARING leaves owner_[] untouched, so
// every entry remains a valid source route.  The phase release-store makes the destination valid,
// after which commit_transfer() publishes destination entries.  A reader may therefore observe
// only the current or previous owner, and a stale route is forwarded by ExLoop before execution.
class Router {
public:
    static constexpr uint32_t kNoOwner = UINT16_MAX;

    enum class TransferPhase : uint8_t { Idle = 0, Preparing = 1, Committed = 2 };

    void build_uniform(int32_t nshards) {
        nshards_ = nshards;
        const uint32_t per = kNumBuckets / nshards;
        for (uint32_t b = 0; b < kNumBuckets; b++) {
            int32_t s = static_cast<int32_t>(b / per);
            if (s >= nshards) s = nshards - 1;      // remainder buckets go to the last shard
            owner_[b].store(pack(s, kNoOwner), std::memory_order_relaxed);
        }
    }

    int32_t shard_of(uint64_t hash) const {
        return unpack_shard(owner_[bucket_of(hash)].load(std::memory_order_relaxed));
    }
    int32_t shard_of_bucket(uint32_t bucket) const {
        return bucket < kNumBuckets
            ? unpack_shard(owner_[bucket].load(std::memory_order_acquire)) : -1;
    }

    uint32_t owner_of(uint64_t hash) const { return owner_of_bucket(bucket_of(hash)); }
    uint32_t owner_of_bucket(uint32_t bucket) const {
        // owner_[] is one atomic packed owner+shard word, so this load cannot tear.  PREPARING does
        // not modify it; after commit, observing the previous owner is a safe stale route.
        return unpack_owner(owner_[bucket].load(std::memory_order_acquire));
    }

    // Boot-only population. Runtime ownership changes must use begin/commit/finish_transfer so
    // destination entries cannot become visible before the ownership edge.
    void set_initial_owner(uint32_t begin, uint32_t end, uint32_t thread_id) {
        if (begin >= end || end > kNumBuckets || thread_id >= kNoOwner) std::abort();
        if (phase_.load(std::memory_order_relaxed) != TransferPhase::Idle) std::abort();
        for (uint32_t bucket = begin; bucket < end; bucket++) {
            const uint32_t entry = owner_[bucket].load(std::memory_order_relaxed);
            owner_[bucket].store(pack(unpack_shard(entry), thread_id),
                                 std::memory_order_relaxed);
        }
    }

    // Both EX loops must be at migration safe points before this call.  The source remains the
    // owner after a successful begin; commit_transfer() performs the one ownership handoff.
    bool begin_transfer(uint32_t begin, uint32_t end, uint32_t source,
                        uint32_t destination) {
        if (begin >= end || end > kNumBuckets || source == destination ||
            source >= kNoOwner || destination >= kNoOwner) return false;
        if (transfer_writer_.test_and_set(std::memory_order_acquire)) return false;
        if (phase_.load(std::memory_order_acquire) != TransferPhase::Idle) {
            transfer_writer_.clear(std::memory_order_release);
            return false;
        }
        for (uint32_t bucket = begin; bucket < end; bucket++) {
            if (unpack_owner(owner_[bucket].load(std::memory_order_acquire)) != source) {
                transfer_writer_.clear(std::memory_order_release);
                return false;
            }
        }

        transfer_begin_.store(begin, std::memory_order_relaxed);
        transfer_end_.store(end, std::memory_order_relaxed);
        transfer_source_.store(source, std::memory_order_relaxed);
        transfer_destination_.store(destination, std::memory_order_relaxed);
        phase_.store(TransferPhase::Preparing, std::memory_order_release);

        return true;
    }

    void commit_transfer() {
        if (phase_.load(std::memory_order_acquire) != TransferPhase::Preparing) std::abort();
        const uint32_t begin = transfer_begin_.load(std::memory_order_relaxed);
        const uint32_t end = transfer_end_.load(std::memory_order_relaxed);
        const uint32_t destination = transfer_destination_.load(std::memory_order_relaxed);

        // This is the single logical ownership edge.  Both executors are quiesced, and the
        // destination's bookkeeping is already installed, so old entries are now safe stale
        // routes.  Publish each destination entry only after the destination is valid.
        phase_.store(TransferPhase::Committed, std::memory_order_release);
        for (uint32_t bucket = begin; bucket < end; bucket++) {
            const uint32_t entry = owner_[bucket].load(std::memory_order_relaxed);
            owner_[bucket].store(pack(unpack_shard(entry), destination),
                                 std::memory_order_release);
        }
    }

    void finish_transfer() {
        if (phase_.load(std::memory_order_acquire) != TransferPhase::Committed) std::abort();
        phase_.store(TransferPhase::Idle, std::memory_order_release);
        transfer_writer_.clear(std::memory_order_release);
    }

    TransferPhase transfer_phase() const { return phase_.load(std::memory_order_acquire); }
    int32_t nshards() const { return nshards_; }

private:
    static uint32_t pack(int32_t shard, uint32_t owner) {
        return (owner << 16) | static_cast<uint32_t>(shard);
    }
    static int32_t unpack_shard(uint32_t entry) {
        return static_cast<int32_t>(entry & UINT16_MAX);
    }
    static uint32_t unpack_owner(uint32_t entry) { return entry >> 16; }

    int32_t nshards_ = 0;
    std::atomic<uint32_t> owner_[kNumBuckets] = {};
    std::atomic<TransferPhase> phase_{TransferPhase::Idle};
    std::atomic<uint32_t> transfer_begin_{0};
    std::atomic<uint32_t> transfer_end_{0};
    std::atomic<uint32_t> transfer_source_{kNoOwner};
    std::atomic<uint32_t> transfer_destination_{kNoOwner};
    std::atomic_flag transfer_writer_ = ATOMIC_FLAG_INIT;
};

}  // namespace tomo
