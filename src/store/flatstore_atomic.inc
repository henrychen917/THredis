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
                bytes_equal(intent->key_data(), key.p, key.n)) return true;
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

    // DEBUG TRIPWIRE helpers.  Both are owner-local list walks and are called only after the
    // process-cold debug registry is armed.  Keeping them here lets the probe ask the exact same
    // question as the writer/resolver instead of reconstructing it in command code.
    bool atomic_tripwire_plain_path() const {
        return !atomic_pending_ || atomic_pending_->live == 0;
    }
    uint64_t atomic_tripwire_ticket(uint64_t hash, Slice key) const {
        if (!atomic_pending_ || !atomic_pending_->live) return 0;
        const uint64_t bit = atomic_membership_bit(hash);
        uint64_t ticket = 0;
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
            if (!(entry->membership & bit)) continue;
            for (uint32_t i = 0; i < entry->count; i++) {
                if (atomic_entry_hash(*entry, i) != hash || !atomic_entry_key(*entry, i).key_eq(key))
                    continue;
                const uint64_t candidate = atomic_epoch(*entry);
                if (candidate && (!ticket || candidate < ticket)) ticket = candidate;
            }
        }
        return ticket;
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
                !bytes_equal(intent->key_data(), key.p, key.n)) continue;
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
                !bytes_equal(intent->key_data(), key.p, key.n)) {
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
                    !atomic_entry_key(*entry, i).key_eq(key)) continue;
                const uint64_t epoch = atomic_epoch(*entry);
                if (!epoch || epoch > cut) return true;
            }
        }
        return false;
    }
    // Same-connection program order is the only ordering dependency between pipelined commands.
    // Foreign undecided nodes never block: resolution simply skips them.
    // A caller that has to exclude its OWN logical unit cannot do it here: a transaction spreads
    // over several epoch words (its own plus one per lowered cross-shard child) and one
    // `ignore_group` pointer names none of them. That question has its own probe --
    // atomic_has_foreign_unit_undecided(), below, which takes the predicate.
    bool atomic_has_own_undecided(uint64_t hash, Slice key, uint64_t origin_conn_id,
                                  const ScatterState* ignore_group = nullptr) const {
        if (!atomic_pending_) return false;
        const uint64_t bit = atomic_membership_bit(hash);
        const uint32_t bucket = static_cast<uint32_t>(mix64(origin_conn_id)) & 63;
        for (AtomicEntry* entry = atomic_pending_->conn_heads[bucket]; entry;
             entry = entry->conn_next) {
            if (!(entry->membership & bit) || !entry->group_epoch ||
                entry->origin_conn_id != origin_conn_id || entry->group == ignore_group)
                continue;
            if (entry->group_epoch->load(std::memory_order_acquire) != 0) continue;
            if (entry->group_aborted && entry->group_aborted->load(std::memory_order_acquire))
                continue;
            for (uint32_t i = 0; i < entry->count; i++)
                if (atomic_entry_hash(*entry, i) == hash &&
                    atomic_entry_key(*entry, i).key_eq(key)) return true;
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
                        atomic_entry_key(*entry, j).key_eq(key)) return true;
            }
        }
        return false;
    }
    // Does this owner still hold an UNDECIDED record belonging to a DIFFERENT logical unit of
    // the same connection? A cross-shard atomic group installs its candidate physically before it
    // knows its own verdict -- MSETNX discovers an existing key on another owner, and any group
    // can still fail on OOM/maxmemory -- so such a record may yet be withdrawn. Program order
    // makes it this connection's business alone: foreign connections never see an epoch-zero
    // record at all, and a same-connection reader must not run past it.
    // `is_own` names the caller's OWN unit BY EPOCH WORD, and it must recognise every word that
    // unit publishes through, not just one: an EXEC's plain per-key installs use the transaction's
    // epoch, while each cross-shard child command lowered inside that EXEC keeps its own
    // ScatterState and therefore its own. Answering with only the transaction's word makes a
    // transaction hold against its own RENAME/LMPOP child and hang -- the same self-hold
    // NOTES-MULTIRES.md §5(a) hit from the other direction. Plain single-command pseudo-entries
    // carry no group_epoch at all: they draw their ticket in the same owner turn that installs
    // them, so they are never undecided and are skipped here.
    template <typename IsOwnUnit>
    bool atomic_has_foreign_unit_undecided(uint64_t origin_conn_id, IsOwnUnit&& is_own) const {
        if (!atomic_pending_ || !atomic_pending_->live) return false;
        const uint32_t bucket = static_cast<uint32_t>(mix64(origin_conn_id)) & 63;
        for (AtomicEntry* entry = atomic_pending_->conn_heads[bucket]; entry;
             entry = entry->conn_next) {
            if (!entry->group_epoch || entry->origin_conn_id != origin_conn_id ||
                is_own(entry->group_epoch))
                continue;
            if (entry->group_epoch->load(std::memory_order_acquire) != 0) continue;
            if (entry->group_aborted && entry->group_aborted->load(std::memory_order_acquire))
                continue;
            return true;
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
                               uint32_t* owner_refs, uint64_t membership,
                               bool copy_keys = false, uint64_t key_bytes = 0) {
        if (!copy_keys && key_bytes) std::abort();
        if (__builtin_expect(read_local_enabled_, false))
            return atomic_prepare_group_read_local(
                group, begin, capacity, group_epoch, group_refs, group_aborted,
                origin_conn_id, owner_refs, membership, copy_keys, key_bytes);
        if (!capacity || !atomic_ensure_pending()) return nullptr;
        if (copy_keys && key_bytes > UINT32_MAX) return nullptr;
        const uint64_t offsets_bytes = copy_keys
            ? static_cast<uint64_t>(capacity) * sizeof(uint32_t) : 0;
        if (key_bytes > UINT64_MAX - offsets_bytes) return nullptr;
        AtomicEntry* entry = atomic_alloc_entry(capacity, offsets_bytes + key_bytes);
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
        entry->copied_group_keys = copy_keys;
        if (copy_keys) entry->key_len = static_cast<uint32_t>(key_bytes);
        return entry;
    }

    void atomic_discard_group(void*& opaque) {
        if (__builtin_expect(read_local_enabled_, false)) {
            atomic_discard_group_read_local(opaque);
            return;
        }
        auto* entry = static_cast<AtomicEntry*>(opaque);
        if (!entry) return;
        if (entry->linked || entry->count) std::abort();
        read_local_pending_unpublish(*entry);
        atomic_free_entry(entry);
        opaque = nullptr;
    }

    // Object-backed groups have initialized the corresponding ScatterState key position; copied
    // groups own their key bytes in this entry's packed tail. Capacity was preflighted for the
    // whole pass, so the table exchange cannot fail. `value == nullptr` is the physical erase used
    // by DEL/UNLINK.
    KvObj* atomic_install_group(void* opaque, uint64_t hash, Slice key, KvObj* value) {
        auto* entry = static_cast<AtomicEntry*>(opaque);
        if (!entry || entry->count >= entry->capacity) std::abort();
        const uint32_t index = entry->count;
        if (entry->copied_group_keys) {
            uint32_t* offsets = entry->group_key_offsets();
            const uint32_t begin = index ? offsets[index - 1] : 0;
            const uint64_t end = static_cast<uint64_t>(begin) + key.n;
            if (end > entry->key_len) std::abort();
            if (key.n) std::memcpy(entry->group_key_data() + begin, key.p, key.n);
            offsets[index] = static_cast<uint32_t>(end);
        }
        KvObj* old = atomic_exchange_physical(hash, key, value);
        entry->parked()[index] = old;
        entry->count = index + 1;
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
        if (__builtin_expect(read_local_enabled_, false))
            return atomic_prepare_capacity_read_local(additional);
        if (rehashing()) {
            if (!snapshot_active_) {
                // Keep drain ahead of fill: advance at least `additional` keys' worth of slots so
                // table 0 cannot fill before this rehash finishes (mirrors insert()'s 1-key : 8-slot
                // ratio, generalised to a whole install pass).
                uint32_t steps = additional / kRehashSlotsPerOp + 1;
                while (steps-- && rehashing()) rehash_step();
            }
            if (rehashing())
                return static_cast<uint64_t>(live_[0]) + tombs_[0] + additional < cap_[0];
            // fell through: rehash completed, re-evaluate against the merged table below.
        }
        if ((static_cast<uint64_t>(live_[0]) + tombs_[0] + additional) * 100 <
            static_cast<uint64_t>(cap_[0]) * kLoadPct)
            return true;
        if (snapshot_prepared_)
            return static_cast<uint64_t>(live_[0]) + tombs_[0] + additional < cap_[0];
        // Size for the LIVE set plus the pass (tombstones are dropped by the coming rehash). When
        // tombstones dominate and the live set is small this leaves `wanted == cap_[0]`: a same-size
        // rehash that reclaims them, exactly maybe_start_grow()'s policy.
        uint64_t wanted = cap_[0];
        const uint64_t need = static_cast<uint64_t>(live_[0]) + additional;
        while (need * 100 >= wanted * kLoadPct) {
            wanted *= uint64_t{2};
            if (wanted > UINT32_MAX) return false;
        }
        auto* fresh = static_cast<uint64_t*>(
            flatstore_table_calloc(static_cast<size_t>(wanted), sizeof(uint64_t)));
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
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            std::destroy_at(block);
            return ::new (block) KvObj;
#else
            return block;
#endif
        }
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        void* memory = alloc_raw(allocation);
        return memory ? ::new (memory) KvObj : nullptr;
#else
        return alloc_raw(allocation);
#endif
    }

    void atomic_discard_value(KvObj* value) {
        if (!atomic_recycle_value(value)) kvobj_free(value);
    }

    bool atomic_prepare_plain(Slice key, uint64_t hash, uint64_t origin_conn_id,
                              void*& opaque) {
        if (__builtin_expect(read_local_enabled_, false))
            return atomic_prepare_plain_read_local(key, hash, origin_conn_id, opaque);
        opaque = nullptr;
        if (!atomic_ensure_pending()) return false;
        AtomicEntry* entry = atomic_alloc_entry(1, key.n);
        if (!entry) return false;
        entry->capacity = 1;
        entry->key_len = key.n;
        entry->plain_hash = hash;
        entry->origin_conn_id = origin_conn_id;
        entry->membership = atomic_membership_bit(hash);
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
        if (__builtin_expect(read_local_enabled_, false))
            return atomic_prepare_transaction_read_local(
                key, hash, origin_conn_id, group_epoch, group_refs, group_aborted,
                owner_refs, opaque);
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
        if (entry->plain_hash != hash) std::abort();
        KvObj* old = atomic_exchange_physical(hash, key, value);
        entry->parked()[0] = old;
        entry->count = 1;
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

    // Does the table hold a physical slot for this key, ignoring every pending version? This is
    // exactly the question the conditional movers' NX validator asks when the destination carries
    // a foreign undecided record: a physical candidate means the reservation may still materialize
    // and the validation must be re-run once that group decides. It used to be answered with a
    // full for_each walk of both tables on every retry; the probe visits the same two tables.
    bool atomic_has_physical(uint64_t hash, Slice key) const {
        return atomic_find_physical(hash, key) != nullptr;
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

    // Promotion is task-scoped: prove that any key in the owner span still has a record with one
    // membership-filtered list walk, then let the key-agnostic collapse reclaim its whole eligible
    // prefix once. The entry teardown below remains the sole place that closes B+ filter cells.
    template <typename KeyAt>
    bool atomic_promote_keys(uint32_t count, uint64_t membership, uint64_t floor,
                             uint64_t cleanup_cutoff, KeyAt&& key_at) {
        if (!count || !atomic_pending_ || !atomic_pending_->live) return false;
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
            if (!(entry->membership & membership)) continue;
            for (uint32_t i = 0; i < entry->count; i++) {
                const uint64_t hash = atomic_entry_hash(*entry, i);
                if (!(membership & atomic_membership_bit(hash))) continue;
                const Slice key = atomic_entry_key(*entry, i);
                for (uint32_t j = 0; j < count; j++) {
                    const auto [candidate_hash, candidate_key] = key_at(j);
                    if (candidate_hash == hash && candidate_key.key_eq(key))
                        return atomic_collapse(floor, cleanup_cutoff);
                }
            }
        }
        return false;
    }

    template <typename KeyAt>
    bool atomic_promote_keys(uint32_t count, uint64_t floor, uint64_t cleanup_cutoff,
                             KeyAt&& key_at) {
        uint64_t membership = 0;
        for (uint32_t i = 0; i < count; i++)
            membership |= atomic_membership_bit(key_at(i).first);
        return atomic_promote_keys(
            count, membership, floor, cleanup_cutoff, std::forward<KeyAt>(key_at));
    }

    bool atomic_promote_key(uint64_t hash, Slice key, uint64_t floor, uint64_t cleanup_cutoff) {
        return atomic_promote_keys(
            1, atomic_membership_bit(hash), floor, cleanup_cutoff,
            [&](uint32_t) { return std::pair<uint64_t, Slice>{hash, key}; });
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
                if (!atomic_prepare_plain(key, hash, 0, opaque)) throw std::bad_alloc();
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

        // atomic_install_plain selects the freshly installed version as the store's read cut,
        // because its ordinary caller (begin_plain_version) needs the handler that follows to see
        // the clone it just installed. FLUSH is not that caller: it runs under an unbound read
        // context and nothing after it restores the cut, so the store was left resolving every
        // later unbound chain read (a direct RENAME's source lookup, a MULTI child's pop hop) at
        // FLUSH's ticket with no origin connection -- a newer committed group of the same
        // connection was then invisible and RENAME answered "no such key" for a key its own MSET
        // had acknowledged. Preserve whatever context the caller had.
        const uint64_t saved_epoch = atomic_read_epoch_;
        const uint64_t saved_origin = atomic_read_origin_conn_id_;
        const uint64_t ticket = atomic_ticket_fn_(atomic_ticket_ctx_);
        for (PreparedClear& item : prepared) {
            Slice key(item.key.data(), static_cast<uint32_t>(item.key.size()));
            void* opaque = item.entry;
            KvObj* tombstone = nullptr;
            atomic_install_plain(item.hash, key, opaque, tombstone, ticket);
        }
        atomic_read_epoch_ = saved_epoch;
        atomic_read_origin_conn_id_ = saved_origin;
        return true;
    }

private:
    void atomic_discard_group_read_local(void*& opaque) {
        auto* entry = static_cast<AtomicEntry*>(opaque);
        if (!entry) return;
        if (entry->linked || entry->count) std::abort();
        read_local_pending_unpublish(*entry);
        atomic_free_entry_read_local(entry);
        opaque = nullptr;
    }

    void* atomic_prepare_group_read_local(
        ScatterState* group, uint32_t begin, uint32_t capacity,
        std::atomic<uint64_t>* group_epoch, std::atomic<uint32_t>* group_refs,
        std::atomic<bool>* group_aborted, uint64_t origin_conn_id,
        uint32_t* owner_refs, uint64_t membership, bool copy_keys,
        uint64_t key_bytes) {
        if (!capacity || !atomic_ensure_pending()) return nullptr;
        if (copy_keys && key_bytes > UINT32_MAX) return nullptr;
        const uint64_t offsets_bytes = copy_keys
            ? static_cast<uint64_t>(capacity) * sizeof(uint32_t) : 0;
        if (key_bytes > UINT64_MAX - offsets_bytes) return nullptr;
        AtomicEntry* entry = atomic_alloc_entry(capacity, offsets_bytes + key_bytes);
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
        entry->copied_group_keys = copy_keys;
        if (copy_keys) entry->key_len = static_cast<uint32_t>(key_bytes);
        // Publish every key occurrence before returning the prepared handle: no caller can perform
        // a physical exchange until an acquiring read-local probe can see its filter cell.
        read_local_pending_publish(*entry);
        return entry;
    }

    bool atomic_prepare_capacity_read_local(uint32_t additional) {
        if (rehashing()) {
            if (!snapshot_active_) {
                uint32_t steps = additional / kRehashSlotsPerOp + 1;
                while (steps-- && rehashing()) rehash_step_read_local();
            }
            if (rehashing())
                return static_cast<uint64_t>(live_[0]) + tombs_[0] + additional < cap_[0];
        }
        if ((static_cast<uint64_t>(live_[0]) + tombs_[0] + additional) * 100 <
            static_cast<uint64_t>(cap_[0]) * kLoadPct)
            return true;
        if (snapshot_prepared_)
            return static_cast<uint64_t>(live_[0]) + tombs_[0] + additional < cap_[0];
        uint64_t wanted = cap_[0];
        const uint64_t need = static_cast<uint64_t>(live_[0]) + additional;
        while (need * 100 >= wanted * kLoadPct) {
            wanted *= uint64_t{2};
            if (wanted > UINT32_MAX) return false;
        }
        auto* fresh = static_cast<uint64_t*>(
            flatstore_table_calloc(static_cast<size_t>(wanted), sizeof(uint64_t)));
        if (!fresh) return false;
        ReadLocalTableGuard table_change(*this);
        read_local_topology_store(&tab_[1], tab_[0]);
        read_local_topology_store(&cap_[1], cap_[0]);
        read_local_topology_store(&mask_[1], mask_[0]);
        live_[1] = live_[0]; tombs_[1] = tombs_[0];
        read_local_topology_store(&tab_[0], fresh);
        read_local_topology_store(&cap_[0], static_cast<uint32_t>(wanted));
        read_local_topology_store(&mask_[0], static_cast<uint32_t>(wanted) - 1);
        live_[0] = tombs_[0] = 0;
        rehash_pos_ = 0;
        return true;
    }

    bool atomic_prepare_plain_read_local(Slice key, uint64_t hash, uint64_t origin_conn_id,
                                         void*& opaque) {
        opaque = nullptr;
        if (!atomic_ensure_pending()) return false;
        AtomicEntry* entry = atomic_alloc_entry(1, key.n);
        if (!entry) return false;
        entry->capacity = 1;
        entry->key_len = key.n;
        entry->plain_hash = hash;
        entry->origin_conn_id = origin_conn_id;
        entry->membership = atomic_membership_bit(hash);
        if (key.n) std::memcpy(entry->plain_key_data(), key.p, key.n);
        read_local_pending_publish(*entry);
        opaque = entry;
        return true;
    }

    bool atomic_prepare_transaction_read_local(
        Slice key, uint64_t hash, uint64_t origin_conn_id,
        std::atomic<uint64_t>* group_epoch, std::atomic<uint32_t>* group_refs,
        std::atomic<bool>* group_aborted, uint32_t* owner_refs, void*& opaque) {
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
        read_local_pending_publish(*entry);
        opaque = entry;
        return true;
    }

    bool atomic_ensure_pending() {
        if (atomic_pending_) return true;
        atomic_pending_ = new (std::nothrow) AtomicPendingState;
        return atomic_pending_ != nullptr;
    }

    ReadLocalStoreState* read_local_store_state() {
        return atomic_pending_ && atomic_pending_->read_local_extended
            ? reinterpret_cast<ReadLocalStoreState*>(atomic_pending_) : nullptr;
    }

    const ReadLocalStoreState* read_local_store_state() const {
        return atomic_pending_ && atomic_pending_->read_local_extended
            ? reinterpret_cast<const ReadLocalStoreState*>(atomic_pending_) : nullptr;
    }

    ReadLocalStoreState& read_local_store_state_required() {
        ReadLocalStoreState* state = read_local_store_state();
        if (!state) std::abort();
        return *state;
    }

    // Owner hot path under the boot latch. configure_read_local() sets read_local_enabled_ only
    // after proving the extended state exists (it aborts otherwise), the pointer never changes
    // while armed, and ~FlatStore clears the latch before freeing it. A caller that holds the
    // latch therefore needs neither the null test nor the shape byte at sidecar offset 1316 --
    // a line the per-write path otherwise never touches.
    ReadLocalStoreState& read_local_store_state_armed() {
        return *reinterpret_cast<ReadLocalStoreState*>(atomic_pending_);
    }

    const ReadLocalStoreState& read_local_store_state_required() const {
        const ReadLocalStoreState* state = read_local_store_state();
        if (!state) std::abort();
        return *state;
    }

    bool ensure_read_local_store_state() {
        if (read_local_store_state()) return true;
        ReadLocalStoreState* extended = new (std::nothrow) ReadLocalStoreState;
        if (!extended) return false;
        if (atomic_pending_) {
            if (atomic_pending_->live || atomic_pending_->script_intent_count) std::abort();
            extended->atomic = *atomic_pending_;
            delete atomic_pending_;
        }
        extended->atomic.read_local_extended = true;
        atomic_pending_ = &extended->atomic;
        return true;
    }

    AtomicEntry* atomic_alloc_entry(uint32_t capacity, uint64_t tail_bytes) {
        const uint64_t request64 = sizeof(AtomicEntry) +
                                   static_cast<uint64_t>(capacity) * sizeof(KvObj*) + tail_bytes;
        if (request64 > std::numeric_limits<size_t>::max()) return nullptr;
        const size_t request = static_cast<size_t>(request64);
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
        if (entry->foreign_read_unsafe_published) std::abort();
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

    void atomic_free_entry_read_local(AtomicEntry* entry) {
        // Filter closure is deliberately separate: grouped key hashes live in ScatterState and
        // must be consumed before its last owner pin is decremented.
        if (entry && entry->foreign_read_unsafe_published) std::abort();
        atomic_free_entry(entry);
    }

    void atomic_destroy_pending() {
        if (!atomic_pending_) return;
        if (atomic_pending_->live || atomic_pending_->script_intent_count) std::abort();
        const bool read_local_extended = atomic_pending_->read_local_extended;
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
        if (read_local_extended)
            delete reinterpret_cast<ReadLocalStoreState*>(atomic_pending_);
        else
            delete atomic_pending_;
        atomic_pending_ = nullptr;
    }

    uint64_t atomic_entry_hash(const AtomicEntry& entry, uint32_t index) const {
        return entry.plain() ? entry.plain_hash
                             : xshard_atomic_key_hash(entry.group, entry.begin + index);
    }

    Slice atomic_entry_key(const AtomicEntry& entry, uint32_t index) const {
        if (entry.plain()) return Slice(entry.plain_key_data(), entry.key_len);
        if (!entry.copied_group_keys)
            return xshard_atomic_key_slice(entry.group, entry.begin + index);
        if (index >= entry.count) std::abort();
        const uint32_t* offsets = entry.group_key_offsets();
        const uint32_t begin = index ? offsets[index - 1] : 0;
        const uint32_t end = offsets[index];
        if (end < begin || end > entry.key_len) std::abort();
        return Slice(entry.group_key_data() + begin, end - begin);
    }

    bool atomic_key_pending(uint64_t hash, Slice key) const {
        if (!atomic_pending_ || !atomic_pending_->live) return false;
        const uint64_t bit = atomic_membership_bit(hash);
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
            if (!(entry->membership & bit)) continue;
            for (uint32_t i = 0; i < entry->count; i++)
                if (atomic_entry_hash(*entry, i) == hash && atomic_entry_key(*entry, i).key_eq(key))
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
        AtomicEntry* winner_entry = nullptr;
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
            if (owner.group_epoch && epoch == 0 && !own_committed) {
                // Armed-only: the undecided-foreign skip. Under a single-connection workload the
                // entry IS the reader's own earlier write, so landing here means the read failed
                // the own_committed test -- the RYOW hole the seed-19 divergence goes through.
                if (__builtin_expect(atomic_tripwire_enabled(), false))
                    atomic_tripwire_excluded(hash, snapshot, 0,
                                             atomic_read_origin_conn_id_,
                                             owner.origin_conn_id);
                return;
            }
            if (!(epoch <= snapshot || own_committed)) {
                // Armed-only: latch WHY a candidate was invisible. Under a single-connection
                // workload every entry is the reader's own, so any exclusion here is either a
                // read that carried no origin conn (context never set on its path) or a stale
                // one -- the two candidate mechanisms for the seed-19 stale-read divergence.
                if (__builtin_expect(atomic_tripwire_enabled(), false))
                    atomic_tripwire_excluded(hash, snapshot, epoch,
                                             atomic_read_origin_conn_id_,
                                             owner.origin_conn_id);
                return;
            }
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
                winner_entry = &owner;
            }
        };
        for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
            if (!(entry->membership & bit)) continue;
            for (uint32_t i = 0; i < entry->count; i++) {
                if (atomic_entry_hash(*entry, i) != hash || !atomic_entry_key(*entry, i).key_eq(key))
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
        // #77 TRIPWIRE B. A finite read cut answered from a parked predecessor while this shard
        // still held a decided group with an older ticket. That conjunction is the rare resolver /
        // cleanup geometry the preserved seed-19 EXISTS overcount needs diagnosed. It is not a
        // correctness gate: count every occurrence, and let the process-global one-shot retain the
        // first answer/ticket pair. The armed check precedes the extra list walk, so a production
        // server pays none of the diagnostic work and performs no new atomic operation.
        if (__builtin_expect(!from_physical && snapshot != UINT64_MAX &&
                             atomic_tripwire_enabled(), false)) {
            const uint64_t answer_ticket = winner_entry ? atomic_epoch(*winner_entry) : 0;
            uint64_t smaller_ticket = 0;
            for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
                if (!entry->group_epoch ||
                    (entry->group_aborted &&
                     entry->group_aborted->load(std::memory_order_acquire)))
                    continue;
                const uint64_t ticket = atomic_epoch(*entry);
                if (ticket && ticket < snapshot &&
                    (!smaller_ticket || ticket < smaller_ticket))
                    smaller_ticket = ticket;
            }
            if (smaller_ticket)
                atomic_tripwire_chain_smaller(hash, snapshot, answer_ticket, smaller_ticket);
            // Same-key discrimination for the geometry above.  The chain walk filtered entries by
            // membership bit; re-walk WITHOUT that filter and compare full hash+key.  Only an
            // entry strictly newer than the answered version matters -- an older visible version
            // losing is ordinary MVCC.  A qualifying entry the walk never saw (bit clear) is the
            // mask-inconsistency smoking gun; one the walk did see is a ranking defect; a matched
            // foreign group with its ticket still unpublished is the publication-race window.
            for (AtomicEntry* entry = atomic_pending_->head; entry; entry = entry->next) {
                const bool walked = (entry->membership & bit) != 0;
                for (uint32_t i = 0; i < entry->count; i++) {
                    if (atomic_entry_hash(*entry, i) != hash ||
                        !atomic_entry_key(*entry, i).key_eq(key))
                        continue;
                    if (!entry->group_epoch ||
                        (entry->group_aborted &&
                         entry->group_aborted->load(std::memory_order_acquire)))
                        break;
                    uint64_t ticket = atomic_epoch(*entry);
                    if (ticket == 0) {
                        // Bounded re-poll (armed path only): distinguish "ticket not drawn yet"
                        // (will draw above our cut -- correct skip) from "ticket drawn at or
                        // below our cut but its store not yet visible" (a write this cut was
                        // required to see -- the publication-race miss).
                        for (int spin = 0; spin < 4096 && ticket == 0; spin++) {
                            __builtin_ia32_pause();
                            ticket = atomic_epoch(*entry);
                        }
                        if (ticket == 0)
                            atomic_tripwire_samekey(AtomicTripwireSamekey::Undecided);
                        else if (ticket <= snapshot)
                            atomic_tripwire_samekey(AtomicTripwireSamekey::UndecidedLeCut);
                        else
                            atomic_tripwire_samekey(AtomicTripwireSamekey::UndecidedGtCut);
                    } else if (ticket <= snapshot && ticket > answer_ticket) {
                        atomic_tripwire_samekey(walked
                                                    ? AtomicTripwireSamekey::VisibleLost
                                                    : AtomicTripwireSamekey::MaskedOut);
                    }
                    break;
                }
            }
        }
        return AtomicResolved{atomic_live_value(value), true, from_physical};
    }

    KvObj* atomic_live_value(KvObj* value) const {
        if (!value) return nullptr;
        const int64_t deadline = value->expire_at_ms();
        if (deadline >= 0 && deadline <= cached_now_ms_) return nullptr;
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
                            atomic_entry_key(*scan, j).key_eq(key)) { later = true; break; }
                }
                if (!later) fn(hash, key);
            }
        }
    }

    KvObj* atomic_exchange_physical_read_local(uint64_t hash, Slice key,
                                               KvObj* replacement) {
        // The key filter was release-published before this call. Enclose every possible physical
        // exchange or direct insertion in the table sequence so a reader that observed the old
        // empty cell cannot consume an epoch-zero/aborted candidate. The same bracket is what a
        // local MGET compares on a group-free participant.
        ReadLocalTableGuard atomic_change(*this);
        uint32_t insertion_slot = 0;
        bool have_insertion_slot = false;
        bool insertion_reuses_tomb = false;
        auto exchange = [&](int table) -> KvObj* {
            if (!tab_[table]) return nullptr;
            const uint16_t tag = tag_of(hash);
            uint32_t slot = slot_start(table, hash);
            uint32_t first_tomb = 0;
            bool have_first_tomb = false;
            const bool retain_insertion = replacement && table == 0;
            for (uint32_t probes = 0; probes <= cap_[table]; probes++) {
                const uint64_t word = tab_[table][slot];
                if (word == 0) {
                    if (retain_insertion) {
                        insertion_slot = have_first_tomb ? first_tomb : slot;
                        insertion_reuses_tomb = have_first_tomb;
                        have_insertion_slot = true;
                    }
                    return nullptr;
                }
                KvObj* object = ptr_of(word);
                if (retain_insertion && !object && !have_first_tomb) {
                    first_tomb = slot;
                    have_first_tomb = true;
                }
                if (object && tag_of_word(word) == tag && object->key().key_eq(key)) {
                    obj_bytes_ -= kvobj_size(object);
                    if (replacement && table == 0) {
                        (void)track_expire(hash, replacement);
                        read_local_slot_store(&tab_[table][slot], make_word(tag, replacement));
                        obj_bytes_ += kvobj_size(replacement);
                    } else {
                        untrack_expire(hash);
                        read_local_slot_store(&tab_[table][slot], kTombBit);
                        live_[table]--; tombs_[table]++;
                    }
                    return object;
                }
                slot = (slot + 1) & mask_[table];
            }
            return nullptr;
        };
        auto insert_replacement = [&] {
            if (!replacement) return;
            if (!have_insertion_slot) std::abort();
            read_local_slot_store(
                &tab_[0][insertion_slot], make_word(tag_of(hash), replacement));
            if (insertion_reuses_tomb) tombs_[0]--;
            live_[0]++;
            const size_t added_bytes = kvobj_size(replacement);
            obj_bytes_ += added_bytes;
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            settax_stats().accounting_add_calls++;
            settax_stats().accounting_bytes += added_bytes;
#endif
            // The folded probe publishes without insert_into_read_local, so it owes the selected
            // tracker the same registration that path performs. Going through the flag alone would
            // register a PERSISTed key that merely RETAINS its slot, and would hand the sidecar
            // prototype an entry with no deadline payload.
            (void)track_expire(hash, replacement);
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            if (replacement->expire_at_ms() < 0) settax_stats().expire_erases++;
#endif
        };
        if (KvObj* object = exchange(0)) return object;
        if (rehashing()) {
            if (KvObj* object = exchange(1)) {
                insert_replacement();
                return object;
            }
        }
        insert_replacement();
        return nullptr;
    }

    // Swap a key in its existing slot, or retain the first tombstone and terminal empty while
    // proving it absent so a new replacement installs without repeating the current-table probe.
    // Atomic ownership needs the displaced pointer, so insert_into's ordinary retire-in-place path
    // is not usable.
    KvObj* atomic_exchange_physical(uint64_t hash, Slice key, KvObj* replacement) {
        if (__builtin_expect(read_local_enabled_, false))
            return atomic_exchange_physical_read_local(hash, key, replacement);
        uint32_t insertion_slot = 0;
        bool have_insertion_slot = false;
        bool insertion_reuses_tomb = false;
        auto exchange = [&](int table) -> KvObj* {
            if (!tab_[table]) return nullptr;
            const uint16_t tag = tag_of(hash);
            uint32_t slot = slot_start(table, hash);
            uint32_t first_tomb = 0;
            bool have_first_tomb = false;
            const bool retain_insertion = replacement && table == 0;
            for (uint32_t probes = 0; probes <= cap_[table]; probes++) {
                const uint64_t word = tab_[table][slot];
                if (word == 0) {
                    if (retain_insertion) {
                        insertion_slot = have_first_tomb ? first_tomb : slot;
                        insertion_reuses_tomb = have_first_tomb;
                        have_insertion_slot = true;
                    }
                    return nullptr;
                }
                KvObj* object = ptr_of(word);
                if (retain_insertion && !object && !have_first_tomb) {
                    first_tomb = slot;
                    have_first_tomb = true;
                }
                if (object && tag_of_word(word) == tag && object->key().key_eq(key)) {
                    obj_bytes_ -= kvobj_size(object);
                    if (replacement && table == 0) {
                        (void)track_expire(hash, replacement);
                        tab_[table][slot] = make_word(tag, replacement);
                        obj_bytes_ += kvobj_size(replacement);
                    } else {
                        untrack_expire(hash);
                        tab_[table][slot] = kTombBit;
                        live_[table]--; tombs_[table]++;
                    }
                    return object;
                }
                slot = (slot + 1) & mask_[table];
            }
            return nullptr;
        };
        auto insert_replacement = [&] {
            if (!replacement) return;
            if (!have_insertion_slot) std::abort();
            tab_[0][insertion_slot] = make_word(tag_of(hash), replacement);
            if (insertion_reuses_tomb) tombs_[0]--;
            live_[0]++;
            obj_bytes_ += kvobj_size(replacement);
            // Same tracker debt as the read-local twin: physical slot presence is not volatility.
            (void)track_expire(hash, replacement);
        };
        if (KvObj* object = exchange(0)) return object;
        if (rehashing()) {
            if (KvObj* object = exchange(1)) {
                insert_replacement();
                return object;
            }
        }
        insert_replacement();
        return nullptr;
    }

    bool atomic_recycle_value(KvObj* object) {
        if (!object || !atomic_pending_ || static_cast<Type>(object->type) != Type::String)
            return false;
        const Enc encoding = static_cast<Enc>(object->enc);
        if (encoding != Enc::Raw && encoding != Enc::Int) return false;
        if (encoding == Enc::Raw && outstanding_borrows_ && is_borrowed(object->str_data()))
            return false;
        const size_t allocation = kvobj_capacity(object);
        const uint32_t cls = atomic_pool_class(allocation);
        static constexpr size_t kValueCacheBytes = 4 * 1024 * 1024;
        if (cls >= AtomicPendingState::kPoolClasses ||
            allocation < sizeof(AtomicPendingState::FreeValue) ||
            atomic_pending_->cached_value_bytes + allocation > kValueCacheBytes) return false;
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        void* memory = object;
        std::destroy_at(object);
        auto* block = ::new (memory) AtomicPendingState::FreeValue{
            atomic_pending_->free_values[cls], allocation};
#else
        auto* block = reinterpret_cast<AtomicPendingState::FreeValue*>(object);
        block->next = atomic_pending_->free_values[cls];
        block->allocation = allocation;
#endif
        atomic_pending_->free_values[cls] = block;
        atomic_pending_->cached_value_bytes += allocation;
        return true;
    }

    // ---- armed-write block cache (src/store/kv_block_cache.h, NOTES-RECYCLE.md) ---------------
    //
    // The cache itself belongs to the fused OWNER and is reached through the retire sink this store
    // is already bound to; the store contributes only the eligibility a shard can judge (object
    // shape and the borrow registry) and its own live footprint as the admission ceiling. The sink
    // pointer is boot-latched with `read_local_enabled_`, so the armed path needs no null test.

    // Post-grace return. `capacity` is the retire-time kvobj_capacity: the exact class this block
    // was requested at. Returns false when the block was not cached and the caller must free it.
    bool read_local_cache_put(KvObj* object, size_t capacity) {
        KvBlockCache* cache = read_local_store_state_armed().retire_sink.block_cache;
        if (!cache) return false;
        // Only inline String blocks take part. An Extern object owns a second allocation whose
        // class is independent of this one, and a collection's block is not what a SET builds.
        if (static_cast<Type>(object->type) != Type::String) return false;
        const Enc encoding = static_cast<Enc>(object->enc);
        if (encoding != Enc::Raw && encoding != Enc::Int) return false;
        // A borrowed value is still being handed to the kernel by an io thread. destroy_retired_obj
        // owns that retention; the cache must not take the block out from under it.
        if (encoding == Enc::Raw && outstanding_borrows_ && is_borrowed(object->str_data()))
            return false;
        // Retirement already subtracted these bytes from obj_bytes_. The cache is deliberately
        // OUTSIDE logical accounting; a later write accounts the reinitialised block exactly once,
        // exactly as a fresh allocation would. obj_bytes_ is also the per-put ceiling: this shard
        // may not push the owner's cache above its own live object footprint, so an emptied
        // keyspace (FLUSHALL, a delete sweep) admits nothing at all.
        return cache->put(object, capacity, obj_bytes_);
    }

    // Armed write path. `allocation` is already good_size()-rounded.
    void* read_local_cache_take(size_t allocation) {
        KvBlockCache* cache = read_local_store_state_armed().retire_sink.block_cache;
        return cache ? cache->take(allocation) : nullptr;
    }

    // Hand every cached block back to the allocator: maxmemory pressure (return cached memory
    // before evicting user data) and FLUSH. Not an accounting event -- these bytes were never in
    // obj_bytes_. The cache is the owner's, so this also releases blocks its sibling shards
    // contributed; that is correct for both callers, which are whole-server events.
    void read_local_cache_release_all() {
        if (!read_local_enabled_) return;
        if (KvBlockCache* cache = read_local_store_state_armed().retire_sink.block_cache)
            cache->release_all();
    }

    void read_local_cache_trim_on_pressure() { read_local_cache_release_all(); }

    // The ring's auxiliary word is the retire-time kvobj_capacity: the sized-free length the
    // reclaim callback needs, not the accounting footprint (see retire_obj_read_local).
    void retire_detached_obj_read_local(KvObj* object) {
        if (!object) return;
        read_local_store_state_required().retire_sink.retire(
            this, object, kvobj_capacity(object), &FlatStore::read_local_reclaim_atomic_object);
    }

    void retire_detached_obj(KvObj* object) {
        if (!object) return;
        if (atomic_recycle_value(object)) return;
        if (outstanding_borrows_ == 0) { kvobj_free(object); return; }
        const char* ptr = (static_cast<Type>(object->type) == Type::String && !object->is_int())
                              ? object->str_data() : nullptr;
        const uint32_t at = ptr ? borrow_find(ptr) : kNoBorrow;
        if (at != kNoBorrow) {
            borrows_[at].retired = object;
            pending_bytes_ += kvobj_size(object);
            return;
        }
        kvobj_free(object);
    }

    bool atomic_collapse(uint64_t floor, uint64_t cleanup_cutoff) {
        if (__builtin_expect(read_local_enabled_, false))
            return atomic_collapse_read_local(floor, cleanup_cutoff);
        if (!atomic_pending_ || !atomic_pending_->live || snapshot_active_) return false;
        uint64_t occurrences = 0;
        AtomicEntry* first_unselected = atomic_pending_->head;
        while (first_unselected) {
            AtomicEntry* entry = first_unselected;
            const bool aborted = entry->group_epoch && entry->group_aborted &&
                entry->group_aborted->load(std::memory_order_acquire);
            const uint64_t epoch = atomic_epoch(*entry);
            if (!aborted && (!epoch || epoch >= floor || epoch > cleanup_cutoff))
                break;
            if (occurrences > UINT64_MAX - entry->count) return false;
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
                                atomic_entry_key(*later, j).key_eq(key)) {
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
                // Grouped hashes live in ScatterState; close before its last owner pin can drop.
                read_local_pending_unpublish(*entry);
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
        if (occurrences > atomic_seen_keys_.max_size()) return false;
        try {
            atomic_seen_keys_.clear();
            atomic_seen_keys_.reserve(static_cast<size_t>(occurrences));
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
                        if (seen.hash == hash && seen.key.key_eq(key)) {
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
                                    atomic_entry_key(*scan, j).key_eq(key)) {
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
                                if (__builtin_expect(atomic_tripwire_enabled(), false))
                                    atomic_tripwire_collapse_write(hash, physical == nullptr,
                                                                   replacement == nullptr,
                                                                   atomic_epoch(*entry));
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
                // Grouped hashes live in ScatterState; close before its last owner pin can drop.
                read_local_pending_unpublish(*entry);
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
        uint64_t table_cap = 8;
        if (occurrences > UINT64_MAX / uint64_t{2}) return false;
        const uint64_t wanted_slots = occurrences * uint64_t{2};
        while (table_cap < wanted_slots) {
            table_cap *= uint64_t{2};
            if (table_cap > UINT32_MAX) return false;
        }
        if (occurrences > UINT64_MAX - occurrences / uint64_t{2}) return false;
        const uint64_t retire_slots = occurrences + occurrences / uint64_t{2};
        if (occurrences > atomic_collapse_keys_.max_size() ||
            table_cap > atomic_collapse_slots_.max_size() ||
            retire_slots > atomic_collapse_retire_.max_size()) return false;
        try {
            atomic_collapse_keys_.clear();
            atomic_collapse_keys_.reserve(static_cast<size_t>(occurrences));
            atomic_collapse_slots_.assign(static_cast<size_t>(table_cap), AtomicCollapseSlot{});
            atomic_collapse_retire_.clear();
            atomic_collapse_retire_.reserve(static_cast<size_t>(retire_slots));
        } catch (const std::bad_alloc&) {
            return false;
        }
        atomic_pending_->cleanup_slow += occurrences;
        const uint32_t table_mask = static_cast<uint32_t>(table_cap - 1);
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
                if (item.hash == hash && found.key.key_eq(key)) return item.index - 1;
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
                        atomic_entry_key(*scan, i).key_eq(key.key)) {
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
                    if (__builtin_expect(atomic_tripwire_enabled(), false))
                        atomic_tripwire_collapse_write(key.hash, slot == nullptr,
                                                       key.winner == nullptr,
                                                       key.winner_epoch);
                    if (slot) {
                        atomic_gauge_sub(kvobj_size(slot));
                        atomic_collapse_retire_.push_back(slot);
                    }
                    slot = key.winner;
                }
            } else if (key.physical != key.winner) {
                if (__builtin_expect(atomic_tripwire_enabled(), false))
                    atomic_tripwire_collapse_write(key.hash, key.physical == nullptr,
                                                   key.winner == nullptr, key.winner_epoch);
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
            // Grouped hashes live in ScatterState; close before its last owner pin can drop.
            read_local_pending_unpublish(*entry);
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


    bool atomic_collapse_read_local(uint64_t floor, uint64_t cleanup_cutoff) {
        if (!atomic_pending_ || !atomic_pending_->live || snapshot_active_) return false;
        uint64_t occurrences = 0;
        AtomicEntry* first_unselected = atomic_pending_->head;
        while (first_unselected) {
            AtomicEntry* entry = first_unselected;
            const bool aborted = entry->group_epoch && entry->group_aborted &&
                entry->group_aborted->load(std::memory_order_acquire);
            const uint64_t epoch = atomic_epoch(*entry);
            if (!aborted && (!epoch || epoch >= floor || epoch > cleanup_cutoff))
                break;
            if (occurrences > UINT64_MAX - entry->count) return false;
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
                                atomic_entry_key(*later, j).key_eq(key)) {
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
                        retire_detached_obj_read_local(parked);
                        entry->parked()[i] = nullptr;
                    }
                }
                // Grouped hashes live in ScatterState; close before its last owner pin can drop.
                read_local_pending_unpublish(*entry);
                if (entry->group_refs && (!entry->owner_refs || --*entry->owner_refs == 0))
                    entry->group_refs->fetch_sub(1, std::memory_order_release);
                if (atomic_records_freed_) (*atomic_records_freed_)++;
                atomic_pending_->live--;
                atomic_unlink_conn(*entry);
                atomic_free_entry_read_local(entry);
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
        if (occurrences > atomic_seen_keys_.max_size()) return false;
        try {
            atomic_seen_keys_.clear();
            atomic_seen_keys_.reserve(static_cast<size_t>(occurrences));
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
                        if (seen.hash == hash && seen.key.key_eq(key)) {
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
                                    atomic_entry_key(*scan, j).key_eq(key)) {
                                    boundary = scan;
                                    boundary_index = j;
                                    break;
                                }
                            }
                        }
                        if (boundary) {
                            KvObj* loser = boundary->parked()[boundary_index];
                            if (loser) atomic_gauge_sub(kvobj_size(loser));
                            if (loser != replacement) retire_detached_obj_read_local(loser);
                            boundary->parked()[boundary_index] = replacement;
                            if (parked && parked != replacement) {
                                atomic_gauge_sub(kvobj_size(parked));
                                retire_detached_obj_read_local(parked);
                            }
                        } else {
                            KvObj* physical = atomic_find_physical(hash, key);
                            if (physical != replacement) {
                                if (__builtin_expect(atomic_tripwire_enabled(), false))
                                    atomic_tripwire_collapse_write(hash, physical == nullptr,
                                                                   replacement == nullptr,
                                                                   atomic_epoch(*entry));
                                KvObj* loser =
                                    atomic_exchange_physical_read_local(hash, key, replacement);
                                if (loser != physical) std::abort();
                                retire_detached_obj_read_local(loser);
                            }
                            if (parked) {
                                atomic_gauge_sub(kvobj_size(parked));
                                if (parked != replacement)
                                    retire_detached_obj_read_local(parked);
                            }
                        }
                    } else if (parked) {
                        atomic_gauge_sub(kvobj_size(parked));
                        retire_detached_obj_read_local(parked);
                    }
                    entry->parked()[i] = nullptr;
                    promoted++;
                }
                // Grouped hashes live in ScatterState; close before its last owner pin can drop.
                read_local_pending_unpublish(*entry);
                if (entry->group_refs && (!entry->owner_refs || --*entry->owner_refs == 0))
                    entry->group_refs->fetch_sub(1, std::memory_order_release);
                if (atomic_records_freed_) (*atomic_records_freed_)++;
                atomic_pending_->live--;
                atomic_unlink_conn(*entry);
                atomic_free_entry_read_local(entry);
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
        uint64_t table_cap = 8;
        if (occurrences > UINT64_MAX / uint64_t{2}) return false;
        const uint64_t wanted_slots = occurrences * uint64_t{2};
        while (table_cap < wanted_slots) {
            table_cap *= uint64_t{2};
            if (table_cap > UINT32_MAX) return false;
        }
        if (occurrences > UINT64_MAX - occurrences / uint64_t{2}) return false;
        const uint64_t retire_slots = occurrences + occurrences / uint64_t{2};
        if (occurrences > atomic_collapse_keys_.max_size() ||
            table_cap > atomic_collapse_slots_.max_size() ||
            retire_slots > atomic_collapse_retire_.max_size()) return false;
        try {
            atomic_collapse_keys_.clear();
            atomic_collapse_keys_.reserve(static_cast<size_t>(occurrences));
            atomic_collapse_slots_.assign(static_cast<size_t>(table_cap), AtomicCollapseSlot{});
            atomic_collapse_retire_.clear();
            atomic_collapse_retire_.reserve(static_cast<size_t>(retire_slots));
        } catch (const std::bad_alloc&) {
            return false;
        }
        atomic_pending_->cleanup_slow += occurrences;
        const uint32_t table_mask = static_cast<uint32_t>(table_cap - 1);
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
                if (item.hash == hash && found.key.key_eq(key)) return item.index - 1;
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
                        atomic_entry_key(*scan, i).key_eq(key.key)) {
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
                    if (__builtin_expect(atomic_tripwire_enabled(), false))
                        atomic_tripwire_collapse_write(key.hash, slot == nullptr,
                                                       key.winner == nullptr,
                                                       key.winner_epoch);
                    if (slot) {
                        atomic_gauge_sub(kvobj_size(slot));
                        atomic_collapse_retire_.push_back(slot);
                    }
                    slot = key.winner;
                }
            } else if (key.physical != key.winner) {
                if (__builtin_expect(atomic_tripwire_enabled(), false))
                    atomic_tripwire_collapse_write(key.hash, key.physical == nullptr,
                                                   key.winner == nullptr, key.winner_epoch);
                key.physical_loser =
                    atomic_exchange_physical_read_local(key.hash, key.key, key.winner);
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
        for (KvObj* object : atomic_collapse_retire_) {
            retire_detached_obj_read_local(object);
        }

        AtomicEntry* entry = atomic_pending_->head;
        uint32_t freed_entries = 0;
        while (entry != first_unselected) {
            AtomicEntry* next = entry->next;
            // Grouped hashes live in ScatterState; close before its last owner pin can drop.
            read_local_pending_unpublish(*entry);
            if (entry->group_refs && (!entry->owner_refs || --*entry->owner_refs == 0))
                entry->group_refs->fetch_sub(1, std::memory_order_release);
            if (atomic_records_freed_) (*atomic_records_freed_)++;
            freed_entries++;
            atomic_unlink_conn(*entry);
            atomic_free_entry_read_local(entry);
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

    // ---- OWNER-WRITTEN ATOMIC BLOCK. Everything from here to atomic_read_origin_conn_id_ is
    // written by the owner on the atomic write / group-resolution path, and NOTHING here may be
    // read by a foreign probe. atomic_pending_ used to be the second word of this block; it is a
    // foreign read on EVERY read-local probe, so it now lives with the reader's topology words in
    // flatstore.h and FlatStoreLayoutLock keeps it 64 offset-bytes away. See the block comment
    // there before adding a field to either side.
    size_t    atomic_version_bytes_ = 0;
    std::vector<AtomicCollapseKey> atomic_collapse_keys_;
    std::vector<AtomicCollapseSlot> atomic_collapse_slots_;
    std::vector<KvObj*> atomic_collapse_retire_;
    std::vector<AtomicSeenKey> atomic_seen_keys_;
    uint64_t atomic_read_epoch_ = UINT64_MAX;
    // LAST owner-written word of the atomic block; FlatStoreLayoutLock::atomic_owner_last names it.
    uint64_t atomic_read_origin_conn_id_ = 0;

    // ---- SEPARATOR. Bind-once counter bindings and the ticket hook: written exactly once by
    // bind_atomic_state() at boot and read-only afterwards, so a foreign reader sharing a line with
    // any of it never sees an owner invalidation. Eighty bytes of it stand between the atomic block
    // above and the reader block in flatstore.h, which is what makes that separation free. Adding
    // a per-operation counter HERE is the way to silently reintroduce the false sharing: the
    // atomic_separator_first assert below is what stops it.
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
