// atomics_glue.inc — epoch-MVCC integration for cross-shard execution.
//
// This file is textually included by xshard.cc. It deliberately remains part of that one
// translation unit and shares its anonymous namespace; there are no new external symbols or
// build-system inputs.

namespace tomo {
namespace {

WorkError begin_plain_version(Server& server, Shard& shard, Slice key, uint64_t hash,
                              uint64_t snapshot, uint64_t origin_conn_id) {
    FlatStore& store = shard.store();
    const uint64_t cleanup_cutoff = server.atomic_snapshot();
    store.atomic_promote_key(hash, key, server.atomic_read_floor(), cleanup_cutoff);
    const bool had_record = store.atomic_has_record(hash, key);
    // A key reserved by a staged cross-owner script carries no MVCC record of its own, so the
    // record test alone sent this write down the untracked physical path -- it mutated the value
    // in place, drew no ticket, and left the script's APPLY-side validation nothing to detect.
    // The reservation is exactly a demand that this branch be taken.
    const bool script_forced = !had_record && store.atomic_has_script_intent(hash, key);
    if (!had_record && !script_forced) {
        if (snapshot != UINT64_MAX) store.atomic_set_read_context(snapshot, origin_conn_id);
        return WorkError::None;
    }
    if (script_forced) server.note_script_forced_write();

    if (snapshot == UINT64_MAX) snapshot = server.atomic_snapshot();
    store.atomic_set_read_context(snapshot, origin_conn_id);
    KvObj* visible = store.find(hash, key);
    KvObj* clone = nullptr;
    if (visible) {
        ObjectImage image;
        if (!serialize_object(visible, image)) {
            store.atomic_clear_read_epoch();
            return WorkError::Oom;
        }
        const SnapshotTypeHooks& hooks = snapshot_type_hooks(image.type);
        const Slice payload(reinterpret_cast<const char*>(image.payload.data()),
                            static_cast<uint32_t>(image.payload.size()));
        if (!hooks.load) {
            store.atomic_clear_read_epoch();
            return WorkError::Corrupt;
        }
        const SnapshotHookStatus loaded = hooks.load(key, image.encoding, image.expire_at_ms,
                                                     payload, shard.type_limits(), clone);
        if (loaded != SnapshotHookStatus::Ok || !clone) {
            store.atomic_clear_read_epoch();
            return loaded == SnapshotHookStatus::Oom ? WorkError::Oom : WorkError::Corrupt;
        }
    }

    void* prepared = nullptr;
    if (!store.atomic_prepare_plain(key, origin_conn_id, prepared) ||
        !store.atomic_prepare_capacity(1) || !store.atomic_admit(key, clone)) {
        if (prepared) store.atomic_discard_plain(prepared);
        if (clone) kvobj_free(clone);
        store.atomic_clear_read_epoch();
        return WorkError::Oom;
    }
    const uint64_t ticket = server.atomic_commit();
    store.atomic_install_plain(hash, key, prepared, clone, ticket);
    return WorkError::None;
}

ScatterTaskResult execute_atomic_direct_rename(const Task& task, Shard& shard, Op& op,
                                               uint32_t owner_thread_id) {
    ScatterState& state = *task.scatter;
    ShardGroup& group = *group_for(state, shard.id());
    bool source_group = false;
    for (uint32_t i = 0; i < group.count; i++)
        source_group |= state.key_order[group.begin + i] == 0;

    if (!source_group) {
        const uint8_t ready = state.direct_ready.load(std::memory_order_acquire);
        if (ready == 0) return ScatterTaskResult::Retry;
        if (ready == 2 || state.aborted.load(std::memory_order_acquire))
            return ScatterTaskResult::Complete;
        return execute_atomic_apply(task, shard, op, owner_thread_id);
    }

    const KeyRef& source_key = state.keys[0];
    KvObj* source = shard.store().find(source_key.hash, op.arg(source_key.arg));
    if (!source) {
        state.final_reply = FinalReply::NoSuchKey;
        state.aborted.store(true, std::memory_order_release);
        state.direct_ready.store(2, std::memory_order_release);
        return ScatterTaskResult::Complete;
    }

    KeyRef& destination_key = state.keys[1];
    ObjectImage& destination = state.apply[1];
    destination.present = true;
    destination.type = static_cast<Type>(source->type);
    destination.expire_at_ms = source->expire_at_ms();
    KvObj* clone = nullptr;
    if (destination.type == Type::String) {
        clone = source->is_int()
            ? kvobj_new_int(op.arg(destination_key.arg), source->int_value(),
                            destination.expire_at_ms)
            : xshard_make_string(op.arg(destination_key.arg), source->str_value(),
                                 destination.expire_at_ms, false);
    } else {
        if (!serialize_object(source, destination)) {
            group.error = WorkError::Oom;
        } else {
            const SnapshotTypeHooks& hooks = snapshot_type_hooks(destination.type);
            const Slice payload(reinterpret_cast<const char*>(destination.payload.data()),
                                static_cast<uint32_t>(destination.payload.size()));
            if (!hooks.load) {
                group.error = WorkError::Corrupt;
            } else {
                const SnapshotHookStatus loaded = hooks.load(
                    op.arg(destination_key.arg), destination.encoding,
                    destination.expire_at_ms, payload, shard.type_limits(), clone);
                if (loaded != SnapshotHookStatus::Ok || !clone)
                    group.error = loaded == SnapshotHookStatus::Oom
                        ? WorkError::Oom : WorkError::Corrupt;
            }
        }
    }
    if (!clone || group.error != WorkError::None) {
        if (clone) kvobj_free(clone);
        if (group.error == WorkError::None) group.error = WorkError::Oom;
        state.aborted.store(true, std::memory_order_release);
        state.direct_ready.store(2, std::memory_order_release);
        return ScatterTaskResult::Complete;
    }
    destination_key.stable_object = clone;
    destination_key.key_anchor = clone;
    state.direct_ready.store(1, std::memory_order_release);
    return execute_atomic_apply(task, shard, op, owner_thread_id);
}

ScatterTaskResult execute_atomic_apply(const Task& task, Shard& shard, Op& op,
                                       uint32_t owner_thread_id) {
    ScatterState& state = *task.scatter;
    ShardGroup& group = *group_for(state, shard.id());
    FlatStore& store = shard.store();

    // The phase-one destination observation is sufficient for computing an unconditional image.
    // NX decisions are different: their precondition must still hold at the owner immediately
    // before installation. The owner cannot interleave another task between this validation and
    // the physical exchange.
    if (!state.aborted.load(std::memory_order_acquire) &&
        (state.kind == Kind::Renamenx ||
         (state.kind == Kind::Copy && !state.copy_replace))) {
        for (uint32_t i = 0; i < group.count; i++) {
            const uint32_t pos = state.key_order[group.begin + i];
            if (pos != 1) continue;
            const KeyRef& key = state.keys[pos];
            store.atomic_clear_read_epoch();
            if (store.find(key.hash, op.arg(key.arg))) {
                state.conditional_failed.store(true, std::memory_order_release);
                state.aborted.store(true, std::memory_order_release);
                return ScatterTaskResult::Complete;
            }
            if (store.atomic_has_record(key.hash, op.arg(key.arg))) {
                // A foreign undecided value is deliberately invisible to ordinary reads. For an
                // NX validator it is a reservation: let its group decide, then re-run validation.
                // Raw table iteration is reached only on this uncommon collision arm and avoids
                // adding a query to the frozen MVCC engine.
                bool physical_candidate = false;
                store.for_each([&](KvObj* object) {
                    physical_candidate |= object->key() == op.arg(key.arg);
                });
                if (physical_candidate) return ScatterTaskResult::Retry;
            }
        }
    }
    if (state.aborted.load(std::memory_order_acquire))
        return ScatterTaskResult::Complete;

    if (!store.atomic_prepare_capacity(group.count)) {
        group.error = WorkError::InsertFailed;
        state.aborted.store(true, std::memory_order_release);
        return ScatterTaskResult::Complete;
    }

    // Materialize the entire owner pass before admission or any exchange. This makes loader OOM
    // and corrupt images pure validation failures, and leaves the normal install loop infallible.
    for (uint32_t i = 0; i < group.count; i++) {
        const uint32_t pos = state.key_order[group.begin + i];
        KeyRef& key = state.keys[pos];
        const Slice key_slice = op.arg(key.arg);
        const ObjectImage& image = state.apply[pos];
        // RENAME/COPY string sources can hand an already materialized, unlinked destination
        // object across the hop. Its source owner built it while the bytes were hot, avoiding a
        // serialize-vector/load round trip; key_anchor remains the ownership bit until install.
        if (key.stable_object) continue;
        KvObj* value = nullptr;
        if (image.present) {
            const SnapshotTypeHooks& hooks = snapshot_type_hooks(image.type);
            const Slice payload(reinterpret_cast<const char*>(image.payload.data()),
                                static_cast<uint32_t>(image.payload.size()));
            if (!hooks.load) {
                group.error = WorkError::Corrupt;
                state.aborted.store(true, std::memory_order_release);
                return ScatterTaskResult::Complete;
            }
            const SnapshotHookStatus loaded = hooks.load(
                key_slice, image.encoding, image.expire_at_ms, payload,
                shard.type_limits(), value);
            if (loaded != SnapshotHookStatus::Ok || !value) {
                group.error = loaded == SnapshotHookStatus::Oom
                    ? WorkError::Oom : WorkError::Corrupt;
                state.aborted.store(true, std::memory_order_release);
                return ScatterTaskResult::Complete;
            }
        } else {
            // A tombstone has no installed value from which the pending entry can borrow key
            // identity. Keep a tiny arena-owned anchor alive until the record itself retires.
            value = xshard_make_string(key_slice, Slice{});
            if (!value) {
                group.error = WorkError::Oom;
                state.aborted.store(true, std::memory_order_release);
                return ScatterTaskResult::Complete;
            }
        }
        key.stable_object = value;
        key.key_anchor = value;
    }

    uint32_t admitted = 0;
    for (; admitted < group.count; admitted++) {
        const uint32_t pos = state.key_order[group.begin + admitted];
        KeyRef& key = state.keys[pos];
        KvObj* incoming = state.apply[pos].present ? key.stable_object : nullptr;
        if (!store.atomic_admit(op.arg(key.arg), incoming)) break;
    }
    if (admitted != group.count) {
        for (uint32_t i = 0; i < admitted; i++) {
            const uint32_t pos = state.key_order[group.begin + i];
            store.atomic_unadmit(state.apply[pos].present ? state.keys[pos].stable_object : nullptr);
        }
        group.error = WorkError::Maxmemory;
        state.aborted.store(true, std::memory_order_release);
        return ScatterTaskResult::Complete;
    }

    const uint32_t owner_slot = state.server->executor_slot(owner_thread_id);
    if (owner_slot >= state.owner_slots) std::abort();
    auto* owner_refs = &state.owner_record_refs[owner_slot].nodes;
    void* pending_entry = store.atomic_prepare_group(
        &state, group.begin, group.count, publish_epoch(state), &state.record_refs,
        publish_aborted(state), state.origin_conn_id, owner_refs, group.membership);
    if (!pending_entry) {
        for (uint32_t i = 0; i < group.count; i++) {
            const uint32_t pos = state.key_order[group.begin + i];
            store.atomic_unadmit(state.apply[pos].present ? state.keys[pos].stable_object : nullptr);
        }
        group.error = WorkError::Oom;
        state.aborted.store(true, std::memory_order_release);
        return ScatterTaskResult::Complete;
    }
    if (state.aborted.load(std::memory_order_acquire)) {
        for (uint32_t i = 0; i < group.count; i++) {
            const uint32_t pos = state.key_order[group.begin + i];
            store.atomic_unadmit(state.apply[pos].present ? state.keys[pos].stable_object : nullptr);
        }
        store.atomic_discard_group(pending_entry);
        return ScatterTaskResult::Complete;
    }

    size_t parked_bytes = 0;
    size_t installed_bytes = 0;
    for (uint32_t i = 0; i < group.count; i++) {
        const uint32_t pos = state.key_order[group.begin + i];
        KeyRef& key = state.keys[pos];
        KvObj* value = state.apply[pos].present ? key.stable_object : nullptr;
        KvObj* old = store.atomic_install_group(
            pending_entry, key.hash, op.arg(key.arg), value);
        if (old) parked_bytes += kvobj_size(old);
        if (value) {
            installed_bytes += kvobj_size(value);
            key.key_anchor = nullptr; // the store owns the installed candidate
        }
    }
    store.atomic_finish_group_install(parked_bytes, installed_bytes);
    store.atomic_publish_group(pending_entry);
    if (*owner_refs == 0)
        state.record_refs.fetch_add(1, std::memory_order_release);
    (*owner_refs)++;
    return ScatterTaskResult::Complete;
}

}  // namespace

uint64_t xshard_atomic_key_hash(const ScatterState* state, uint32_t ordered_index) {
    if (!state || ordered_index >= state->key_count) std::abort();
    return state->keys[state->key_order[ordered_index]].hash;
}

Slice xshard_atomic_key_slice(const ScatterState* state, uint32_t ordered_index) {
    if (!state || ordered_index >= state->key_count) std::abort();
    const KeyRef& key = state->keys[state->key_order[ordered_index]];
    if (!key.stable_object) std::abort();
    return key.stable_object->key();
}

namespace {

// THE KEY THIS OP WAS ROUTED ON, which is not always argv[1].  Op::key() is argv[1] by
// construction; the registry has carried the real key position since OBJECT ENCODING arrived, and
// the IO router already hashes arg(spec->first_key) into op.hash.  The program-order and MVCC
// bookkeeping below must name the SAME key the router hashed.  Pairing op.hash with argv[1] made a
// container subcommand record its hazard under the literal "ENCODING"/"USAGE": no record ever
// matched, so a younger OBJECT ENCODING overtook an older cross-shard group of its own connection
// and answered from before it -- nil for a key its own MSET had already created, and the old
// encoding for a key its own DEL had already removed.  For every ordinary single-key row
// (first_key == 1) this is argv[1] and nothing changes.
inline Slice op_routed_key(const Op& op) {
    const int16_t first = op.spec->first_key;
    if (first >= 1 && static_cast<uint32_t>(first) < op.argc())
        return op.arg(static_cast<uint32_t>(first));
    return op.key();
}

template <typename Fn>
void for_each_touched_key(Op& op, Fn&& fn) {
    if (op.spec->flags & CmdFlags::ScriptRoute) {
        uint32_t first = 0, count = 0;
        if (!command_script_key_range(op, first, count)) return;
        for (uint32_t i = 0; i < count; i++) {
            const Slice key = op.arg(first + i);
            fn(FlatStore::hash_key(key), key);
        }
        return;
    }
    Kind kind;
    if (!classify(op, kind)) {
        if (op.argc() > 1) fn(op.hash, op_routed_key(op));
        return;
    }
    uint32_t sinter_count = 0;
    uint32_t mpop_count = 0;
    bool sort_store = false;
    uint32_t sort_store_arg = 0;
    uint32_t zset_count = 0;
    GeoRoute geo_route;
    if (kind == Kind::Sintercard) {
        uint64_t parsed = 0;
        if (parse_u64(op.arg(1), parsed) && parsed <= UINT32_MAX)
            sinter_count = static_cast<uint32_t>(parsed);
    } else if (kind == Kind::Lmpop || kind == Kind::Zmpop) {
        int64_t parsed = 0;
        if (parse_i64(op.arg(1), parsed) && parsed > 0 && parsed <= UINT32_MAX)
            mpop_count = static_cast<uint32_t>(parsed);
    } else if (kind == Kind::Sort) {
        SortOptions options;
        if (parse_sort_options(op, options)) {
            sort_store = options.store;
            sort_store_arg = options.store_arg;
        }
    } else if (is_any_zsetop(kind)) {
        uint64_t limit = 0;
        std::vector<double> weights;
        ZAggregate aggregate = ZAggregate::Sum;
        bool withscores = false;
        if (!parse_zset_operation(op, kind, zset_count, weights, aggregate, withscores, limit))
            return;
    } else if (is_geo_kind(kind)) {
        if (!geo_prepare_route(op, geo_route)) return;
    }
    const uint32_t count = key_count_for(kind, op, sinter_count, mpop_count, sort_store, 0,
                                         zset_count, geo_route.store);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t arg = key_arg_for(kind, i, sinter_count, sort_store_arg, 0,
                                         geo_route.store, geo_route.source_arg,
                                         geo_route.destination_arg);
        const Slice key = op.arg(arg);
        fn(FlatStore::hash_key(key), key);
    }
}

uint64_t snapshot_exclusive_floor(uint64_t snapshot) {
    return snapshot == UINT64_MAX ? UINT64_MAX : snapshot + 1;
}

struct SnapshotFloor {
    uint64_t floor = UINT64_MAX;
    uint32_t refs = 0;
};

SnapshotFloor active_snapshot_floor(const std::vector<SnapshotCut*>& unresolved_snapshots) {
    SnapshotFloor result;
    for (SnapshotCut* active : unresolved_snapshots) {
        const uint64_t cut = snapshot_exclusive_floor(active->snapshot);
        if (cut < result.floor) {
            result.floor = cut;
            result.refs = 1;
        } else if (cut == result.floor) {
            result.refs++;
        }
    }
    return result;
}

}  // namespace

bool ScatterArenaPool::register_snapshot(Server& server, SnapshotCut* state, uint32_t owner_io) {
    try { unresolved_snapshots_.push_back(state); }
    catch (const std::bad_alloc&) { return false; }
    state->snapshot_slot = static_cast<uint32_t>(unresolved_snapshots_.size() - 1);
    state->snapshot_registered = true;
    uint64_t cut = snapshot_exclusive_floor(state->snapshot);
    if (cut < unresolved_snapshot_floor_) {
        unresolved_snapshot_floor_ = cut;
        unresolved_floor_refs_ = 1;
    } else if (cut == unresolved_snapshot_floor_) {
        unresolved_floor_refs_++;
    }
    if (unresolved_snapshot_floor_ != published_snapshot_floor_) {
        published_snapshot_floor_ = unresolved_snapshot_floor_;
        server.publish_atomic_read_floor(owner_io, unresolved_snapshot_floor_);
    }

    // Publish the successor of each inclusive read cut. A version visible at the oldest snapshot
    // is then strictly below the floor and may become the sole physical representation without
    // violating the freeing invariant. Publishing the cut is a hazard-pointer handshake: cleanup
    // may have sampled the old floor
    // between our first commit_seq load and the store above. If a commit raced through that
    // window, adopt the confirmed (newer) cut; cleanup can only have retained its winner. If the
    // sequence is unchanged, promotion of that winner is also a valid representation of our cut.
    const uint64_t confirmed = server.atomic_snapshot();
    if (confirmed != state->snapshot) {
        const uint64_t old_cut = cut;
        state->snapshot = confirmed;
        cut = snapshot_exclusive_floor(confirmed);
        if (old_cut == unresolved_snapshot_floor_) {
            if (!unresolved_floor_refs_) std::abort();
            unresolved_floor_refs_--;
        }
        if (cut < unresolved_snapshot_floor_) {
            unresolved_snapshot_floor_ = cut;
            unresolved_floor_refs_ = 1;
        } else if (cut == unresolved_snapshot_floor_) {
            unresolved_floor_refs_++;
        }
        if (!unresolved_floor_refs_) {
            const SnapshotFloor exact = active_snapshot_floor(unresolved_snapshots_);
            unresolved_snapshot_floor_ = exact.floor;
            unresolved_floor_refs_ = exact.refs;
        }
        if (unresolved_snapshot_floor_ != published_snapshot_floor_) {
            published_snapshot_floor_ = unresolved_snapshot_floor_;
            server.publish_atomic_read_floor(owner_io, unresolved_snapshot_floor_);
        }
    }
    return true;
}

void ScatterArenaPool::unregister_snapshot(Server& server, SnapshotCut* state, uint32_t owner_io) {
    if (!state->snapshot_registered) return;
    const uint32_t slot = state->snapshot_slot;
    if (slot != UINT32_MAX) {
        if (slot >= unresolved_snapshots_.size() || unresolved_snapshots_[slot] != state)
            std::abort();
        const uint64_t cut = snapshot_exclusive_floor(state->snapshot);
        if (cut == unresolved_snapshot_floor_) {
            if (!unresolved_floor_refs_) std::abort();
            unresolved_floor_refs_--;
        }
        SnapshotCut* moved = unresolved_snapshots_.back();
        unresolved_snapshots_[slot] = moved;
        moved->snapshot_slot = slot;
        unresolved_snapshots_.pop_back();
        state->snapshot_slot = UINT32_MAX;
    }
    state->snapshot_registered = false;
    if (!unresolved_floor_refs_) {
        const SnapshotFloor exact = active_snapshot_floor(unresolved_snapshots_);
        unresolved_snapshot_floor_ = exact.floor;
        unresolved_floor_refs_ = exact.refs;
    }
    if (unresolved_snapshot_floor_ != published_snapshot_floor_) {
        published_snapshot_floor_ = unresolved_snapshot_floor_;
        server.publish_atomic_read_floor(owner_io, unresolved_snapshot_floor_);
    }
}

uint32_t ScatterArenaPool::refresh_snapshot_floor(Server& server, uint32_t owner_io) {
    const uint64_t completed = server.atomic_snapshot_completions(owner_io);
    if (completed == observed_snapshot_completions_) return 0;
    observed_snapshot_completions_ = completed;
    for (size_t i = 0; i < unresolved_snapshots_.size();) {
        SnapshotCut* state = unresolved_snapshots_[i];
        if (!state->snapshot_complete.load(std::memory_order_acquire)) {
            i++;
            continue;
        }
        const uint64_t cut = snapshot_exclusive_floor(state->snapshot);
        if (cut == unresolved_snapshot_floor_) {
            if (!unresolved_floor_refs_) std::abort();
            unresolved_floor_refs_--;
        }
        SnapshotCut* moved = unresolved_snapshots_.back();
        unresolved_snapshots_[i] = moved;
        moved->snapshot_slot = static_cast<uint32_t>(i);
        unresolved_snapshots_.pop_back();
        state->snapshot_slot = UINT32_MAX;
    }
    if (!unresolved_floor_refs_) {
        const SnapshotFloor exact = active_snapshot_floor(unresolved_snapshots_);
        unresolved_snapshot_floor_ = exact.floor;
        unresolved_floor_refs_ = exact.refs;
    }
    if (unresolved_snapshot_floor_ != published_snapshot_floor_) {
        published_snapshot_floor_ = unresolved_snapshot_floor_;
        server.publish_atomic_read_floor(owner_io, unresolved_snapshot_floor_);
    }
    return 1;
}

bool ScatterArenaPool::can_register_snapshot(uint32_t limit) const {
    return limit != 0 && unresolved_snapshots_.size() < limit;
}

void ScatterArenaPool::defer_destroy(ScatterState* state) {
    try { deferred_destroy_.push_back(state); }
    catch (const std::bad_alloc&) {
        // The state must remain address-stable while pending entries name its epoch. Losing this
        // bookkeeping leaks one arena at OOM but never turns a retained pointer into a UAF.
    }
}

uint32_t ScatterArenaPool::reap_deferred() {
    uint32_t reaped = 0;
    for (size_t i = 0; i < deferred_destroy_.size();) {
        ScatterState* state = deferred_destroy_[i];
        if (state->record_refs.load(std::memory_order_acquire) != 0 ||
            state->watch_refs.load(std::memory_order_acquire) != 0) { i++; continue; }
        const bool pooled = state->pooled;
        free_state_key_anchors(*state);
        state->~ScatterState();
        release(state, pooled);
        deferred_destroy_[i] = deferred_destroy_.back();
        deferred_destroy_.pop_back();
        reaped++;
    }
    return reaped;
}

FlatStore::SnapshotWriteResult xshard_local_snapshot_prepare(Op& op, Shard& shard) {
    if (op.spec->flags & CmdFlags::ScriptRoute) {
        uint32_t first = 0, count = 0;
        if (!command_script_key_range(op, first, count))
            return FlatStore::SnapshotWriteResult::Error;
        while (op.zc_len < count) {
            const Slice key = op.arg(first + op.zc_len);
            const auto result = shard.store().snapshot_prepare_write(
                FlatStore::hash_key(key), key);
            if (result != FlatStore::SnapshotWriteResult::Ready) return result;
            op.zc_len++;
        }
        return FlatStore::SnapshotWriteResult::Ready;
    }
    Kind kind;
    if (!classify(op, kind)) return FlatStore::SnapshotWriteResult::Ready;
    uint32_t count = 0;
    uint32_t mpop_count = 0;
    bool pop_edge = false;
    uint64_t pop_count = 1;
    SortOptions sort_options;
    GeoRoute geo_route;
    if (kind == Kind::Lmpop || kind == Kind::Zmpop) {
        if (!parse_mpop(op, kind, mpop_count, pop_edge, pop_count))
            return FlatStore::SnapshotWriteResult::Error;
    }
    if (kind == Kind::Sort) {
        if (!parse_sort_options(op, sort_options))
            return FlatStore::SnapshotWriteResult::Error;
        if (!sort_options.store) return FlatStore::SnapshotWriteResult::Ready;
    }
    if (is_geo_kind(kind)) {
        if (!geo_prepare_route(op, geo_route)) return FlatStore::SnapshotWriteResult::Error;
        if (!geo_route.store) return FlatStore::SnapshotWriteResult::Ready;
    }
    auto arg_at = [&](uint32_t i) -> uint32_t {
        if (kind == Kind::Mset || kind == Kind::Msetnx) return 1 + i * 2;
        if (kind == Kind::Bitop) return 2;
        if (kind == Kind::Copy) return 2;
        if (is_store_setop(kind) || is_store_zsetop(kind)) return 1;
        if (is_geo_kind(kind)) return geo_route.destination_arg;
        if (kind == Kind::Lmpop || kind == Kind::Zmpop) return 2 + i;
        if (kind == Kind::Zrangestore) return 1;
        if (kind == Kind::Sort) return sort_options.store_arg;
        return 1 + i;
    };
    if (kind == Kind::Mset || kind == Kind::Msetnx) count = (op.argc() - 1) / 2;
    else if (kind == Kind::Del || kind == Kind::Unlink) count = op.argc() - 1;
    else if (kind == Kind::Copy || is_store_setop(kind) || is_store_zsetop(kind) ||
             kind == Kind::Bitop) count = 1;
    else if (is_geo_kind(kind)) count = 1;
    else if (kind == Kind::Lmpop || kind == Kind::Zmpop) count = mpop_count;
    else if (kind == Kind::Zrangestore || kind == Kind::Sort) count = 1;
    else if (kind == Kind::Rename || kind == Kind::Renamenx || kind == Kind::Smove ||
             kind == Kind::Lmove || kind == Kind::Rpoplpush) count = 2;
    else return FlatStore::SnapshotWriteResult::Ready;

    while (op.zc_len < count) {
        const uint32_t arg = arg_at(op.zc_len);
        const Slice key = op.arg(arg);
        const uint64_t hash = arg == 1 ? op.hash : FlatStore::hash_key(key);
        const auto result = shard.store().snapshot_prepare_write(hash, key);
        if (result != FlatStore::SnapshotWriteResult::Ready) return result;
        op.zc_len++;
    }
    return FlatStore::SnapshotWriteResult::Ready;
}

FlatStore::SnapshotWriteResult xshard_snapshot_prepare(const Task& task, Shard& shard) {
    ScatterState& state = *task.scatter;
    Op& op = task.client->rob().at(task.op_id);
    auto prepare_group = [&](Shard& owner, ShardGroup& group) {
        if (!owner.store().snapshot_active()) return FlatStore::SnapshotWriteResult::Ready;
        if (state.phase == 1 &&
            !(state.kind == Kind::Mset || state.kind == Kind::Del || state.kind == Kind::Unlink ||
              (state.atomic_write && state.kind == Kind::Msetnx))) {
            if (state.kind == Kind::AllShards && (op.spec->flags & CmdFlags::Write)) {
                try {
                    if (!group.aux) group.aux = new GroupAux;
                    if (!group.aux->flush_keys_built) {
                        owner.store().for_each([&](KvObj* object) {
                            group.aux->flush_keys.emplace_back(object->key().p, object->key().n);
                        });
                        group.aux->flush_keys_built = true;
                    }
                    while (group.snapshot_pos < group.aux->flush_keys.size()) {
                        const std::string& key = group.aux->flush_keys[group.snapshot_pos];
                        const Slice slice(key.data(), static_cast<uint32_t>(key.size()));
                        const auto result = owner.store().snapshot_prepare_write(
                            FlatStore::hash_key(slice), slice);
                        if (result != FlatStore::SnapshotWriteResult::Ready) return result;
                        group.snapshot_pos++;
                    }
                } catch (const std::bad_alloc&) {
                    return FlatStore::SnapshotWriteResult::Error;
                }
            }
            return FlatStore::SnapshotWriteResult::Ready;
        }
        while (group.snapshot_pos < group.count) {
            const uint32_t pos = state.key_order[group.begin + group.snapshot_pos];
            const KeyRef& key = state.keys[pos];
            const auto result = owner.store().snapshot_prepare_write(key.hash, op.arg(key.arg));
            if (result != FlatStore::SnapshotWriteResult::Ready) return result;
            group.snapshot_pos++;
        }
        return FlatStore::SnapshotWriteResult::Ready;
    };

    ShardGroup* group = group_for(state, shard.id());
    return group ? prepare_group(shard, *group) : FlatStore::SnapshotWriteResult::Ready;
}

bool xshard_task_should_defer(Server& server, Shard& shard, const Task& task, Op& op) {
    (void)server;
    if (!task.client) return false;
    if (task.scatter) {
        ScatterState& state = *task.scatter;
        // TEST HOOK (DEBUG ATOMIC-FANOUT-DEFER): park every fragment but the lead one until the
        // deadline. A park, not a stall -- the owner returns to its queue, so the transaction this
        // widened window exists to race is free to execute and publish its ticket in the middle of
        // the read. Off, this is one load of an arena word the task already touched.
        if (__builtin_expect(state.debug_fanout_deadline != 0, false) &&
            shard.id() != state.debug_fanout_lead &&
            now_ns() < state.debug_fanout_deadline)
            return true;
        // TEST HOOK (DEBUG SCRIPT-STAGE-DEFER): same shape for the cross-owner script GATHER wave.
        // The coordinator's own shard reads immediately; every other declared key's owner is parked
        // until the deadline, which is the interval a competing plain write has to land in.
        if (__builtin_expect(state.debug_script_defer_deadline != 0, false) &&
            state.script_phase == ScriptPhase::Read &&
            shard.id() != state.script_coordinator_shard &&
            now_ns() < state.debug_script_defer_deadline)
            return true;
        if (state.atomic_direct &&
            (state.direct_ready.load(std::memory_order_acquire) == 0 ||
             __builtin_expect(state.debug_direct_defer != 0, false))) {
            ShardGroup* group = group_for(state, shard.id());
            bool source_group = false;
            for (uint32_t i = 0; group && i < group->count; i++)
                source_group |= state.key_order[group->begin + i] == 0;
            if (!source_group) {
                // The hook only lengthens an existing park; it never invents one, and it never
                // touches the source hop, so the group still completes on its own.
                if (state.debug_direct_defer) state.debug_direct_defer--;
                return true;
            }
        }
    }
    if (!op.atomic_hazard()) return false;
    const uint64_t origin_conn_id = task.client->id();
    if (task.scatter) {
        ScatterState& state = *task.scatter;
        FlatStore& store = shard.store();
        if (!store.atomic_has_records()) return false;
        ShardGroup* group = group_for(state, shard.id());
        if (sort_deref_gathering(state)) {
            if (!group) return false;
            SortDerefState& sort = *state.sort_deref;
            return store.atomic_group_has_own_undecided(
                &state, group->count, origin_conn_id, group->membership, [&](uint32_t i) {
                    const SortDerefRequest& request =
                        sort.requests[sort.key_order[group->begin + i]];
                    return std::pair<uint64_t, Slice>{request.hash, sort_deref_key(request)};
                });
        }
        // KEYS/DBSIZE/all-shards commands have a zero-key group for every shard: logically they
        // touch the whole owner, so any own undecided node is their precise program-order hazard.
        if (!group || state.key_count == 0)
            return store.atomic_has_any_own_undecided(origin_conn_id, &state);
        return store.atomic_group_has_own_undecided(
            &state, group->count, origin_conn_id, group->membership, [&](uint32_t i) {
                const uint32_t pos = state.key_order[group->begin + i];
                const KeyRef& key = state.keys[pos];
                return std::pair<uint64_t, Slice>{key.hash, op.arg(key.arg)};
            });
    }

    FlatStore& store = shard.store();
    if (!store.atomic_has_records()) return false;
    bool own_undecided = false;
    auto check_own = [&](uint64_t hash, Slice key) {
        own_undecided |= store.atomic_has_own_undecided(hash, key, origin_conn_id);
    };
    if (op.local_xshard()) for_each_touched_key(op, check_own);
    else check_own(op.hash, op_routed_key(op));
    if (own_undecided) return true;

    // Committed entries never park a localfast write. xshard_plain_prepare() materializes one
    // direct plain version for each surviving touched key and the original same-owner handler
    // mutates those versions inline. Only an own undecided predecessor above is a true hazard.
    return false;
}

namespace {

template <typename Fn>
bool for_each_task_key(const Task& task, Op& op, int32_t shard_id, Fn&& fn) {
    if (task.scatter) {
        ScatterState& state = *task.scatter;
        // WHOLE-OWNER COMMANDS NAME NO KEYS, AND THAT IS THE POINT. KEYS, exact DBSIZE and
        // FLUSHDB/FLUSHALL carry a zero-key ShardGroup for EVERY shard, so group_for() succeeds
        // and the old `!group` test below never produced the whole-owner answer for them. They
        // therefore reported "touches nothing", xshard_tasks_share_key() found no overlap, and
        // has_atomic_deferred_predecessor() let a younger walker run past an older
        // same-connection group still parked in atomic_deferred_ (a direct RENAME's destination
        // task waiting on direct_ready is the common case). The walker's bounded cursor then
        // swept the destination slot before the install landed and the key was silently absent
        // from a listing whose own RENAME had already answered +OK. A zero-key group logically
        // touches every key this owner holds, so it overlaps every other task on this shard.
        //
        // Scoped to THIS task's own shard on purpose. A whole-owner command carries a group for
        // every shard, so answering "touches everything" for a shard the parked task is not
        // executing on would make one parked walker hold every younger task on every other shard
        // its executor owns -- which livelocked the xshard differ at ~1/20 (a walker parked on an
        // undecided entry stalled the very groups whose completion would decide it).
        if (state.key_count == 0)
            return (task.shard >= 0 ? task.shard : op.shard) == shard_id;
        ShardGroup* group = group_for(state, shard_id);
        if (!group) return false;
        if (sort_deref_gathering(state)) {
            SortDerefState& sort = *state.sort_deref;
            for (uint32_t i = 0; i < group->count; i++) {
                const SortDerefRequest& request =
                    sort.requests[sort.key_order[group->begin + i]];
                fn(request.hash, sort_deref_key(request));
            }
            return false;
        }
        for (uint32_t i = 0; i < group->count; i++) {
            const uint32_t pos = state.key_order[group->begin + i];
            const KeyRef& key = state.keys[pos];
            fn(key.hash, op.arg(key.arg));
        }
        return false;
    }
    if (op.local_xshard()) {
        for_each_touched_key(op, std::forward<Fn>(fn));
    } else {
        fn(op.hash, op_routed_key(op));
    }
    return false;
}

}  // namespace

bool xshard_task_is_whole_owner(const Task& task) {
    return task.scatter != nullptr && task.scatter->key_count == 0;
}

bool xshard_tasks_share_key(const Task& older, Op& older_op,
                            const Task& younger, Op& younger_op, int32_t shard_id) {
    if (!older.client || older.client != younger.client || older.op_id >= younger.op_id)
        return false;
    bool overlap = false;
    const bool older_all = for_each_task_key(older, older_op, shard_id,
        [&](uint64_t older_hash, Slice older_key) {
            const bool younger_all = for_each_task_key(younger, younger_op, shard_id,
                [&](uint64_t younger_hash, Slice younger_key) {
                    overlap |= older_hash == younger_hash && older_key == younger_key;
                });
            overlap |= younger_all;
        });
    return overlap || older_all;
}

// The cut this command reads at. IO pinned it in the connection's program order (Op::read_cut_lo);
// `now` is a sequence value this owner has already loaded, which is all the widening needs. An
// unstamped op -- a write, or any op parsed while the engine had no atomic tracking at all -- keeps
// the newest world, exactly as before.
uint64_t plain_read_cut(Server& server, const Op& op, uint64_t now) {
    if (!op.has_read_cut()) return now;
    const uint64_t cut = op.read_cut(now);
    if (cut != now) server.note_atomic_read_cut_held();
    return cut;
}

bool xshard_plain_prepare(Server& server, Shard& shard, Op& op, uint64_t origin_conn_id) {
    FlatStore& store = shard.store();
    if (!store.atomic_has_records()) return true;
    const uint32_t read_stall = server.debug_atomic_read_delay();
    if (__builtin_expect(read_stall != 0, false) &&
        !(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)))
        Server::debug_stall_us(read_stall);
    const uint64_t cleanup_cutoff = server.atomic_snapshot();
    const uint64_t floor = server.atomic_read_floor();
    if (op.local_xshard()) {
        struct RecordedKey { uint64_t hash; Slice key; };
        std::vector<RecordedKey> recorded;
        bool allocation_failed = false;
        for_each_touched_key(op, [&](uint64_t hash, Slice key) {
            if (allocation_failed) return;
            store.atomic_promote_key(hash, key, floor, cleanup_cutoff);
            if (!store.atomic_needs_version(hash, key)) return;
            if (!store.atomic_has_record(hash, key) &&
                (op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)))
                server.note_script_forced_write();
            for (const RecordedKey& prior : recorded)
                if (prior.hash == hash && prior.key == key) return;
            try { recorded.push_back(RecordedKey{hash, key}); }
            catch (const std::bad_alloc&) { allocation_failed = true; }
        });
        if (allocation_failed) { set_oom(op); return false; }
        if (recorded.empty()) return true;

        if (!(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) {
            store.atomic_set_read_context(
                plain_read_cut(server, op, cleanup_cutoff), origin_conn_id);
            return true;
        }
        const uint64_t snapshot = server.atomic_snapshot();

        struct PreparedLocal {
            RecordedKey key;
            void* entry = nullptr;
            KvObj* clone = nullptr;
            bool admitted = false;
        };
        std::vector<PreparedLocal> prepared;
        WorkError failure = WorkError::None;
        try { prepared.reserve(recorded.size()); }
        catch (const std::bad_alloc&) { failure = WorkError::Oom; }
        store.atomic_set_read_context(snapshot, origin_conn_id);
        for (const RecordedKey& item : recorded) {
            if (failure != WorkError::None) break;
            PreparedLocal next{item};
            KvObj* visible = store.find(item.hash, item.key);
            if (visible) {
                ObjectImage image;
                if (!serialize_object(visible, image)) {
                    failure = WorkError::Oom;
                    break;
                }
                const SnapshotTypeHooks& hooks = snapshot_type_hooks(image.type);
                const Slice payload(reinterpret_cast<const char*>(image.payload.data()),
                                    static_cast<uint32_t>(image.payload.size()));
                if (!hooks.load) {
                    failure = WorkError::Corrupt;
                    break;
                }
                const SnapshotHookStatus loaded = hooks.load(
                    item.key, image.encoding, image.expire_at_ms, payload,
                    shard.type_limits(), next.clone);
                if (loaded != SnapshotHookStatus::Ok || !next.clone) {
                    failure = loaded == SnapshotHookStatus::Oom
                        ? WorkError::Oom : WorkError::Corrupt;
                    break;
                }
            }
            if (!store.atomic_prepare_plain(item.key, origin_conn_id, next.entry)) {
                if (next.clone) kvobj_free(next.clone);
                failure = WorkError::Oom;
                break;
            }
            try { prepared.push_back(next); }
            catch (const std::bad_alloc&) {
                store.atomic_discard_plain(next.entry);
                if (next.clone) kvobj_free(next.clone);
                failure = WorkError::Oom;
                break;
            }
        }
        if (failure == WorkError::None &&
            !store.atomic_prepare_capacity(static_cast<uint32_t>(prepared.size())))
            failure = WorkError::InsertFailed;
        for (PreparedLocal& item : prepared) {
            if (failure != WorkError::None) break;
            if (!store.atomic_admit(item.key.key, item.clone)) {
                failure = WorkError::Oom;
                break;
            }
            item.admitted = true;
        }
        if (failure != WorkError::None) {
            for (PreparedLocal& item : prepared) {
                store.atomic_discard_plain(item.entry);
                if (item.admitted) store.atomic_unadmit(item.clone);
                if (item.clone) kvobj_free(item.clone);
            }
            store.atomic_clear_read_epoch();
            reply_work_error(op, failure);
            return false;
        }

        // The ticket is drawn once, before any physical install. No other task can enter this shard
        // until the handler finishes, so a snapshot that observes the ticket can only execute after
        // every key is installed; an older one resolves every recorded predecessor at the same cut.
        const uint64_t ticket = server.atomic_commit();
        for (PreparedLocal& item : prepared)
            store.atomic_install_plain(
                item.key.hash, item.key.key, item.entry, item.clone, ticket);
        return true;
    }
    // Two fixes meet here and both are needed: the routed key (MEMORY/OBJECT do not route by
    // argv[1]) and atomic_needs_version as the single decision point (a key reserved by a script
    // carries no record yet, so has_record would bail out and the write would take the untracked
    // physical path).
    const Slice routed = op_routed_key(op);
    store.atomic_promote_key(op.hash, routed, floor, cleanup_cutoff);
    if (!store.atomic_needs_version(op.hash, routed)) return true;
    if (!(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) {
        store.atomic_set_read_context(
            plain_read_cut(server, op, cleanup_cutoff), origin_conn_id);
        return true;
    }
    const uint64_t snapshot = server.atomic_snapshot();
    const WorkError result = begin_plain_version(
        server, shard, routed, op.hash, snapshot, origin_conn_id);
    if (result == WorkError::None) return true;
    reply_work_error(op, result);
    return false;
}

void xshard_plain_finish(Shard& shard) {
    shard.store().atomic_finish_plain();
}

uint32_t xshard_cleanup_shard(Server& server, Shard& shard, uint32_t budget) {
    // Load the commit cutoff before the floor. Together with snapshot registration's
    // publish-then-confirm sequence and seq_cst ordering, a cleanup that missed a newly published
    // reader can never reclaim a version committed after that reader's chosen cut.
    const uint64_t cleanup_cutoff = server.atomic_snapshot();
    return xshard_cleanup_shard_at(
        shard, server.atomic_read_floor(), cleanup_cutoff, budget);
}

uint32_t xshard_cleanup_shard_at(Shard& shard, uint64_t floor, uint64_t cleanup_cutoff,
                                 uint32_t budget) {
    return shard.store().atomic_sweep(floor, cleanup_cutoff, budget);
}

}  // namespace tomo
