// flatstore_atomic.inc — epoch-MVCC members textually included inside class FlatStore.
//
// This file is intentionally not a standalone header. Keeping these definitions inside the
// header-only class preserves the original single-TU concatenation model without new external
// symbols, translation units, or build-system changes.

    // Main calls this only after every IO/ex thread has joined. It releases group references before
    // IO-owned arena pools are destroyed; the destructor repeats it harmlessly for early exits.
    void atomic_shutdown_release_records() { atomic_promote_all_for_shutdown(); }

    // Epoch-MVCC is a shard-local pending-entry list so KvObj remains byte-identical. These
    // bindings are installed once at boot; the null tracker pointer below is the complete
    // disabled-feature tax on store accesses.
    // FLUSH's logical clear is the store's only ticket draw. It goes through the server's
    // reserve/publish bracket rather than touching commit_seq_ directly, so the visible read
    // watermark can never be left behind a ticket this store already installed.
    using AtomicTicketFn = uint64_t (*)(void*);
    void bind_atomic_state(AtomicTicketFn ticket_fn, void* ticket_ctx,
                           std::atomic<uint64_t>* activity,
                           std::atomic<uint64_t>* script_intents,
                           uint64_t* predecessor_reads, uint64_t* chain_max,
                           uint64_t* promotions, uint64_t* records_freed,
                           uint64_t* entries, uint64_t* gauge_underflows) {
        atomic_ticket_fn_ = ticket_fn;
        atomic_ticket_ctx_ = ticket_ctx;
        atomic_activity_ = activity;
        atomic_script_intents_ = script_intents;
        atomic_predecessor_reads_ = predecessor_reads;
        atomic_chain_max_ = chain_max;
        atomic_promotions_ = promotions;
        atomic_records_freed_ = records_freed;
        atomic_entries_ = entries;
        atomic_gauge_underflows_ = gauge_underflows;
    }

    bool atomic_has_records() const {
        return atomic_pending_ &&
               (atomic_pending_->live != 0 || atomic_pending_->script_intent_count != 0);
    }
    uint32_t atomic_pending_entries() const {
        return atomic_pending_ ? atomic_pending_->live : 0;
    }
    uint64_t atomic_cleanup_fast() const {
        return atomic_pending_ ? atomic_pending_->cleanup_fast : 0;
    }
    uint64_t atomic_cleanup_slow() const {
        return atomic_pending_ ? atomic_pending_->cleanup_slow : 0;
    }
    bool atomic_has_record(uint64_t hash, Slice key) const {
        return atomic_key_pending(hash, key);
    }
    bool atomic_has_script_intent(uint64_t hash, Slice key) const {
        if (!atomic_pending_ || !atomic_pending_->script_intent_count) return false;
        for (AtomicScriptIntent* intent = atomic_pending_->script_intents; intent;
             intent = intent->next) {
            if (intent->hash == hash && intent->key_len == key.n &&
                (!key.n || std::memcmp(intent->key_data(), key.p, key.n) == 0)) return true;
        }
        return false;
    }
    // THE SINGLE DECISION POINT FOR "THIS WRITE MUST GO THROUGH MVCC".
    //
    // A cross-owner script's reservation is worth nothing unless every write that lands on a
    // declared key afterwards leaves an epoch behind: that epoch is the only thing APPLY-side
    // validation can see, and the whole reservation sub-wave exists to create it. Testing only
    // atomic_has_record() here is what made the reservation inert -- an ordinary key carries no
    // record, so the competing write took the untracked physical path and validation had nothing
    // to find. Every caller that decides whether to materialize a plain version asks THIS, so
    // the answer cannot drift between call sites again.
    bool atomic_needs_version(uint64_t hash, Slice key) const {
        return atomic_has_record(hash, key) || atomic_has_script_intent(hash, key);
    }

    // PIN/UNPIN run only on the key's owning executor. The first intent contributes one unit to
    // atomic_activity_, exactly like the first pending version, so writers reach the already-cold
    // preparation path even when --atomic=0. Duplicate declared keys and overlapping activations
    // share one copied key record through refs.
    bool atomic_script_pin(uint64_t hash, Slice key) {
        if (!atomic_ensure_pending()) return false;
        for (AtomicScriptIntent* intent = atomic_pending_->script_intents; intent;
             intent = intent->next) {
            if (intent->hash != hash || intent->key_len != key.n ||
                (key.n && std::memcmp(intent->key_data(), key.p, key.n) != 0)) continue;
            intent->refs++;
            return true;
        }
        const size_t bytes = sizeof(AtomicScriptIntent) + key.n;
        void* memory = std::malloc(bytes);
        if (!memory) return false;
        auto* intent = new (memory) AtomicScriptIntent;
        intent->hash = hash;
        intent->refs = 1;
        intent->key_len = key.n;
        if (key.n) std::memcpy(intent->key_data(), key.p, key.n);
        const bool first_activity = atomic_pending_->live == 0 &&
                                    atomic_pending_->script_intent_count == 0;
        intent->next = atomic_pending_->script_intents;
        atomic_pending_->script_intents = intent;
        atomic_pending_->script_intent_count++;
        if (atomic_pending_->script_intent_count == 1 && atomic_script_intents_)
            atomic_script_intents_->fetch_add(1, std::memory_order_release);
        if (first_activity && atomic_activity_)
            atomic_activity_->fetch_add(1, std::memory_order_release);
        return true;
    }

    void atomic_script_unpin(uint64_t hash, Slice key) {
        if (!atomic_pending_) std::abort();
        AtomicScriptIntent** link = &atomic_pending_->script_intents;
        while (*link) {
            AtomicScriptIntent* intent = *link;
            if (intent->hash != hash || intent->key_len != key.n ||
                (key.n && std::memcmp(intent->key_data(), key.p, key.n) != 0)) {
                link = &intent->next;
                continue;
            }
            if (!intent->refs) std::abort();
            if (--intent->refs) return;
            *link = intent->next;
            std::free(intent);
            if (!atomic_pending_->script_intent_count) std::abort();
            atomic_pending_->script_intent_count--;
            if (!atomic_pending_->script_intent_count && atomic_script_intents_)
                atomic_script_intents_->fetch_sub(1, std::memory_order_release);
            if (!atomic_pending_->script_intent_count && !atomic_pending_->live &&
                atomic_activity_)
                atomic_activity_->fetch_sub(1, std::memory_order_release);
            return;
        }
        std::abort();
    }

    // True when ANY version changed this key after cut. A matching undecided group is
    // conservatively a conflict too: it may already have reserved an earlier ticket but not yet
    // published its epoch. Completing this validation wave and restarting the activation lets that
    // group make progress without ever parking an owner.
    //
    // The test used to also require `epoch < reserved_ticket`, on the reasoning that a newer ticket
    // legally serializes after the script. It does not: a writer whose ticket is newer still READ
    // the pre-activation value (the script has installed nothing yet at validation time), so
    // ordering it after the activation is only sound for a blind write and silently loses a plain
    // read-modify-write. Restarting is unobservable and the fresh cut clears the conflict, so the
    // conservative test costs at most one retry and cannot livelock.
    bool atomic_script_changed(uint64_t hash, Slice key, uint64_t cut) const {
        if (!atomic_pending_ || !atomic_pending_->live) return false;
        const uint64_t bit = atomic_membership_bit(hash);
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
            if (!(entry->membership & bit)) continue;
            if (entry->group_epoch && entry->group_aborted &&
                entry->group_aborted->load(std::memory_order_acquire)) continue;
            for (uint32_t i = 0; i < entry->count; i++) {
                if (atomic_entry_hash(*entry, i) != hash ||
                    atomic_entry_key(*entry, i) != key) continue;
                const uint64_t epoch = atomic_epoch(*entry);
                if (!epoch || epoch > cut) return true;
            }
        }
        return false;
    }
    // Same-connection program order is the only ordering dependency between pipelined commands.
    // Foreign undecided nodes never block: resolution simply skips them.
    // `ignore_epoch` excludes every record belonging to the caller's OWN logical unit, which the
    // group pointer cannot do for a transaction: an EXEC's plain per-key installs carry a null
    // group pointer, while a cross-shard child command inside that same EXEC installs through the
    // scatter path and carries a non-null one. Both publish through the transaction's single
    // epoch word, so that word -- not the group pointer -- is what identifies "mine". Callers that
    // pass nothing keep the previous behaviour exactly.
    bool atomic_has_own_undecided(uint64_t hash, Slice key, uint64_t origin_conn_id,
                                  const ScatterState* ignore_group = nullptr,
                                  const std::atomic<uint64_t>* ignore_epoch = nullptr) const {
        if (!atomic_pending_) return false;
        const uint64_t bit = atomic_membership_bit(hash);
        const uint32_t bucket = static_cast<uint32_t>(mix64(origin_conn_id)) & 63;
        for (AtomicEntry* entry = atomic_pending_->conn_heads[bucket]; entry;
             entry = entry->conn_next) {
            if (!(entry->membership & bit) || !entry->group_epoch ||
                entry->origin_conn_id != origin_conn_id || entry->group == ignore_group ||
                entry->group_epoch == ignore_epoch)
                continue;
            if (entry->group_epoch->load(std::memory_order_acquire) != 0) continue;
            if (entry->group_aborted && entry->group_aborted->load(std::memory_order_acquire))
                continue;
            for (uint32_t i = 0; i < entry->count; i++)
                if (atomic_entry_hash(*entry, i) == hash &&
                    atomic_entry_key(*entry, i) == key) return true;
        }
        return false;
    }
    template <typename KeyAt>
    bool atomic_group_has_own_undecided(const ScatterState* group, uint32_t count,
                                        uint64_t origin_conn_id, uint64_t membership,
                                        KeyAt&& key_at) const {
        if (!atomic_pending_ || !group || !count) return false;
        if (!membership)
            for (uint32_t i = 0; i < count; i++)
                membership |= atomic_membership_bit(key_at(i).first);
        const uint32_t bucket = static_cast<uint32_t>(mix64(origin_conn_id)) & 63;
        for (AtomicEntry* entry = atomic_pending_->conn_heads[bucket]; entry;
             entry = entry->conn_next) {
            if (!entry->group_epoch || entry->origin_conn_id != origin_conn_id ||
                entry->group == group || !(entry->membership & membership)) continue;
            if (entry->group_epoch->load(std::memory_order_acquire) != 0) continue;
            if (entry->group_aborted && entry->group_aborted->load(std::memory_order_acquire))
                continue;
            for (uint32_t i = 0; i < count; i++) {
                const auto [hash, key] = key_at(i);
                if (!(entry->membership & atomic_membership_bit(hash))) continue;
                for (uint32_t j = 0; j < entry->count; j++)
                    if (atomic_entry_hash(*entry, j) == hash &&
                        atomic_entry_key(*entry, j) == key) return true;
            }
        }
        return false;
    }
    bool atomic_has_any_own_undecided(uint64_t origin_conn_id,
                                      const ScatterState* ignore_group = nullptr) const {
        if (!atomic_pending_) return false;
        const uint32_t bucket = static_cast<uint32_t>(mix64(origin_conn_id)) & 63;
        for (AtomicEntry* entry = atomic_pending_->conn_heads[bucket]; entry;
             entry = entry->conn_next) {
            if (!entry->group_epoch || entry->origin_conn_id != origin_conn_id ||
                entry->group == ignore_group) continue;
            if (entry->group_epoch->load(std::memory_order_acquire) != 0) continue;
            if (entry->group_aborted && entry->group_aborted->load(std::memory_order_acquire))
                continue;
            return true;
        }
        return false;
    }
    void atomic_set_read_context(uint64_t epoch, uint64_t origin_conn_id) {
        atomic_read_epoch_ = epoch;
        atomic_read_origin_conn_id_ = origin_conn_id;
    }
    void atomic_clear_read_epoch() {
        atomic_read_epoch_ = UINT64_MAX;
        atomic_read_origin_conn_id_ = 0;
    }
    // READ THE CONTEXT BACK. The read cut and the originating connection are per-STORE, so a
    // command that has to resolve a key on a second shard of the same executor (SORT's BY/GET
    // dereference is the only one) must carry its own context across rather than read the second
    // shard at "latest, no connection" -- which loses read-your-own-writes and answers from before
    // this connection's last committed group.
    uint64_t atomic_read_epoch() const { return atomic_read_epoch_; }
    uint64_t atomic_read_origin_conn_id() const { return atomic_read_origin_conn_id_; }

    // Allocate one owner-local entry for this group's whole shard span. The trailing array is the
    // only per-key bookkeeping: each install writes its displaced physical pointer once.
    void* atomic_prepare_group(ScatterState* group, uint32_t begin, uint32_t capacity,
                               std::atomic<uint64_t>* group_epoch,
                               std::atomic<uint32_t>* group_refs,
                               std::atomic<bool>* group_aborted, uint64_t origin_conn_id,
                               uint32_t* owner_refs, uint64_t membership) {
        if (!capacity || !atomic_ensure_pending()) return nullptr;
        AtomicEntry* entry = atomic_alloc_entry(capacity, 0);
        if (!entry) return nullptr;
        entry->group = group;
        entry->group_epoch = group_epoch;
        entry->group_refs = group_refs;
        entry->group_aborted = group_aborted;
        entry->origin_conn_id = origin_conn_id;
        entry->owner_refs = owner_refs;
        entry->begin = begin;
        entry->capacity = capacity;
        entry->membership = membership;
        return entry;
    }

    void atomic_discard_group(void*& opaque) {
        auto* entry = static_cast<AtomicEntry*>(opaque);
        if (!entry) return;
        if (entry->linked || entry->count) std::abort();
        atomic_free_entry(entry);
        opaque = nullptr;
    }

    // The caller has initialized the corresponding ScatterState key position to point at stable
    // KvObj key storage. Capacity was preflighted for the whole pass, so the table exchange cannot
    // fail. `value == nullptr` is the physical erase used by DEL/UNLINK.
    KvObj* atomic_install_group(void* opaque, uint64_t hash, Slice key, KvObj* value) {
        auto* entry = static_cast<AtomicEntry*>(opaque);
        if (!entry || entry->count >= entry->capacity) std::abort();
        KvObj* old = atomic_exchange_physical(hash, key, value);
        entry->parked()[entry->count++] = old;
        return old;
    }

    void atomic_finish_group_install(size_t parked_bytes, size_t installed_bytes) {
        // atomic_admit charged every incoming object before its exchange. Once the whole shard pass
        // is physical, only the displaced predecessors remain version bytes. Fold the owner-local
        // gauge change into one write instead of mutating it twice for every installed key.
        atomic_version_bytes_ += parked_bytes;
        atomic_gauge_sub(installed_bytes);
    }

    // THE ONE PLACE THE VERSION-BYTES GAUGE SHRINKS.  It used to be `-=` at eleven sites plus one
    // std::abort() guard at the group-install site, which meant an over-decrement anywhere wrapped
    // the gauge and the process died in whichever unrelated group pass next noticed the wrap.  The
    // over-decrement it was catching was real (a read-modify-write mutated an already-parked
    // predecessor in place, so collapse returned more bytes than install had charged), but that is
    // a resolver defect, fixed in atomic_resolve_internal; killing the server over a memory
    // ACCOUNTING number is a strictly worse failure than carrying a mis-sized one.  Clamp, count,
    // and let a test assert the count is zero.  atomic_gauge_underflows is in INFO stats.
    void atomic_gauge_sub(size_t bytes) {
        if (__builtin_expect(bytes > atomic_version_bytes_, false)) {
            if (atomic_gauge_underflows_) (*atomic_gauge_underflows_)++;
            atomic_version_bytes_ = 0;
            return;
        }
        atomic_version_bytes_ -= bytes;
    }

    void atomic_publish_group(void* opaque) {
        auto* entry = static_cast<AtomicEntry*>(opaque);
        if (!entry || !entry->count || entry->linked) std::abort();
        atomic_link_entry(*entry);
    }


    // Guarantee room in table 0 for a whole owner install pass of `additional` keys, running the
    // SAME resize discipline as the ordinary insert() path so the atomic path shares its two hard
    // guarantees: (1) grow at kLoadPct (70%), not at 100% -- a table run to full has no free slot
    // and insert_into would fail; (2) reclaim tombstones. Every atomic detach leaves a tombstone,
    // and DELETE passes attach nothing, so without a reclaiming trigger on every pass (this is
    // called for deletes too, unlike the old value-writes-only reserve) tombstones fill the table.
    // Once it returns true, the group's physical install pass cannot hit a capacity
    // failure. Returns false only on real OOM (calloc) or a locked snapshot with no raw room.
    bool atomic_prepare_capacity(uint32_t additional) {
        if (rehashing()) {
            if (!snapshot_active_) {
                // Keep drain ahead of fill: advance at least `additional` keys' worth of slots so
                // table 0 cannot fill before this rehash finishes (mirrors insert()'s 1-key : 8-slot
                // ratio, generalised to a whole install pass).
                uint32_t steps = additional / kRehashSlotsPerOp + 1;
                while (steps-- && rehashing()) rehash_step();
            }
            if (rehashing())
                return static_cast<uint64_t>(live_[0] + tombs_[0] + additional) < cap_[0];
            // fell through: rehash completed, re-evaluate against the merged table below.
        }
        if (static_cast<uint64_t>(live_[0] + tombs_[0] + additional) * 100 <
            static_cast<uint64_t>(cap_[0]) * kLoadPct)
            return true;
        if (snapshot_prepared_)
            return static_cast<uint64_t>(live_[0] + tombs_[0] + additional) < cap_[0];
        // Size for the LIVE set plus the pass (tombstones are dropped by the coming rehash). When
        // tombstones dominate and the live set is small this leaves `wanted == cap_[0]`: a same-size
        // rehash that reclaims them, exactly maybe_start_grow()'s policy.
        uint64_t wanted = cap_[0];
        const uint64_t need = static_cast<uint64_t>(live_[0]) + additional;
        while (need * 100 >= wanted * kLoadPct) {
            wanted *= 2;
            if (wanted > UINT32_MAX) return false;
        }
        auto* fresh = static_cast<uint64_t*>(std::calloc(static_cast<size_t>(wanted),
                                                         sizeof(uint64_t)));
        if (!fresh) return false;
        tab_[1] = tab_[0]; cap_[1] = cap_[0]; mask_[1] = mask_[0];
        live_[1] = live_[0]; tombs_[1] = tombs_[0];
        tab_[0] = fresh; cap_[0] = static_cast<uint32_t>(wanted); mask_[0] = cap_[0] - 1;
        live_[0] = tombs_[0] = 0;
        rehash_pos_ = 0;
        return true;
    }

    bool atomic_admit(Slice protected_key, const KvObj* incoming) {
        const size_t bytes = incoming ? kvobj_size(incoming) : 0;
        if (!__builtin_expect(maxmemory_enabled_, false) || snapshot_active_) {
            atomic_version_bytes_ += bytes;
            return true;
        }
        auto fits = [&] {
            const size_t used = accounted_bytes();
            return bytes <= maxmemory_limit_ && used <= maxmemory_limit_ - bytes;
        };
        if (fits()) { atomic_version_bytes_ += bytes; return true; }
        if (maxmemory_policy_ == MaxmemoryPolicy::NoEviction) return false;
        uint32_t budget = kEvictionsPerOp;
        while (budget-- && !fits()) {
            KvObj* victim = choose_victim(protected_key);
            if (!victim) return false;
            const uint64_t hash = hash_key(victim->key());
            const Slice key = victim->key();
            // A key an in-flight group or a staged cross-owner script depends on is not a legal
            // victim: evicting it would destroy the predecessor those readers resolve against.
            if (atomic_needs_version(hash, key)) continue;
            const uint32_t before = size();
            const bool live = erase(hash, key);
            if (size() == before) return false;
            if (live && evicted_counter_) (*evicted_counter_)++;
        }
        if (!fits()) return false;
        atomic_version_bytes_ += bytes;
        return true;
    }

    void atomic_unadmit(const KvObj* incoming) {
        if (incoming) atomic_gauge_sub(kvobj_size(incoming));
    }

    void* atomic_acquire_value_block(size_t allocation) {
        allocation = good_size(allocation);
        if (!atomic_ensure_pending()) return nullptr;
        const uint32_t cls = atomic_pool_class(allocation);
        if (cls < AtomicPendingState::kPoolClasses && atomic_pending_->free_values[cls]) {
            auto* block = atomic_pending_->free_values[cls];
            if (block->allocation != allocation) std::abort();
            atomic_pending_->free_values[cls] = block->next;
            atomic_pending_->cached_value_bytes -= allocation;
            return block;
        }
        return alloc_raw(allocation);
    }

    void atomic_discard_value(KvObj* value) {
        if (!atomic_recycle_value(value)) kvobj_free(value);
    }

    bool atomic_prepare_plain(Slice key, uint64_t origin_conn_id, void*& opaque) {
        opaque = nullptr;
        if (!atomic_ensure_pending()) return false;
        AtomicEntry* entry = atomic_alloc_entry(1, key.n);
        if (!entry) return false;
        entry->capacity = 1;
        entry->key_len = key.n;
        entry->origin_conn_id = origin_conn_id;
        if (key.n) std::memcpy(entry->plain_key_data(), key.p, key.n);
        opaque = entry;
        return true;
    }

    // EXEC's heterogeneous owner work uses copied-key entries rather than ScatterState key spans,
    // but otherwise participates in the identical MVCC list.  Every entry points at the one
    // transaction epoch, so publishing that single ticket makes all owners visible together.
    bool atomic_prepare_transaction(Slice key, uint64_t hash, uint64_t origin_conn_id,
                                    std::atomic<uint64_t>* group_epoch,
                                    std::atomic<uint32_t>* group_refs,
                                    std::atomic<bool>* group_aborted, uint32_t* owner_refs,
                                    void*& opaque) {
        opaque = nullptr;
        if (!atomic_ensure_pending()) return false;
        AtomicEntry* entry = atomic_alloc_entry(1, key.n);
        if (!entry) return false;
        entry->capacity = 1;
        entry->key_len = key.n;
        entry->plain_hash = hash;
        entry->origin_conn_id = origin_conn_id;
        entry->group_epoch = group_epoch;
        entry->group_refs = group_refs;
        entry->group_aborted = group_aborted;
        entry->owner_refs = owner_refs;
        entry->membership = atomic_membership_bit(hash);
        if (key.n) std::memcpy(entry->plain_key_data(), key.p, key.n);
        opaque = entry;
        return true;
    }

    void atomic_discard_plain(void*& opaque) { atomic_discard_group(opaque); }

    void atomic_install_plain(uint64_t hash, Slice key, void*& opaque, KvObj*& value,
                              uint64_t epoch) {
        auto* entry = static_cast<AtomicEntry*>(opaque);
        opaque = nullptr;
        if (!entry || !entry->plain() || entry->key_len != key.n ||
            Slice(entry->plain_key_data(), entry->key_len) != key) std::abort();
        entry->epoch = epoch;
        entry->plain_hash = hash;
        KvObj* old = atomic_exchange_physical(hash, key, value);
        entry->parked()[0] = old;
        entry->count = 1;
        entry->membership = atomic_membership_bit(hash);
        if (old) atomic_version_bytes_ += kvobj_size(old);
        if (value) atomic_gauge_sub(kvobj_size(value));
        value = nullptr;
        atomic_link_entry(*entry);
        atomic_read_epoch_ = epoch;
    }

    KvObj* atomic_install_transaction(uint64_t hash, Slice key, void*& opaque, KvObj*& value) {
        auto* entry = static_cast<AtomicEntry*>(opaque);
        opaque = nullptr;
        if (!entry || !entry->plain() || !entry->group_epoch || entry->key_len != key.n ||
            Slice(entry->plain_key_data(), entry->key_len) != key) std::abort();
        KvObj* old = atomic_exchange_physical(hash, key, value);
        entry->parked()[0] = old;
        entry->count = 1;
        if (old) atomic_version_bytes_ += kvobj_size(old);
        if (value) atomic_gauge_sub(kvobj_size(value));
        value = nullptr;
        atomic_link_entry(*entry);
        return old;
    }

    void atomic_finish_plain() {
        atomic_read_epoch_ = UINT64_MAX;
        atomic_read_origin_conn_id_ = 0;
    }

    KvObj* atomic_resolve(uint64_t hash, Slice key, uint64_t snapshot) {
        return atomic_resolve_internal(hash, key, snapshot, true).value;
    }

    // A scatter owner hoists atomic_has_records() once before its per-key read loop. This direct arm
    // retains find()'s bounded rehash progress but avoids reloading and retesting the pending-list
    // pointer for every key in the group.
    KvObj* atomic_find_tracked(uint64_t hash, Slice key) {
        if (rehashing() && !snapshot_active_) rehash_step();
        KvObj* found = atomic_resolve(hash, key, atomic_read_epoch_);
        if (__builtin_expect(maxmemory_enabled_, false) && found) touch(found);
        return found;
    }

    // Table walkers see one physical pointer per slot, while atomic tombstones may be pending-only.
    // Filter physical keys through the same epoch resolver and then enumerate only visible entry
    // keys that have no physical representative; callers can combine the two streams
    // without a duplicate set or per-key allocation.
    bool atomic_physical_key_visible(uint64_t hash, Slice key, uint64_t snapshot) {
        return !atomic_key_pending(hash, key) || atomic_resolve(hash, key, snapshot) != nullptr;
    }

    template <typename Fn>
    void atomic_for_each_side_key(uint64_t snapshot, Fn&& fn) {
        if (!atomic_pending_) return;
        atomic_for_each_unique_key([&](uint64_t hash, Slice key) {
            if (!atomic_find_physical(hash, key) && atomic_resolve(hash, key, snapshot)) fn(key);
        });
    }

    uint64_t atomic_resolved_size(uint64_t snapshot) {
        int64_t logical = size();
        if (!atomic_pending_) return static_cast<uint64_t>(logical);
        atomic_for_each_unique_key([&](uint64_t hash, Slice key) {
            const bool physical = atomic_find_physical(hash, key) != nullptr;
            const bool visible = atomic_resolve(hash, key, snapshot) != nullptr;
            logical += static_cast<int64_t>(visible) - static_cast<int64_t>(physical);
        });
        return logical < 0 ? 0 : static_cast<uint64_t>(logical);
    }

    bool atomic_promote_key(uint64_t hash, Slice key, uint64_t floor, uint64_t cleanup_cutoff) {
        if (!atomic_key_pending(hash, key)) return false;
        return atomic_collapse(floor, cleanup_cutoff);
    }

    uint32_t atomic_sweep(uint64_t floor, uint64_t cleanup_cutoff, uint32_t budget) {
        if (!budget) return 0;
        return atomic_collapse(floor, cleanup_cutoff) ? 1 : 0;
    }

    // FLUSH is outside the cross-shard atomic set, but it must coexist with live entries. Prepare
    // every pseudo-entry first, then give this owner's logical clear one committed ticket. The
    // subsequent table clear sees tombstones for tracked keys, so protected predecessors remain
    // parked while unrelated plain keys are reclaimed immediately.
    bool atomic_tombstone_all() {
        if (!atomic_pending_ || atomic_pending_->live == 0) return true;
        struct PreparedClear { uint64_t hash; std::string key; AtomicEntry* entry; };
        std::vector<PreparedClear> prepared;
        try {
            uint32_t count = 0;
            atomic_for_each_unique_key([&](uint64_t, Slice) { count++; });
            prepared.reserve(count);
            atomic_for_each_unique_key([&](uint64_t hash, Slice key) {
                PreparedClear item{hash, std::string(key.p, key.n), nullptr};
                void* opaque = nullptr;
                if (!atomic_prepare_plain(key, 0, opaque)) throw std::bad_alloc();
                item.entry = static_cast<AtomicEntry*>(opaque);
                prepared.push_back(std::move(item));
            });
        } catch (const std::bad_alloc&) {
            for (PreparedClear& item : prepared) {
                void* discard = item.entry;
                atomic_discard_plain(discard);
            }
            return false;
        }

        const uint64_t ticket = atomic_ticket_fn_(atomic_ticket_ctx_);
        for (PreparedClear& item : prepared) {
            Slice key(item.key.data(), static_cast<uint32_t>(item.key.size()));
            void* opaque = item.entry;
            KvObj* tombstone = nullptr;
            atomic_install_plain(item.hash, key, opaque, tombstone, ticket);
        }
        return true;
    }

private:
    bool atomic_ensure_pending() {
        if (atomic_pending_) return true;
        atomic_pending_ = new (std::nothrow) AtomicPendingState;
        return atomic_pending_ != nullptr;
    }

    AtomicEntry* atomic_alloc_entry(uint32_t capacity, uint32_t key_bytes) {
        const size_t request = sizeof(AtomicEntry) + sizeof(KvObj*) * capacity + key_bytes;
        const size_t allocation = good_size(request);
        const uint32_t cls = atomic_pool_class(allocation);
        AtomicEntry* entry = nullptr;
        if (cls < AtomicPendingState::kPoolClasses && atomic_pending_->free_entries[cls]) {
            entry = atomic_pending_->free_entries[cls];
            atomic_pending_->free_entries[cls] = entry->pool_next;
            atomic_pending_->cached_entries--;
            atomic_pending_->cached_entry_bytes -= entry->allocation;
            *entry = AtomicEntry{};
        } else {
            void* memory = alloc_raw(allocation);
            if (!memory) return nullptr;
            entry = new (memory) AtomicEntry;
        }
        entry->allocation = allocation;
        return entry;
    }

    void atomic_free_entry(AtomicEntry* entry) {
        if (!entry) return;
        const size_t allocation = entry->allocation;
        const uint32_t cls = atomic_pool_class(allocation);
        static constexpr uint32_t kEntryCache = 4096;
        static constexpr size_t kEntryCacheBytes = 4 * 1024 * 1024;
        if (atomic_pending_ && cls < AtomicPendingState::kPoolClasses &&
            atomic_pending_->cached_entries < kEntryCache &&
            atomic_pending_->cached_entry_bytes + allocation <= kEntryCacheBytes) {
            entry->pool_next = atomic_pending_->free_entries[cls];
            atomic_pending_->free_entries[cls] = entry;
            atomic_pending_->cached_entries++;
            atomic_pending_->cached_entry_bytes += allocation;
        } else {
            free_sized(entry, allocation);
        }
    }

    void atomic_destroy_pending() {
        if (!atomic_pending_) return;
        if (atomic_pending_->live || atomic_pending_->script_intent_count) std::abort();
        for (uint32_t cls = 0; cls < AtomicPendingState::kPoolClasses; cls++) {
            while (atomic_pending_->free_values[cls]) {
                auto* block = atomic_pending_->free_values[cls];
                atomic_pending_->free_values[cls] = block->next;
                free_sized(block, block->allocation);
            }
            while (atomic_pending_->free_entries[cls]) {
                AtomicEntry* entry = atomic_pending_->free_entries[cls];
                atomic_pending_->free_entries[cls] = entry->pool_next;
                free_sized(entry, entry->allocation);
            }
        }
        delete atomic_pending_;
        atomic_pending_ = nullptr;
    }

    uint64_t atomic_entry_hash(const AtomicEntry& entry, uint32_t index) const {
        return entry.plain() ? entry.plain_hash
                             : xshard_atomic_key_hash(entry.group, entry.begin + index);
    }

    Slice atomic_entry_key(const AtomicEntry& entry, uint32_t index) const {
        return entry.plain()
            ? Slice(entry.plain_key_data(), entry.key_len)
            : xshard_atomic_key_slice(entry.group, entry.begin + index);
    }

    bool atomic_key_pending(uint64_t hash, Slice key) const {
        if (!atomic_pending_ || !atomic_pending_->live) return false;
        const uint64_t bit = atomic_membership_bit(hash);
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
            if (!(entry->membership & bit)) continue;
            for (uint32_t i = 0; i < entry->count; i++)
                if (atomic_entry_hash(*entry, i) == hash && atomic_entry_key(*entry, i) == key)
                    return true;
        }
        return false;
    }

    void atomic_link_entry(AtomicEntry& entry) {
        if (entry.linked) std::abort();
        const bool first = atomic_pending_->live == 0 &&
                           atomic_pending_->script_intent_count == 0;
        entry.prev = atomic_pending_->tail;
        if (atomic_pending_->tail) atomic_pending_->tail->next = &entry;
        else atomic_pending_->head = &entry;
        atomic_pending_->tail = &entry;
        const uint32_t bucket = static_cast<uint32_t>(mix64(entry.origin_conn_id)) & 63;
        entry.conn_next = atomic_pending_->conn_heads[bucket];
        if (entry.conn_next) entry.conn_next->conn_prev = &entry;
        atomic_pending_->conn_heads[bucket] = &entry;
        entry.linked = true;
        atomic_pending_->live++;
        if (atomic_entries_) (*atomic_entries_)++;
        if (atomic_chain_max_ && atomic_pending_->live > *atomic_chain_max_)
            *atomic_chain_max_ = atomic_pending_->live;
        if (first && atomic_activity_)
            atomic_activity_->fetch_add(1, std::memory_order_release);
    }

    void atomic_unlink_conn(AtomicEntry& entry) {
        const uint32_t bucket = static_cast<uint32_t>(mix64(entry.origin_conn_id)) & 63;
        if (entry.conn_prev) entry.conn_prev->conn_next = entry.conn_next;
        else {
            if (atomic_pending_->conn_heads[bucket] != &entry) std::abort();
            atomic_pending_->conn_heads[bucket] = entry.conn_next;
        }
        if (entry.conn_next) entry.conn_next->conn_prev = entry.conn_prev;
        entry.conn_next = entry.conn_prev = nullptr;
    }

    KvObj* atomic_find_physical(uint64_t hash, Slice key) const {
        if (KvObj* object = find_in(0, hash, key)) return object;
        return rehashing() ? find_in(1, hash, key) : nullptr;
    }

    AtomicResolved atomic_resolve_internal(uint64_t hash, Slice key, uint64_t snapshot,
                                           bool count_predecessor) {
        KvObj* physical_value = atomic_find_physical(hash, key);
        if (!atomic_pending_ || !atomic_pending_->live)
            return AtomicResolved{atomic_live_value(physical_value), false, true};
        const uint64_t bit = atomic_membership_bit(hash);
        AtomicEntry* previous = nullptr;
        KvObj* base = nullptr;
        KvObj* winner = nullptr;
        uint64_t winner_epoch = 0;
        bool winner_set = false;
        bool winner_physical = false;
        bool winner_private = false;
        bool matched = false;
        // THE SAME PROGRAM-ORDER REPAIR atomic_collapse() APPLIES, AND IT IS LOAD-BEARING HERE.
        // A read reaches this chain whenever it beats the owner's post-batch cleanup to the pair,
        // and it must then rank a connection's records the way that connection sent them, not by
        // their commit tickets -- otherwise it answers from the older cross-shard group after the
        // younger transaction's write, which is the stale-read half of the defect.
        // This was measured, deleted, and restored: with the target on 16 cores the cleanup pass
        // always won the race, so removing this scored an identical 0 on the stale-read probe and
        // looked free. With the target correctly pinned to 8 cores the read wins often enough that
        // collapse-alone failed tests/multires.py in 3 of 8 runs while this restores 0 of 8. The
        // cost is real (~2% on the atomic-path A/B) and is bought deliberately.
        uint64_t prev_conn_id = 0;
        uint64_t prev_epoch = 0;
        auto consider = [&](AtomicEntry& owner, KvObj* candidate, bool is_physical) {
            if (owner.group_epoch && owner.group_aborted &&
                owner.group_aborted->load(std::memory_order_acquire)) return;
            uint64_t epoch = atomic_epoch(owner);
            // Applied BEFORE the visibility tests below: a snapshot excluding a connection's
            // earlier record must exclude its later one too, which is what lifting the epoch
            // does. Epoch 0 is an own still-private candidate -- it carries no ticket, so it
            // neither repairs nor becomes the floor, and prev_epoch==0 doubles as "none yet".
            if (__builtin_expect(prev_epoch != 0, false)) {
                if (epoch && epoch < prev_epoch && owner.origin_conn_id == prev_conn_id)
                    epoch = prev_epoch;
            }
            if (epoch) {
                prev_conn_id = owner.origin_conn_id;
                prev_epoch = epoch;
            }
            const bool own_committed = atomic_read_origin_conn_id_ != 0 &&
                owner.origin_conn_id == atomic_read_origin_conn_id_;
            // A transaction's own still-private installs are its RYOW overlay.  Foreign epoch-zero
            // candidates remain invisible exactly as before; ordinary pipelined commands still
            // defer on their own undecided predecessor before reaching this resolver.
            if (owner.group_epoch && epoch == 0 && !own_committed) return;
            if (!(epoch <= snapshot || own_committed)) return;
            // RANK, NOT EPOCH.  An own still-private candidate carries epoch 0 because its ticket is
            // not drawn yet -- it is the NEWEST value this connection has, not the oldest.  Ranking
            // it by its epoch made it lose to any older but committed version of the same key, so
            // the second touch of a key inside one transaction answered from before the first, and
            // an in-place read-modify-write (RPUSH/APPEND/INCRBY) then mutated the parked
            // predecessor -- a write that replies normally and is discarded at collapse.  A private
            // candidate therefore outranks every committed one; among private candidates, and among
            // committed ones, the later occurrence still wins (equal epochs are several physical
            // installs inside one command, whose last occurrence is that command's final value).
            const bool own_private = owner.group_epoch && epoch == 0;
            if (!winner_set ||
                (own_private ? true : (!winner_private && epoch >= winner_epoch))) {
                winner = candidate;
                winner_epoch = epoch;
                winner_set = true;
                winner_physical = is_physical;
                winner_private = own_private;
            }
        };
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
            if (!(entry->membership & bit)) continue;
            for (uint32_t i = 0; i < entry->count; i++) {
                if (atomic_entry_hash(*entry, i) != hash || atomic_entry_key(*entry, i) != key)
                    continue;
                if (!matched) base = entry->parked()[i];
                else consider(*previous, entry->parked()[i], false);
                previous = entry;
                matched = true;
            }
        }
        if (!matched) return AtomicResolved{atomic_live_value(physical_value), false, true};
        consider(*previous, physical_value, true);
        KvObj* value = winner_set ? winner : base;
        const bool from_physical = winner_set && winner_physical;
        if (!from_physical && count_predecessor && atomic_predecessor_reads_)
            (*atomic_predecessor_reads_)++;
        return AtomicResolved{atomic_live_value(value), true, from_physical};
    }

    KvObj* atomic_live_value(KvObj* value) const {
        if (!value) return nullptr;
        if ((value->flags & KvObjFlags::HasTtl) && value->expire_at_ms() <= cached_now_ms_)
            return nullptr;
        return value;
    }

    template <typename Fn>
    void atomic_for_each_unique_key(Fn&& fn) {
        if (!atomic_pending_) return;
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
            for (uint32_t i = 0; i < entry->count; i++) {
                const uint64_t hash = atomic_entry_hash(*entry, i);
                const Slice key = atomic_entry_key(*entry, i);
                bool later = false;
                for (AtomicEntry* scan = entry; scan && !later; scan = scan->next) {
                    const uint32_t first = scan == entry ? i + 1 : 0;
                    if (!(scan->membership & atomic_membership_bit(hash))) continue;
                    for (uint32_t j = first; j < scan->count; j++)
                        if (atomic_entry_hash(*scan, j) == hash &&
                            atomic_entry_key(*scan, j) == key) { later = true; break; }
                }
                if (!later) fn(hash, key);
            }
        }
    }

    // Swap a key's physical representation in its existing probe slot. The old detach+attach pair
    // searched the table twice and manufactured a tombstone for every promotion; atomic ownership
    // needs the displaced pointer, so insert_into's ordinary retire-in-place path is not usable.
    KvObj* atomic_exchange_physical(uint64_t hash, Slice key, KvObj* replacement) {
        auto exchange = [&](int table) -> KvObj* {
            if (!tab_[table]) return nullptr;
            const uint16_t tag = tag_of(hash);
            uint32_t slot = slot_start(table, hash);
            for (uint32_t probes = 0; probes <= cap_[table]; probes++) {
                const uint64_t word = tab_[table][slot];
                if (word == 0) return nullptr;
                KvObj* object = ptr_of(word);
                if (object && tag_of_word(word) == tag && object->key() == key) {
                    obj_bytes_ -= kvobj_size(object);
                    if (replacement && table == 0) {
                        tab_[table][slot] = make_word(tag, replacement);
                        obj_bytes_ += kvobj_size(replacement);
                        if (replacement->flags & KvObjFlags::HasTtl) expires_.insert(hash);
                        else expires_.erase(hash);
                    } else {
                        tab_[table][slot] = kTombBit;
                        live_[table]--; tombs_[table]++;
                        expires_.erase(hash);
                    }
                    return object;
                }
                slot = (slot + 1) & mask_[table];
            }
            return nullptr;
        };
        if (KvObj* object = exchange(0)) return object;
        if (rehashing()) {
            if (KvObj* object = exchange(1)) {
                if (replacement && !insert_into(0, hash, replacement, true)) std::abort();
                return object;
            }
        }
        if (replacement && !insert_into(0, hash, replacement, true)) std::abort();
        return nullptr;
    }

    bool atomic_recycle_value(KvObj* object) {
        if (!object || !atomic_pending_ || static_cast<Type>(object->type) != Type::String)
            return false;
        const Enc encoding = static_cast<Enc>(object->enc);
        if (encoding != Enc::Raw && encoding != Enc::Int) return false;
        if (encoding == Enc::Raw && outstanding_borrows_ && is_borrowed(object->str_value().p))
            return false;
        const size_t allocation = kvobj_capacity(object);
        const uint32_t cls = atomic_pool_class(allocation);
        static constexpr size_t kValueCacheBytes = 4 * 1024 * 1024;
        if (cls >= AtomicPendingState::kPoolClasses ||
            allocation < sizeof(AtomicPendingState::FreeValue) ||
            atomic_pending_->cached_value_bytes + allocation > kValueCacheBytes) return false;
        auto* block = reinterpret_cast<AtomicPendingState::FreeValue*>(object);
        block->next = atomic_pending_->free_values[cls];
        block->allocation = allocation;
        atomic_pending_->free_values[cls] = block;
        atomic_pending_->cached_value_bytes += allocation;
        return true;
    }

    void retire_detached_obj(KvObj* object) {
        if (!object) return;
        if (atomic_recycle_value(object)) return;
        if (outstanding_borrows_ == 0) { kvobj_free(object); return; }
        const char* ptr = (static_cast<Type>(object->type) == Type::String && !object->is_int())
                              ? object->str_value().p : nullptr;
        const uint32_t at = ptr ? borrow_find(ptr) : kNoBorrow;
        if (at != kNoBorrow) {
            borrows_[at].retired = object;
            pending_bytes_ += kvobj_size(object);
            return;
        }
        kvobj_free(object);
    }

    bool atomic_collapse(uint64_t floor, uint64_t cleanup_cutoff) {
        if (!atomic_pending_ || !atomic_pending_->live || snapshot_active_) return false;
        uint32_t occurrences = 0;
        AtomicEntry* first_unselected = atomic_pending_->head;
        while (first_unselected) {
            AtomicEntry* entry = first_unselected;
            const bool aborted = entry->group_epoch && entry->group_aborted &&
                entry->group_aborted->load(std::memory_order_acquire);
            const uint64_t epoch = atomic_epoch(*entry);
            if (!aborted && (!epoch || epoch >= floor || epoch > cleanup_cutoff))
                break;
            occurrences += entry->count;
            first_unselected = entry->next;
        }
        if (!occurrences) return false;

        // Only an actual key overlap whose commit tickets invert needs argmax collapse. Masks reject
        // almost all inverted entry pairs; when no such pair exists, the prefix reduces to freeing
        // parked predecessors and unlinking group entries. The usual monotonic case touches no key,
        // hash, table slot, or transient map.
        bool direct = true;
        uint64_t previous_epoch = 0;
        for (AtomicEntry* entry = atomic_pending_->head; entry != first_unselected;
             entry = entry->next) {
            if ((entry->group_epoch && entry->group_aborted &&
                 entry->group_aborted->load(std::memory_order_acquire)) ||
                atomic_epoch(*entry) < previous_epoch) {
                direct = false;
                break;
            }
            previous_epoch = atomic_epoch(*entry);
        }
        // Install order and ticket order are normally the same. Only an inversion needs the
        // pairwise membership/exact-key proof that the inverted entries do not overlap.
        if (!direct) {
            direct = true;
            for (AtomicEntry* later = atomic_pending_->head;
                 later != first_unselected && direct; later = later->next) {
                if (later->group_epoch && later->group_aborted &&
                    later->group_aborted->load(std::memory_order_acquire)) {
                    direct = false;
                    break;
                }
                const uint64_t later_epoch = atomic_epoch(*later);
                for (AtomicEntry* earlier = atomic_pending_->head; earlier != later;
                     earlier = earlier->next) {
                    if (atomic_epoch(*earlier) <= later_epoch ||
                        !(earlier->membership & later->membership)) continue;
                    for (uint32_t i = 0; i < earlier->count && direct; i++) {
                        const uint64_t hash = atomic_entry_hash(*earlier, i);
                        if (!(later->membership & atomic_membership_bit(hash))) continue;
                        const Slice key = atomic_entry_key(*earlier, i);
                        for (uint32_t j = 0; j < later->count; j++) {
                            if (atomic_entry_hash(*later, j) == hash &&
                                atomic_entry_key(*later, j) == key) {
                                direct = false;
                                break;
                            }
                        }
                    }
                    if (!direct) break;
                }
            }
        }
        if (direct) {
            atomic_pending_->cleanup_fast += occurrences;
            AtomicEntry* entry = atomic_pending_->head;
            while (entry != first_unselected) {
                AtomicEntry* next = entry->next;
                for (uint32_t i = 0; i < entry->count; i++) {
                    if (KvObj* parked = entry->parked()[i]) {
                        atomic_gauge_sub(kvobj_size(parked));
                        retire_detached_obj(parked);
                        entry->parked()[i] = nullptr;
                    }
                }
                if (entry->group_refs && (!entry->owner_refs || --*entry->owner_refs == 0))
                    entry->group_refs->fetch_sub(1, std::memory_order_release);
                if (atomic_records_freed_) (*atomic_records_freed_)++;
                atomic_pending_->live--;
                atomic_unlink_conn(*entry);
                atomic_free_entry(entry);
                entry = next;
            }
            atomic_pending_->head = first_unselected;
            if (first_unselected) first_unselected->prev = nullptr;
            else atomic_pending_->tail = nullptr;
            if (!atomic_pending_->live && !atomic_pending_->script_intent_count &&
                atomic_activity_)
                atomic_activity_->fetch_sub(1, std::memory_order_release);
            if (atomic_promotions_) *atomic_promotions_ += occurrences;
            return true;
        }

        // Detect overlap only inside the reclaimable prefix. A successor after the prefix already
        // parks the prefix's last installed candidate, so a committed non-overlapping entry needs
        // no table probe or chain surgery at all.
        bool overlap = false;
        uint64_t seen_bits = 0;
        try {
            atomic_seen_keys_.clear();
            atomic_seen_keys_.reserve(occurrences);
        } catch (const std::bad_alloc&) {
            return false;
        }
        for (AtomicEntry* entry = atomic_pending_->head;
             entry != first_unselected && !overlap;
             entry = entry->next) {
            for (uint32_t i = 0; i < entry->count; i++) {
                const uint64_t hash = atomic_entry_hash(*entry, i);
                const Slice key = atomic_entry_key(*entry, i);
                const uint64_t bit = atomic_membership_bit(hash);
                if (seen_bits & bit) {
                    for (const AtomicSeenKey& seen : atomic_seen_keys_) {
                        if (seen.hash == hash && seen.key == key) {
                            overlap = true;
                            break;
                        }
                    }
                }
                if (overlap) break;
                atomic_seen_keys_.push_back(AtomicSeenKey{hash, key});
                seen_bits |= bit;
            }
        }
        if (!overlap) {
            atomic_pending_->cleanup_slow += occurrences;
            uint64_t promoted = 0;
            AtomicEntry* entry = atomic_pending_->head;
            while (entry != first_unselected) {
                AtomicEntry* next = entry->next;
                const bool aborted = entry->group_epoch && entry->group_aborted &&
                    entry->group_aborted->load(std::memory_order_acquire);
                for (uint32_t i = 0; i < entry->count; i++) {
                    const uint64_t hash = atomic_entry_hash(*entry, i);
                    const Slice key = atomic_entry_key(*entry, i);
                    KvObj* parked = entry->parked()[i];
                    if (aborted) {
                        KvObj* replacement = atomic_live_value(parked);
                        AtomicEntry* boundary = nullptr;
                        uint32_t boundary_index = 0;
                        const uint64_t bit = atomic_membership_bit(hash);
                        for (AtomicEntry* scan = first_unselected; scan && !boundary;
                             scan = scan->next) {
                            if (!(scan->membership & bit)) continue;
                            for (uint32_t j = 0; j < scan->count; j++) {
                                if (atomic_entry_hash(*scan, j) == hash &&
                                    atomic_entry_key(*scan, j) == key) {
                                    boundary = scan;
                                    boundary_index = j;
                                    break;
                                }
                            }
                        }
                        if (boundary) {
                            KvObj* loser = boundary->parked()[boundary_index];
                            if (loser) atomic_gauge_sub(kvobj_size(loser));
                            if (loser != replacement) retire_detached_obj(loser);
                            boundary->parked()[boundary_index] = replacement;
                            if (parked && parked != replacement) {
                                atomic_gauge_sub(kvobj_size(parked));
                                retire_detached_obj(parked);
                            }
                        } else {
                            KvObj* physical = atomic_find_physical(hash, key);
                            if (physical != replacement) {
                                KvObj* loser = atomic_exchange_physical(hash, key, replacement);
                                if (loser != physical) std::abort();
                                retire_detached_obj(loser);
                            }
                            if (parked) {
                                atomic_gauge_sub(kvobj_size(parked));
                                if (parked != replacement) retire_detached_obj(parked);
                            }
                        }
                    } else if (parked) {
                        atomic_gauge_sub(kvobj_size(parked));
                        retire_detached_obj(parked);
                    }
                    entry->parked()[i] = nullptr;
                    promoted++;
                }
                if (entry->group_refs && (!entry->owner_refs || --*entry->owner_refs == 0))
                    entry->group_refs->fetch_sub(1, std::memory_order_release);
                if (atomic_records_freed_) (*atomic_records_freed_)++;
                atomic_pending_->live--;
                atomic_unlink_conn(*entry);
                atomic_free_entry(entry);
                entry = next;
            }
            atomic_pending_->head = first_unselected;
            if (first_unselected) first_unselected->prev = nullptr;
            else atomic_pending_->tail = nullptr;
            if (!atomic_pending_->live && !atomic_pending_->script_intent_count &&
                atomic_activity_)
                atomic_activity_->fetch_sub(1, std::memory_order_release);
            if (atomic_promotions_) *atomic_promotions_ += promoted;
            return true;
        }

        // Actual overlap is rare, but install order and commit-ticket order may invert. Collapse
        // the prefix by committed argmax, then splice its winner into the first unselected
        // occurrence (or the physical table when no successor exists).
        uint32_t table_cap = 8;
        atomic_pending_->cleanup_slow += occurrences;
        while (table_cap < occurrences * 2) table_cap <<= 1;
        try {
            atomic_collapse_keys_.clear();
            atomic_collapse_keys_.reserve(occurrences);
            atomic_collapse_slots_.assign(table_cap, AtomicCollapseSlot{});
            atomic_collapse_retire_.clear();
            atomic_collapse_retire_.reserve(static_cast<size_t>(occurrences) + occurrences / 2);
        } catch (const std::bad_alloc&) {
            return false;
        }
        const uint32_t table_mask = table_cap - 1;
        auto key_index = [&](uint64_t hash, Slice key, KvObj* base) -> uint32_t {
            uint32_t slot = static_cast<uint32_t>(mix64(hash)) & table_mask;
            for (;;) {
                AtomicCollapseSlot& item = atomic_collapse_slots_[slot];
                if (!item.index) {
                    atomic_collapse_keys_.push_back(AtomicCollapseKey{hash, key, base});
                    item.hash = hash;
                    item.index = static_cast<uint32_t>(atomic_collapse_keys_.size());
                    return item.index - 1;
                }
                AtomicCollapseKey& found = atomic_collapse_keys_[item.index - 1];
                if (item.hash == hash && found.key == key) return item.index - 1;
                slot = (slot + 1) & table_mask;
            }
        };
        auto consider = [&](AtomicCollapseKey& key, AtomicEntry& owner, KvObj* candidate) {
            const bool aborted = owner.group_epoch && owner.group_aborted &&
                owner.group_aborted->load(std::memory_order_acquire);
            if (aborted) return;
            uint64_t epoch = atomic_epoch(owner);
            // PROGRAM ORDER BEATS TICKET ORDER WITHIN ONE CONNECTION.
            // This walk visits a key's records in list order, and for records from the SAME
            // connection that order IS the client's program order: owner tasks are posted to a
            // shard in arrival order and drained in order, so a connection's later command
            // cannot install ahead of its earlier one on the same owner.
            // Their commit TICKETS may still invert, because a cross-shard unit draws its ticket
            // only when its LAST fragment has installed: a wide MSET/DEL group that is still
            // waiting on other shards can be overtaken at the ticket counter by a MULTI/EXEC the
            // same connection sent afterwards. Ranking those two by raw ticket then re-installed
            // the OLDER group's value over the newer transaction's, destroying a write EXEC had
            // already acknowledged -- reproduced at 246/1120 (NOTES-MULTIRES.md).
            // Repairing the epoch to be monotone per connection removes the inversion without
            // reordering anything across connections, where the ticket remains the only truth.
            if (key.prev_set && owner.origin_conn_id == key.prev_conn_id &&
                epoch < key.prev_epoch)
                epoch = key.prev_epoch;
            key.prev_conn_id = owner.origin_conn_id;
            key.prev_epoch = epoch;
            key.prev_set = true;
            if (!key.winner_set || epoch >= key.winner_epoch) {
                key.winner = candidate;
                key.winner_epoch = epoch;
                key.winner_set = true;
            }
        };
        for (AtomicEntry* entry = atomic_pending_->head; entry != first_unselected;
             entry = entry->next) {
            for (uint32_t i = 0; i < entry->count; i++) {
                const uint64_t hash = atomic_entry_hash(*entry, i);
                const Slice key = atomic_entry_key(*entry, i);
                const uint32_t index = key_index(hash, key, entry->parked()[i]);
                AtomicCollapseKey& collapse = atomic_collapse_keys_[index];
                if (collapse.previous) consider(collapse, *collapse.previous,
                                                entry->parked()[i]);
                collapse.previous = entry;
            }
        }

        uint32_t restore = 0;
        for (AtomicCollapseKey& key : atomic_collapse_keys_) {
            const uint64_t bit = atomic_membership_bit(key.hash);
            for (AtomicEntry* scan = first_unselected; scan && !key.boundary;
                 scan = scan->next) {
                if (!(scan->membership & bit)) continue;
                for (uint32_t i = 0; i < scan->count; i++) {
                    if (atomic_entry_hash(*scan, i) == key.hash &&
                        atomic_entry_key(*scan, i) == key.key) {
                        key.boundary = scan;
                        key.boundary_index = i;
                        break;
                    }
                }
            }
            key.physical = key.boundary ? key.boundary->parked()[key.boundary_index]
                                        : atomic_find_physical(key.hash, key.key);
            consider(key, *key.previous, key.physical);
            if (!key.winner_set) key.winner = key.base;
            key.winner = atomic_live_value(key.winner);
            if (!key.boundary && key.winner && !key.physical) restore++;
        }
        if (restore && !atomic_prepare_capacity(restore)) return false;

        for (AtomicCollapseKey& key : atomic_collapse_keys_) {
            if (key.boundary) {
                KvObj*& slot = key.boundary->parked()[key.boundary_index];
                if (slot != key.winner) {
                    if (slot) {
                        atomic_gauge_sub(kvobj_size(slot));
                        atomic_collapse_retire_.push_back(slot);
                    }
                    slot = key.winner;
                }
            } else if (key.physical != key.winner) {
                key.physical_loser = atomic_exchange_physical(key.hash, key.key, key.winner);
                if (key.physical_loser != key.physical) std::abort();
            }
        }
        for (AtomicEntry* entry = atomic_pending_->head; entry != first_unselected;
             entry = entry->next) {
            for (uint32_t i = 0; i < entry->count; i++) {
                KvObj* parked = entry->parked()[i];
                if (!parked) continue;
                AtomicCollapseKey& key = atomic_collapse_keys_[key_index(
                    atomic_entry_hash(*entry, i), atomic_entry_key(*entry, i), nullptr)];
                if (!(key.boundary && parked == key.winner))
                            atomic_gauge_sub(kvobj_size(parked));
                if (parked != key.winner) atomic_collapse_retire_.push_back(parked);
                entry->parked()[i] = nullptr;
            }
        }
        for (AtomicCollapseKey& key : atomic_collapse_keys_)
            if (key.physical_loser) atomic_collapse_retire_.push_back(key.physical_loser);
        std::sort(atomic_collapse_retire_.begin(), atomic_collapse_retire_.end());
        atomic_collapse_retire_.erase(
            std::unique(atomic_collapse_retire_.begin(), atomic_collapse_retire_.end()),
            atomic_collapse_retire_.end());
        for (KvObj* object : atomic_collapse_retire_) retire_detached_obj(object);

        AtomicEntry* entry = atomic_pending_->head;
        uint32_t freed_entries = 0;
        while (entry != first_unselected) {
            AtomicEntry* next = entry->next;
            if (entry->group_refs && (!entry->owner_refs || --*entry->owner_refs == 0))
                entry->group_refs->fetch_sub(1, std::memory_order_release);
            if (atomic_records_freed_) (*atomic_records_freed_)++;
            freed_entries++;
            atomic_unlink_conn(*entry);
            atomic_free_entry(entry);
            entry = next;
        }
        atomic_pending_->head = first_unselected;
        if (first_unselected) first_unselected->prev = nullptr;
        else atomic_pending_->tail = nullptr;
        atomic_pending_->live -= freed_entries;
        if (!atomic_pending_->live && !atomic_pending_->script_intent_count && atomic_activity_)
            atomic_activity_->fetch_sub(1, std::memory_order_release);
        if (atomic_promotions_) *atomic_promotions_ += occurrences;
        return true;
    }

    void atomic_promote_all_for_shutdown() {
        if (!atomic_pending_) return;
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next)
            if (entry->group_epoch && atomic_epoch(*entry) == 0 && entry->group_aborted)
                entry->group_aborted->store(true, std::memory_order_relaxed);
        if (atomic_pending_->live && !atomic_collapse(UINT64_MAX, UINT64_MAX)) std::abort();
        atomic_destroy_pending();
        atomic_version_bytes_ = 0;
    }

    size_t    atomic_version_bytes_ = 0;
    // Null until the first atomic group reaches this owner. The object stays as a pool after the
    // list drains; its zero live count is the common ON read test. KvObj remains byte-identical.
    AtomicPendingState* atomic_pending_ = nullptr;
    std::vector<AtomicCollapseKey> atomic_collapse_keys_;
    std::vector<AtomicCollapseSlot> atomic_collapse_slots_;
    std::vector<KvObj*> atomic_collapse_retire_;
    std::vector<AtomicSeenKey> atomic_seen_keys_;
    uint64_t atomic_read_epoch_ = UINT64_MAX;
    uint64_t atomic_read_origin_conn_id_ = 0;
    AtomicTicketFn atomic_ticket_fn_ = nullptr;
    void* atomic_ticket_ctx_ = nullptr;
    std::atomic<uint64_t>* atomic_activity_ = nullptr;
    std::atomic<uint64_t>* atomic_script_intents_ = nullptr;
    uint64_t* atomic_predecessor_reads_ = nullptr;
    uint64_t* atomic_chain_max_ = nullptr;
    uint64_t* atomic_promotions_ = nullptr;
    uint64_t* atomic_records_freed_ = nullptr;
    uint64_t* atomic_entries_ = nullptr;
    // Cold: written only on the clamped path in atomic_gauge_sub, read only by INFO.
    uint64_t* atomic_gauge_underflows_ = nullptr;

public:
