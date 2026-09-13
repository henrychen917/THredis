// scatter_engine.inc — arena-backed scatter/gather state and execution engine.
//
// This file is textually included by xshard.cc. It deliberately remains part of that one
// translation unit and shares its anonymous namespace; there are no new external symbols or
// build-system inputs.

#line 34 "src/cmd/xshard.cc"

namespace tomo {
namespace {
enum class Kind : uint8_t {
    AllShards, DebugReload, DebugLoadAof, DebugBorrowCount, DbsizeExact, Mget, Mset, Del, Unlink, Exists, Touch, Keys, Pfcount, Xread, Xreadgroup, Lcs, Msetnx,
    Rename, Renamenx, Copy, Smove, Lmove, Rpoplpush,
    Sinter, Sunion, Sdiff, Sintercard, Sinterstore, Sunionstore, Sdiffstore,
    Zunion, Zinter, Zdiff, Zintercard, Zunionstore, Zinterstore, Zdiffstore,
    Bitop, Pfmerge,
    Lmpop, Zmpop, Zrangestore, Sort,
    Geosearchstore, Georadius, Georadiusbymember,
    Script,
};

enum class ScriptPhase : uint8_t { None, Pin, Read, Run, Validate, Apply, Unpin };

enum class BitOperation : uint8_t { And, Or, Xor, Not };
enum class ZAggregate : uint8_t { Sum, Min, Max };

enum class WorkError : uint8_t {
    None, WrongType, BadHll, CorruptHll, Oom, Maxmemory, InsertFailed, Corrupt, GeoMember
};
enum class FinalReply : uint8_t {
    None, Ok, Integer, Nil, NullArray, Bulk, Array, Lmpop, Zmpop, SortConversion,
    Work, NoSuchKey, SameObject, QueueFull, Internal, Lcs, LcsError
};
enum class PopReplyShape : uint8_t { Mpop, ListPair, ZsetTriple };
enum class ValueKind : uint8_t { Nil, Inline, Borrow };

struct ObjectImage {
    bool present = false;
    // In-memory rewrites carry the physical deadline reservation separately from logical TTL.
    // Persistence/reload starts a new lifetime and does not serialize this field.
    bool reserve_ttl_slot = false;
    // Collections only: the source key was in the EXPANDED (skiplist/hashtable/deque) encoding
    // rather than the compact one. Carried because ZRANGESTORE's negative-LIMIT-offset rule is
    // encoding-selected on the oracle (see zset_resolve_limit_offset in t_zset.h) and the payload
    // itself is encoding-free. `encoding` below is the SNAPSHOT type byte, which is 0 for zsets.
    bool expanded = false;
    Type type = Type::String;
    uint8_t encoding = 0;
    uint32_t entries = 0;
    int64_t expire_at_ms = -1;
    std::vector<uint8_t> payload;
};

// Integers are at most 20 bytes including the sign.  Tiny raw strings use the same inline lane;
// anything larger is cheaper to retain through FlatStore's existing borrow registry.
struct ValueSlot {
    // Inline-copy cutover for gathered values. 24 was sized for integers; at 64B-class values it
    // pushed EVERY gather through the borrow path, whose bookkeeping (registry entry + segment
    // iov + release round-trip through the owner channel) costs more than a small memcpy — the
    // fork copies once at reassembly for exactly this reason (its OPT-1 keeps refs, but its
    // values cross as one copy into the reply). Borrow now reserves for genuinely large values
    // where wire zero-copy pays, mirroring the zc-min philosophy on the single-shard path.
    static constexpr uint32_t kInline = 1024;
    const char* ptr = nullptr;
    uint32_t len = 0;
    int32_t shard = -1;
    ValueKind kind = ValueKind::Nil;
    // Deliberately UNINITIALIZED: placement-new value-init was zeroing kInline bytes per slot per
    // op (8KB of memset per MGET-8 at the 1KB slot -- measured -34% at 64B values). Only the first
    // `len` bytes are ever written and read.
    char small[kInline];
};

struct GroupAux {
    std::vector<std::string> flush_keys;
    std::vector<std::string> keys_result;
    bool flush_keys_built = false;
};

// Groups are dense (one record per touched shard), and each owns one range in key_order[].
// No per-group vector exists on the common path.
struct ShardGroup {
    int32_t shard = -1;
    uint32_t begin = 0;
    uint32_t count = 0;
    uint64_t membership = 0;
    uint32_t snapshot_pos = 0;
    uint64_t scan_cursor = 0;
    WorkError error = WorkError::None;
    GroupAux* aux = nullptr;                 // KEYS / capture-safe FLUSH only
};

struct KeyRef {
    uint64_t hash = 0;
    uint32_t arg = 0;
    int32_t shard = -1;
    KvObj* stable_object = nullptr;          // value-backed entry key identity
    KvObj* key_anchor = nullptr;             // uninstalled value ownership, freed with ScatterState
};

struct ResultHeap {
    std::string bulk;
    std::vector<std::string> members;
    std::vector<double> scores;
    // SORT's GET projection: one flag per row in `members`, 0 = the pattern resolved to nothing and
    // the row is a RESP null (an empty element under STORE). Empty for every other command.
    std::vector<uint8_t> present;
    // LCS's answer. The heap sidecar already exists for computed results, and growing it costs
    // the arena nothing -- it is allocated only by commands that reach ensure_result().
    LcsResult lcs;
};

struct RouteKey {
    uint64_t hash = 0;
    uint32_t arg = 0;
    int32_t shard = -1;
};

enum class XshardKeyUse : uint8_t { Read, SnapshotWrite };

struct XshardKeySpan {
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t step = 1;
    XshardKeyUse use = XshardKeyUse::Read;
};

// Every supported argv shape is at most two ordered spans: destination/source forms deliberately
// keep their execution-sensitive order. This is stack-only classification state, not arena state.
struct XshardKeyPlan {
    XshardKeySpan spans[2];
    uint32_t span_count = 0;
    uint32_t key_count = 0;
};

// xshard_prepare has already parsed these values for execution. Passing that result into the key
// oracle avoids parsing option grammars or allocating parser scratch a second time on IO.
struct XshardKeyHints {
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t source = 1;
    uint32_t destination = 0;
    bool store = false;
};

constexpr uint32_t kRouteStack = 32;

enum class ImageRangeKind : uint8_t { Rank, Score, Lex };

struct ImageRangeOptions {
    ImageRangeKind kind = ImageRangeKind::Rank;
    bool reverse = false;
    bool limit_seen = false;
    int64_t offset = 0;
    int64_t limit = -1;
};

struct SortOptions {
    bool alpha = false;
    bool descending = false;
    bool store = false;
    uint32_t store_arg = 0;
    int64_t offset = 0;
    int64_t count = -1;
    // BY/GET dereference (src/cmd/t_sort.cc).  by_arg == 0 means no BY was given; get_args holds
    // the argv index of each GET pattern in order.  Both stay empty for every SORT that does not
    // use them, so the ordinary form allocates nothing extra.
    uint32_t by_arg = 0;
    std::vector<uint32_t> get_args;
    // True once a pattern will actually READ a derived key: a '*'-bearing BY, or a GET that is
    // neither `#` nor globless. These forms take a source wave followed by an owner-grouped gather
    // wave and therefore always carry the cross-shard engine's read/time cuts.
    bool deref = false;
    // True once the command needs the general path: any BY (including a `BY nosort`, which
    // suppresses ordering) or any GET (including `GET #`, which changes the reply shape).
    bool general() const { return by_arg != 0 || !get_args.empty(); }
};

enum class SortDerefWave : uint8_t { Source, Gather, Resolved };

struct SortDerefRequest {
    std::string key;
    std::string field;
    std::string value;
    uint64_t hash = 0;
    size_t output = 0;
    int32_t shard = -1;
    bool by = false;
    bool present = false;
};

// Cold SORT-only state. The object itself is placement-constructed in the scatter arena; its
// vectors own the dynamic plan whose size cannot be known until the source image has been decoded.
struct SortDerefState {
    SortSpec spec;
    SortResolved resolved;
    std::vector<std::string> elements;
    std::vector<SortDerefRequest> requests;
    std::vector<uint32_t> key_order;
    std::vector<std::string> by_values;
    std::vector<uint8_t> by_present;
    std::vector<std::string> get_values;
    std::vector<uint8_t> get_present;
    SortDerefWave wave = SortDerefWave::Source;
};

struct ImageScoreRange {
    double min = 0;
    double max = 0;
    bool min_exclusive = false;
    bool max_exclusive = false;
};

enum class ImageInfinity : int8_t { Minus = -1, Finite = 0, Plus = 1 };
struct ImageLexBound {
    ImageInfinity infinity = ImageInfinity::Finite;
    Slice value;
    bool exclusive = false;
};
struct ImageLexRange { ImageLexBound min, max; };

struct ImageRangeSpec {
    int64_t rank_start = 0;
    int64_t rank_stop = 0;
    ImageScoreRange score;
    ImageLexRange lex;
};

bool is_two_hop(Kind kind) { return kind >= Kind::Msetnx; }
bool is_store_setop(Kind kind) {
    return kind == Kind::Sinterstore || kind == Kind::Sunionstore ||
           kind == Kind::Sdiffstore;
}
bool is_plain_setop(Kind kind) {
    return kind == Kind::Sinter || kind == Kind::Sunion || kind == Kind::Sdiff;
}
bool is_store_zsetop(Kind kind) {
    return kind == Kind::Zunionstore || kind == Kind::Zinterstore ||
           kind == Kind::Zdiffstore;
}
bool is_plain_zsetop(Kind kind) {
    return kind == Kind::Zunion || kind == Kind::Zinter || kind == Kind::Zdiff;
}
bool is_any_zsetop(Kind kind) {
    return is_plain_zsetop(kind) || is_store_zsetop(kind) || kind == Kind::Zintercard;
}
bool is_geo_kind(Kind kind) {
    return kind == Kind::Geosearchstore || kind == Kind::Georadius ||
           kind == Kind::Georadiusbymember;
}
bool is_atomic_write_kind(Kind kind) {
    return kind == Kind::Mset || kind == Kind::Msetnx || kind == Kind::Del ||
           kind == Kind::Unlink;
}

bool is_atomic_apply_kind(Kind kind, bool sort_store, bool geo_store = false) {
    return kind == Kind::Bitop || kind == Kind::Pfmerge ||
           kind == Kind::Rename || kind == Kind::Renamenx || kind == Kind::Copy ||
           kind == Kind::Smove || kind == Kind::Lmove || kind == Kind::Rpoplpush ||
           is_store_setop(kind) || is_store_zsetop(kind) || kind == Kind::Zrangestore ||
           (kind == Kind::Sort && sort_store) || (is_geo_kind(kind) && geo_store);
}

bool is_atomic_pop_kind(Kind kind) {
    return kind == Kind::Lmpop || kind == Kind::Zmpop;
}

template <typename T>
size_t carve_size(size_t& offset, uint32_t count) {
    if (!count) return offset;
    const size_t align = alignof(T);
    offset = (offset + align - 1) & ~(align - 1);
    const size_t at = offset;
    if (count > (std::numeric_limits<size_t>::max() - offset) / sizeof(T)) {
        offset = std::numeric_limits<size_t>::max();
        return offset;
    }
    offset += sizeof(T) * static_cast<size_t>(count);
    return at;
}

bool serialize_object(KvObj* object, ObjectImage& image) {
    image = ObjectImage{};
    if (!object) return true;
    image.present = true;
    image.type = static_cast<Type>(object->type);
    if (image.type != Type::String) {
        const CollectionRef ref(object);
        image.entries = ref.entries();
        image.expanded = ref.encoding() != CollectionEncoding::Compact;
    }
    image.expire_at_ms = object->expire_at_ms();
    image.reserve_ttl_slot = object->has_ttl_slot();
    const SnapshotTypeHooks& hooks = snapshot_type_hooks(image.type);
    SnapshotSaveCursor cursor;
    if (!hooks.begin_save || !hooks.read_save ||
        hooks.begin_save(*object, cursor, image.encoding) != SnapshotHookStatus::Ok ||
        cursor.total > UINT32_MAX) return false;
    try {
        image.payload.resize(static_cast<size_t>(cursor.total));
    } catch (const std::bad_alloc&) {
        return false;
    }
    while (cursor.offset < cursor.total) {
        size_t written = 0;
        if (hooks.read_save(cursor, image.payload.data() + cursor.offset,
                            image.payload.size() - static_cast<size_t>(cursor.offset), written) !=
                SnapshotHookStatus::Ok || !written) return false;
    }
    return true;
}

SnapshotHookStatus load_object_image(Slice key, const ObjectImage& image,
                                     const TypeLimits& limits, KvObj*& object) {
    object = nullptr;
    const SnapshotTypeHooks& hooks = snapshot_type_hooks(image.type);
    if (!hooks.load) return SnapshotHookStatus::Corrupt;
    const Slice payload(reinterpret_cast<const char*>(image.payload.data()),
                        static_cast<uint32_t>(image.payload.size()));

    // Snapshot hooks intentionally know only logical deadlines because persistence starts a new
    // in-memory lifetime. Internal images additionally need to retain a PERSISTed physical slot.
    // Give those loaders a private, immediately-expired deadline to allocate the slot, then clear
    // it before publication. A hash loader may replace 0 with its own expired-key marker when all
    // field TTLs elapsed; preserve that marker instead of resurrecting an empty hash.
    constexpr int64_t kReserveSlotDeadline = 0;
    const bool materialize_slot = image.reserve_ttl_slot && image.expire_at_ms < 0;
    const int64_t construction_deadline =
        materialize_slot ? kReserveSlotDeadline : image.expire_at_ms;
    const SnapshotHookStatus loaded = hooks.load(
        key, image.encoding, construction_deadline, payload, limits, object);
    if (loaded != SnapshotHookStatus::Ok || !object || !materialize_slot) return loaded;
    if (!object->has_ttl_slot()) {
        kvobj_free(object);
        object = nullptr;
        return SnapshotHookStatus::Corrupt;
    }
    if (object->expire_at_ms() == kReserveSlotDeadline)
        object->set_expire_at_ms(kNoTtlDeadline);
    return SnapshotHookStatus::Ok;
}

template <bool kNotify>
WorkError apply_image(Shard& shard, Slice key, uint64_t hash, const ObjectImage& image,
                      bool suppress_del = false) {
    if (!image.present) {
        shard.store_erase<kNotify>(hash, key, suppress_del ? FlatStore::EraseEvent::None
                                                           : FlatStore::EraseEvent::Del);
        return WorkError::None;
    }
    KvObj* object = nullptr;
    const SnapshotHookStatus loaded = load_object_image(key, image, shard.type_limits(), object);
    if (loaded == SnapshotHookStatus::Oom) return WorkError::Oom;
    if (loaded != SnapshotHookStatus::Ok || !object) return WorkError::Corrupt;
    const FlatStore::InsertResult result = shard.store_insert<kNotify>(hash, object);
    if (result == FlatStore::InsertResult::Inserted) {
        shard.store().note_loaded_object(hash, object);
        return WorkError::None;
    }
    kvobj_free(object);
    return result == FlatStore::InsertResult::MaxmemoryOom ? WorkError::Maxmemory
                                                           : WorkError::InsertFailed;
}

template <bool kNotify>
WorkError store_xstring(Shard& shard, Slice key, uint64_t hash, Slice value,
                        int64_t expire_at_ms = -1, bool integer_encode = true,
                        bool reserve_ttl_slot = false) {
    const XshardStringStoreResult stored = kNotify
        ? xshard_store_string_notify(
              shard, key, hash, value, expire_at_ms, integer_encode, reserve_ttl_slot)
        : xshard_store_string(
              shard, key, hash, value, expire_at_ms, integer_encode, reserve_ttl_slot);
    switch (stored) {
        case XshardStringStoreResult::Stored: return WorkError::None;
        case XshardStringStoreResult::Oom: return WorkError::Oom;
        case XshardStringStoreResult::Maxmemory: return WorkError::Maxmemory;
        case XshardStringStoreResult::InsertFailed: return WorkError::InsertFailed;
    }
    return WorkError::InsertFailed;
}

WorkError apply_image_selected(bool notify, Shard& shard, Slice key, uint64_t hash,
                               const ObjectImage& image, bool suppress_del = false) {
    return notify ? apply_image<true>(shard, key, hash, image, suppress_del)
                  : apply_image<false>(shard, key, hash, image, suppress_del);
}

WorkError store_xstring_selected(bool notify, Shard& shard, Slice key, uint64_t hash, Slice value,
                                 int64_t expire_at_ms = -1, bool integer_encode = true,
                                 bool reserve_ttl_slot = false) {
    return notify ? store_xstring<true>(
                        shard, key, hash, value, expire_at_ms, integer_encode, reserve_ttl_slot)
                  : store_xstring<false>(
                        shard, key, hash, value, expire_at_ms, integer_encode, reserve_ttl_slot);
}

bool is_element_mover(Kind kind) {
    return kind == Kind::Smove || kind == Kind::Lmove || kind == Kind::Rpoplpush;
}

WorkError element_work_error(XshardElementResult result) {
    switch (result) {
        case XshardElementResult::Ok:
        case XshardElementResult::Missing: return WorkError::None;
        case XshardElementResult::WrongType: return WorkError::WrongType;
        case XshardElementResult::Oom: return WorkError::Oom;
        case XshardElementResult::Maxmemory: return WorkError::Maxmemory;
        case XshardElementResult::InsertFailed: return WorkError::InsertFailed;
    }
    return WorkError::InsertFailed;
}

}  // namespace

struct alignas(64) OwnerRecordRefs {
    // A slot is touched only by its executor: every shard mapped to one owner is serialized there.
    // Cache-line spacing prevents cleanup on distinct owners from bouncing a shared counter line.
    // A pre-counted standalone wave starts nodes at one. That completion sentinel prevents cleanup
    // of an early-aborted entry from taking the count to zero before the owner's last task runs.
    uint32_t nodes = 0;
    uint32_t remaining = 0;
    uint32_t group_count = 0;
};

// The read cut lives in the shared SnapshotCut base so a transaction can hold one through the
// same per-IO-thread floor arithmetic (see xshard.h).  Member names are unchanged, so every
// state->snapshot / state->snapshot_registered reference below still resolves.
struct ScatterState : SnapshotCut {
    ~ScatterState() { notify_discard_batch(notify.exchange(nullptr)); }
    std::atomic<uint32_t> pending{0};
    // Pending group entries may retain this state after reply retirement, hence record_refs.
    std::atomic<uint32_t> record_refs{0};
    // A watched-key reservation names the group decision too.  It holds this second lifetime pin
    // until its owner shard consumes the published decision, including groups that install no
    // MVCC record (for example a failed conditional write).
    std::atomic<uint32_t> watch_refs{0};
    std::atomic<bool> conditional_failed{false};
    std::atomic<bool> script_conflict{false};
    std::atomic<uint8_t> direct_ready{0};     // 0=source pending, 1=image ready, 2=terminal
    std::atomic<uint64_t> exact_sum{0};
    Kind kind = Kind::AllShards;
    FinalReply final_reply = FinalReply::None;
    WorkError final_error = WorkError::None;
    uint8_t phase = 1;
    ScriptPhase script_phase = ScriptPhase::None;
    // Set when a wave could not find task slots and the poster is holding its own `pending`
    // reference across the requeue. While it is set no owner task may execute a phase body: the
    // phase word already names the wave that is still being published.
    bool script_resume_post = false;
    bool script_pins_live = false;
    bool script_ticket_live = false;
    bool script_apply_after_unpin = false;
    bool script_commit_after_unpin = false;
    bool script_has_writes = false;
    bool script_stage_limit = false;
    bool script_certifier = false;
    bool runtime_loading = false;
    // Set for a child lowered inside EXEC. The child keeps its own decision words, but the parent
    // publishes their common ticket and owns the outer transaction decision.
    bool multi_child = false;
    bool barrier = false;
    bool pooled = false;
    bool copy_replace = false;
    bool from_left = false;
    bool to_left = false;
    bool pop_edge = false;                 // RIGHT for LMPOP, MAX for ZMPOP
    PopReplyShape pop_reply_shape = PopReplyShape::Mpop;
    bool atomic_write = false;
    bool atomic_apply = false;
    bool atomic_pop = false;
    bool atomic_group = false;
    bool atomic_direct = false;
    bool pop_missed = false;
    // SORT's general form is computed on an executor. Dereferencing forms set this only after the
    // last owner-grouped gather task has returned; non-dereferencing forms may set it in the source
    // task. Completion consumes the one result in either case.
    bool sort_general_done = false;
    bool sort_conversion_error = false;
    bool admitted = false;
    // True between admission and the moment this group's records are installed on every owner.
    // See Server::atomic_apply_inflight() for why the snapshot cut waits on this and not on the
    // admission lease.
    std::atomic<bool> apply_open{false};
    // A MULTI child's records deliberately point at the CHILD epoch/abort words: a runtime error
    // cancels that one queued command without turning the whole EXEC into EXECABORT. The parent
    // finalizer installs one common ticket into every successful decision word before advancing
    // the safe watermark.
    bool contents_freed = false;
    bool retired = false;
    bool two_hop = false;
    uint32_t owner_io = 0;
    uint32_t owner_slots = 0;
    uint64_t admission_generation = 0;
    uint64_t origin_conn_id = 0;
    Server* server = nullptr;
    uint32_t arena_bytes = 0;
    uint32_t key_count = 0;
    uint32_t nsub = 0;
    uint32_t group_cap = 0;
    uint32_t shard_slots = 0;
    uint32_t phase1_first = 0;
    uint32_t set_first = 0;
    uint32_t set_count = 0;
    uint32_t hop2_count = 0;
    uint32_t selected = std::numeric_limits<uint32_t>::max();
    uint32_t pop_retries = 0;
    uint32_t script_retries = 0;
    int32_t script_coordinator_shard = -1;
    // TEST HOOK budget copied from Server::debug_atomic_direct_defer() when this group takes the
    // direct-RENAME path. Touched only by the destination task's owner. Zero in production.
    uint32_t debug_direct_defer = 0;
    // TEST HOOK deadline copied from Server::debug_atomic_fanout_defer() when this state is a
    // cross-shard READ. Written once at prepare, before the tasks are posted; read by the owners
    // through the task handoff's release/acquire pair. Zero in production, and the field lives in
    // the arena, not in Op or Client.
    uint64_t debug_fanout_deadline = 0;
    int32_t debug_fanout_lead = -1;               // the one shard that is never parked
    // TEST HOOK for the cross-owner script GATHER window (DEBUG SCRIPT-STAGE-DEFER). The budget is
    // copied at prepare; the deadline is stamped when the cut is chosen, so the park covers exactly
    // the interval between reservation and gather. Both zero in production.
    uint32_t debug_script_defer_us = 0;
    uint64_t debug_script_defer_deadline = 0;
    uint64_t sinter_limit = 0;
    uint64_t zset_limit = 0;
    LcsOptions lcs_options;
    uint64_t pop_count = 1;
    uint64_t xread_count = 0;
    uint64_t script_cut = UINT64_MAX;
    uint64_t script_ticket = 0;
    std::atomic<uint64_t> script_staged_bytes{0};
    // THE OPERATION'S EXPIRY CUT. A cross-shard command pins a commit cut so it cannot straddle a
    // foreign write; without this it could still straddle a DEADLINE, because expiry is time-driven
    // and each owner otherwise answers against its own executor's per-pass clock. Absolute
    // CLOCK_REALTIME milliseconds, stamped once at prepare in the connection's program order and
    // installed on every participating owner by xshard_execute. Zero means "no cut stamped", which
    // is a safety net only: a zero cut would read as 1970 and hide the whole keyspace.
    int64_t now_cut_ms = 0;
    uint64_t pop_retry_snapshot = UINT64_MAX;
    long long final_integer = 0;
    ImageRangeOptions range_options;
    ImageRangeSpec range_spec;
    SortOptions sort_options;
    ZAggregate zset_aggregate = ZAggregate::Sum;
    bool zset_withscores = false;
    std::vector<double> zset_weights;
    bool geo_store = false;
    bool geo_store_distance = false;
    uint32_t geo_source_arg = 1;
    uint32_t geo_destination_arg = 0;
    uint32_t xread_first_key = 0;
    uint32_t xread_first_id = 0;
    ShardGroup* groups = nullptr;
    KeyRef* keys = nullptr;
    uint32_t* key_order = nullptr;
    ValueSlot* values = nullptr;
    uint8_t* status = nullptr;
    uint8_t* script_pinned = nullptr;
    ObjectImage* images = nullptr;
    ObjectImage* apply = nullptr;
    uint32_t* hop2 = nullptr;
    uint16_t* shard_groups = nullptr;
    OwnerRecordRefs* owner_record_refs = nullptr;
    ResultHeap* result = nullptr;
    std::unique_ptr<SnapshotLoadPlan> reload_plan;
    std::unique_ptr<SnapshotLoadPlan> aof_reload_base;
    std::vector<std::unique_ptr<AofReplayPlan>> aof_reload_increments;
    std::shared_ptr<AofGroupDecision> aof_group;
    // Non-null only after a parked BLMOVE/BRPOPLPUSH is resumed by its owning IO. The blocking
    // context remains independently refcounted while this ordinary scatter owns the Op marker.
    BlockingState* blocking_origin = nullptr;
    std::atomic<NotifyBatch*> notify{nullptr};
    // Successful last-owner completions link themselves here until this executor's pass boundary.
    // The input Task is retired as soon as execute() returns, so retain its stable ROB identity by
    // value. No allocation is permitted after the group has installed records and reached pending
    // zero: failure to enqueue at that point would leave an epoch-zero group alive forever.
    ScatterState* commit_next = nullptr;
    Client* commit_client = nullptr;
    uint64_t commit_op_id = 0;
    int32_t commit_shard = -1;
    // COLD TAIL: only a SORT with an actual `*` dereference carves and constructs this arena object.
    SortDerefState* sort_deref = nullptr;
    // Appended test-only state: do not move any production ScatterState member. The destination
    // shard is keys[1].shard; parking that phase-one validation leaves its executor available.
    // The hop delay is armed only for the first non-atomic mutation wave spanning multiple owners.
    // Every group on the lead owner may publish; other owners park in the existing deferred queue.
    uint64_t debug_hop_deadline = 0;
    uint32_t debug_hop_lead_owner = std::numeric_limits<uint32_t>::max();
    uint64_t debug_conditional_deadline = 0;
    // READ-MOSTLY DECISION LINE. Zero means installs are still private; the last owner
    // release-publishes a ticket here. Resolvers load epoch and aborted for every candidate, so
    // isolate them from pending/record_refs/watch_refs RMWs and pad the line completely.
    alignas(64) std::atomic<uint64_t> epoch{0};
    std::atomic<bool> aborted{false};
    char decision_line_pad[64 - sizeof(std::atomic<uint64_t>) -
                           sizeof(std::atomic<bool>)];
};

static_assert(sizeof(std::atomic<uint64_t>) + sizeof(std::atomic<bool>) <= 64 &&
              sizeof(std::atomic<uint64_t>) % alignof(std::atomic<bool>) == 0,
              "ScatterState decision words no longer pack contiguously");
static_assert(alignof(ScatterState) == 64 && sizeof(ScatterState) % 64 == 0,
              "ScatterState decision line lost cache-line isolation");
static_assert(sizeof(ScatterState) + 8 * (sizeof(ShardGroup) + sizeof(KeyRef) +
                                           sizeof(uint32_t) + sizeof(ValueSlot)) <= 16384,
              "MGET-8 ScatterState no longer fits ScatterArenaPool::kCommonBytes");

namespace {

struct XshardCommitBatch {
    ScatterState* head = nullptr;
    ScatterState* tail = nullptr;
    uint64_t count = 0;
    Server* server = nullptr;
    uint32_t owner = UINT32_MAX;
};

thread_local XshardCommitBatch xshard_commit_batch;

ShardGroup* group_for(ScatterState& state, int32_t shard) {
    if (state.shard_groups) {
        if (shard < 0 || static_cast<uint32_t>(shard) >= state.shard_slots) return nullptr;
        const uint16_t index = state.shard_groups[static_cast<uint32_t>(shard)];
        return index == UINT16_MAX || index >= state.nsub ? nullptr : &state.groups[index];
    }
    for (uint32_t i = 0; i < state.nsub; i++)
        if (state.groups[i].shard == shard) return &state.groups[i];
    return nullptr;
}

std::atomic<uint64_t>* publish_epoch(ScatterState& state) {
    return &state.epoch;
}

std::atomic<bool>* publish_aborted(ScatterState& state) {
    return &state.aborted;
}

ResultHeap* ensure_result(ScatterState& state) {
    if (!state.result) state.result = new (std::nothrow) ResultHeap;
    return state.result;
}

bool aof_prepare_scatter_group(ScatterState& state) {
    if (!state.atomic_group || !state.server || !state.server->aof().recording()) return true;
    std::vector<uint32_t> participants;
    try {
        participants.reserve(state.nsub);
        for (uint32_t index = 0; index < state.nsub; index++)
            participants.push_back(static_cast<uint32_t>(state.groups[index].shard));
    } catch (const std::bad_alloc&) {
        state.server->aof().fail("out of memory collecting AOF group participants");
        return false;
    }
    state.aof_group = aof_create_group(state.server->aof(), participants);
    return state.aof_group != nullptr;
}

void aof_abort_scatter_group(ScatterState& state) {
    aof_abort_group(state.aof_group);
}

void aof_commit_scatter_group(ScatterState& state, ThreadCtx& owner, Ring& ring,
                              uint64_t ticket) {
    if (!state.aof_group) return;
    AofOwnerContext context{owner.id(), &ring, &owner.sig()};
    (void)aof_commit_group(state.server->aof(), state.aof_group, ticket, context);
}

// Whole-function moves put the engine before command and atomic helpers in the concatenation.
// These internal declarations preserve the original signatures and anonymous-namespace linkage.
bool name_is(const Op& op, const char* name);
bool parse_u64(Slice s, uint64_t& value);
bool parse_i64(Slice s, int64_t& out);
bool parse_mpop(Op& op, Kind kind, uint32_t& numkeys, bool& edge, uint64_t& count);
bool parse_zrangestore_options(Op& op, ImageRangeOptions& options);
bool parse_zrangestore_range(Op& op, const ImageRangeOptions& options, ImageRangeSpec& range);
bool parse_sort_options(Op& op, SortOptions& options);
WorkError sort_image(const ObjectImage& image, const Op& op, const SortOptions& options,
                     const SortResolved* resolved,
                     std::vector<std::string>& values, std::vector<uint8_t>& present,
                     bool& conversion_error);
WorkError sort_image_prepare(const ObjectImage& image, const Op& op, const SortOptions& options,
                             SortSpec& spec, std::vector<std::string>& elements);
WorkError sort_image_finish(const SortSpec& spec, const SortResolved* resolved,
                            std::vector<std::string>& elements,
                            std::vector<std::string>& values, std::vector<uint8_t>& present,
                            bool& conversion_error);
bool sort_encode_store(const std::vector<std::string>& values,
                       const std::vector<uint8_t>& present, ObjectImage& image);
void sort_reply_rows(Op& op, const std::vector<std::string>& values,
                     const std::vector<uint8_t>& present);
bool parse_zset_operation(Op& op, Kind kind, uint32_t& count,
                          std::vector<double>& weights, ZAggregate& aggregate,
                          bool& withscores, uint64_t& limit);
void finish_lcs(ScatterState& state);
void set_oom(Op& op);
void reply_work_error(Op& op, WorkError error);
bool classify(const Op& op, Kind& kind);
bool classify_keys(const Op& op, Kind kind, const XshardKeyHints& hints,
                   XshardKeyPlan& keys);
bool classify_keys(Op& op, Kind kind, XshardKeyPlan& keys);
bool bitop_operation(Slice name, BitOperation& operation);
WorkError begin_plain_version(Server& server, Shard& shard, Slice key, uint64_t hash,
                              uint64_t snapshot, uint64_t origin_conn_id);
ScatterTaskResult execute_atomic_apply(const Task& task, Shard& shard, Op& op,
                                       uint32_t owner_thread_id);
ScatterTaskResult execute_atomic_direct_rename(const Task& task, Shard& shard, Op& op,
                                               uint32_t owner_thread_id);
bool xshard_owner_mutation_phase(const ScatterState& state, const Op& op);
void finish_pfcount(ScatterState& state);
bool finish_phase1(ScatterState& state, Op& op);
bool finish_phase2(ScatterState& state);
void assemble_final(Client& client, Op& op, ScatterState& state, void* release_ctx,
                    void (*release_fn)(void*, int32_t, const char*),
                    void (*suppressed_fn)(void*));

bool first_error(const ScatterState& state, WorkError& error) {
    for (uint32_t gi = 0; gi < state.nsub; gi++) {
        const ShardGroup& group = state.groups[gi];
        if (group.error != WorkError::None) { error = group.error; return true; }
        // The gather wave's ranges index SortDerefState::key_order, not the fixed argv-key order.
        // Gather tasks report allocation failures in group.error above and have no per-key status,
        // so a terminal OOM/queue-full path must not scan state.key_order before source groups are
        // restored.
        if (state.sort_deref && state.sort_deref->wave == SortDerefWave::Gather) continue;
        if (!state.status || !(state.kind == Kind::Mset || state.kind == Kind::Pfcount ||
                               state.kind == Kind::Xread || state.kind == Kind::Lcs ||
                               is_two_hop(state.kind))) continue;
        for (uint32_t i = 0; i < group.count; i++) {
            const WorkError value = static_cast<WorkError>(state.status[state.key_order[group.begin + i]]);
            if (value != WorkError::None) { error = value; return true; }
        }
    }
    return false;
}

uint32_t key_count_for(const XshardKeyPlan& keys, bool snapshot_writes_only = false) {
    if (!snapshot_writes_only) return keys.key_count;
    uint32_t count = 0;
    for (uint32_t i = 0; i < keys.span_count; i++)
        if (keys.spans[i].use == XshardKeyUse::SnapshotWrite) count += keys.spans[i].count;
    return count;
}

uint32_t key_arg_for(const XshardKeyPlan& keys, uint32_t key,
                     bool snapshot_writes_only = false) {
    for (uint32_t i = 0; i < keys.span_count; i++) {
        const XshardKeySpan& span = keys.spans[i];
        if (snapshot_writes_only && span.use != XshardKeyUse::SnapshotWrite) continue;
        if (key < span.count) return span.first + key * span.step;
        key -= span.count;
    }
    std::abort();
}

bool has_values(Kind kind, bool atomic_write) {
    return kind == Kind::Mget || (kind == Kind::Msetnx && !atomic_write);
}
bool has_status(Kind kind, bool atomic_write) {
    if (atomic_write && (kind == Kind::Mset || kind == Kind::Msetnx)) return false;
    return kind == Kind::Mset || kind == Kind::Del || kind == Kind::Unlink ||
           kind == Kind::Exists || kind == Kind::Touch || kind == Kind::Pfcount ||
           kind == Kind::Xread || kind == Kind::Lcs || is_two_hop(kind);
}
bool has_images(Kind kind) {
    return kind == Kind::Pfcount || kind == Kind::Xread || kind == Kind::Lcs ||
           (is_two_hop(kind) && kind != Kind::Msetnx);
}

size_t arena_layout_bytes(Kind kind, uint32_t key_count, uint32_t group_cap,
                          uint32_t shard_slots, bool atomic_write, bool atomic_apply,
                          uint32_t owner_slots, bool sort_deref) {
    const bool atomic_install = atomic_write || atomic_apply;
    size_t off = sizeof(ScatterState);
    carve_size<ShardGroup>(off, group_cap);
    carve_size<KeyRef>(off, key_count);
    carve_size<uint32_t>(off, key_count);             // key_order
    if (has_values(kind, atomic_write)) carve_size<ValueSlot>(off, key_count);
    if (has_status(kind, atomic_write)) carve_size<uint8_t>(off, key_count);
    if (kind == Kind::Script) carve_size<uint8_t>(off, key_count);
    if (has_images(kind)) {
        carve_size<ObjectImage>(off, key_count);
        carve_size<ObjectImage>(off, key_count);
    }
    if (is_two_hop(kind) && !atomic_write)
        carve_size<uint32_t>(off, key_count);  // complete hop-2 plan
    if (atomic_install)
        carve_size<uint16_t>(off, shard_slots); // direct shard -> dense group lookup
    if (atomic_install) carve_size<OwnerRecordRefs>(off, owner_slots);
    if (sort_deref) carve_size<SortDerefState>(off, 1);
    return off;
}

template <typename T>
T* carve_at(char* base, size_t& off, uint32_t count) {
    if (!count) return nullptr;
    const size_t align = alignof(T);
    off = (off + align - 1) & ~(align - 1);
    T* result = reinterpret_cast<T*>(base + off);
    off += sizeof(T) * static_cast<size_t>(count);
    return result;
}

void init_arena_arrays(ScatterState& state) {
    char* base = reinterpret_cast<char*>(&state);
    size_t off = sizeof(ScatterState);
    state.groups = carve_at<ShardGroup>(base, off, state.group_cap);
    state.keys = carve_at<KeyRef>(base, off, state.key_count);
    state.key_order = carve_at<uint32_t>(base, off, state.key_count);
    if (has_values(state.kind, state.atomic_write))
        state.values = carve_at<ValueSlot>(base, off, state.key_count);
    if (has_status(state.kind, state.atomic_write))
        state.status = carve_at<uint8_t>(base, off, state.key_count);
    if (state.kind == Kind::Script)
        state.script_pinned = carve_at<uint8_t>(base, off, state.key_count);
    if (has_images(state.kind)) {
        state.images = carve_at<ObjectImage>(base, off, state.key_count);
        state.apply = carve_at<ObjectImage>(base, off, state.key_count);
    }
    if (state.two_hop) state.hop2 = carve_at<uint32_t>(base, off, state.key_count);
    if (state.atomic_write || state.atomic_apply)
        state.shard_groups = carve_at<uint16_t>(base, off, state.shard_slots);
    if (state.atomic_write || state.atomic_apply)
        state.owner_record_refs = carve_at<OwnerRecordRefs>(base, off, state.owner_slots);
    if (state.kind == Kind::Sort && state.sort_options.deref)
        state.sort_deref = carve_at<SortDerefState>(base, off, 1);

    for (uint32_t i = 0; state.groups && i < state.group_cap; i++)
        new (&state.groups[i]) ShardGroup;
    for (uint32_t i = 0; state.keys && i < state.key_count; i++) new (&state.keys[i]) KeyRef;
    if (state.key_order) std::memset(state.key_order, 0, sizeof(uint32_t) * state.key_count);
    for (uint32_t i = 0; state.values && i < state.key_count; i++) new (&state.values[i]) ValueSlot;
    if (state.status) std::memset(state.status, 0, state.key_count);
    if (state.script_pinned) std::memset(state.script_pinned, 0, state.key_count);
    if (state.hop2) std::memset(state.hop2, 0, sizeof(uint32_t) * state.key_count);
    if (state.shard_groups)
        std::fill_n(state.shard_groups, state.shard_slots, UINT16_MAX);
    // owner_record_refs is deliberately NOT constructed here. Each slot is its own cache line
    // (alignas(64)) and there is one per server thread, so value-initializing all of them wrote
    // nthreads lines per atomic group -- 64 on the 64-thread box -- for a group that typically has
    // eight owners. initialize_owner_completion() constructs exactly the participating slots, and
    // every later reader (xshard_execute, execute_atomic_apply, xshard_complete_impl,
    // xshard_multi_child_complete) indexes by the executor that is running, which is by
    // construction a participant.
    if (state.sort_deref) new (state.sort_deref) SortDerefState;
    for (uint32_t i = 0; state.images && i < state.key_count; i++) new (&state.images[i]) ObjectImage;
    for (uint32_t i = 0; state.apply && i < state.key_count; i++) new (&state.apply[i]) ObjectImage;
}

void reset_groups(ScatterState& state) {
    for (uint32_t i = 0; i < state.group_cap; i++) {
        ShardGroup& group = state.groups[i];
        delete group.aux;
        group = ShardGroup{};
    }
    if (state.shard_groups)
        std::fill_n(state.shard_groups, state.shard_slots, UINT16_MAX);
    state.nsub = 0;
}

void build_groups(ScatterState& state, const uint32_t* positions, uint32_t count) {
    uint32_t counts[256] = {};
    for (uint32_t i = 0; i < count; i++) counts[state.keys[positions[i]].shard]++;
    uint32_t cursor = 0;
    // Shard ids are dense below nshards(); scanning all 256 ran ~190 useless iterations per op.
    // Ascending shard order is preserved exactly (groups[0] is the fan-out lead).
    const uint32_t nshards = state.server->nshards();
    for (uint32_t sid = 0; sid < nshards; sid++) {
        if (!counts[sid]) continue;
        ShardGroup& group = state.groups[state.nsub++];
        group.shard = static_cast<int32_t>(sid);
        group.begin = cursor;
        group.count = counts[sid];
        if (state.shard_groups)
            state.shard_groups[sid] = static_cast<uint16_t>(state.nsub - 1);
        cursor += counts[sid];
    }
    uint32_t fill[256] = {};
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t pos = positions[i];
        ShardGroup* group = group_for(state, state.keys[pos].shard);
        if (state.atomic_apply)
            group->membership |= atomic_membership_bit(state.keys[pos].hash);
        state.key_order[group->begin + fill[group->shard]++] = pos;
    }
}

void build_initial_groups(ScatterState& state) {
    if (state.atomic_direct) {
        // Direct movers may defer the destination task until the source image is ready. Preserve
        // source-before-destination order on an executor that owns both shards so its retry queue
        // can never sit ahead of the task that satisfies the dependency.
        for (uint32_t pos = state.phase1_first; pos < state.key_count; pos++) {
            ShardGroup& group = state.groups[state.nsub];
            group.shard = state.keys[pos].shard;
            group.begin = pos - state.phase1_first;
            group.count = 1;
            group.membership = atomic_membership_bit(state.keys[pos].hash);
            state.key_order[group.begin] = pos;
            state.shard_groups[group.shard] = static_cast<uint16_t>(state.nsub++);
        }
        return;
    }
    uint32_t counts[256] = {};
    for (uint32_t pos = state.phase1_first; pos < state.key_count; pos++)
        counts[state.keys[pos].shard]++;
    uint32_t cursor = 0;
    const uint32_t nshards = state.server->nshards();   // see build_groups
    for (uint32_t sid = 0; sid < nshards; sid++) {
        if (!counts[sid]) continue;
        ShardGroup& group = state.groups[state.nsub++];
        group.shard = static_cast<int32_t>(sid);
        group.begin = cursor;
        group.count = counts[sid];
        if (state.shard_groups)
            state.shard_groups[sid] = static_cast<uint16_t>(state.nsub - 1);
        cursor += counts[sid];
    }
    uint32_t fill[256] = {};
    if (state.atomic_write) {
        for (uint32_t pos = state.phase1_first; pos < state.key_count; pos++) {
            ShardGroup* group = group_for(state, state.keys[pos].shard);
            group->membership |= atomic_membership_bit(state.keys[pos].hash);
            state.key_order[group->begin + fill[group->shard]++] = pos;
        }
    } else {
        for (uint32_t pos = state.phase1_first; pos < state.key_count; pos++) {
            ShardGroup* group = group_for(state, state.keys[pos].shard);
            state.key_order[group->begin + fill[group->shard]++] = pos;
        }
    }
}

bool sort_deref_gathering(const ScatterState& state) {
    return state.sort_deref && state.sort_deref->wave == SortDerefWave::Gather;
}

Slice sort_deref_key(const SortDerefRequest& request) {
    return Slice(request.key.data(), static_cast<uint32_t>(request.key.size()));
}

// A scatter task runs on the one owner of `group.shard`, and the group's dense range contains only
// hashes routed to that shard. Warm every local home bucket before the first per-key storage touch;
// singleton groups have no independent miss to overlap. The hint carries no lookup result, so the
// command loops below remain authoritative and retain their original order.
void prefetch_owner_group(FlatStore& store, const ScatterState& state, const ShardGroup& group) {
    if (group.count < 2) return;
    if (sort_deref_gathering(state)) {
        const SortDerefState& sort = *state.sort_deref;
        for (uint32_t i = 0; i < group.count; i++) {
            const SortDerefRequest& request = sort.requests[sort.key_order[group.begin + i]];
            store.prefetch(request.hash);
        }
        return;
    }
    for (uint32_t i = 0; i < group.count; i++) {
        const uint32_t pos = state.key_order[group.begin + i];
        store.prefetch(state.keys[pos].hash);
    }
}

// Replace the explicit-key source groups with the dynamically discovered request groups. The
// generic ShardGroup table remains the dispatch index; only its begin/count ranges point into the
// SORT sidecar's key_order during this wave.
void build_sort_deref_groups(ScatterState& state) {
    SortDerefState& sort = *state.sort_deref;
    reset_groups(state);
    uint32_t counts[256] = {};
    for (const SortDerefRequest& request : sort.requests)
        counts[static_cast<uint32_t>(request.shard)]++;
    uint32_t cursor = 0;
    const uint32_t nshards = state.server->nshards();   // see build_groups
    for (uint32_t sid = 0; sid < nshards; sid++) {
        if (!counts[sid]) continue;
        ShardGroup& group = state.groups[state.nsub++];
        group.shard = static_cast<int32_t>(sid);
        group.begin = cursor;
        group.count = counts[sid];
        if (state.shard_groups)
            state.shard_groups[sid] = static_cast<uint16_t>(state.nsub - 1);
        cursor += counts[sid];
    }
    uint32_t fill[256] = {};
    for (uint32_t pos = 0; pos < sort.requests.size(); pos++) {
        const SortDerefRequest& request = sort.requests[pos];
        ShardGroup* group = group_for(state, request.shard);
        group->membership |= atomic_membership_bit(request.hash);
        sort.key_order[group->begin + fill[static_cast<uint32_t>(request.shard)]++] = pos;
    }
}

void restore_sort_source_groups(ScatterState& state) {
    reset_groups(state);
    build_initial_groups(state);
}

WorkError prepare_sort_deref(ScatterState& state, Op& op) {
    SortDerefState& sort = *state.sort_deref;
    const uint32_t source_pos = state.sort_options.store ? 1u : 0u;
    WorkError prepared = sort_image_prepare(
        state.images[source_pos], op, state.sort_options, sort.spec, sort.elements);
    if (prepared != WorkError::None) return prepared;

    try {
        const size_t elements = sort.elements.size();
        const size_t gets = sort.spec.gets.size();
        if (gets && elements > std::numeric_limits<size_t>::max() / gets)
            return WorkError::Oom;
        const size_t get_slots = elements * gets;
        sort.by_values.resize(elements);
        sort.by_present.assign(elements, 0);
        sort.get_values.resize(get_slots);
        sort.get_present.assign(get_slots, 0);
        sort.resolved.by_values = &sort.by_values;
        sort.resolved.by_present = &sort.by_present;
        sort.resolved.get_values = &sort.get_values;
        sort.resolved.get_present = &sort.get_present;

        uint64_t request_count = sort.spec.by_given && sort.spec.by.has_star ? elements : 0;
        for (const SortPattern& pattern : sort.spec.gets)
            if (!pattern.self && pattern.has_star) request_count += elements;
        if (request_count > UINT32_MAX) return WorkError::Oom;
        sort.requests.reserve(static_cast<size_t>(request_count));

        auto add_request = [&](const SortPattern& pattern, Slice element, bool by, size_t output) {
            SortDerefRequest request;
            if (!sort_pattern_key(pattern, element, request.key)) return;
            if (pattern.field.n) request.field.assign(pattern.field.p, pattern.field.n);
            const Slice key(request.key.data(), static_cast<uint32_t>(request.key.size()));
            request.hash = FlatStore::hash_key(key);
            request.shard = state.server->router().shard_of(request.hash);
            request.output = output;
            request.by = by;
            sort.requests.push_back(std::move(request));
        };
        for (size_t element_index = 0; element_index < elements; element_index++) {
            const std::string& value = sort.elements[element_index];
            const Slice element(value.data(), static_cast<uint32_t>(value.size()));
            if (sort.spec.by_given && sort.spec.by.has_star)
                add_request(sort.spec.by, element, true, element_index);
            for (size_t get = 0; get < gets; get++) {
                const SortPattern& pattern = sort.spec.gets[get];
                if (!pattern.self && pattern.has_star)
                    add_request(pattern, element, false, element_index * gets + get);
            }
        }
        sort.key_order.resize(sort.requests.size());
    } catch (const std::bad_alloc&) {
        return WorkError::Oom;
    }

    if (sort.requests.empty()) {
        sort.wave = SortDerefWave::Resolved;
        return WorkError::None;
    }
    sort.wave = SortDerefWave::Gather;
    build_sort_deref_groups(state);
    return WorkError::None;
}

WorkError finish_sort_deref(ScatterState& state) {
    SortDerefState& sort = *state.sort_deref;
    for (uint32_t i = 0; i < state.nsub; i++)
        if (state.groups[i].error != WorkError::None) return state.groups[i].error;
    for (SortDerefRequest& request : sort.requests) {
        if (!request.present) continue;
        if (request.by) {
            sort.by_values[request.output] = std::move(request.value);
            sort.by_present[request.output] = 1;
        } else {
            sort.get_values[request.output] = std::move(request.value);
            sort.get_present[request.output] = 1;
        }
    }
    ResultHeap* result = ensure_result(state);
    if (!result) return WorkError::Oom;
    bool conversion_error = false;
    const WorkError finished = sort_image_finish(
        sort.spec, &sort.resolved, sort.elements, result->members, result->present,
        conversion_error);
    if (finished != WorkError::None) return finished;
    state.sort_conversion_error = conversion_error;
    state.sort_general_done = true;
    sort.wave = SortDerefWave::Resolved;
    state.server->note_sort_scatter_general();
    return WorkError::None;
}

uint32_t initialize_owner_completion(ScatterState& state) {
    if (!state.atomic_write && !(state.atomic_apply && state.phase == 2)) return state.nsub;
    if (state.record_refs.load(std::memory_order_relaxed) != 0) std::abort();
    // Construct and count only the slots this wave actually uses (see init_arena_arrays). A slot
    // that no group maps to keeps whatever the recycled arena held; nobody indexes it.
    bool seen[kMaxThreads] = {};
    uint32_t participants[kMaxThreads];
    uint32_t owners = 0;
    for (uint32_t i = 0; i < state.nsub; i++) {
        const uint32_t tid = state.server->worker_of_shard(state.groups[i].shard);
        const uint32_t slot = state.server->executor_slot(tid);
        if (slot >= state.owner_slots || slot >= kMaxThreads) std::abort();
        if (!seen[slot]) {
            seen[slot] = true;
            new (&state.owner_record_refs[slot]) OwnerRecordRefs;
            participants[owners++] = slot;
        }
        state.owner_record_refs[slot].group_count++;
    }
    for (uint32_t p = 0; p < owners; p++) {
        OwnerRecordRefs& progress = state.owner_record_refs[participants[p]];
        progress.remaining = progress.group_count;
        if (!state.multi_child) progress.nodes = 1;  // owner-completion sentinel
    }
    if (!state.multi_child) {
        state.record_refs.store(owners, std::memory_order_release);
    }
    return owners;
}

// DEBUG's atomic-OFF half widens an already non-atomic physical publication gap; it must never
// turn a read/probe wave into a delay. Classify the actual routed owners first, then load the shared
// knob only for a mutation wave that spans more than one owner. A zero setting does no clock read
// and installs no per-task deadline. The arena is published to tasks only after this returns.
void arm_debug_off_hop_delay(ScatterState& state, const Op& op) {
    // Forced children inside EXEC are atomic_group too: they must reach the parent's one-ticket
    // atomic_commit_group boundary, where the same Server word supplies the ON delay.
    if (state.atomic_group || !xshard_owner_mutation_phase(state, op) || state.nsub < 2) return;
    const uint32_t delay_us = state.server->debug_hop_delay();
    if (__builtin_expect(delay_us == 0, true)) return;
    const uint32_t lead_owner = state.server->worker_of_shard(state.groups[0].shard);
    bool cross_owner = false;
    for (uint32_t i = 1; i < state.nsub; i++)
        if (state.server->worker_of_shard(state.groups[i].shard) != lead_owner) {
            cross_owner = true;
            break;
        }
    if (!cross_owner) return;
    state.debug_hop_lead_owner = lead_owner;
    state.debug_hop_deadline = now_ns() + static_cast<uint64_t>(delay_us) * 1000ull;
}

void note_owner_record_install(ScatterState& state, uint32_t& owner_nodes) {
    if (!owner_nodes) {
        // Only a MULTI child is intentionally lazy. Standalone waves published their sentinel before
        // any task became visible, so zero here would mean their lifetime pin was lost.
        if (!state.multi_child) std::abort();
        state.record_refs.fetch_add(1, std::memory_order_release);
    }
    owner_nodes++;
}

bool complete_owner_record_wave(ScatterState& state, uint32_t owner_thread_id) {
    const uint32_t slot = state.server->executor_slot(owner_thread_id);
    if (slot >= state.owner_slots) std::abort();
    OwnerRecordRefs& progress = state.owner_record_refs[slot];
    if (!progress.remaining) std::abort();
    if (--progress.remaining != 0) return false;
    if (!state.multi_child) {
        if (!progress.nodes) std::abort();
        if (--progress.nodes == 0 &&
            state.record_refs.fetch_sub(1, std::memory_order_release) == 0) std::abort();
    }
    return true;
}

void clear_unpublished_owner_record_refs(ScatterState& state) {
    // Before initialize_owner_completion there are no constructed slots to inspect. Once it has
    // pre-counted a standalone wave, no unpublished task can have installed a real node, so a
    // nonzero group count names exactly the per-owner sentinels below.
    if (state.record_refs.load(std::memory_order_relaxed) == 0) return;
    if (state.multi_child) std::abort();
    bool seen[kMaxThreads] = {};
    uint32_t owners = 0;
    for (uint32_t i = 0; i < state.nsub; i++) {
        const uint32_t tid = state.server->worker_of_shard(state.groups[i].shard);
        const uint32_t slot = state.server->executor_slot(tid);
        if (slot >= state.owner_slots || slot >= kMaxThreads) std::abort();
        if (seen[slot]) continue;
        seen[slot] = true;
        OwnerRecordRefs& progress = state.owner_record_refs[slot];
        // With no published task, each owner can hold only its prepare-time completion sentinel.
        if (progress.nodes != 1) std::abort();
        progress.nodes = 0;
        owners++;
    }
    if (state.record_refs.load(std::memory_order_relaxed) != owners) std::abort();
    state.record_refs.store(0, std::memory_order_relaxed);
}

template <bool IofusedPrivateQueue>
uint32_t scatter_task_free_slots(ThreadCtx& owner, uint32_t producer) {
    if constexpr (IofusedPrivateQueue)
        return owner.iofused_task_free_slots(producer);
    else
        return owner.task_free_slots(producer);
}

template <bool IofusedPrivateQueue>
bool scatter_post_task_quiet(ThreadCtx& owner, uint32_t producer, const Task& task,
                             LoopSignals& signals) {
    if constexpr (IofusedPrivateQueue)
        return owner.post_iofused_task_quiet(producer, task, signals);
    else
        return owner.post_task_quiet(producer, task, signals);
}

template <bool IofusedPrivateQueue>
bool scatter_post_tasks_quiet(ThreadCtx& owner, uint32_t producer, const Task* tasks,
                              uint32_t count, LoopSignals& signals) {
    if constexpr (IofusedPrivateQueue)
        return owner.post_iofused_tasks_quiet(producer, tasks, count, signals);
    else
        return owner.post_tasks_quiet(producer, tasks, count, signals);
}

template <bool IofusedPrivateQueue>
bool publish_sort_deref(Server& server, ThreadCtx& self, Ring& ring, const Task& task,
                        ScatterState& state) {
    uint32_t needed[kMaxThreads] = {};
    uint32_t participants[kMaxThreads];
    uint16_t routed_owner[256];
    uint32_t nparticipants = 0;
    for (uint32_t i = 0; i < state.nsub; i++) {
        const uint32_t tid = server.worker_of_shard(state.groups[i].shard);
        routed_owner[i] = static_cast<uint16_t>(tid);
        if (needed[tid]++ == 0) participants[nparticipants++] = tid;
    }
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        if (scatter_task_free_slots<IofusedPrivateQueue>(
                server.thread(tid), self.id()) < needed[tid]) {
            state.final_reply = FinalReply::QueueFull;
            return false;
        }
    }
    state.pending.store(state.nsub, std::memory_order_release);
    Task posts[256];
    uint16_t participant_begin[kMaxThreads];
    uint32_t cursor = 0;
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        participant_begin[p] = static_cast<uint16_t>(cursor);
        cursor += needed[tid];
        needed[tid] = participant_begin[p];
    }
    for (uint32_t i = 0; i < state.nsub; i++)
        posts[needed[routed_owner[i]]++] = Task{
            task.client, task.op_id, state.groups[i].shard, &state};
    if (cursor != state.nsub) std::abort();
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        const uint32_t begin = participant_begin[p];
        const uint32_t end = p + 1 < nparticipants ? participant_begin[p + 1] : state.nsub;
        ThreadCtx& owner = server.thread(tid);
        if (!scatter_post_tasks_quiet<IofusedPrivateQueue>(
                owner, self.id(), posts + begin, end - begin, self.sig()))
            std::abort();
        owner.flush_task_notify(self.id(), ring, self.sig());
    }
    return true;
}

template <bool IofusedPrivateQueue>
bool publish_phase2(Server& server, ThreadCtx& self, Ring& ring, const Task& task, Op& op,
                    ScatterState& state) {
    reset_groups(state);
    build_groups(state, state.hop2, state.hop2_count);
    uint32_t needed[kMaxThreads] = {};
    uint32_t participants[kMaxThreads];
    uint16_t routed_owner[256];
    uint32_t nparticipants = 0;
    for (uint32_t i = 0; i < state.nsub; i++) {
        const uint32_t tid = server.worker_of_shard(state.groups[i].shard);
        routed_owner[i] = static_cast<uint16_t>(tid);
        if (needed[tid]++ == 0) participants[nparticipants++] = tid;
    }
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        if (scatter_task_free_slots<IofusedPrivateQueue>(
                server.thread(tid), self.id()) < needed[tid]) {
            state.final_reply = FinalReply::QueueFull;
            return false;
        }
    }
    state.phase = 2;
    if (!state.multi_child && !aof_prepare_scatter_group(state)) {
        state.aborted.store(true, std::memory_order_release);
        state.final_reply = FinalReply::Work;
        state.final_error = WorkError::Oom;
        return false;
    }
    arm_debug_off_hop_delay(state, op);
    state.pending.store(initialize_owner_completion(state), std::memory_order_release);
    Task posts[256];
    uint16_t participant_begin[kMaxThreads];
    uint32_t cursor = 0;
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        participant_begin[p] = static_cast<uint16_t>(cursor);
        cursor += needed[tid];
        needed[tid] = participant_begin[p];
    }
    for (uint32_t i = 0; i < state.nsub; i++)
        posts[needed[routed_owner[i]]++] = Task{
            task.client, task.op_id, state.groups[i].shard, &state};
    if (cursor != state.nsub) std::abort();
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        const uint32_t begin = participant_begin[p];
        const uint32_t end = p + 1 < nparticipants ? participant_begin[p + 1] : state.nsub;
        ThreadCtx& owner = server.thread(tid);
        if (!scatter_post_tasks_quiet<IofusedPrivateQueue>(
                owner, self.id(), posts + begin, end - begin, self.sig()))
            std::abort();
        owner.flush_task_notify(self.id(), ring, self.sig());
    }
    return true;
}

template <bool IofusedPrivateQueue>
bool publish_pop_retry(Server& server, ThreadCtx& self, Ring& ring, const Task& task,
                       ScatterState& state) {
    for (uint32_t i = 0; i < state.key_count; i++) {
        state.hop2[i] = i;
        state.images[i] = ObjectImage{};
    }
    if (state.status) std::memset(state.status, 0, state.key_count);
    if (state.result) {
        state.result->members.clear();
        state.result->scores.clear();
    }
    state.selected = std::numeric_limits<uint32_t>::max();
    state.pop_missed = false;
    reset_groups(state);
    build_groups(state, state.hop2, state.key_count);

    uint32_t needed[kMaxThreads] = {};
    uint32_t participants[kMaxThreads];
    uint16_t routed_owner[256];
    uint32_t nparticipants = 0;
    for (uint32_t i = 0; i < state.nsub; i++) {
        const uint32_t tid = server.worker_of_shard(state.groups[i].shard);
        routed_owner[i] = static_cast<uint16_t>(tid);
        if (needed[tid]++ == 0) participants[nparticipants++] = tid;
    }
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        if (scatter_task_free_slots<IofusedPrivateQueue>(
                server.thread(tid), self.id()) < needed[tid]) {
            state.final_reply = FinalReply::QueueFull;
            return false;
        }
    }

    state.phase = 1;
    state.pop_retry_snapshot = server.atomic_snapshot();
    state.pending.store(state.nsub, std::memory_order_release);
    Task posts[256];
    uint16_t participant_begin[kMaxThreads];
    uint32_t cursor = 0;
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        participant_begin[p] = static_cast<uint16_t>(cursor);
        cursor += needed[tid];
        needed[tid] = participant_begin[p];
    }
    for (uint32_t i = 0; i < state.nsub; i++)
        posts[needed[routed_owner[i]]++] = Task{
            task.client, task.op_id, state.groups[i].shard, &state};
    if (cursor != state.nsub) std::abort();
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        const uint32_t begin = participant_begin[p];
        const uint32_t end = p + 1 < nparticipants ? participant_begin[p + 1] : state.nsub;
        ThreadCtx& owner = server.thread(tid);
        if (!scatter_post_tasks_quiet<IofusedPrivateQueue>(
                owner, self.id(), posts + begin, end - begin, self.sig()))
            std::abort();
        owner.flush_task_notify(self.id(), ring, self.sig());
    }
    return true;
}

void free_state_contents(ScatterState& state) {
    if (state.contents_freed) return;
    for (uint32_t i = 0; i < state.group_cap; i++) delete state.groups[i].aux;
    for (uint32_t i = 0; state.images && i < state.key_count; i++) state.images[i].~ObjectImage();
    for (uint32_t i = 0; state.apply && i < state.key_count; i++) state.apply[i].~ObjectImage();
    delete state.result;
    if (state.sort_deref) state.sort_deref->~SortDerefState();
    state.contents_freed = true;
}

void free_state_key_anchors(ScatterState& state) {
    for (uint32_t i = 0; i < state.key_count; i++) {
        if (!state.keys[i].key_anchor) continue;
        kvobj_free(state.keys[i].key_anchor);
        state.keys[i].key_anchor = nullptr;
        state.keys[i].stable_object = nullptr;
    }
}

}  // namespace

ScatterArenaPool::~ScatterArenaPool() {
    reap_deferred();
    for (uint32_t i = 0; i < count_; i++) std::free(cached_[i]);
}

void* ScatterArenaPool::acquire(size_t bytes, bool& pooled) {
    pooled = bytes <= kCommonBytes;
    if (pooled && count_) return cached_[--count_];
    void* memory = nullptr;
    if (::posix_memalign(&memory, alignof(OwnerRecordRefs), pooled ? kCommonBytes : bytes) != 0)
        return nullptr;
    return memory;
}

void ScatterArenaPool::release(void* ptr, bool pooled) {
    if (!ptr) return;
    if (pooled && count_ < kCached) cached_[count_++] = ptr;
    else std::free(ptr);
}

ScatterPrepare xshard_prepare(Server& server, Op& op, ScatterArenaPool& pool,
                              uint32_t owner_io, uint64_t origin_conn_id,
                              ScatterDispatch& dispatch, bool force_atomic,
                              Client* origin_client) {
    const bool all_shards = (op.spec->flags & CmdFlags::AllShards) ||
                            ((op.spec->flags & CmdFlags::ConfigRoute) &&
                             command_config_routes_all_shards(op));
    Kind kind;
    if (all_shards)
        kind = op.cmd_name().eq_icase("dbsize") ? Kind::DbsizeExact :
               op.cmd_name().eq_icase("debug") && op.arg(1).eq_icase("borrowcount")
                   ? Kind::DebugBorrowCount :
               op.cmd_name().eq_icase("debug") && op.arg(1).eq_icase("loadaof")
                   ? Kind::DebugLoadAof :
               op.cmd_name().eq_icase("debug") ? Kind::DebugReload : Kind::AllShards;
    else if (!(op.spec->flags & CmdFlags::MultiShard) || !classify(op, kind))
        return ScatterPrepare::NotScatter;

    // DEBUG RELOAD/LOADAOF are asynchronous all-owner loads. Publish that fact before any file
    // planning so FLIP refuses instead of entering a quiescence transaction concurrently. A count
    // (not a bool) makes overlapping refused/debug attempts unable to clear each other's witness.
    struct RuntimeLoadingGuard {
        Server* server = nullptr;
        bool active = false;
        ~RuntimeLoadingGuard() { if (active) server->loading_end(); }
    } loading_guard;
    if (kind == Kind::DebugReload || kind == Kind::DebugLoadAof) {
        if (!server.loading_begin()) {
            reply_err(op.sink(), "ERR loading is not allowed while FLIP is in progress");
            return ScatterPrepare::Error;
        }
        loading_guard.server = &server;
        loading_guard.active = true;
    }

    uint32_t script_first = 0, script_count = 0;
    if (kind == Kind::Script) {
        if (!command_validate_script_route(op, script_first, script_count))
            return ScatterPrepare::Error;
        // Zero/one-key and feature-off activations retain the established local route. The later
        // ScriptRoute pass owns the exact CROSSSLOT refusal when a disabled boot sees >1 owner.
        if (script_count <= 1 || server.script_crossshard_max_bytes() == 0 ||
            server.script_crossshard_workbench_bytes() == 0 ||
            server.script_crossshard_cut_slots() == 0)
            return ScatterPrepare::NotScatter;
    }

    const uint64_t atomic_mode = server.atomic_mode_state();
    const bool tracking = atomic_mode != 0;
    const bool atomic_enabled = force_atomic || (atomic_mode & Server::kAtomicEnabledBit);
    const bool script_guard = server.script_intents_active();
    const bool atomic_write = (atomic_enabled || script_guard) && is_atomic_write_kind(kind);

    // The ordinary handlers are already the optimal one-key path.
    if ((kind == Kind::Del || kind == Kind::Unlink || kind == Kind::Exists ||
         kind == Kind::Touch) && op.argc() == 2) return ScatterPrepare::NotScatter;
    if (kind == Kind::Pfcount && op.argc() == 2) return ScatterPrepare::NotScatter;

    XshardKeyHints key_hints;
    if (kind == Kind::Script) {
        key_hints.first = script_first;
        key_hints.count = script_count;
    }
    std::unique_ptr<SnapshotLoadPlan> reload_plan;
    std::unique_ptr<SnapshotLoadPlan> aof_reload_base;
    std::vector<std::unique_ptr<AofReplayPlan>> aof_reload_increments;
    if (all_shards) {
        if (kind == Kind::DebugBorrowCount) {
            if (!debug_command_allowed(server, origin_client)) {
                reply_debug_command_denied(op);
                return ScatterPrepare::Error;
            }
        } else if (kind == Kind::DebugReload) {
            if (!debug_command_allowed(server, origin_client)) {
                reply_debug_command_denied(op);
                return ScatterPrepare::Error;
            }
            ThreadCtx& writer = server.thread(owner_io);
            Ring* writer_ring = writer.ring();
            if (!writer_ring) {
                reply_err(op.sink(), "ERR snapshot command has no IO owner");
                return ScatterPrepare::Error;
            }
            std::string error;
            const SnapshotManager::StartResult saved = server.snapshot().start(
                server, writer, *writer_ring, true, error);
            if (saved == SnapshotManager::StartResult::Busy) {
                reply_err(op.sink(), "ERR Background save already in progress");
                return ScatterPrepare::Error;
            }
            if (saved == SnapshotManager::StartResult::Failed) {
                std::string message = "ERR ";
                message += error.empty() ? "snapshot failed" : error;
                reply_err(op.sink(), message.c_str());
                return ScatterPrepare::Error;
            }
            const std::string path = server.snapshot().dir() + "/" +
                                     server.snapshot().dbfilename();
            reload_plan = snapshot_read_plan(path.c_str(), server.nshards(), error);
            if (!reload_plan) {
                reply_err(op.sink(),
                          "ERR Error trying to load the snapshot dump, check server logs.");
                return ScatterPrepare::Error;
            }
        } else if (kind == Kind::DebugLoadAof) {
            if (!debug_command_allowed(server, origin_client)) {
                reply_debug_command_denied(op);
                return ScatterPrepare::Error;
            }
            if (!server.aof().configured()) {
                reply_err(op.sink(), "ERR appendonly is disabled");
                return ScatterPrepare::Error;
            }
            ThreadCtx& caller = server.thread(owner_io);
            Ring* caller_ring = caller.ring();
            if (server.aof().writer_is(owner_io) && caller_ring) {
                while (server.aof().pending_chunks()) {
                    if (!server.aof().writer_pass(caller, *caller_ring, true))
                        std::this_thread::yield();
                }
            } else if (!server.aof().wait_until_drained(5000)) {
                reply_err(op.sink(), "ERR timed out flushing AOF before reload");
                return ScatterPrepare::Error;
            }
            std::string warning;
            std::string error;
            if (!aof_read_recovery(server.cfg(), server.nshards(), aof_reload_base,
                                   aof_reload_increments, warning, error) ||
                (!aof_reload_base && aof_reload_increments.empty())) {
                reply_err(op.sink(), "ERR Error trying to load the AOF, check server logs.");
                return ScatterPrepare::Error;
            }
        } else {
            const bool valid = (op.spec->flags & CmdFlags::ConfigRoute)
                ? command_validate_config_set(op) : command_validate_all_shards(op);
            if (!valid) return ScatterPrepare::Error;
        }
    }
    if ((kind == Kind::Mset || kind == Kind::Msetnx) && (op.argc() & 1u) == 0) {
        reply_err(op.sink(), name_is(op, "MSET")
            ? "ERR wrong number of arguments for 'mset' command"
            : "ERR wrong number of arguments for 'msetnx' command");
        return ScatterPrepare::Error;
    }
    if (kind == Kind::Bitop) {
        BitOperation operation;
        if (!bitop_operation(op.arg(1), operation)) {
            reply_syntax(op.sink());
            return ScatterPrepare::Error;
        }
        if (operation == BitOperation::Not && op.argc() != 4) {
            reply_err(op.sink(), "ERR BITOP NOT must be called with a single source key.");
            return ScatterPrepare::Error;
        }
    }

    uint64_t sinter_limit = 0;
    LcsOptions lcs_options;
    uint32_t sinter_count = 0;
    // LCS parses its trailing option words on IO, exactly like SINTERCARD: the owners receive a
    // fully validated request and the parsed scalars ride in the arena header.
    if (kind == Kind::Lcs && !lcs_parse_options(op, lcs_options))
        return ScatterPrepare::Error;
    if (kind == Kind::Sintercard) {
        // Both scalars come from getRangeLongFromObject with a message of their own, so an
        // unparseable numkeys and a numkeys of 0 answer alike, and likewise for LIMIT. Only the
        // "more keys than arguments" case has its own text.
        int64_t parsed = 0;
        if (!parse_i64(op.arg(1), parsed) || parsed < 1) {
            reply_err(op.sink(), "ERR numkeys should be greater than 0");
            return ScatterPrepare::Error;
        }
        if (static_cast<uint64_t>(parsed) > op.argc() - 2) {
            reply_err(op.sink(), "ERR Number of keys can't be greater than number of args");
            return ScatterPrepare::Error;
        }
        sinter_count = static_cast<uint32_t>(parsed);
        const uint32_t tail = 2 + sinter_count;
        if (tail < op.argc()) {
            int64_t parsed_limit = 0;
            if (tail + 2 != op.argc() || !op.arg(tail).eq_icase("limit")) {
                reply_syntax(op.sink());
                return ScatterPrepare::Error;
            }
            if (!parse_i64(op.arg(tail + 1), parsed_limit) || parsed_limit < 0) {
                reply_err(op.sink(), "ERR LIMIT can't be negative");
                return ScatterPrepare::Error;
            }
            sinter_limit = static_cast<uint64_t>(parsed_limit);
        }
        key_hints.count = sinter_count;
    }

    uint32_t zset_count = 0;
    uint64_t zset_limit = 0;
    std::vector<double> zset_weights;
    ZAggregate zset_aggregate = ZAggregate::Sum;
    bool zset_withscores = false;
    if (is_any_zsetop(kind) &&
        !parse_zset_operation(op, kind, zset_count, zset_weights, zset_aggregate,
                              zset_withscores, zset_limit))
        return ScatterPrepare::Error;
    if (is_any_zsetop(kind)) key_hints.count = zset_count;

    GeoRoute geo_route;
    if (is_geo_kind(kind) && !geo_prepare_route(op, geo_route))
        return ScatterPrepare::Error;
    if (is_geo_kind(kind)) {
        key_hints.store = geo_route.store;
        key_hints.source = geo_route.source_arg;
        key_hints.destination = geo_route.destination_arg;
    }

    uint32_t mpop_count = 0;
    bool pop_edge = false;
    uint64_t pop_count = 1;
    if ((kind == Kind::Lmpop || kind == Kind::Zmpop) &&
        !parse_mpop(op, kind, mpop_count, pop_edge, pop_count))
        return ScatterPrepare::Error;
    if (kind == Kind::Lmpop || kind == Kind::Zmpop) key_hints.count = mpop_count;

    ImageRangeOptions range_options;
    ImageRangeSpec range_spec;
    if (kind == Kind::Zrangestore &&
        (!parse_zrangestore_options(op, range_options) ||
         !parse_zrangestore_range(op, range_options, range_spec)))
        return ScatterPrepare::Error;

    SortOptions sort_options;
    if (kind == Kind::Sort && !parse_sort_options(op, sort_options))
        return ScatterPrepare::Error;
    if (kind == Kind::Sort) {
        key_hints.store = sort_options.store;
        key_hints.destination = sort_options.store_arg;
    }
    StreamXreadArgs xread_options;
    if (kind == Kind::Xread && !stream_parse_xread(op, xread_options))
        return ScatterPrepare::Error;
    StreamXreadGroupArgs xreadgroup_options;
    if (kind == Kind::Xreadgroup) {
        if (!stream_parse_xreadgroup(op, xreadgroup_options)) return ScatterPrepare::Error;
        xread_options.first_key = xreadgroup_options.key_arg;
        xread_options.key_count = 1;
        xread_options.block = xreadgroup_options.block && xreadgroup_options.new_entries;
    }
    if (kind == Kind::Xread || kind == Kind::Xreadgroup) {
        key_hints.first = xread_options.first_key;
        key_hints.count = xread_options.key_count;
    }
    const bool atomic_apply = kind == Kind::Script || ((atomic_enabled || script_guard) &&
        is_atomic_apply_kind(kind, sort_options.store, geo_route.store));
    const bool atomic_pop = (atomic_enabled || script_guard) && is_atomic_pop_kind(kind);
    const bool atomic_group = atomic_write || atomic_apply || atomic_pop;
    const bool atomic_direct = atomic_apply && kind == Kind::Rename;

    bool copy_replace = false, from_left = false, to_left = false;
    if (kind == Kind::Copy) {
        for (uint32_t i = 3; i < op.argc();) {
            if (op.arg(i).eq_icase("replace")) { copy_replace = true; i++; }
            else if (op.arg(i).eq_icase("db") && i + 1 < op.argc()) {
                // Three answers, in redis's order: not an integer, outside the int range it
                // parses into, and only then "out of range" for the database itself. This server
                // has exactly one database, so every index but 0 lands on the last one -- but a
                // typo must still be reported as a typo.
                int64_t db = 0;
                if (!parse_i64(op.arg(i + 1), db)) {
                    reply_err(op.sink(), "ERR value is not an integer or out of range");
                    return ScatterPrepare::Error;
                }
                if (db < INT32_MIN || db > INT32_MAX) {
                    reply_err(op.sink(), "ERR value is out of range, value must between "
                                         "-2147483648 and 2147483647");
                    return ScatterPrepare::Error;
                }
                if (db != 0) {
                    reply_err(op.sink(), "ERR DB index is out of range");
                    return ScatterPrepare::Error;
                }
                i += 2;
            } else {
                reply_syntax(op.sink());
                return ScatterPrepare::Error;
            }
        }
    }
    if (kind == Kind::Lmove) {
        if (!(op.arg(3).eq_icase("left") || op.arg(3).eq_icase("right")) ||
            !(op.arg(4).eq_icase("left") || op.arg(4).eq_icase("right"))) {
            reply_syntax(op.sink());
            return ScatterPrepare::Error;
        }
        from_left = op.arg(3).eq_icase("left");
        to_left = op.arg(4).eq_icase("left");
    } else if (kind == Kind::Rpoplpush) {
        from_left = false; to_left = true;
    }

    XshardKeyPlan classified_keys;
    if (!classify_keys(op, kind, key_hints, classified_keys)) {
        reply_err(op.sink(), "ERR internal cross-shard key classification error");
        return ScatterPrepare::Error;
    }
    const uint32_t key_count = key_count_for(classified_keys);
    RouteKey route_stack[kRouteStack];
    RouteKey* routes = route_stack;
    if (key_count > kRouteStack) {
        routes = new (std::nothrow) RouteKey[key_count];
        if (!routes) { set_oom(op); return ScatterPrepare::Error; }
    }
    for (uint32_t i = 0; i < key_count; i++) {
        const uint32_t arg = key_arg_for(classified_keys, i);
        const uint64_t hash = FlatStore::hash_key(op.arg(arg));
        routes[i] = RouteKey{hash, arg, server.router().shard_of(hash)};
    }

    // Every explicit key, including a STORE destination, must agree before taking localfast.
    if (key_count) {
        bool same_owner = true;
        for (uint32_t i = 1; i < key_count; i++) same_owner &= routes[i].shard == routes[0].shard;
        // Common same-owner commands retain localfast. If an atomic window touches one of their
        // keys, the owner resolves it through the short pending-entry list and clones only writes.
        const bool xread_kind = kind == Kind::Xread || kind == Kind::Xreadgroup;
        const bool xread_local = xread_kind && key_count == 1 && !xread_options.block;
        // A SORT that dereferences BY/GET patterns must NOT take localfast. Its derived keys are
        // read on shards the routed group never named, and the read cut and originating connection
        // are per-store: only the scatter path carries both (state.snapshot / state.origin_conn_id)
        // to bind on those shards. Without them a weight key written by this very connection reads
        // as absent -- which is what the differ caught, on whichever boot placed the weight keys
        // off the source's shard.
        const bool sort_deref_group = kind == Kind::Sort && sort_options.deref;
        if (same_owner && !force_atomic && !sort_deref_group &&
            kind != Kind::Pfcount && kind != Kind::Pfmerge &&
            (!(op.spec->flags & CmdFlags::Blocking) || xread_local) &&
            (!xread_kind || xread_local)) {
            op.shard = routes[0].shard;
            op.hash = routes[0].hash;
            op.mark_local_xshard();
            if (atomic_group) server.thread(owner_io).note_atomic_localfast();
            if (routes != route_stack) delete[] routes;
            return ScatterPrepare::NotScatter;
        }
    }

    uint32_t phase1_first = (is_store_setop(kind) || is_store_zsetop(kind) ||
                             (is_geo_kind(kind) && geo_route.store) || kind == Kind::Bitop ||
                             kind == Kind::Zrangestore ||
                             (kind == Kind::Sort && sort_options.store)) ? 1u : 0u;
    uint32_t group_cap = 0;
    if (kind == Kind::AllShards || kind == Kind::DebugReload || kind == Kind::DebugLoadAof ||
        kind == Kind::DebugBorrowCount || kind == Kind::DbsizeExact || kind == Kind::Keys) {
        group_cap = server.nshards();
    } else if (atomic_write) {
        bool seen[256] = {};
        for (uint32_t i = phase1_first; i < key_count; i++)
            if (!seen[static_cast<uint32_t>(routes[i].shard)]) {
                seen[static_cast<uint32_t>(routes[i].shard)] = true;
                group_cap++;
            }
    } else {
        for (uint32_t i = phase1_first; i < key_count; i++) {
            bool first = true;
            for (uint32_t prior = phase1_first; prior < i; prior++)
                if (routes[prior].shard == routes[i].shard) {
                    first = false;
                    break;
                }
            group_cap += first;
        }
    }
    // The source wave names only one explicit key, but a derived-key gather can touch every shard.
    // Reserve the dense group table now; the request/order vectors themselves are built later in
    // the SORT-only arena sidecar once the source elements are known.
    if (kind == Kind::Sort && sort_options.deref) group_cap = server.nshards();
    // A CROSS-SHARD READ CARRIES A CUT WHETHER OR NOT A GROUP HAPPENS TO BE IN FLIGHT RIGHT NOW.
    // `tracking` is a sample of atomic_activity_, and EXEC is what writes that word: EXEC
    // force-admits (multi.inc:948) even at --atomic 0, so the word reads zero in every gap between
    // transactions. A read prepared in such a gap left state->snapshot at UINT64_MAX, which makes
    // ReadEpochGuard (below) leave the owner unbound, and every fragment then answers "newest
    // committed at the instant this fragment runs". A transaction publishing its one ticket
    // (multi.inc:1352) while the fan-out is in flight is therefore seen by the fragments that
    // execute after it and missed by those that executed before -- one MGET, two transactions.
    // Contention does not cause that; it only widens the fan-out from microseconds to milliseconds,
    // which is why a quiet gate never caught it.
    // Scoped exactly to the commands that can straddle: a read whose fragments run on more than one
    // owner. A single-owner scatter resolves in one task and cannot straddle anything, and a write
    // group keeps the old gate because at --atomic 0 it is not a group and promises concurrent
    // readers nothing that a cut would buy.
    const bool read_fanout = group_cap > 1 &&
        !(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite));
    // A dereferencing SORT is a fan-out READ even when STORE also makes the command a write. It must
    // carry one cut at atomic mode 0 as well. A forced child inside EXEC borrows the transaction's
    // registered cut later and therefore must not register its private scatter pool here.
    const bool sort_deref_snapshot = !force_atomic && kind == Kind::Sort && sort_options.deref;
    const bool needs_snapshot = kind == Kind::Script || sort_deref_snapshot ||
        (!force_atomic && (tracking || read_fanout) &&
         (key_count || kind == Kind::Keys || kind == Kind::DbsizeExact) &&
         !(atomic_write && kind == Kind::Mset));
    if (kind == Kind::Script &&
        !pool.can_register_snapshot(server.script_crossshard_cut_slots())) {
        if (routes != route_stack) delete[] routes;
        server.note_script_window_refusal();
        reply_err(op.sink(), "BUSY Cross-shard script snapshot window is full");
        return ScatterPrepare::Error;
    }
    if (needs_snapshot && (atomic_mode & ~Server::kAtomicEnabledBit) &&
        !pool.can_register_snapshot()) {
        if (routes != route_stack) delete[] routes;
        return ScatterPrepare::Backpressure;
    }
    bool admitted = false;
    uint64_t admission_generation = 0;
    if (atomic_group && !force_atomic) {
        if (!server.atomic_try_admit(owner_io, admission_generation,
                                     kind == Kind::Script || script_guard)) {
            if (routes != route_stack) delete[] routes;
            return ScatterPrepare::Backpressure;
        }
        admitted = true;
    }
    const size_t bytes = arena_layout_bytes(
        kind, key_count, group_cap, server.nshards(), atomic_write, atomic_apply,
        server.nthreads(), sort_options.deref);
    if (bytes == std::numeric_limits<size_t>::max() || bytes > UINT32_MAX) {
        if (admitted) server.atomic_retire_group(owner_io, admission_generation);
        if (routes != route_stack) delete[] routes;
        set_oom(op);
        return ScatterPrepare::Error;
    }
    bool pooled = false;
    void* memory = pool.acquire(bytes, pooled);
    if (!memory) {
        if (admitted) server.atomic_retire_group(owner_io, admission_generation);
        if (routes != route_stack) delete[] routes;
        set_oom(op);
        return ScatterPrepare::Error;
    }
    auto* state = new (memory) ScatterState;
    state->kind = kind;
    state->runtime_loading = loading_guard.active;
    loading_guard.active = false;
    state->atomic_write = atomic_write;
    state->atomic_apply = atomic_apply;
    state->atomic_pop = atomic_pop;
    state->atomic_group = atomic_group;
    state->atomic_direct = atomic_direct;
    if (__builtin_expect(atomic_direct, false))
        state->debug_direct_defer = server.debug_atomic_direct_defer();
    state->phase = atomic_direct ? 2 : 1;
    state->script_phase = kind == Kind::Script ? ScriptPhase::Pin : ScriptPhase::None;
    state->two_hop = is_two_hop(kind) && !atomic_write && !atomic_direct;
    state->barrier = state->two_hop || all_shards;
    state->admitted = admitted;
    state->server = &server;
    // Opened only once state->server is set, so "apply_open is true" always implies xshard_destroy
    // can close it. The two failure arms above return before any ScatterState exists at all, so
    // the window can never be opened on a group with nothing to close it.
    if (__builtin_expect(admitted, false))
        server.atomic_apply_open(owner_io, state->apply_open);
    state->pooled = pooled;
    state->owner_io = owner_io;
    state->owner_slots = server.nthreads();
    state->admission_generation = admission_generation;
    state->origin_conn_id = origin_conn_id;
    state->script_coordinator_shard = kind == Kind::Script && key_count
        ? routes[0].shard : -1;
    // One wall-clock read per cross-shard group, taken here because this is the point the
    // connection's program order already uses for the commit cut a few lines below. Every owner of
    // this group then answers deadlines against this one instant, which is what the reference does
    // for a multi-key read (redis pins server.mstime for the whole command).
    state->now_cut_ms = now_realtime_ms();
    state->arena_bytes = static_cast<uint32_t>(bytes);
    state->key_count = key_count;
    state->group_cap = group_cap;
    state->shard_slots = (atomic_write || atomic_apply) ? server.nshards() : 0;
    state->phase1_first = phase1_first;
    state->copy_replace = copy_replace;
    state->from_left = from_left;
    state->to_left = to_left;
    state->pop_edge = pop_edge;
    state->pop_count = pop_count;
    state->xread_count = xread_options.count;
    state->xread_first_key = xread_options.first_key;
    state->xread_first_id = xread_options.first_id;
    state->sinter_limit = sinter_limit;
    state->zset_limit = zset_limit;
    state->zset_aggregate = zset_aggregate;
    state->zset_withscores = zset_withscores;
    state->zset_weights = std::move(zset_weights);
    state->geo_store = geo_route.store;
    state->geo_store_distance = geo_route.store_distance;
    state->geo_source_arg = geo_route.source_arg;
    state->geo_destination_arg = geo_route.destination_arg;
    state->lcs_options = lcs_options;
    state->range_options = range_options;
    state->range_spec = range_spec;
    state->sort_options = sort_options;
    state->reload_plan = std::move(reload_plan);
    state->aof_reload_base = std::move(aof_reload_base);
    state->aof_reload_increments = std::move(aof_reload_increments);
    // MSET is a blind replacement. Registering a read cut for it only pins predecessors across a
    // deep pipeline; MSETNX and delete still need their one dispatch snapshot for existence.
    // A read that IO already stamped reuses that exact cut, so a plain read and a multi-key read
    // prepared back to back on one connection cannot disagree about which of them is older.
    if (needs_snapshot) {
        state->snapshot = op.has_read_cut() ? op.read_cut(server.atomic_snapshot())
                                            : server.atomic_snapshot();
        // Vacuous-validation guard: this counts exactly the reads the old `tracking` gate sent out
        // cut-less. A regression for the fan-out defect that never moves it never armed the window.
        if (!tracking) server.note_atomic_fanout_cut();
        // Instrumentation, once per multi-key command and never on the GET/SET path: the visible
        // watermark trailing the drawn sequence is the publication guard actively excluding an
        // undecided ticket from this read's cut. A battery that never sees this counter move never
        // entered the window it claims to close.
        if (__builtin_expect(server.atomic_snapshot() < server.atomic_commit_drawn(), false))
            server.note_atomic_commit_hold();
    }
    if (kind == Kind::Sintercard) { state->set_first = 0; state->set_count = sinter_count; }
    else if (is_store_setop(kind)) { state->set_first = 1; state->set_count = key_count - 1; }
    else if (is_plain_setop(kind)) { state->set_first = 0; state->set_count = key_count; }
    else if (is_store_zsetop(kind)) { state->set_first = 1; state->set_count = zset_count; }
    else if (is_plain_zsetop(kind) || kind == Kind::Zintercard) {
        state->set_first = 0;
        state->set_count = zset_count;
    }
    init_arena_arrays(*state);
    for (uint32_t i = 0; i < key_count; i++)
        state->keys[i] = KeyRef{routes[i].hash, routes[i].arg, routes[i].shard};
    if (routes != route_stack) delete[] routes;

    // Blind string values belong to the op's IO thread until an owner links them. Allocation and
    // memcpy therefore run on the underutilized stage without touching a shard or shard metadata.
    // key_anchor is the ownership bit: an owner clears it only after atomic_install_group takes the
    // object; abandonment and prepare failures leave it for IO-side destruction.
    if (atomic_write && (kind == Kind::Mset || kind == Kind::Msetnx)) {
        for (uint32_t i = 0; i < key_count; i++) {
            KeyRef& key = state->keys[i];
            // Small/int objects already come from the shard owner's hot value pool. Moving those to
            // generic jemalloc regressed MSET-8 despite shifting cycles to IO. Large strings bypass
            // that pool in V3, so prebuild precisely the allocation+memcpy work that was already a
            // heap operation and preserve the allocation-free common cell.
            if (op.arg(key.arg + 1).n <= kEmbedThreshold) continue;
            KvObj* value = xshard_make_string(op.arg(key.arg), op.arg(key.arg + 1));
            if (!value) {
                set_oom(op);
                xshard_abandon_unpublished(state, pool, owner_io);
                return ScatterPrepare::Error;
            }
            key.stable_object = value;
            key.key_anchor = value;
        }
    }

    if (state->snapshot != UINT64_MAX && !pool.register_snapshot(server, state, owner_io)) {
        set_oom(op);
        xshard_abandon_unpublished(state, pool, owner_io);
        return ScatterPrepare::Error;
    }

    if (kind == Kind::AllShards || kind == Kind::DebugReload || kind == Kind::DebugLoadAof ||
        kind == Kind::DebugBorrowCount || kind == Kind::DbsizeExact || kind == Kind::Keys) {
        state->nsub = server.nshards();
        for (uint32_t sid = 0; sid < state->nsub; sid++) {
            state->groups[sid].shard = sid;
            if (state->shard_groups)
                state->shard_groups[sid] = static_cast<uint16_t>(sid);
        }
    } else {
        build_initial_groups(*state);
    }
    arm_debug_off_hop_delay(*state, op);
    // Only the deliberately non-atomic conditional controls arm this window. Atomic-ON movers use
    // destination reservation/revalidation and must retain their unmodified scheduling semantics.
    if (__builtin_expect(state->nsub > 1 && !state->atomic_apply &&
                         (kind == Kind::Renamenx ||
                          (kind == Kind::Copy && !copy_replace)), false)) {
        const uint64_t conditional_deadline = server.debug_atomic_conditional_deadline();
        if (__builtin_expect(conditional_deadline != 0, false) &&
            now_ns() < conditional_deadline)
            state->debug_conditional_deadline = conditional_deadline;
    }
    // TEST HOOK arming, after the groups exist so a lead shard can be named. Only cross-shard READS
    // arm it: they are the commands whose fragments each answer at their own moment.
    if (__builtin_expect(state->nsub > 1, false) &&
        !(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) {
        const uint32_t fanout_defer = server.debug_atomic_fanout_defer();
        if (__builtin_expect(fanout_defer != 0, false)) {
            state->debug_fanout_lead = state->groups[0].shard;
            state->debug_fanout_deadline =
                now_ns() + static_cast<uint64_t>(fanout_defer) * 1000ull;
        }
    }
    // TEST HOOK arming for the cross-owner script GATHER wave. A script is a Write row, so it can
    // never reach the read-only arm above; it gets its own deadline, consumed in the Read phase.
    if (__builtin_expect(kind == Kind::Script, false)) {
        const uint32_t stage_defer = server.debug_script_stage_defer();
        if (__builtin_expect(stage_defer != 0, false))
            state->debug_script_defer_us = stage_defer;
    }
    state->pending.store(initialize_owner_completion(*state), std::memory_order_relaxed);
    if (state->atomic_group && !force_atomic &&
        (state->atomic_write || state->atomic_direct) &&
        !aof_prepare_scatter_group(*state)) {
        xshard_abandon_unpublished(state, pool, owner_io);
        set_oom(op);
        return ScatterPrepare::Error;
    }
    dispatch.state = state;
    dispatch.nshards = state->nsub;
    dispatch.barrier = state->barrier;
    dispatch.atomic_write = state->atomic_group;
    return ScatterPrepare::Ready;
}

int32_t xshard_dispatch_shard(const ScatterDispatch& dispatch, uint32_t index) {
    if (!dispatch.state) return -1;
    return index < dispatch.state->nsub ? dispatch.state->groups[index].shard : -1;
}

void xshard_abandon_unpublished(ScatterState* state, ScatterArenaPool& pool,
                                uint32_t owner_io) {
    if (!state) return;
    // No task was published, hence no AtomicEntry can name these counts. Clear only the pre-counted
    // phantom pins; ordinary destruction must leave installed-node lifetime accounting untouched.
    clear_unpublished_owner_record_refs(*state);
    xshard_destroy(state, pool, owner_io);
}

void xshard_destroy(ScatterState* state, ScatterArenaPool& pool, uint32_t owner_io) {
    if (!state) return;
    if (__builtin_expect(atomic_tripwire_enabled(), false))
        atomic_tripwire_forget_scope(state);
    if (state->owner_io != owner_io) std::abort();
    if (state->snapshot_registered && state->server)
        pool.unregister_snapshot(*state->server, state, state->owner_io);
    // Ordinary completions closed this on the owner that installed the last record; this arm is the
    // one that matters for a group torn down BEFORE dispatch (no owner ever sees it).
    if (state->server)
        state->server->atomic_apply_close(owner_io, state->apply_open);
    if (state->admitted && state->server) {
        state->server->atomic_retire_group(state->owner_io, state->admission_generation);
        state->admitted = false;
    }
    if (state->script_certifier && state->server) {
        state->server->script_finish_certification();
        state->script_certifier = false;
    }
    if (state->runtime_loading && state->server) {
        state->server->loading_end();
        state->runtime_loading = false;
    }
    if (state->aof_group && state->aof_group->ticket.load(std::memory_order_acquire) == 0)
        aof_abort_scatter_group(*state);
    const bool pooled = state->pooled;
    free_state_contents(*state);
    state->retired = true;
    if (state->record_refs.load(std::memory_order_acquire) != 0 ||
        state->watch_refs.load(std::memory_order_acquire) != 0) {
        pool.defer_destroy(state);
        return;
    }
    free_state_key_anchors(*state);
    state->~ScatterState();
    pool.release(state, pooled);
}

bool xshard_is_local(const Op& op) { return op.local_xshard(); }

void xshard_aof_emit(const Task& task, Shard& shard, Op& op, AofOwnerContext& context) {
    ScatterState& state = *task.scatter;
    if (!xshard_owner_mutation_phase(state, op)) return;
    if (state.atomic_group) {
        if (!state.aof_group) return;
        ShardGroup* group = group_for(state, shard.id());
        if (!group || !group->count) return;
        AofProducer& producer = shard.store().aof();
        if (!producer.begin_group(state.aof_group)) return;
        bool ok = true;
        for (uint32_t i = 0; i < group->count; i++) {
            const uint32_t pos = state.key_order[group->begin + i];
            const KeyRef& key = state.keys[pos];
            ok &= state.atomic_pop
                ? producer.record_group_visible_post_image(
                      shard.store(), key.hash, op.arg(key.arg))
                : producer.record_group_post_image(
                      shard.store(), key.hash, op.arg(key.arg));
        }
        ok &= producer.finish_group();
        if (!ok) aof_abort_scatter_group(state);
        (void)context;
        return;
    }
    if (state.kind == Kind::AllShards) {
        if (op.cmd_name().eq_icase("flushdb") || op.cmd_name().eq_icase("flushall"))
            (void)shard.store().aof().record_flush();
        return;
    }
    if (state.kind == Kind::DebugReload || state.kind == Kind::DebugLoadAof ||
        state.kind == Kind::DebugBorrowCount || state.kind == Kind::DbsizeExact ||
        state.kind == Kind::Keys) return;
    ShardGroup* group = group_for(state, shard.id());
    if (!group) return;
    for (uint32_t i = 0; i < group->count; i++) {
        const uint32_t pos = state.key_order[group->begin + i];
        const KeyRef& key = state.keys[pos];
        (void)shard.store().aof().record_post_image(
            shard.store(), key.hash, op.arg(key.arg), context);
    }
}

void xshard_aof_emit_local(Shard& shard, Op& op, AofOwnerContext& context) {
    // Script commands need grouped AOF treatment before the ordinary local post-image policy.
    // Their declared KEYS range is the closed touched set enforced by the Lua bridge. Preserve one
    // script activation as one AOF group so recovery can never apply only part of a multi-key
    // effect. The *_RO rows are Readonly and must not produce post images.
    if (op.spec->flags & CmdFlags::ScriptRoute) {
        if (!(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) return;
        uint32_t first = 0, count = 0;
        if (!command_script_key_range(op, first, count) || count == 0) return;
        Server* server = shard.server();
        if (!server || !server->aof().recording()) return;

        std::vector<uint32_t> participants;
        try { participants.push_back(static_cast<uint32_t>(shard.id())); }
        catch (const std::bad_alloc&) {
            server->aof().fail("out of memory collecting script AOF group participant");
            return;
        }
        std::shared_ptr<AofGroupDecision> group =
            aof_create_group(server->aof(), participants);
        if (!group) return;

        AofProducer& producer = shard.store().aof();
        if (!producer.begin_group(group)) {
            aof_abort_group(group);
            return;
        }
        bool ok = true;
        for (uint32_t i = 0; i < count; i++) {
            const Slice key = op.arg(first + i);
            ok &= producer.record_group_post_image(
                shard.store(), FlatStore::hash_key(key), key);
        }
        ok &= producer.finish_group();
        if (!ok) {
            aof_abort_group(group);
            return;
        }
        const uint64_t ticket = server->atomic_commit();
        (void)aof_commit_group(server->aof(), group, ticket, context);
        return;
    }

    Kind kind;
    if (!classify(op, kind)) return;
    auto emit = [&](uint32_t arg) {
        if (arg >= op.argc()) return;
        const Slice key = op.arg(arg);
        (void)shard.store().aof().record_post_image(
            shard.store(), FlatStore::hash_key(key), key, context);
    };
    switch (kind) {
        case Kind::Mset:
        case Kind::Msetnx:
            for (uint32_t arg = 1; arg < op.argc(); arg += 2) emit(arg);
            break;
        case Kind::Del:
        case Kind::Unlink:
            for (uint32_t arg = 1; arg < op.argc(); arg++) emit(arg);
            break;
        case Kind::Rename:
        case Kind::Renamenx:
        case Kind::Copy:
        case Kind::Smove:
        case Kind::Lmove:
        case Kind::Rpoplpush:
            emit(1); emit(2);
            break;
        case Kind::Sinterstore:
        case Kind::Sunionstore:
        case Kind::Sdiffstore:
        case Kind::Zunionstore:
        case Kind::Zinterstore:
        case Kind::Zdiffstore:
        case Kind::Pfmerge:
        case Kind::Zrangestore:
            emit(1);
            break;
        case Kind::Bitop:
            emit(2);
            break;
        case Kind::Xreadgroup: {
            StreamXreadGroupArgs parsed;
            if (stream_parse_xreadgroup(op, parsed)) emit(parsed.key_arg);
            break;
        }
        case Kind::Lmpop:
        case Kind::Zmpop: {
            uint32_t numkeys = 0;
            bool edge = false;
            uint64_t count = 1;
            if (!parse_mpop(op, kind, numkeys, edge, count)) return;
            for (uint32_t i = 0; i < numkeys; i++) emit(2 + i);
            break;
        }
        case Kind::Sort: {
            SortOptions options;
            if (parse_sort_options(op, options) && options.store)
                emit(options.store_arg);
            break;
        }
        case Kind::Geosearchstore:
        case Kind::Georadius:
        case Kind::Georadiusbymember: {
            GeoRoute route;
            if (geo_prepare_route(op, route) && route.store) emit(route.destination_arg);
            break;
        }
        default:
            (void)aof_record_local_op(shard, op, context);
            break;
    }
}

namespace {

enum class ScriptPostResult : uint8_t { Posted, Empty, Full };

bool script_group_error(const ScatterState& state, WorkError& error) {
    for (uint32_t i = 0; i < state.nsub; i++) {
        if (state.groups[i].error == WorkError::None) continue;
        error = state.groups[i].error;
        return true;
    }
    return false;
}

void script_clear_attempt(ScatterState& state, Op& op) {
    op.clear_reply();
    if (state.status) std::memset(state.status, 0, state.key_count);
    for (uint32_t i = 0; i < state.key_count; i++) {
        state.images[i] = ObjectImage{};
        state.apply[i] = ObjectImage{};
        state.keys[i].stable_object = nullptr;
        if (state.keys[i].key_anchor) {
            kvobj_free(state.keys[i].key_anchor);
            state.keys[i].key_anchor = nullptr;
        }
    }
    state.script_staged_bytes.store(0, std::memory_order_relaxed);
    state.script_conflict.store(false, std::memory_order_relaxed);
    notify_discard_batch(state.notify.exchange(nullptr));
}

uint32_t script_select_positions(ScatterState& state, ScriptPhase phase) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < state.key_count; i++) {
        bool selected = phase == ScriptPhase::Read || phase == ScriptPhase::Unpin;
        if (phase == ScriptPhase::Validate) selected = (state.status[i] & 1u) != 0;
        if (phase == ScriptPhase::Apply) selected = (state.status[i] & 2u) != 0;
        if (selected) state.hop2[count++] = i;
    }
    return count;
}

template <bool IofusedPrivateQueue>
ScriptPostResult script_post_phase(Server& server, ThreadCtx& self, Ring& ring,
                                   const Task& source, ScatterState& state,
                                   ScriptPhase phase) {
    state.script_phase = phase;
    state.phase = phase == ScriptPhase::Apply ? 2 : 1;

    int32_t one_shard = -1;
    if (phase == ScriptPhase::Run) {
        one_shard = state.script_coordinator_shard;
    } else {
        const uint32_t count = script_select_positions(state, phase);
        reset_groups(state);
        build_groups(state, state.hop2, count);
        if (!state.nsub) return ScriptPostResult::Empty;
    }

    uint32_t needed[kMaxThreads] = {};
    uint32_t participants[kMaxThreads];
    uint32_t nparticipants = 0;
    if (phase == ScriptPhase::Run) {
        const uint32_t tid = server.worker_of_shard(one_shard);
        participants[nparticipants++] = tid;
        needed[tid] = 1;
    } else {
        for (uint32_t i = 0; i < state.nsub; i++) {
            const uint32_t tid = server.worker_of_shard(state.groups[i].shard);
            if (needed[tid]++ == 0) participants[nparticipants++] = tid;
        }
    }
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        if (scatter_task_free_slots<IofusedPrivateQueue>(
                server.thread(tid), self.id()) < needed[tid])
            return ScriptPostResult::Full;
    }
    if (phase == ScriptPhase::Apply && !state.aof_group &&
        !aof_prepare_scatter_group(state)) {
        state.final_error = WorkError::Oom;
        state.aborted.store(true, std::memory_order_release);
        return ScriptPostResult::Empty;
    }
    // Script starts in phase 1, so xshard_prepare's completion setup deliberately skipped these
    // slots. APPLY is its first and only record-installing wave; construct exactly its owner slots
    // before execute_atomic_apply attaches entries to their `nodes` counters. UNPIN has completed
    // and no record can reference this state yet, so initializing them here cannot reset a live
    // owner count. It is also where APPLY's pre-counted sentinels are published, so it must run
    // exactly once per wave -- the completion count it returns is the same owner count.
    uint32_t task_completions = phase == ScriptPhase::Run ? 1u : state.nsub;
    if (phase == ScriptPhase::Apply)
        task_completions = initialize_owner_completion(state);

    // THE +1 IS THE POSTER'S OWN REFERENCE and it is load-bearing. Owner tasks become visible one
    // participant at a time, so without it the LAST task could finish and try to advance the wave
    // while this thread was still inside the publishing loop -- and the next wave's build_groups()
    // would then rewrite state.groups under a thread already reading it. Two executors inside
    // build_groups() on one arena is exactly how the differ's script leg segfaulted. The poster
    // releases this reference in xshard_complete_script() once publication is complete.
    state.pending.store(task_completions + 1u, std::memory_order_release);
    if (phase == ScriptPhase::Run) {
        ThreadCtx& owner = server.thread(participants[0]);
        const Task post{source.client, source.op_id, one_shard, &state};
        if (!scatter_post_task_quiet<IofusedPrivateQueue>(
                owner, self.id(), post, self.sig())) std::abort();
        owner.flush_task_notify(self.id(), ring, self.sig());
        return ScriptPostResult::Posted;
    }

    Task posts[256];
    uint16_t begin[kMaxThreads];
    uint32_t cursor = 0;
    for (uint32_t p = 0; p < nparticipants; p++) {
        begin[p] = static_cast<uint16_t>(cursor);
        cursor += needed[participants[p]];
        needed[participants[p]] = begin[p];
    }
    for (uint32_t i = 0; i < state.nsub; i++) {
        const uint32_t tid = server.worker_of_shard(state.groups[i].shard);
        posts[needed[tid]++] = Task{source.client, source.op_id, state.groups[i].shard, &state};
    }
    for (uint32_t p = 0; p < nparticipants; p++) {
        const uint32_t tid = participants[p];
        const uint32_t end = p + 1 < nparticipants ? begin[p + 1] : cursor;
        ThreadCtx& owner = server.thread(tid);
        if (!scatter_post_tasks_quiet<IofusedPrivateQueue>(
                owner, self.id(), posts + begin[p], end - begin[p], self.sig()))
            std::abort();
        owner.flush_task_notify(self.id(), ring, self.sig());
    }
    return ScriptPostResult::Posted;
}

struct ScriptWorkbench {
    std::unique_ptr<Shard> shard;
    Server* server = nullptr;
};
thread_local ScriptWorkbench t_script_workbench;

Shard* script_workbench(Server& server, int32_t coordinator) {
    if (!t_script_workbench.shard) {
        auto shard = std::make_unique<Shard>();
        shard->init_private(&server, coordinator, server.cfg().type_limits,
                            server.cfg().stream_limits);
        t_script_workbench.server = &server;
        t_script_workbench.shard = std::move(shard);
    }
    if (t_script_workbench.server != &server) std::abort();
    Shard* workbench = t_script_workbench.shard.get();
    // One executor can coordinate activations for different KEYS[1] owners, so re-stamp the
    // identity when it changes. Nothing in the whitelisted handler set consults shard.id() today
    // except the zero-copy borrow, which zc_min == UINT32_MAX already makes unreachable here --
    // but a stale id is the kind of thing the next handler quietly inherits. Re-stamping also
    // picks up live CONFIG SET changes to the type/stream limits.
    if (workbench->id() != coordinator)
        workbench->init_private(&server, coordinator, server.cfg().type_limits,
                                server.cfg().stream_limits);
    return workbench;
}

struct ScriptAccessScope {
    explicit ScriptAccessScope(Op& op, uint8_t* access, uint32_t count) {
        script_cross_access_begin(op, access, count);
    }
    ~ScriptAccessScope() { script_cross_access_end(); }
};

ScatterTaskResult execute_script_phase(const Task& task, Shard& shard, Op& op) {
    ScatterState& state = *task.scatter;
    ShardGroup* group = group_for(state, shard.id());
    if (state.script_resume_post) return ScatterTaskResult::Complete;
    if (!group && state.script_phase != ScriptPhase::Run) std::abort();

    switch (state.script_phase) {
        case ScriptPhase::Pin: {
            for (uint32_t i = 0; i < group->count; i++) {
                const uint32_t pos = state.key_order[group->begin + i];
                const KeyRef& key = state.keys[pos];
                if (state.aborted.load(std::memory_order_acquire)) break;
#ifdef TOMO_XSCRIPT_NO_RESERVE
                // NEGATIVE-CONTROL BUILD ONLY (make noreserve). Everything else about the
                // activation is unchanged -- same waves, same cut, same validation -- so a battery
                // that passes here is a battery that was never testing the reservation.
                continue;
#endif
                if (!shard.store().atomic_script_pin(key.hash, op.arg(key.arg))) {
                    group->error = WorkError::Oom;
                    state.aborted.store(true, std::memory_order_release);
                    break;
                }
                state.server->note_script_key_armed();
                state.script_pinned[pos] = 1;
            }
            return ScatterTaskResult::Complete;
        }
        case ScriptPhase::Read: {
            if (state.aborted.load(std::memory_order_acquire))
                return ScatterTaskResult::Complete;
            // The owner's expiry cut is already state.now_cut_ms: xshard_execute installed it
            // before dispatching this phase, and it is the same instant every other owner of this
            // script is reading against.
            shard.store().atomic_set_read_context(state.script_cut, state.origin_conn_id);
            uint64_t local_bytes = 0;
            for (uint32_t i = 0; i < group->count; i++) {
                const uint32_t pos = state.key_order[group->begin + i];
                const KeyRef& key = state.keys[pos];
                KvObj* value = shard.store().find(key.hash, op.arg(key.arg));
                if (!serialize_object(value, state.images[pos])) {
                    group->error = WorkError::Oom;
                    state.aborted.store(true, std::memory_order_release);
                    break;
                }
                local_bytes += sizeof(ObjectImage) + op.arg(key.arg).n +
                               state.images[pos].payload.size();
            }
            shard.store().atomic_clear_read_epoch();
            const uint64_t total = state.script_staged_bytes.fetch_add(
                local_bytes, std::memory_order_relaxed) + local_bytes;
            state.server->note_script_stage_owner(local_bytes);
            if (total > state.server->script_crossshard_max_bytes()) {
                state.script_stage_limit = true;
                group->error = WorkError::Oom;
                state.aborted.store(true, std::memory_order_release);
            }
            return ScatterTaskResult::Complete;
        }
        case ScriptPhase::Run: {
            state.server->note_script_run();
            Shard* work = script_workbench(*state.server, state.script_coordinator_shard);
            work->store().clear();
            // The workbench is a shard object of its own, so xshard_execute's cut does not reach
            // it; give it the same instant explicitly.
            work->pin_now_ms(state.now_cut_ms);
            for (uint32_t pos = 0; pos < state.key_count; pos++) {
                const KeyRef& key = state.keys[pos];
                const WorkError loaded = apply_image<false>(
                    *work, op.arg(key.arg), key.hash, state.images[pos], true);
                if (loaded != WorkError::None) {
                    state.final_error = loaded;
                    state.aborted.store(true, std::memory_order_release);
                    return ScatterTaskResult::Complete;
                }
            }
            if (work->store().object_bytes() >
                state.server->script_crossshard_workbench_bytes()) {
                state.final_error = WorkError::Maxmemory;
                state.aborted.store(true, std::memory_order_release);
                return ScatterTaskResult::Complete;
            }
            op.clear_reply();
            const uint64_t workbench_hits = work->stats().hits;
            const uint64_t workbench_misses = work->stats().misses;
            {
                ScriptAccessScope access(op, state.status, state.key_count);
                op.spec->handler(*work, op);
            }
            // The workbench is intentionally absent from Server::shards(), so forward the nested
            // command's read-lookup counters to the real coordinator shard before it is reused.
            shard.stats().hits += work->stats().hits - workbench_hits;
            shard.stats().misses += work->stats().misses - workbench_misses;
            if (work->store().object_bytes() >
                state.server->script_crossshard_workbench_bytes()) {
                state.final_error = WorkError::Maxmemory;
                state.aborted.store(true, std::memory_order_release);
                return ScatterTaskResult::Complete;
            }
            for (uint32_t pos = 0; pos < state.key_count; pos++) {
                if (!(state.status[pos] & 2u)) continue;
                const KeyRef& key = state.keys[pos];
                KvObj* value = work->store().find(key.hash, op.arg(key.arg));
                if (!serialize_object(value, state.apply[pos])) {
                    state.final_error = WorkError::Oom;
                    state.aborted.store(true, std::memory_order_release);
                    break;
                }
            }
            return ScatterTaskResult::Complete;
        }
        case ScriptPhase::Validate:
            state.server->note_script_validate_owner();
            for (uint32_t i = 0; i < group->count; i++) {
                const uint32_t pos = state.key_order[group->begin + i];
                const KeyRef& key = state.keys[pos];
                if (shard.store().atomic_script_changed(
                        key.hash, op.arg(key.arg), state.script_cut)) {
                    state.script_conflict.store(true, std::memory_order_release);
                    break;
                }
            }
            return ScatterTaskResult::Complete;
        case ScriptPhase::Unpin:
            for (uint32_t i = 0; i < group->count; i++) {
                const uint32_t pos = state.key_order[group->begin + i];
                if (!state.script_pinned[pos]) continue;
                const KeyRef& key = state.keys[pos];
                shard.store().atomic_script_unpin(key.hash, op.arg(key.arg));
                state.server->note_script_key_released();
                state.script_pinned[pos] = 0;
            }
            return ScatterTaskResult::Complete;
        case ScriptPhase::Apply:
        case ScriptPhase::None:
            break;
    }
    return ScatterTaskResult::Complete;
}

bool xshard_owner_mutation_phase(const ScatterState& state, const Op& op) {
    if (state.kind == Kind::Script && state.script_resume_post) return false;
    if (!(op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite))) return false;
    if (state.atomic_write) return true;
    if (state.atomic_apply || state.atomic_pop || state.two_hop) return state.phase == 2;
    return true;
}

}  // namespace

void xshard_visit_task_hashes(const Task& task, void* context,
                              void (*visit)(void*, uint64_t)) {
    if (!task.scatter || !visit) return;
    const ScatterState& state = *task.scatter;
    const ShardGroup* group = group_for(*task.scatter, task.shard);
    if (!group) return;
    if (sort_deref_gathering(state)) {
        const SortDerefState& sort = *state.sort_deref;
        for (uint32_t i = 0; i < group->count; i++)
            visit(context, sort.requests[sort.key_order[group->begin + i]].hash);
        return;
    }
    for (uint32_t i = 0; i < group->count; i++)
        visit(context, state.keys[state.key_order[group->begin + i]].hash);
}

ScatterTaskResult xshard_execute(const Task& task, Shard& shard, Op& op,
                                 uint32_t owner_thread_id) {
    const bool notify = (op.spec->flags & CmdFlags::NotifySelected) != 0;
    NotifyExecutionScope notify_scope(shard, op, notify, shard.notify_order_base());
    ScatterState& state = *task.scatter;
    // ONE EXPIRY CUT FOR THE WHOLE FAN-OUT. Before this, each fragment compared deadlines against
    // its own executor's per-pass clock, so an eight-owner MGET could report a key alive on one
    // owner and elapsed on another -- a keyspace that existed at no instant. Cheap enough to be
    // unconditional on the cross-shard path: two stores in, two out, and single-shard commands
    // never reach this function at all.
    PinnedNowMs now_cut(shard, state.now_cut_ms);
    if (state.kind == Kind::Script &&
        (state.script_phase != ScriptPhase::Apply || state.script_resume_post))
        return execute_script_phase(task, shard, op);
    ShardGroup& group = *group_for(state, shard.id());
    if (xshard_owner_mutation_phase(state, op) && shard.has_watches()) {
        const void* token = state.atomic_group ? static_cast<const void*>(&state) : nullptr;
        if (group.count == 0) {
            if (!shard.watch_all_write_ready()) return ScatterTaskResult::Retry;
        } else {
            for (uint32_t i = 0; i < group.count; i++) {
                const uint32_t pos = state.key_order[group.begin + i];
                const KeyRef& key = state.keys[pos];
                if (!shard.watch_write_ready(op.arg(key.arg), token))
                    return ScatterTaskResult::Retry;
            }
            if (state.atomic_group) {
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    shard.watch_reserve_write(
                        op.arg(key.arg), task.client, task.client->watch_generation(), &state,
                        publish_epoch(state), publish_aborted(state), &state.watch_refs);
                }
            }
        }
    }
    // The executor-wide batch prefetch deliberately excludes scatter tasks. Recover their structural
    // advantage here, after routing has made the batch owner-local and before MVCC promotion or the
    // ordinary gather serializes the same storage accesses. Atomic writes prepare capacity first
    // below because that preparation may change table geometry.
    if (!state.atomic_write) prefetch_owner_group(shard.store(), state, group);
    if (!state.atomic_write && shard.store().atomic_has_records()) {
        const uint64_t cleanup_cutoff = state.server->atomic_snapshot();
        const uint64_t floor = state.server->atomic_read_floor();
        if (sort_deref_gathering(state)) {
            SortDerefState& sort = *state.sort_deref;
            shard.store().atomic_promote_keys(
                group.count, group.membership, floor, cleanup_cutoff, [&](uint32_t i) {
                    const SortDerefRequest& request =
                        sort.requests[sort.key_order[group.begin + i]];
                    return std::pair<uint64_t, Slice>{request.hash, sort_deref_key(request)};
                });
        } else {
            shard.store().atomic_promote_keys(
                group.count, group.membership, floor, cleanup_cutoff, [&](uint32_t i) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    return std::pair<uint64_t, Slice>{key.hash, op.arg(key.arg)};
                });
        }
    }
    struct ReadEpochGuard {
        FlatStore& store;
        bool bound;
        ReadEpochGuard(FlatStore& store_, uint64_t snapshot, uint64_t origin_conn_id)
            : store(store_), bound(snapshot != UINT64_MAX) {
            if (bound) store.atomic_set_read_context(snapshot, origin_conn_id);
        }
        ~ReadEpochGuard() { if (bound) store.atomic_clear_read_epoch(); }
    };
    const uint64_t read_snapshot = state.atomic_pop && state.phase == 1 &&
                                   state.pop_retry_snapshot != UINT64_MAX
        ? state.pop_retry_snapshot : state.snapshot;
    const bool read_latest = state.atomic_direct || (state.atomic_pop && state.phase == 2);
    ReadEpochGuard read_epoch(
        shard.store(), read_latest ? UINT64_MAX : read_snapshot, state.origin_conn_id);
    try {
        if (state.atomic_direct)
            return execute_atomic_direct_rename(task, shard, op, owner_thread_id);
        if (state.atomic_apply && state.phase == 2) {
            if (state.kind == Kind::Script) state.server->note_script_apply_owner();
            return execute_atomic_apply(task, shard, op, owner_thread_id);
        }
        if (state.atomic_write) {
            // One owner pass preflights the table, allocates one shard entry, and performs ordinary
            // physical replacements. Per key, the entry records only the displaced pointer.
            if (!shard.store().atomic_prepare_capacity(group.count)) {
                group.error = WorkError::InsertFailed;
                state.aborted.store(true, std::memory_order_release);
                return ScatterTaskResult::Complete;
            }
            const uint32_t owner_slot = state.server->executor_slot(owner_thread_id);
            if (owner_slot >= state.owner_slots) std::abort();
            auto* owner_refs = &state.owner_record_refs[owner_slot].nodes;
            const bool copy_record_keys = state.kind == Kind::Del || state.kind == Kind::Unlink;
            uint64_t record_key_bytes = 0;
            if (copy_record_keys) {
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    record_key_bytes += op.arg(state.keys[pos].arg).n;
                }
            }
            void* pending_entry = shard.store().atomic_prepare_group(
                &state, group.begin, group.count, publish_epoch(state), &state.record_refs,
                publish_aborted(state), state.origin_conn_id, owner_refs, group.membership,
                copy_record_keys, record_key_bytes);
            if (!pending_entry) {
                group.error = WorkError::Oom;
                state.aborted.store(true, std::memory_order_release);
                return ScatterTaskResult::Complete;
            }
            uint32_t installed = 0;
            size_t parked_bytes = 0;
            size_t installed_bytes = 0;
            if (state.aborted.load(std::memory_order_acquire)) {
                shard.store().atomic_discard_group(pending_entry);
                return ScatterTaskResult::Complete;
            }
            prefetch_owner_group(shard.store(), state, group);
            // MSETNX tests the pre-command state. Duplicate keys in one command are not an
            // existence failure (Redis installs the last supplied value); probing after the first
            // physical exchange would see our own still-private candidate because the entry is
            // intentionally linked only after the complete shard pass. Preflight every local key
            // before any exchange, then materialize the whole candidate span.
            if (state.kind == Kind::Msetnx) {
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    if (notify ? shard.store_find<true>(key.hash, op.arg(key.arg))
                               : shard.store().find(key.hash, op.arg(key.arg))) {
                        state.aborted.store(true, std::memory_order_release);
                        shard.store().atomic_discard_group(pending_entry);
                        return ScatterTaskResult::Complete;
                    }
                }
            }
            // Atomic string installs bypass FlatStore::insert(). Keep the common path to one
            // predicted-cold gate; only an enabled n class performs the visibility probes. The
            // records remain on the scatter sidecar and are discarded if the group later aborts.
            if (notify && __builtin_expect(notify_flat_enabled(&shard, NOTIFY_NEW), false) &&
                (state.kind == Kind::Mset || state.kind == Kind::Msetnx)) {
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    const Slice key_slice = op.arg(key.arg);
                    bool duplicate = false;
                    for (uint32_t prior = 0; prior < i; prior++) {
                        const uint32_t p = state.key_order[group.begin + prior];
                        duplicate = state.keys[p].hash == key.hash &&
                                    op.arg(state.keys[p].arg) == key_slice;
                        if (duplicate) break;
                    }
                    if (!duplicate && !shard.store().atomic_find_tracked(key.hash, key_slice))
                        notify_record(shard, op, NOTIFY_NEW, NotifyEventId::New, key_slice);
                }
            }
            for (uint32_t i = 0; i < group.count; i++) {
                const uint32_t pos = state.key_order[group.begin + i];
                KeyRef& key = state.keys[pos];
                const Slice key_slice = op.arg(key.arg);
                KvObj* visible = (state.kind == Kind::Mset || state.kind == Kind::Msetnx)
                    ? nullptr : (notify ? shard.store_find<true>(key.hash, key_slice)
                                        : shard.store().find(key.hash, key_slice));
                if (state.kind == Kind::Del || state.kind == Kind::Unlink) {
                    bool duplicate = false;
                    for (uint32_t prior = 0; prior < i; prior++) {
                        const uint32_t p = state.key_order[group.begin + prior];
                        const Slice prior_key = op.arg(state.keys[p].arg);
                        duplicate = state.keys[p].hash == key.hash && prior_key == key_slice;
                        if (duplicate) break;
                    }
                    state.status[pos] = visible && !duplicate;
                    // DEL installs no object of its own. atomic_install_group copies key identity
                    // into this owner span's packed record tail before the record is published.
                    KvObj* old = shard.store().atomic_install_group(
                        pending_entry, key.hash, key_slice, nullptr);
                    if (old) parked_bytes += kvobj_size(old);
                    installed++;
                    continue;
                }

                KvObj* value = key.key_anchor;
                const bool io_prebuilt = value != nullptr;
                if (io_prebuilt && key.stable_object != value) std::abort();
                if (!value) {
                    value = xshard_make_atomic_string(
                        shard, key_slice, op.arg(key.arg + 1));
                    if (!value) {
                        group.error = WorkError::Oom;
                        state.aborted.store(true, std::memory_order_release);
                        break;
                    }
                }
                if (!shard.store().atomic_admit(key_slice, value)) {
                    if (!io_prebuilt) shard.store().atomic_discard_value(value);
                    group.error = WorkError::Maxmemory;
                    state.aborted.store(true, std::memory_order_release);
                    break;
                }
                key.stable_object = value;
                KvObj* old = shard.store().atomic_install_group(
                    pending_entry, key.hash, key_slice, value);
                if (old) parked_bytes += kvobj_size(old);
                installed_bytes += kvobj_size(value);
                if (io_prebuilt) key.key_anchor = nullptr;
                installed++;
            }
            if (installed) {
                shard.store().atomic_finish_group_install(parked_bytes, installed_bytes);
                shard.store().atomic_publish_group(pending_entry);
                note_owner_record_install(state, *owner_refs);
            } else {
                shard.store().atomic_discard_group(pending_entry);
            }
            return ScatterTaskResult::Complete;
        }

        const bool pending_reads = shard.store().atomic_has_records();
        if (__builtin_expect(atomic_tripwire_enabled(), false))
            xshard_tripwire_fragment_execute(
                task, shard, op, !pending_reads, read_latest ? UINT64_MAX : read_snapshot);
        auto find_value = [&](uint64_t hash, Slice key, bool read_lookup = false) {
            KvObj* object = pending_reads ? shard.store().atomic_find_tracked(hash, key)
                                          : (notify ? shard.store_find<true>(hash, key)
                                                    : shard.store().find(hash, key));
            if (read_lookup) shard.note_keyspace_read(object);
            return object;
        };

        // SORT's second read wave has concrete keys but no argv positions. Each request is executed
        // only by its routed owner, under the ReadEpochGuard and PinnedNowMs installed above.
        if (sort_deref_gathering(state)) {
            SortDerefState& sort = *state.sort_deref;
            for (uint32_t i = 0; i < group.count; i++) {
                SortDerefRequest& request = sort.requests[sort.key_order[group.begin + i]];
                const Slice key = sort_deref_key(request);
                state.server->note_sort_deref_lookup();
                KvObj* object = find_value(request.hash, key, true);
                if (!object) continue;
                if (!request.field.empty()) {
                    if (!object->is_type(Type::Hash)) continue;
                    const Slice field(request.field.data(),
                                      static_cast<uint32_t>(request.field.size()));
                    Slice value;
                    if (!hash_field_value_ro(object, field, value)) continue;
                    if (shard.store().field_expire_count() &&
                        hash_ttl_field_lapsed(object, field, shard.now_ms())) continue;
                    request.value.assign(value.p, value.n);
                    request.present = true;
                    continue;
                }
                if (!object->is_type(Type::String)) continue;
                if (object->is_int()) {
                    char integer[24];
                    request.value.assign(integer, i64_to_dec(integer, object->int_value()));
                } else {
                    KvObjRawReadBuffer raw;
                    const Slice value = kvobj_string_value(object, raw);
                    request.value.assign(value.p, value.n);
                }
                request.present = true;
            }
            return ScatterTaskResult::Complete;
        }

        // MGET is the read-heavy matrix cell. Specialize its loop once per owner task so the
        // no-pending arm is byte-for-byte the ordinary FlatStore lookup, with no per-key selector.
        if (state.phase != 2 && state.kind == Kind::Mget) {
            auto gather = [&]<bool Tracked, bool Notify>() {
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    KvObj* object;
                    if constexpr (Tracked)
                        object = shard.store().atomic_find_tracked(key.hash, op.arg(key.arg));
                    else if constexpr (Notify)
                        object = shard.store_find<true>(key.hash, op.arg(key.arg));
                    else object = shard.store().find(key.hash, op.arg(key.arg));
                    shard.note_keyspace_read(object);
                    if (!object || static_cast<Type>(object->type) != Type::String) continue;
                    ValueSlot& slot = state.values[pos];
                    if (object->is_int()) {
                        slot.len = i64_to_dec(slot.small, object->int_value());
                        slot.kind = ValueKind::Inline;
                        continue;
                    }
                    if constexpr (kReadLocalSetTaxVariant ==
                                  ReadLocalSetTaxVariant::ObjectSequenceOverwrite) {
                        if (object->encoding() == Enc::Raw) {
                            const uint32_t length = kvobj_read_local_raw_length(object);
                            slot.len = length;
                            const uint32_t cutover =
                                std::min<uint32_t>(shard.zc_min(), ValueSlot::kInline);
                            if (length <= cutover) {
                                kvobj_read_local_copy_raw(
                                    object, object->read_local_flags(), length, slot.small);
                                slot.kind = ValueKind::Inline;
                            } else {
                                const char* value = object->str_data();
                                // Registration precedes the next owner command, whose overwrite
                                // gate will force immutable replacement until IO releases this ptr.
                                shard.store().borrow(value);
                                slot.ptr = value;
                                slot.shard = shard.id();
                                slot.kind = ValueKind::Borrow;
                            }
                            continue;
                        }
                    }
                    KvObjRawReadBuffer raw;
                    const Slice value = kvobj_string_value(object, raw);
                    slot.len = value.n;
                    uint32_t cutover =
                        std::min<uint32_t>(shard.zc_min(), ValueSlot::kInline);
                    if constexpr (kReadLocalSetTaxVariant ==
                                  ReadLocalSetTaxVariant::SequenceOverwrite) {
                        // A materializes Raw through atomic cells. Keep that owner-local copy in
                        // the gather slot instead of publishing its stack address as a borrow.
                        if (object->encoding() == Enc::Raw)
                            cutover = ValueSlot::kInline;
                    }
                    if (value.n <= cutover) {
                        if (value.n) std::memcpy(slot.small, value.p, value.n);
                        slot.kind = ValueKind::Inline;
                    } else {
                        shard.store().borrow(value.p);
                        slot.ptr = value.p;
                        slot.shard = shard.id();
                        slot.kind = ValueKind::Borrow;
                    }
                }
            };
            if (pending_reads) gather.template operator()<true, false>();
            else if (notify) gather.template operator()<false, true>();
            else gather.template operator()<false, false>();
            return ScatterTaskResult::Complete;
        }

        if (state.phase != 2 && state.kind == Kind::Xread) {
            for (uint32_t i = 0; i < group.count; i++) {
                const uint32_t pos = state.key_order[group.begin + i];
                const KeyRef& key = state.keys[pos];
                StreamID cursor{};
                StreamID upper{};
                bool latest = false;
                bool bounded = false;
                if (state.blocking_origin) {
                    if (!blocking_stream_bounds(state.blocking_origin, pos, cursor,
                                                upper, bounded)) {
                        state.status[pos] = static_cast<uint8_t>(WorkError::Corrupt);
                        continue;
                    }
                } else if (!stream_parse_xread_id(op.arg(state.xread_first_id + pos),
                                                  cursor, latest)) {
                    state.status[pos] = static_cast<uint8_t>(WorkError::Corrupt);
                    continue;
                }
                ObjectImage& image = state.images[pos];
                if (state.blocking_origin) {
                    if (const std::vector<uint8_t>* captured =
                            blocking_stream_captured_payload(state.blocking_origin, pos)) {
                        try { image.payload = *captured; }
                        catch (const std::bad_alloc&) {
                            state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                            continue;
                        }
                        image.present = true;
                        continue;
                    }
                }
                const StreamReadResult read = stream_xread_gather(
                    shard, op.arg(key.arg), key.hash, cursor, latest, state.xread_count,
                    image.payload, bounded ? &upper : nullptr);
                image.present = read == StreamReadResult::Ready;
                if (read == StreamReadResult::WrongType)
                    state.status[pos] = static_cast<uint8_t>(WorkError::WrongType);
                else if (read == StreamReadResult::Oom)
                    state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                else if (read == StreamReadResult::Corrupt)
                    state.status[pos] = static_cast<uint8_t>(WorkError::Corrupt);
            }
            return ScatterTaskResult::Complete;
        }

        // Atomic apply still needs complete candidates for MVCC. The default lane carries only
        // source type/emptiness plus the selected element; destination hop one is a type probe.
        // Hop two below mutates each live owner instead of replaying stale collection images.
        if (state.phase != 2 && !state.atomic_apply && is_element_mover(state.kind)) {
            for (uint32_t i = 0; i < group.count; i++) {
                const uint32_t pos = state.key_order[group.begin + i];
                const KeyRef& key = state.keys[pos];
                KvObj* object = find_value(key.hash, op.arg(key.arg));
                ObjectImage& image = state.images[pos];
                if (object) {
                    image.present = true;
                    image.type = static_cast<Type>(object->type);
                    if (image.type == Type::List || image.type == Type::Set)
                        image.entries = CollectionRef(object).entries();
                }
                if (pos != 0) continue;

                if (state.kind == Kind::Smove) {
                    bool contains = false;
                    const XshardElementResult probed =
                        xshard_set_contains(object, op.arg(3), contains);
                    if (probed == XshardElementResult::Oom)
                        state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                    state.final_integer = contains ? 1 : 0;
                    continue;
                }
                if (!object || image.type != Type::List || !image.entries) continue;
                ResultHeap* result = ensure_result(state);
                if (!result) {
                    state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                    continue;
                }
                const XshardElementResult probed =
                    xshard_peek_list(object, state.from_left, result->bulk);
                if (probed == XshardElementResult::Oom)
                    state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
            }
            return ScatterTaskResult::Complete;
        }

        if (state.phase == 2) {
            if (state.kind == Kind::Lmpop || state.kind == Kind::Zmpop) {
                const uint32_t pos = state.key_order[group.begin];
                const KeyRef& key = state.keys[pos];
                // The bare phase-two pop may mutate in place without needing an MVCC record.
                // Publish its selected key for the complete handler interval anyway; an EXEC
                // child can safely hold this duplicate scope beside its transaction entry.
                auto foreign_read_scope =
                    shard.store().foreign_read_key_guard(key.hash);
                bool plain_active = false;
                if (!state.multi_child &&
                    shard.store().atomic_needs_version(key.hash, op.arg(key.arg))) {
                    const WorkError prepared = begin_plain_version(
                        *state.server, shard, op.arg(key.arg), key.hash,
                        state.atomic_pop ? UINT64_MAX : state.snapshot, state.origin_conn_id);
                    if (prepared != WorkError::None) {
                        state.status[pos] = static_cast<uint8_t>(prepared);
                        return ScatterTaskResult::Complete;
                    }
                    plain_active = true;
                }
                ResultHeap* result = ensure_result(state);
                if (!result) {
                    state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                    if (plain_active) shard.store().atomic_finish_plain();
                    return ScatterTaskResult::Complete;
                }
                const XshardPopResult popped = state.kind == Kind::Lmpop
                    ? xshard_pop_list(shard, op.arg(key.arg), key.hash, !state.pop_edge,
                                      state.pop_count, result->members)
                    : xshard_pop_zset(shard, op.arg(key.arg), key.hash, state.pop_edge,
                                      state.pop_count, result->members, result->scores);
                if (popped == XshardPopResult::WrongType)
                    state.status[pos] = static_cast<uint8_t>(WorkError::WrongType);
                else if (popped == XshardPopResult::Oom)
                    state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                else if (popped == XshardPopResult::Missing && state.atomic_pop)
                    state.pop_missed = true;
                if (plain_active) shard.store().atomic_finish_plain();
                return ScatterTaskResult::Complete;
            }
            for (uint32_t i = 0; i < group.count; i++) {
                const uint32_t pos = state.key_order[group.begin + i];
                const KeyRef& key = state.keys[pos];
                bool plain_active = false;
                if (shard.store().atomic_needs_version(key.hash, op.arg(key.arg))) {
                    const WorkError prepared = begin_plain_version(
                        *state.server, shard, op.arg(key.arg), key.hash, state.snapshot,
                        state.origin_conn_id);
                    if (prepared != WorkError::None) {
                        state.status[pos] = static_cast<uint8_t>(prepared);
                        continue;
                    }
                    plain_active = true;
                }
                WorkError result;
                if (!state.atomic_apply && is_element_mover(state.kind)) {
                    XshardElementResult moved = XshardElementResult::InsertFailed;
                    if (state.kind == Kind::Smove) {
                        moved = pos == 0
                            ? xshard_remove_set_element(
                                  shard, op.arg(key.arg), key.hash, op.arg(3))
                            : xshard_insert_set_element(
                                  shard, op.arg(key.arg), key.hash, op.arg(3));
                    } else {
                        const std::string& element = state.result->bulk;
                        const Slice value(element.data(), static_cast<uint32_t>(element.size()));
                        moved = pos == 0
                            ? xshard_remove_list_element(
                                  shard, op.arg(key.arg), key.hash, state.from_left, value)
                            : xshard_push_list_element(
                                  shard, op.arg(key.arg), key.hash, state.to_left, value);
                    }
                    result = element_work_error(moved);
                } else if (state.kind == Kind::Msetnx) {
                    result = store_xstring_selected(
                        notify, shard, op.arg(key.arg), key.hash, op.arg(key.arg + 1));
                } else if (state.kind == Kind::Bitop) {
                    const ObjectImage& image = state.apply[pos];
                    if (!image.present) {
                        if (notify)
                            shard.store_erase<true>(key.hash, op.arg(key.arg),
                                                   FlatStore::EraseEvent::None);
                        else shard.store().erase(key.hash, op.arg(key.arg));
                        result = WorkError::None;
                    } else {
                        // BITOP overwrites the destination like SET: TTL cleared (-1), and the
                        // result must stay raw bytes -- never integer-encoded -- so GET
                        // round-trips exactly. Positional args: expire_at_ms BEFORE
                        // integer_encode (a bare `false` here lands in expire_at_ms as
                        // "expired at epoch 0" and stores a dead key).
                        result = store_xstring_selected(
                            notify, shard, op.arg(key.arg), key.hash,
                            Slice(reinterpret_cast<const char*>(image.payload.data()),
                                  static_cast<uint32_t>(image.payload.size())),
                            -1, false);
                    }
                } else if (state.kind == Kind::Pfmerge) {
                    const ObjectImage& image = state.apply[pos];
                    const Slice value(reinterpret_cast<const char*>(image.payload.data()),
                                      static_cast<uint32_t>(image.payload.size()));
                    // PFMERGE is a string command even though its bytes are structured. Hop two
                    // installs through the same string funnel as SET and preserves destination TTL.
                    result = store_xstring_selected(
                        notify, shard, op.arg(key.arg), key.hash, value,
                        image.expire_at_ms, false, image.reserve_ttl_slot);
                } else {
                    const bool suppress_del =
                        ((state.kind == Kind::Rename || state.kind == Kind::Renamenx) && pos == 0) ||
                        ((state.kind == Kind::Bitop || is_store_setop(state.kind) ||
                          is_store_zsetop(state.kind) ||
                          (is_geo_kind(state.kind) && state.geo_store) ||
                          state.kind == Kind::Zrangestore ||
                          (state.kind == Kind::Sort && state.sort_options.store)) && pos == 0);
                    result = apply_image_selected(
                        notify, shard, op.arg(key.arg), key.hash, state.apply[pos], suppress_del);
                }
                state.status[pos] = static_cast<uint8_t>(result);
                if (plain_active) shard.store().atomic_finish_plain();
            }
            return ScatterTaskResult::Complete;
        }

        switch (state.kind) {
            case Kind::AllShards: {
                auto foreign_read_scope = shard.store().foreign_read_poison_guard(
                    (op.spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)) != 0);
                // FLUSH remains outside the atomic command set. On a recorded shard, install one
                // owner-local tombstone cut first so clear() cannot free a protected predecessor.
                // Allocation happens before the ticket or any relink; OOM retries are deferrable.
                if ((op.cmd_name().eq_icase("flushdb") || op.cmd_name().eq_icase("flushall")) &&
                    !shard.store().atomic_tombstone_all()) return ScatterTaskResult::Retry;
                op.spec->handler(shard, op);
                return ScatterTaskResult::Complete;
            }
            case Kind::DebugReload: {
                auto foreign_read_scope = shard.store().foreign_read_poison_guard();
                if (!shard.store().atomic_tombstone_all()) return ScatterTaskResult::Retry;
                shard.store().clear();
                std::string error;
                if (!state.reload_plan ||
                    !snapshot_load_shard(*state.reload_plan, *state.server, shard, error)) {
                    group.error = WorkError::Corrupt;
                    return ScatterTaskResult::Complete;
                }
                return ScatterTaskResult::Complete;
            }
            case Kind::DebugLoadAof: {
                auto foreign_read_scope = shard.store().foreign_read_poison_guard();
                if (!shard.store().atomic_tombstone_all()) return ScatterTaskResult::Retry;
                shard.store().clear();
                std::string error;
                if (state.aof_reload_base &&
                    !snapshot_load_shard(*state.aof_reload_base, *state.server, shard, error)) {
                    std::fprintf(stderr, "DEBUG LOADAOF base failed on shard %d: %s\n",
                                 shard.id(), error.c_str());
                    group.error = WorkError::Corrupt;
                    return ScatterTaskResult::Complete;
                }
                for (const auto& increment : state.aof_reload_increments) {
                    if (!aof_load_shard(*increment, *state.server, shard, error)) {
                        std::fprintf(stderr, "DEBUG LOADAOF increment failed on shard %d: %s\n",
                                     shard.id(), error.c_str());
                        group.error = WorkError::Corrupt;
                        return ScatterTaskResult::Complete;
                    }
                }
                return ScatterTaskResult::Complete;
            }
            case Kind::DebugBorrowCount:
                // Cold, owner-thread inspection of the registry's existing exact reference count.
                // No publication counter or ordinary borrow/reply-path work is needed.
                state.exact_sum.fetch_add(
                    shard.store().outstanding_borrows(), std::memory_order_relaxed);
                return ScatterTaskResult::Complete;
            case Kind::DbsizeExact:
                state.exact_sum.fetch_add(
                    shard.store().atomic_resolved_size(state.snapshot), std::memory_order_relaxed);
                return ScatterTaskResult::Complete;
            case Kind::Mget:
                std::abort(); // handled by the owner-hoisted arm above
            case Kind::Mset:
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    bool plain_active = false;
                    if (shard.store().atomic_needs_version(key.hash, op.arg(key.arg))) {
                        const WorkError prepared = begin_plain_version(
                            *state.server, shard, op.arg(key.arg), key.hash, state.snapshot,
                            state.origin_conn_id);
                        if (prepared != WorkError::None) {
                            state.status[pos] = static_cast<uint8_t>(prepared);
                            continue;
                        }
                        plain_active = true;
                    }
                    state.status[pos] = static_cast<uint8_t>(store_xstring_selected(
                        notify, shard, op.arg(key.arg), key.hash, op.arg(key.arg + 1)));
                    if (plain_active) shard.store().atomic_finish_plain();
                }
                return ScatterTaskResult::Complete;
            case Kind::Del:
            case Kind::Unlink:
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    bool plain_active = false;
                    if (shard.store().atomic_needs_version(key.hash, op.arg(key.arg))) {
                        const WorkError prepared = begin_plain_version(
                            *state.server, shard, op.arg(key.arg), key.hash, state.snapshot,
                            state.origin_conn_id);
                        if (prepared != WorkError::None) {
                            group.error = prepared;
                            continue;
                        }
                        plain_active = true;
                    }
                    state.status[pos] = notify
                        ? shard.store_erase<true>(key.hash, op.arg(key.arg))
                        : shard.store().erase(key.hash, op.arg(key.arg));
                    if (plain_active) shard.store().atomic_finish_plain();
                }
                return ScatterTaskResult::Complete;
            case Kind::Exists:
            case Kind::Touch:
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    state.status[pos] = find_value(key.hash, op.arg(key.arg), true) != nullptr;
                }
                return ScatterTaskResult::Complete;
            case Kind::Keys: {
                if (!group.aux) group.aux = new GroupAux;
                const Slice pattern = op.arg(1);
                group.scan_cursor = shard.store().scan(group.scan_cursor, 256, [&](KvObj* object) {
                    const Slice key = object->key();
                    const uint64_t hash = FlatStore::hash_key(key);
                    if (shard.store().atomic_physical_key_visible(hash, key, state.snapshot) &&
                        command_glob_match(pattern, key))
                        group.aux->keys_result.emplace_back(key.p, key.n);
                });
                if (group.scan_cursor) return ScatterTaskResult::Retry;
                shard.store().atomic_for_each_side_key(state.snapshot, [&](Slice key) {
                    if (command_glob_match(pattern, key))
                        group.aux->keys_result.emplace_back(key.p, key.n);
                });
                return ScatterTaskResult::Complete;
            }
            case Kind::Msetnx:
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    if (find_value(key.hash, op.arg(key.arg)))
                        state.values[pos].kind = ValueKind::Inline;
                }
                return ScatterTaskResult::Complete;
            case Kind::Lmpop:
            case Kind::Zmpop:
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    KvObj* object = find_value(key.hash, op.arg(key.arg));
                    ObjectImage& probe = state.images[pos];
                    if (!object) continue;
                    probe.present = true;
                    probe.type = static_cast<Type>(object->type);
                    const Type wanted = state.kind == Kind::Lmpop ? Type::List : Type::Zset;
                    if (probe.type == wanted) probe.entries = CollectionRef(object).entries();
                }
                return ScatterTaskResult::Complete;
            case Kind::Rename:
            case Kind::Renamenx:
            case Kind::Copy:
                if (state.atomic_apply) {
                    for (uint32_t i = 0; i < group.count; i++) {
                        const uint32_t pos = state.key_order[group.begin + i];
                        const KeyRef& key = state.keys[pos];
                        KvObj* object = find_value(
                            key.hash, op.arg(key.arg), state.kind == Kind::Copy && pos == 0);
                        ObjectImage& image = state.images[pos];
                        if (!object) continue;
                        image.present = true;
                        image.type = static_cast<Type>(object->type);
                        image.expire_at_ms = object->expire_at_ms();

                        // The destination image is needed only for its existence verdict. A
                        // string source can be cloned directly for the destination and carried as
                        // an unlinked object, while collections retain the generic image path.
                        if (pos != 0) continue;
                        if (image.type != Type::String) {
                            if (!serialize_object(object, image))
                                state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                            continue;
                        }
                        KeyRef& destination = state.keys[1];
                        KvObjRawReadBuffer raw;
                        const bool reserve_ttl_slot =
                            state.kind != Kind::Copy && object->has_ttl_slot();
                        KvObj* clone = object->is_int()
                            ? kvobj_new_int(op.arg(destination.arg), object->int_value(),
                                            image.expire_at_ms, reserve_ttl_slot)
                            : xshard_make_string(op.arg(destination.arg),
                                                 kvobj_string_value(object, raw),
                                                 image.expire_at_ms, false, reserve_ttl_slot);
                        if (!clone) {
                            state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                            continue;
                        }
                        destination.stable_object = clone;
                        destination.key_anchor = clone;
                    }
                    return ScatterTaskResult::Complete;
                }
                [[fallthrough]];
            default:
                for (uint32_t i = 0; i < group.count; i++) {
                    const uint32_t pos = state.key_order[group.begin + i];
                    const KeyRef& key = state.keys[pos];
                    // SORT expands a zset source in place before reading it (see
                    // zset_sort_promote). This runs on the OWNER of that key, the only thread
                    // allowed to mutate it, and BEFORE find_value so the image is taken from the
                    // promoted object: the source sits at key 1 when STORE moved the destination
                    // into key 0, and at key 0 otherwise.
                    const bool sort_source =
                        state.kind == Kind::Sort && pos == (state.sort_options.store ? 1u : 0u);
                    if (sort_source)
                        zset_sort_promote(shard, key.hash, op.arg(key.arg), notify);
                    const bool source_read =
                        state.kind == Kind::Pfcount || state.kind == Kind::Pfmerge ||
                        state.kind == Kind::Lcs ||
                        (state.kind == Kind::Bitop && pos != 0) ||
                        (is_plain_setop(state.kind) || state.kind == Kind::Sintercard) ||
                        (is_store_setop(state.kind) && pos != 0) ||
                        (is_plain_zsetop(state.kind) || state.kind == Kind::Zintercard) ||
                        (is_store_zsetop(state.kind) && pos != 0) ||
                        (state.kind == Kind::Zrangestore && pos != 0) ||
                        (state.kind == Kind::Sort && sort_source) ||
                        (is_geo_kind(state.kind) &&
                         pos == (state.geo_store ? 1u : 0u));
                    KvObj* object = find_value(key.hash, op.arg(key.arg), source_read);
                    if (!serialize_object(object, state.images[pos])) {
                        state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                        break;
                    }
                    // General forms that read no derived key can finish immediately on this
                    // executor. Actual dereferences are expanded only after this source wave and
                    // reduced after their owner-grouped gather completes.
                    if (!sort_source || !state.sort_options.general() ||
                        state.sort_options.deref) continue;
                    ResultHeap* result = ensure_result(state);
                    if (!result) {
                        state.status[pos] = static_cast<uint8_t>(WorkError::Oom);
                        break;
                    }
                    bool conversion_error = false;
                    const WorkError sorted = sort_image(
                        state.images[pos], op, state.sort_options, nullptr,
                        result->members, result->present, conversion_error);
                    if (sorted != WorkError::None) {
                        state.status[pos] = static_cast<uint8_t>(sorted);
                        break;
                    }
                    state.sort_conversion_error = conversion_error;
                    state.sort_general_done = true;
                    state.server->note_sort_scatter_general();
                }
                return ScatterTaskResult::Complete;
        }
    } catch (const std::bad_alloc&) {
        group.error = WorkError::Oom;
        return ScatterTaskResult::Complete;
    }
}

void script_snapshot_done(Server& server, ScatterState& state) {
    if (state.snapshot_registered &&
        !state.snapshot_complete.exchange(true, std::memory_order_acq_rel))
        server.note_atomic_snapshot_complete(state.owner_io);
}

void script_engine_error_reply(Op& op, ScatterState& state, WorkError error) {
    op.clear_reply();
    if (state.script_stage_limit) {
        reply_err(op.sink(), "ERR cross-shard script staging limit exceeded");
    } else if (error == WorkError::Maxmemory) {
        reply_maxmemory_oom(op);
    } else {
        reply_work_error(op, error == WorkError::None ? WorkError::Oom : error);
    }
}

// Drives one activation from wave to wave. Exactly ONE thread is inside the switch below at any
// moment: the one that took `pending` to zero, which cannot happen until the poster has released
// its own reference. An empty wave, or a wave whose tasks all finished before the poster let go,
// is driven onward by this thread in the loop rather than by recursion -- an OCC retry storm would
// otherwise recurse once per restart.
template <bool IofusedPrivateQueue>
ScatterFinish xshard_complete_script(Server& server, ThreadCtx& self, Ring& ring,
                                     const Task& task, Op& op, ScatterState& state) {
    ScatterFinish outcome = ScatterFinish::Waiting;
    bool driving = false;

    // Publishes `next`; returns true when THIS thread still owns the activation afterwards.
    auto launch = [&](ScriptPhase next) -> bool {
        // A queue-full retry re-enters on a synthetic poster task. Clear its marker before making
        // any new owner task visible; otherwise a fast peer can mistake itself for that poster,
        // skip APPLY's owner-sentinel completion, and recursively republish the wave.
        state.script_resume_post = false;
        const ScriptPostResult result = script_post_phase<IofusedPrivateQueue>(
            server, self, ring, task, state, next);
        if (result == ScriptPostResult::Full) {
            // No task slots anywhere. Nothing else references the activation, so the poster's
            // reference simply survives the requeue and the retry resumes this same publication.
            state.script_resume_post = true;
            outcome = ScatterFinish::Retry;
            return false;
        }
        if (result == ScriptPostResult::Empty) return true;   // nothing posted; keep driving
        if (state.pending.fetch_sub(1, std::memory_order_acq_rel) == 1) return true;
        outcome = ScatterFinish::Waiting;
        return false;
    };

    for (;;) {
        if (!driving) {
            if (state.script_resume_post) {
                if (!launch(state.script_phase)) return outcome;
            } else if (state.pending.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return ScatterFinish::Waiting;
            }
        }
        driving = false;

        switch (state.script_phase) {
            case ScriptPhase::Pin: {
                WorkError error = WorkError::None;
                if (script_group_error(state, error) ||
                    state.aborted.load(std::memory_order_acquire)) {
                    server.note_script_abort_oom();
                    script_engine_error_reply(op, state, error);
                    state.script_apply_after_unpin = false;
                    state.script_commit_after_unpin = false;
                    if (!launch(ScriptPhase::Unpin)) return outcome;
                    driving = true;
                    continue;
                }
                state.script_pins_live = true;
                // All intents now exist. A write before this point is part of the new
                // cut; every write after it is forced through MVCC and can be validated.
                state.script_cut = server.atomic_snapshot();
                state.script_staged_bytes.store(0, std::memory_order_relaxed);
                // TEST HOOK: the park window opens HERE, not at prepare. The point of the
                // hook is to expose the interval between "every declared key is reserved
                // and the cut is chosen" and "the owners actually read", which is the
                // interval AMENDMENT 1 is about.
                if (__builtin_expect(state.debug_script_defer_us != 0, false) &&
                    state.debug_script_defer_deadline == 0)
                    state.debug_script_defer_deadline =
                        now_ns() + static_cast<uint64_t>(state.debug_script_defer_us) * 1000ull;
                if (!launch(ScriptPhase::Read)) return outcome;
                driving = true;
                continue;
            }
            case ScriptPhase::Read: {
                WorkError error = WorkError::None;
                if (script_group_error(state, error) ||
                    state.aborted.load(std::memory_order_acquire)) {
                    server.note_script_abort_oom();
                    script_engine_error_reply(op, state, error);
                    state.script_apply_after_unpin = false;
                    state.script_commit_after_unpin = false;
                    if (!launch(ScriptPhase::Unpin)) return outcome;
                    driving = true;
                    continue;
                }
                if (!launch(ScriptPhase::Run)) return outcome;
                driving = true;
                continue;
            }
            case ScriptPhase::Run:
                if (state.aborted.load(std::memory_order_acquire)) {
                    server.note_script_abort_oom();
                    script_engine_error_reply(op, state, state.final_error);
                    notify_discard_batch(state.notify.exchange(nullptr));
                    state.script_apply_after_unpin = false;
                    state.script_commit_after_unpin = false;
                    if (!launch(ScriptPhase::Unpin)) return outcome;
                    driving = true;
                    continue;
                }
                state.script_has_writes = false;
                for (uint32_t i = 0; i < state.key_count; i++)
                    state.script_has_writes |= (state.status[i] & 2u) != 0;
                // A read-only activation already has a legal serialization point at script_cut: PIN
                // forced every later writer to preserve the predecessor read by STAGE. It therefore
                // needs neither an OCC validation wave nor a commit ticket.
                if (!state.script_has_writes) {
                    server.note_script_activation();
                    state.script_apply_after_unpin = false;
                    state.script_commit_after_unpin = false;
                    if (!launch(ScriptPhase::Unpin)) return outcome;
                    driving = true;
                    continue;
                }
                // Certification through APPLY is deliberately exclusive. Two activations
                // can STAGE and RUN concurrently, but without this seam both can validate
                // the same predecessor before either has installed a candidate, producing a
                // classic lost update. A loser restages instead of parking an executor;
                // plain writers remain fully concurrent and are caught by the ordinary
                // read-set validation below.
                if (!server.script_try_certification()) {
                    if (state.script_retries++ < server.script_crossshard_conflict_retries()) {
                        server.note_script_retry();
                        script_clear_attempt(state, op);
                        state.script_cut = server.atomic_snapshot();
                        if (!launch(ScriptPhase::Read)) return outcome;
                        driving = true;
                        continue;
                    }
                    server.note_script_giveup();
                    op.clear_reply();
                    reply_err(op.sink(), "TRYAGAIN Cross-shard script conflicted, retry");
                    notify_discard_batch(state.notify.exchange(nullptr));
                    state.script_apply_after_unpin = false;
                    state.script_commit_after_unpin = false;
                    if (!launch(ScriptPhase::Unpin)) return outcome;
                    driving = true;
                    continue;
                }
                state.script_certifier = true;
                state.script_ticket = server.atomic_commit_reserve();
                state.script_ticket_live = true;
                state.script_conflict.store(false, std::memory_order_relaxed);
                if (!launch(ScriptPhase::Validate)) return outcome;
                driving = true;
                continue;

            case ScriptPhase::Validate:
                if (state.script_conflict.load(std::memory_order_acquire)) {
                    if (!state.script_ticket_live) std::abort();
                    server.atomic_commit_publish();       // publish the unused reservation
                    state.script_ticket_live = false;
                    if (!state.script_certifier) std::abort();
                    server.script_finish_certification();
                    state.script_certifier = false;
                    if (state.script_retries++ < server.script_crossshard_conflict_retries()) {
                        server.note_script_retry();
                        script_clear_attempt(state, op);
                        state.script_cut = server.atomic_snapshot();
                        if (!launch(ScriptPhase::Read)) return outcome;
                        driving = true;
                        continue;
                    }
                    server.note_script_giveup();
                    op.clear_reply();
                    reply_err(op.sink(), "TRYAGAIN Cross-shard script conflicted, retry");
                    notify_discard_batch(state.notify.exchange(nullptr));
                    state.script_apply_after_unpin = false;
                    state.script_commit_after_unpin = false;
                    if (!launch(ScriptPhase::Unpin)) return outcome;
                    driving = true;
                    continue;
                }
                // Drop the read intents in a separate all-owner wave before installing candidates.
                // The reserved ticket orders every later writer after this activation, and no MVCC
                // record yet points at the group arrays that UNPIN is about to rebuild.
                state.script_apply_after_unpin = true;
                state.script_commit_after_unpin = false;
                if (!launch(ScriptPhase::Unpin)) return outcome;
                driving = true;
                continue;

            case ScriptPhase::Unpin:
                state.script_pins_live = false;
                if (state.script_apply_after_unpin) {
                    state.script_apply_after_unpin = false;
                    if (!launch(ScriptPhase::Apply)) return outcome;
                    driving = true;
                    continue;
                }
                if (state.script_commit_after_unpin) {
                    state.script_commit_after_unpin = false;
                    if (!state.script_ticket_live) std::abort();
                    server.atomic_commit_publish();
                    state.script_ticket_live = false;
                    server.note_script_activation();
                }
                script_snapshot_done(server, state);
                return ScatterFinish::Final;

            case ScriptPhase::Apply: {
                WorkError error = state.final_error;
                const bool failed = script_group_error(state, error) ||
                                    state.aborted.load(std::memory_order_acquire);
                if (failed) {
                    server.note_script_abort_oom();
                    state.aborted.store(true, std::memory_order_release);
                    aof_abort_scatter_group(state);
                    notify_discard_batch(state.notify.exchange(nullptr));
                    if (state.script_ticket_live) {
                        server.atomic_commit_publish();
                        state.script_ticket_live = false;
                    }
                    if (state.script_certifier) {
                        server.script_finish_certification();
                        state.script_certifier = false;
                    }
                    script_engine_error_reply(op, state, error);
                    script_snapshot_done(server, state);
                    return ScatterFinish::Final;
                }
                if (!state.script_ticket_live) std::abort();
                state.epoch.store(state.script_ticket, std::memory_order_release);
                server.atomic_commit_publish();
                state.script_ticket_live = false;
                if (!state.script_certifier) std::abort();
                server.script_finish_certification();
                state.script_certifier = false;
                aof_commit_scatter_group(state, self, ring, state.script_ticket);
                server.note_script_activation();
                server.note_script_group_commit();
                if (__builtin_expect(server.blocking_waiters() != 0, false))
                    blocking_atomic_published(server, self, ring, state);
                script_snapshot_done(server, state);
                return ScatterFinish::Final;
            }
            case ScriptPhase::None:
                std::abort();
        }
        std::abort();
    }
}

template <bool IofusedPrivateQueue>
ScatterFinish xshard_complete_impl(Server& server, ThreadCtx& self, Ring& ring,
                                   const Task& task, Op& op) {
    ScatterState& state = *task.scatter;
    if (state.kind == Kind::Script) {
        if (state.script_phase == ScriptPhase::Apply && !state.script_resume_post &&
            !complete_owner_record_wave(state, self.id())) return ScatterFinish::Waiting;
        return xshard_complete_script<IofusedPrivateQueue>(
            server, self, ring, task, op, state);
    }
    auto final = [&]() {
        if (op.spec->flags & CmdFlags::NotifySelected)
            notify_xshard_finished(server.shard(task.shard), op, state);
        // Every participating owner has installed its share of this group -- this is the last
        // owner, running on an EX thread. That is the instant the group stops being able to
        // straddle a snapshot cut, and it is much earlier than reply retirement, which happens on
        // the admitting IO thread (the very thread a blocking SAVE occupies).
        server.atomic_apply_close(self.id(), state.apply_open);
        return ScatterFinish::Final;
    };
    if (state.atomic_write || (state.atomic_apply && state.phase == 2)) {
        if (!complete_owner_record_wave(state, self.id())) return ScatterFinish::Waiting;
    }
    if (state.pending.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return ScatterFinish::Waiting;
    if (state.atomic_pop && state.phase == 2 && state.pop_missed) {
        aof_abort_scatter_group(state);
        state.aof_group.reset();
        static constexpr uint32_t kMaxPopRetries = 8;
        if (state.pop_retries++ < kMaxPopRetries &&
            publish_pop_retry<IofusedPrivateQueue>(server, self, ring, task, state))
            return ScatterFinish::Waiting;
        if (state.final_reply == FinalReply::QueueFull) return final();
        state.final_reply = FinalReply::NullArray;
        return final();
    }
    if (state.kind == Kind::Sort && state.sort_options.deref && state.phase == 1) {
        if (state.sort_deref->wave == SortDerefWave::Source) {
            WorkError source_error;
            if (first_error(state, source_error)) {
                state.final_reply = FinalReply::Work;
                state.final_error = source_error;
                return final();
            }
            const WorkError prepared = prepare_sort_deref(state, op);
            if (prepared != WorkError::None) {
                state.final_reply = FinalReply::Work;
                state.final_error = prepared;
                return final();
            }
            if (sort_deref_gathering(state)) {
                if (publish_sort_deref<IofusedPrivateQueue>(
                        server, self, ring, task, state))
                    return ScatterFinish::Waiting;
                return final();
            }
            const WorkError finished = finish_sort_deref(state);
            if (finished != WorkError::None) {
                state.final_reply = FinalReply::Work;
                state.final_error = finished;
                return final();
            }
            restore_sort_source_groups(state);
        } else if (sort_deref_gathering(state)) {
            const WorkError finished = finish_sort_deref(state);
            if (finished != WorkError::None) {
                state.final_reply = FinalReply::Work;
                state.final_error = finished;
                return final();
            }
            restore_sort_source_groups(state);
        }
    }
    // Once the last owner has materialized its result, this read cut protects no future
    // dereference. Value borrows have their own store lifetime pins. Tell the owning IO to advance
    // its floor even if ordered reply retirement is still behind a deep pipeline.
    if ((!state.two_hop || state.phase == 2) && state.snapshot_registered &&
        !state.snapshot_complete.exchange(true, std::memory_order_acq_rel))
        server.note_atomic_snapshot_complete(state.owner_io);
    if (state.atomic_write) {
        WorkError error;
        if (first_error(state, error)) {
            state.aborted.store(true, std::memory_order_release);
            aof_abort_scatter_group(state);
            state.final_reply = FinalReply::Work;
            state.final_error = error;
            return final();
        }
        if (state.aborted.load(std::memory_order_acquire)) {
            aof_abort_scatter_group(state);
            // MSETNX existence is a normal failed condition, not a work error. Its installed
            // candidates (if another owner got that far) remain forever invisible at epoch zero.
            if (state.kind == Kind::Msetnx) {
                state.final_reply = FinalReply::Integer;
                state.final_integer = 0;
            } else {
                state.final_reply = FinalReply::Internal;
            }
            return final();
        }
        if (state.kind == Kind::Msetnx) {
            state.final_reply = FinalReply::Integer;
            state.final_integer = 1;
        }
        if (!state.multi_child) return ScatterFinish::CommitQueued;
        return final();
    }
    if (state.phase == 2) {
        const bool commit_ready = finish_phase2(state);
        const bool pop_commit_ready = state.aof_group && state.atomic_pop &&
            (state.final_reply == FinalReply::Lmpop ||
             state.final_reply == FinalReply::Zmpop);
        if (commit_ready || pop_commit_ready) return ScatterFinish::CommitQueued;
        if (state.aof_group) {
            const uint64_t ticket = state.epoch.load(std::memory_order_acquire);
            if (ticket && !state.aborted.load(std::memory_order_acquire))
                aof_commit_scatter_group(state, self, ring, ticket);
            else
                aof_abort_scatter_group(state);
        }
        if (__builtin_expect(server.blocking_waiters() != 0, false))
            blocking_atomic_published(server, self, ring, state);
        return final();
    }
    if (state.kind == Kind::Pfcount) {
        finish_pfcount(state);
        return final();
    }
    if (state.kind == Kind::Lcs) {
        finish_lcs(state);
        return final();
    }
    if (!state.two_hop) return final();
    if (finish_phase1(state, op)) {
        return final();
    }
    if (publish_phase2<IofusedPrivateQueue>(server, self, ring, task, op, state))
        return ScatterFinish::Waiting;
    return final();
}

ScatterFinish xshard_complete(Server& server, ThreadCtx& self, Ring& ring,
                              const Task& task, Op& op) {
    return xshard_complete_impl<false>(server, self, ring, task, op);
}

ScatterFinish xshard_complete_iofused(Server& server, ThreadCtx& self, Ring& ring,
                                      const Task& task, Op& op) {
    return xshard_complete_impl<true>(server, self, ring, task, op);
}

void xshard_queue_commit(Server& server, ThreadCtx& self, const Task& task) {
    if (!task.client || !task.scatter || task.shard < 0) std::abort();
    ScatterState& state = *task.scatter;
    if (state.commit_client || state.commit_next ||
        state.epoch.load(std::memory_order_relaxed) != 0) std::abort();

    XshardCommitBatch& batch = xshard_commit_batch;
    if (!batch.head) {
        if (batch.tail || batch.count || batch.server || batch.owner != UINT32_MAX)
            std::abort();
        batch.server = &server;
        batch.owner = self.id();
        batch.head = &state;
    } else if (batch.server != &server || batch.owner != self.id() || !batch.tail) {
        std::abort();
    } else {
        batch.tail->commit_next = &state;
    }
    batch.tail = &state;
    batch.count++;
    state.commit_client = task.client;
    state.commit_op_id = task.op_id;
    state.commit_shard = task.shard;
}

void xshard_flush_commits(Server& server, ThreadCtx& self, Ring& ring,
                          void* notify_context, XshardCommitNotify notify) {
    if (!notify) std::abort();
    for (;;) {
        XshardCommitBatch& pending = xshard_commit_batch;
        if (!pending.head) {
            if (pending.tail || pending.count || pending.server ||
                pending.owner != UINT32_MAX) std::abort();
            return;
        }
        if (!pending.tail || !pending.count || pending.server != &server ||
            pending.owner != self.id()) std::abort();

        // Detach before any AOF or completion callback can re-enter the fused executor. A nested
        // completion starts a fresh list, which the outer loop drains under a separate bracket.
        ScatterState* head = pending.head;
        const uint64_t count = pending.count;
        pending = XshardCommitBatch{};

        const uint64_t first = server.atomic_commit_batch(count, [&](uint64_t ticket) {
            uint64_t stored = 0;
            for (ScatterState* state = head; state; state = state->commit_next) {
                if (stored == count || !state->commit_client ||
                    state->epoch.load(std::memory_order_relaxed) != 0) std::abort();
                state->epoch.store(ticket + stored, std::memory_order_release);
                stored++;
            }
            if (stored != count) std::abort();
        });

        uint64_t completed = 0;
        for (ScatterState* state = head; state;) {
            ScatterState* next = state->commit_next;
            Client* client = state->commit_client;
            Op& op = client->rob().at(state->commit_op_id);
            const uint64_t ticket = first + completed++;

            if (!state->aborted.load(std::memory_order_acquire))
                aof_commit_scatter_group(*state, self, ring, ticket);
            else
                aof_abort_scatter_group(*state);
            if (state->phase == 2 &&
                __builtin_expect(server.blocking_waiters() != 0, false))
                blocking_atomic_published(server, self, ring, *state);
            if (op.spec->flags & CmdFlags::NotifySelected)
                notify_xshard_finished(server.shard(state->commit_shard), op, *state);
            server.atomic_apply_close(self.id(), state->apply_open);

            // Capture `next` above: a fused self-notification may immediately retire the Op and
            // destroy this arena state. The release store remains the one reply-visibility edge.
            op.state.store(OpState::Done, std::memory_order_release);
            notify(notify_context, client);
            state = next;
        }
        if (completed != count) std::abort();
    }
}

enum class MultiChildFinish : uint8_t { Waiting, PhaseAdvanced, Final };

void xshard_multi_child_bind(ScatterState& state) {
    // xshard_prepare does not yet know that this state will be driven by an outer EXEC. Undo its
    // standalone phantom pins before publication; child installs retain lazy first-node accounting.
    clear_unpublished_owner_record_refs(state);
    state.multi_child = true;
}

void xshard_multi_child_capture_groups(ScatterState& state,
                                       std::array<uint64_t, 4>& mask) {
    mask.fill(0);
    for (uint32_t i = 0; i < state.nsub; i++) {
        const int32_t shard = state.groups[i].shard;
        if (shard < 0 || shard >= 256) std::abort();
        const uint32_t sid = static_cast<uint32_t>(shard);
        mask[sid >> 6] |= 1ull << (sid & 63);
    }
}

bool xshard_multi_child_records_done(const ScatterState& state) {
    return state.record_refs.load(std::memory_order_acquire) == 0 &&
           state.watch_refs.load(std::memory_order_acquire) == 0;
}

MultiChildFinish xshard_multi_child_complete(ScatterState& state, Op& op,
                                             uint32_t owner_thread_id) {
    if (state.atomic_write || (state.atomic_apply && state.phase == 2)) {
        if (!complete_owner_record_wave(state, owner_thread_id))
            return MultiChildFinish::Waiting;
    }
    if (state.pending.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return MultiChildFinish::Waiting;
    if (state.atomic_pop && state.phase == 2 && state.pop_missed) {
        state.final_reply = FinalReply::NullArray;
        return MultiChildFinish::Final;
    }
    if (state.kind == Kind::Sort && state.sort_options.deref && state.phase == 1) {
        if (state.sort_deref->wave == SortDerefWave::Source) {
            WorkError source_error;
            if (first_error(state, source_error)) {
                state.final_reply = FinalReply::Work;
                state.final_error = source_error;
                return MultiChildFinish::Final;
            }
            const WorkError prepared = prepare_sort_deref(state, op);
            if (prepared != WorkError::None) {
                state.final_reply = FinalReply::Work;
                state.final_error = prepared;
                return MultiChildFinish::Final;
            }
            if (sort_deref_gathering(state)) {
                state.pending.store(state.nsub, std::memory_order_release);
                return MultiChildFinish::PhaseAdvanced;
            }
            const WorkError finished = finish_sort_deref(state);
            if (finished != WorkError::None) {
                state.final_reply = FinalReply::Work;
                state.final_error = finished;
                return MultiChildFinish::Final;
            }
            restore_sort_source_groups(state);
        } else if (sort_deref_gathering(state)) {
            const WorkError finished = finish_sort_deref(state);
            if (finished != WorkError::None) {
                state.final_reply = FinalReply::Work;
                state.final_error = finished;
                return MultiChildFinish::Final;
            }
            restore_sort_source_groups(state);
        }
    }
    if (state.atomic_write) {
        WorkError error;
        if (first_error(state, error)) {
            state.aborted.store(true, std::memory_order_release);
            state.final_reply = FinalReply::Work;
            state.final_error = error;
            return MultiChildFinish::Final;
        }
        if (state.aborted.load(std::memory_order_acquire)) {
            if (state.kind == Kind::Msetnx) {
                state.final_reply = FinalReply::Integer;
                state.final_integer = 0;
            } else {
                state.final_reply = FinalReply::Internal;
            }
            return MultiChildFinish::Final;
        }
        if (state.kind == Kind::Msetnx) {
            state.final_reply = FinalReply::Integer;
            state.final_integer = 1;
        }
        return MultiChildFinish::Final;
    }
    if (state.phase == 2) {
        finish_phase2(state);
        return MultiChildFinish::Final;
    }
    if (state.kind == Kind::Pfcount) {
        finish_pfcount(state);
        return MultiChildFinish::Final;
    }
    // Gather kinds must reduce their fragments into a final_reply HERE, exactly as xshard_finish
    // does for the same command run bare. LCS was missing: its child reached assemble_final with
    // final_reply still None, fell through that function's `default:` and answered
    // "ERR internal cross-shard completion error" for two keys on different shards, while the
    // identical bare LCS answered correctly. Kinds that reduce in a finish_* hook belong in this
    // list; single-fragment and two-hop kinds are handled below.
    if (state.kind == Kind::Lcs) {
        finish_lcs(state);
        return MultiChildFinish::Final;
    }
    if (!state.two_hop) return MultiChildFinish::Final;
    if (finish_phase1(state, op)) return MultiChildFinish::Final;

    reset_groups(state);
    build_groups(state, state.hop2, state.hop2_count);
    state.phase = 2;
    arm_debug_off_hop_delay(state, op);
    state.pending.store(initialize_owner_completion(state), std::memory_order_release);
    return MultiChildFinish::PhaseAdvanced;
}

void xshard_retire(Server& server, ThreadCtx& self, Ring& ring, Client& client, Op& op,
                   ScatterArenaPool& pool, uint32_t owner_io,
                   void* release_ctx, void (*release_fn)(void*, int32_t, const char*),
                   void (*suppressed_fn)(void*)) {
    (void)self;
    (void)ring;
    ScatterState* state = op.scatter_state();
    if (!state) return;
    assemble_final(client, op, *state, release_ctx, release_fn, suppressed_fn);
    // A terminal error can bypass normal MGET assembly. Return any already-gathered borrows through
    // the same IO->owner channel used by send completion and teardown.
    if (state->kind == Kind::Mget && state->values) {
        for (uint32_t i = 0; i < state->key_count; i++) {
            ValueSlot& slot = state->values[i];
            if (slot.kind == ValueKind::Borrow && release_fn) {
                release_fn(release_ctx, slot.shard, slot.ptr);
                slot.kind = ValueKind::Nil;
            }
        }
    }
    if (state->atomic_group) {
        // Installed nodes retain only the compact group decision fields. Reclamation is batched by
        // each owner on later passes; retirement posts no per-group cleanup fan-out.
        free_state_contents(*state);
        if (state->snapshot_registered)
            pool.unregister_snapshot(server, state, owner_io);
        client.atomic_group_finished();
    }
    if (state->blocking_origin) blocking_scatter_retire(server, client, *state);
    op.detach_scatter_state();
    xshard_destroy(state, pool, owner_io);
}

}  // namespace tomo
