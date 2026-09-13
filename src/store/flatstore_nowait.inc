// Unconditional publication for armed read-local tables. Included after FlatStore, inside namespace tomo.
// No builder crosses an owner turn. Arrays belong to exactly one image; values do not.
// Atomic visibility still uses the semantic sequence and ForeignReadSafety.

struct ReadLocalTableMetrics {
    uint64_t probes = 0, served = 0, atomic_validations = 0;
    uint64_t published = 0, retired = 0, reclaimed = 0;
    uint64_t current_bytes = 0, retired_bytes = 0, detached_bytes = 0, peak_retired_bytes = 0;
    uint64_t build_bytes = 0, peak_build_bytes = 0, prepared_bytes = 0, sidecar_bytes = 0;
    uint64_t copied_slots = 0, allocation_failures = 0;
    uint64_t grows = 0, shrinks = 0, cleanups = 0, merges = 0;
    uint64_t snapshots = 0, clears = 0, snapshot_clear_oom = 0;
    uint64_t debug_phase = 0, debug_held = 0, debug_grace_blocked = 0;
    uint64_t debug_retired_reads = 0, debug_prepublication_reads = 0;
    uint64_t debug_timeouts = 0;
};

struct ReadLocalTablePublication {
    using Topology = FlatStore::ReadLocalTopology;
    struct Image {
        Topology topology{};
        size_t bytes() const {
            return sizeof(Image) + sizeof(uint64_t) *
                (uint64_t(topology.tables[0].cap) + topology.tables[1].cap);
        }
    };
    // No writable slot is shared with this sentinel. First insertion must allocate a table.
    static Image empty;
    struct alignas(64) Reader {
        std::atomic<uint64_t> probes{0}, served{0}, atomic_validations{0};
    };
    // Publication traffic shares neither the semantic word nor reader/owner telemetry lines.
    alignas(64) std::atomic<Image*> root{nullptr};
    alignas(64) std::unique_ptr<Reader[]> readers;
    uint32_t reader_count = 0;
    Image* initial = nullptr; // descriptor only, allocated before replay, populated at arming

    alignas(64) std::atomic<uint64_t> published{0}, retired{0}, reclaimed{0};
    std::atomic<uint64_t> current_bytes{0}, retired_bytes{0}, detached_bytes{0}, peak_retired_bytes{0};
    std::atomic<uint64_t> build_bytes{0}, peak_build_bytes{0}, prepared_bytes{0};
    std::atomic<uint64_t> copied_slots{0}, allocation_failures{0};
    std::atomic<uint64_t> grows{0}, shrinks{0}, cleanups{0}, merges{0};
    std::atomic<uint64_t> snapshots{0}, clears{0}, snapshot_clear_oom{0};

    // DEBUG only. Configuration is published before phase=1. One foreign operation consumes
    // the arm; the normal reader pays just this cold check, on the armed read-local lane.
    alignas(64) std::atomic<uint32_t> phase{0}, mode{0}, owner{0}, holder{UINT32_MAX};
    std::atomic<uint64_t> hash{0};
    std::atomic<Image*> held{nullptr};
    std::atomic<uint64_t> grace_blocked{0}, retired_reads{0}, prepublication_reads{0};
    std::atomic<uint64_t> timeouts{0};

    static_assert(std::atomic<Image*>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr));

    // Each counter has one physical writer, including each reader's own cache line. INFO uses
    // relaxed loads, avoiding both C++ telemetry data races and locked per-probe increments.
    static void add(std::atomic<uint64_t>& counter, uint64_t n = 1) {
        counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    static void peak(std::atomic<uint64_t>& counter, uint64_t n) {
        if (counter.load(std::memory_order_relaxed) < n)
            counter.store(n, std::memory_order_relaxed);
    }
    Reader& reader(uint32_t id) {
        if (id >= reader_count) std::abort();
        return readers[id];
    }
    static void free_image(Image* image) {
        if (!image || image == &empty) return;
        for (const auto& table : image->topology.tables) std::free(table.slots);
        delete image;
    }
    Image* allocate(uint32_t current, uint32_t old = 0) {
        auto* image = new (std::nothrow) Image;
        if (image) {
            build_bytes.store(sizeof(Image), std::memory_order_relaxed);
            peak(peak_build_bytes, sizeof(Image));
            const uint32_t caps[] = {current, old};
            for (int t = 0; t < 2; ++t) {
                if (!caps[t]) continue;
                auto* slots = FlatStore::allocate_table(caps[t]);
                if (!slots) { free_image(image); image = nullptr; break; }
                image->topology.tables[t] = {slots, caps[t], caps[t] - 1};
                build_bytes.store(image->bytes(), std::memory_order_relaxed);
                peak(peak_build_bytes, image->bytes());
            }
        }
        if (!image) {
            build_bytes.store(0, std::memory_order_relaxed);
            add(allocation_failures);
            return nullptr;
        }
        build_bytes.store(image->bytes(), std::memory_order_relaxed);
        peak(peak_build_bytes, image->bytes());
        return image;
    }
    // Private insertion: no retirement, expiry, or object accounting. Source keys are unique
    // across both authoritative arrays, even during snapshot capture (publication never moves them).
    static bool insert(Image& image, KvObj* object) {
        auto& table = image.topology.tables[0];
        const uint64_t h = FlatStore::hash_key(object->key());
        uint32_t slot = static_cast<uint32_t>(mix64(h)) & table.mask;
        for (uint32_t n = 0; n < table.cap; ++n) {
            if (!table.slots[slot]) {
                table.slots[slot] = FlatStore::make_word(FlatStore::tag_of(h), object);
                return true;
            }
            slot = (slot + 1) & table.mask;
        }
        return false;
    }
    static void reclaim(const ReadLocalRetireSink&, void* owner, void* payload, size_t) {
        auto& state = *static_cast<ReadLocalTablePublication*>(owner);
        auto* image = static_cast<Image*>(payload);
        // A deliberate early-free test variant fails HERE before any dangling dereference. This
        // witness is diagnostic; production lifetime comes solely from the queue's grace test.
        const Image* current = state.root.load(std::memory_order_acquire);
        if (state.held.load(std::memory_order_acquire) == image || current == image)
            std::abort();
        // Missing root publication and descriptor-only replacement must fail deterministically,
        // even on an allocator that leaves freed bytes readable. Current owns disjoint arrays.
        for (const auto& old_table : image->topology.tables)
            for (const auto& live_table : current->topology.tables)
                if (old_table.slots && old_table.slots == live_table.slots) std::abort();
        const size_t bytes = image->bytes();
        add(state.reclaimed);
        state.retired_bytes.store(
            state.retired_bytes.load(std::memory_order_relaxed) - bytes,
            std::memory_order_relaxed);
        free_image(image); // NEVER free values: current and retired images can share them.
    }
    void retire(FlatStore& store, Image* image) {
        if (image == &empty) return;
        add(retired);
        add(retired_bytes, image->bytes());
        detached_bytes.store(0, std::memory_order_relaxed);
        peak(peak_retired_bytes, retired_bytes.load(std::memory_order_relaxed));
        store.read_local_store_state_armed().retire_sink.retire(
            this, image, 0, &reclaim);
    }
    template <typename Predicate>
    bool wait_debug(Predicate&& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                // More than one test participant can time out: cold diagnostic RMW is intentional.
                timeouts.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }
    bool capture_debug(Image* image, uint64_t key, uint32_t id) {
        if (phase.load(std::memory_order_acquire) != 1 ||
            hash.load(std::memory_order_relaxed) != key ||
            owner.load(std::memory_order_relaxed) == id) return true;
        uint32_t expected = 1;
        // Reserve the arm before announcing held; the writer waits for phase 2, not this phase.
        if (!phase.compare_exchange_strong(expected, 8, std::memory_order_acq_rel)) return true;
        holder.store(id, std::memory_order_relaxed);
        held.store(image, std::memory_order_release);
        phase.store(2, std::memory_order_release);
        const bool ready = wait_debug([&] {
            const uint32_t p = phase.load(std::memory_order_acquire);
            return p == 5 || p == 0;
        });
        if (!ready || phase.load(std::memory_order_acquire) == 0) {
            held.store(nullptr, std::memory_order_release);
            if (!ready) phase.store(7, std::memory_order_release);
            return false;
        }
        return true;
    }
    void before_publish_debug() {
        if (phase.load(std::memory_order_acquire) != 2) return;
        phase.store(3, std::memory_order_release);
        if (mode.load(std::memory_order_relaxed) == 2)
            (void)wait_debug([&] {
                const uint32_t p = phase.load(std::memory_order_acquire);
                return p == 6 || p == 0 || p == 7;
            });
    }
    // Commit aliases for the sole owner, then ONE release store publishes the complete view.
    // No retained array is changed or republished. Caller owns `old` until retire() below.
    Image* publish(FlatStore& store, Image* image, const uint32_t* live,
                   const uint32_t* tombs) {
        Image* old = root.load(std::memory_order_relaxed);
        for (int t = 0; t < 2; ++t) {
            const auto& table = image->topology.tables[t];
            // These aliases belong exclusively to the shard owner.
            store.tab_[t] = table.slots;
            store.cap_[t] = table.cap;
            store.mask_[t] = table.mask;
            store.live_[t] = live[t];
            store.tombs_[t] = tombs[t];
        }
        store.rehash_pos_ = 0;
        root.store(image, std::memory_order_release);
        // Clear can spend time enumerating values or waiting on ring capacity before enqueuing
        // its image. Account that explicit owner custody as transient storage too.
        detached_bytes.store(old == &empty ? 0 : old->bytes(), std::memory_order_relaxed);
        add(published);
        current_bytes.store(image == &empty ? 0 : image->bytes(), std::memory_order_relaxed);
        build_bytes.store(0, std::memory_order_relaxed);
        return old;
    }
    void after_retire_debug() {
        if (phase.load(std::memory_order_acquire) == 3 &&
            mode.load(std::memory_order_relaxed) == 1)
            phase.store(4, std::memory_order_release);
    }
};

static_assert(offsetof(ReadLocalTablePublication, readers) -
              offsetof(ReadLocalTablePublication, root) >= 64);
static_assert(offsetof(ReadLocalTablePublication, published) -
              offsetof(ReadLocalTablePublication, readers) >= 64);
inline ReadLocalTablePublication::Image ReadLocalTablePublication::empty{};

inline void FlatStore::read_local_table_prepared_bytes(uint64_t bytes) {
    read_local_store_state_armed().published_table->prepared_bytes.store(
        bytes, std::memory_order_relaxed);
}

inline bool FlatStore::prepare_read_local_table(uint32_t readers) {
    if constexpr (kReadLocalSetTaxAtomicRaw) {
        std::fprintf(stderr, "armed --read-local requires TOMO_READ_LOCAL_SET_TAX_VARIANT=0\n");
        return false;
    }
    auto& base = read_local_store_state_required();
    if (base.published_table) return true;
    if (!readers) return false;
    std::unique_ptr<ReadLocalTablePublication> state(new (std::nothrow) ReadLocalTablePublication);
    if (!state) return false;
    state->readers.reset(new (std::nothrow) ReadLocalTablePublication::Reader[readers]);
    if (!state->readers) return false;
    state->initial = new (std::nothrow) ReadLocalTablePublication::Image;
    if (!state->initial) return false;
    state->reader_count = readers;
    base.published_table = state.release();
    return true;
}

inline void FlatStore::arm_read_local_table() {
    auto* state = read_local_store_state_required().published_table;
    if (!state) std::abort();
    if (state->root.load(std::memory_order_relaxed)) return;
    auto* image = state->initial;
    for (int t = 0; t < 2; ++t) image->topology.tables[t] = {tab_[t], cap_[t], mask_[t]};
    state->initial = nullptr;
    state->current_bytes.store(image->bytes(), std::memory_order_relaxed);
    state->root.store(image, std::memory_order_release);
    // Boot installation is deliberately excluded from the mutation publication counter.
}

inline void FlatStore::destroy_read_local_table() {
    auto* state = read_local_store_state_required().published_table;
    if (!state) return;
    if (state->retired_bytes.load(std::memory_order_relaxed) ||
        state->detached_bytes.load(std::memory_order_relaxed)) std::abort();
    auto* image = state->root.load(std::memory_order_relaxed);
    if (image != &ReadLocalTablePublication::empty) delete image;
    delete state->initial;
    delete state; // current arrays are still uniquely owned by FlatStore's teardown
}

inline bool FlatStore::read_local_atomic_validate(uint64_t before, uint32_t reader) const {
    auto& state = *read_local_store_state_armed().published_table;
    ReadLocalTablePublication::add(state.reader(reader).atomic_validations);
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t after = read_local_state_acquire();
    return read_local_state_eligible(before) && read_local_state_eligible(after) &&
           read_local_generation_equal(before, after);
}
inline FlatStore::ReadLocalPrefetchCapture FlatStore::read_local_table_capture(
        uint64_t hash, Slice key, uint32_t reader) const {
    // Preserve the boot-latch check before touching a root another owner has not armed yet.
    if (__builtin_expect(!read_local_enabled_, false))
        return {ReadLocalProbeResult::AtomicPending, nullptr, nullptr, 0};
    auto& state = *read_local_store_state_armed().published_table;
    ReadLocalTablePublication::add(state.reader(reader).probes);
    const uint64_t atomic = read_local_state_acquire();
    if (!read_local_state_eligible(atomic) || foreign_read_key_unsafe(atomic, hash))
        return {ReadLocalProbeResult::AtomicPending, nullptr, nullptr, atomic};
    auto* image = state.root.load(std::memory_order_acquire); // exactly one answering root
    if (__builtin_expect(state.phase.load(std::memory_order_relaxed) != 0, false)) {
        if (!state.capture_debug(image, hash, reader))
            return {ReadLocalProbeResult::AtomicPending, nullptr, nullptr, atomic};
    }
    const uint64_t* slot = nullptr;
    const KvObj* object = read_local_capture_in(image->topology.tables[0], hash, key, slot);
    if (!object) {
        const uint64_t* old_slot = nullptr;
        object = read_local_capture_in(image->topology.tables[1], hash, key, old_slot);
        if (old_slot) slot = old_slot;
    }
    if (!read_local_atomic_validate(atomic, reader))
        return {ReadLocalProbeResult::AtomicPending, nullptr, nullptr, atomic};
    if (object) read_local_prefetch_object(object);
    return {object ? ReadLocalProbeResult::Hit : ReadLocalProbeResult::Missing,
            slot, object, atomic};
}
inline void FlatStore::read_local_table_prefetch(uint64_t hash) const {
    if (__builtin_expect(!read_local_enabled_, false)) return;
    const auto* image = read_local_store_state_armed().published_table->root.load(
        std::memory_order_acquire);
    for (const auto& table : image->topology.tables)
        if (table.slots)
            __builtin_prefetch(table.slots + (static_cast<uint32_t>(mix64(hash)) & table.mask), 0, 1);
}
inline void FlatStore::read_local_table_served(uint64_t hash, uint32_t reader) const {
    auto& state = *read_local_store_state_armed().published_table;
    ReadLocalTablePublication::add(state.reader(reader).served);
    if (__builtin_expect(state.phase.load(std::memory_order_acquire) == 5, false) &&
        state.hash.load(std::memory_order_relaxed) == hash &&
        state.holder.load(std::memory_order_relaxed) == reader &&
        state.held.load(std::memory_order_acquire)) {
        // Test witness only: the reply must have consumed its held image after retirement.
        // This comparison never accepts/rejects a production read or refreshes its root.
        if (state.root.load(std::memory_order_acquire) != state.held.load(std::memory_order_acquire))
            ReadLocalTablePublication::add(state.retired_reads);
        if (state.mode.load(std::memory_order_relaxed) == 2)
            ReadLocalTablePublication::add(state.prepublication_reads);
        state.held.store(nullptr, std::memory_order_release); // reply is now private bytes
        state.phase.store(6, std::memory_order_release);
    }
}

inline bool FlatStore::rebuild_read_local_table(uint32_t capacity) {
    if (snapshot_active_) return false;
    auto& state = *read_local_store_state_armed().published_table;
    capacity = std::max(capacity, kMinCap);
    if (size() >= capacity) return false;
    auto* fresh = state.allocate(capacity);
    if (!fresh) return false;
    const bool merging = rehashing();
    const uint32_t previous_cap = cap_[0];
    for (int t = 0; t < 2; ++t)
        for (uint32_t i = 0; i < cap_[t]; ++i)
            if (KvObj* object = ptr_of(tab_[t][i])) {
                if (!ReadLocalTablePublication::insert(*fresh, object)) std::abort();
                ReadLocalTablePublication::add(state.copied_slots);
            }
    state.before_publish_debug();
    const uint32_t live[] = {size(), 0}, tombs[] = {0, 0};
    auto* old = state.publish(*this, fresh, live, tombs);
    if (rehash_counter_) (*rehash_counter_)++;
    ReadLocalTablePublication::add(merging ? state.merges : capacity > previous_cap
        ? state.grows : capacity < previous_cap ? state.shrinks : state.cleanups);
    state.retire(*this, old);
    state.after_retire_debug();
    return true;
}

inline bool FlatStore::snapshot_mark_read_local_table() {
    auto& state = *read_local_store_state_armed().published_table;
    // Allocate at the cut before touching the prepared table. OOM cancels the cut; the original
    // image and snapshot_new_tab_ remain intact. No stale object pointer survived preparation.
    auto* fresh = state.allocate(0, cap_[0]);
    if (!fresh) return false;
    fresh->topology.tables[0] = {snapshot_new_tab_, snapshot_new_cap_, snapshot_new_cap_ - 1};
    state.prepared_bytes.store(0, std::memory_order_relaxed);
    state.build_bytes.store(fresh->bytes(), std::memory_order_relaxed);
    ReadLocalTablePublication::peak(state.peak_build_bytes, fresh->bytes());
    if (cap_[0]) std::memcpy(fresh->topology.tables[1].slots, tab_[0],
                            size_t(cap_[0]) * sizeof(uint64_t));
    ReadLocalTablePublication::add(state.copied_slots, live_[0]);
    const uint32_t live[] = {0, live_[0]}, tombs[] = {0, tombs_[0]};
    auto* old = state.publish(*this, fresh, live, tombs);
    ReadLocalTablePublication::add(state.snapshots);
    state.retire(*this, old);
    return true;
}

inline void FlatStore::clear_read_local_table(bool snapshot) {
    auto& state = *read_local_store_state_armed().published_table;
    auto* fresh = snapshot ? state.allocate(cap_[0], cap_[1]) : state.allocate(1024);
    ForeignReadPoisonGuard broad_change(*this); // logical all-key change, including MGET epochs
    if (snapshot && !fresh) {
        // No topology change is necessary to delete in a fixed layout. On OOM retain the
        // snapshot cursor contract by using bounded-chain tombstones, NEVER zero/reuse an array.
        // This is the retained atomic/broad-handler exception, reported explicitly in INFO.
        ReadLocalTablePublication::add(state.snapshot_clear_oom);
        expires_.clear();
        for (int t = 0; t < 2; ++t)
            for (uint32_t i = 0; i < cap_[t]; ++i)
                if (KvObj* object = ptr_of(tab_[t][i])) {
                    read_local_slot_store(&tab_[t][i], kTombBit);
                    retire_obj_read_local(object);
                    --live_[t]; ++tombs_[t];
                }
    } else {
        if (!fresh) fresh = &ReadLocalTablePublication::empty;
        const uint32_t live[] = {0, 0};
        uint32_t tombs[] = {0, 0};
        if (snapshot) {
            // Preserve every slot number and stopper for the snapshot walker. It holds object
            // pointers only during a prepared record, which FLUSH's preimage gate has finished.
            if (snapshot_record_.active) std::abort();
            for (int t = 0; t < 2; ++t)
                for (uint32_t i = 0; i < cap_[t]; ++i)
                    if (tab_[t][i]) {
                        fresh->topology.tables[t].slots[i] = kTombBit;
                        ++tombs[t];
                    }
        }
        auto* old = state.publish(*this, fresh, live, tombs);
        expires_.clear();
        // Retain explicit owner custody of the array while enumerating values: a full-ring
        // drain inside retire_obj may reclaim any previously enqueued image, never this one.
        for (const auto& table : old->topology.tables)
            for (uint32_t i = 0; i < table.cap; ++i)
                if (KvObj* object = ptr_of(table.slots[i])) retire_obj_read_local(object);
        state.retire(*this, old);
    }
    field_expires_.clear();
    field_ttl_gate_ = 0;
    ReadLocalTablePublication::add(state.clears);
    if (!snapshot) read_local_cache_release_all();
}

inline bool FlatStore::debug_read_local_table(uint64_t hash, uint32_t owner, uint32_t mode) {
    auto* base = read_local_store_state();
    if (!base || !base->published_table || !read_local_enabled_) return false;
    auto& state = *base->published_table;
    if (mode == 0) {
        state.phase.store(0, std::memory_order_release);
        return true;
    }
    if (mode == 3) {
        const uint32_t phase = state.phase.load(std::memory_order_acquire);
        if (phase != 3 && phase != 4) return false;
        state.phase.store(5, std::memory_order_release);
        return true;
    }
    if (mode > 2 || state.phase.load(std::memory_order_acquire) ||
        state.held.load(std::memory_order_acquire)) return false;
    state.hash.store(hash, std::memory_order_relaxed);
    state.owner.store(owner, std::memory_order_relaxed);
    state.mode.store(mode, std::memory_order_relaxed);
    state.phase.store(1, std::memory_order_release);
    return true;
}

inline void FlatStore::read_local_table_metrics(ReadLocalTableMetrics& total) const {
    const auto* base = read_local_store_state();
    if (!base || !base->published_table) return;
    const auto& state = *base->published_table;
    for (uint32_t r = 0; r < state.reader_count; ++r) {
        total.probes += state.readers[r].probes.load(std::memory_order_relaxed);
        total.served += state.readers[r].served.load(std::memory_order_relaxed);
        total.atomic_validations += state.readers[r].atomic_validations.load(std::memory_order_relaxed);
    }
#define TOMO_TABLE_METRIC(name) total.name += state.name.load(std::memory_order_relaxed)
    TOMO_TABLE_METRIC(published); TOMO_TABLE_METRIC(retired); TOMO_TABLE_METRIC(reclaimed);
    TOMO_TABLE_METRIC(current_bytes); TOMO_TABLE_METRIC(retired_bytes);
    TOMO_TABLE_METRIC(detached_bytes);
    TOMO_TABLE_METRIC(peak_retired_bytes); TOMO_TABLE_METRIC(build_bytes);
    TOMO_TABLE_METRIC(peak_build_bytes); TOMO_TABLE_METRIC(prepared_bytes);
    TOMO_TABLE_METRIC(copied_slots);
    TOMO_TABLE_METRIC(allocation_failures); TOMO_TABLE_METRIC(grows);
    TOMO_TABLE_METRIC(shrinks); TOMO_TABLE_METRIC(cleanups); TOMO_TABLE_METRIC(merges);
    TOMO_TABLE_METRIC(snapshots); TOMO_TABLE_METRIC(clears); TOMO_TABLE_METRIC(snapshot_clear_oom);
#undef TOMO_TABLE_METRIC
    total.sidecar_bytes += sizeof(state) + state.reader_count * sizeof(ReadLocalTablePublication::Reader);
    total.debug_phase = std::max<uint64_t>(total.debug_phase, state.phase.load(std::memory_order_acquire));
    total.debug_held += state.held.load(std::memory_order_acquire) != nullptr;
    total.debug_grace_blocked += state.grace_blocked.load(std::memory_order_relaxed);
    total.debug_retired_reads += state.retired_reads.load(std::memory_order_relaxed);
    total.debug_prepublication_reads += state.prepublication_reads.load(std::memory_order_relaxed);
    total.debug_timeouts += state.timeouts.load(std::memory_order_relaxed);
}

// Called only when an armed owner's queue actually tested grace and could reclaim nothing.
inline void read_local_table_grace_blocked(ReadLocalRetireSink::ReclaimFn reclaim,
                                           void* owner, void* payload) {
    if (reclaim != &ReadLocalTablePublication::reclaim) return;
    auto& state = *static_cast<ReadLocalTablePublication*>(owner);
    if (state.held.load(std::memory_order_acquire) == payload)
        ReadLocalTablePublication::add(state.grace_blocked);
}
