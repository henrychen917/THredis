// Deterministic storage regressions. No server, sockets, clock sleeps, or scheduler lottery.
// The real FlatStore, hash representation, and hash reaper are linked below. AOF/notification
// spies observe the store boundary; the scatter adapter supplies the owner span's key identity.
// Snapshot hooks retain and incrementally read the same KvObj pointer as production hooks.
// Each case asserts its vulnerable state. See FIXES.md for independent negative controls.
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "src/store/flatstore.h"
#include "src/cmd/t_hash_ttl.h"

using namespace tomo;

static void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static Slice slice(const std::string& s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
static uint64_t hash(const std::string& s) { return FlatStore::hash_key(slice(s)); }
static std::vector<std::string> deletes;
static uint32_t notifications = 0;

namespace tomo {
struct ScatterState { std::vector<std::string> keys; };
uint64_t xshard_atomic_key_hash(const ScatterState* s, uint32_t i) { return hash(s->keys.at(i)); }
Slice xshard_atomic_key_slice(const ScatterState* s, uint32_t i) { return slice(s->keys.at(i)); }
bool AofProducer::record_delete(Slice key, uint64_t group) {
    require(group == 0, "eviction/expiry must emit an independent AOF delete");
    deletes.emplace_back(key.p, key.n);
    return true;
}
bool AofProducer::record_post_image_buffered(FlatStore&, uint64_t, Slice, uint64_t) { return true; }
void notify_flat_store_emit(const FlatStore*, uint32_t, NotifyEventId, Slice) { notifications++; }
void notify_flat_emit(void*, uint32_t, NotifyEventId, Slice) { notifications++; }
bool notify_flat_enabled(void*, uint32_t) { return false; }
// Hash command tables retain their handlers at link time; these command-only dependencies are
// outside this store test. Reaching any of them is a fixture error, never a silent stub success.
void notify_execute_handler(Shard&, Op&, void (*)(Shard&, Op&)) { std::abort(); }
bool notify_record_slow(Shard&, Op&, uint32_t, uint32_t, NotifyEventId, Slice) { std::abort(); }
void reply_maxmemory_oom(Op&) { std::abort(); }
void cmd_xshard_only(Shard&, Op&) { std::abort(); }
void cmd_xshard_only_notify(Shard&, Op&) { std::abort(); }
bool command_glob_match(Slice, Slice, bool) { std::abort(); }
bool command_parse_scan_cursor(Slice, uint64_t&) { std::abort(); }
// No case creates these types. Fail loudly if a fixture accidentally reaches their destructors.
StreamVal::~StreamVal() { std::abort(); }
ZsetVal::~ZsetVal() { std::abort(); }
uint64_t stream_groups_allocation_bytes(const void*) { std::abort(); }

const SnapshotTypeHooks& snapshot_type_hooks(Type type) {
    require(type == Type::String, "snapshot fixture type");
    static const SnapshotTypeHooks hooks{
        [](const KvObj& o, SnapshotSaveCursor& c, uint8_t& encoding) {
            require(!o.is_int(), "snapshot fixture uses raw/external string");
            c.object = &o; c.total = o.str_value().n; encoding = 0;
            return SnapshotHookStatus::Ok;
        },
        [](SnapshotSaveCursor& c, uint8_t* out, size_t capacity, size_t& written) {
            written = std::min<uint64_t>(capacity, c.total - c.offset);
            std::memcpy(out, c.object->str_data() + c.offset, written);
            c.offset += written;
            return SnapshotHookStatus::Ok;
        }, nullptr};
    return hooks;
}

struct FlatStoreRegressionTest {
    static uint32_t cap(const FlatStore& s) { return s.cap_[0]; }
    static void step(FlatStore& s) { s.rehash_step(); }
    static void raw_insert(FlatStore& s, KvObj* o) {
        require(s.read_local_enabled_ ? s.insert_into_read_local(0, FlatStore::hash_key(o->key()), o, true)
                                     : s.insert_into(0, FlatStore::hash_key(o->key()), o, true),
                "fixture physical insertion");
    }
    static void raw_erase(FlatStore& s, const std::string& key) {
        require(s.erase_in(0, hash(key), slice(key)), "fixture physical erase");
    }
    static KvObj* candidate(FlatStore& s, const std::string& protected_key = "") {
        return s.choose_victim(slice(protected_key));
    }
    static bool capturing_record(const FlatStore& s, const KvObj* o) {
        return s.snapshot_record_.active && s.snapshot_record_.value.object == o;
    }
    static uint32_t field_index_size(const FlatStore& s) { return s.field_expires_.size(); }
};
} // namespace tomo

// The test executes no foreign reader during store mutation, so reclamation may happen immediately.
// The dedicated flags case below overlaps only immutable-object metadata touches.
static void arm(FlatStore& s, bool armed) {
    if (!armed) return;
    require(s.prepare_read_local(), "read-local allocation");
    s.configure_read_local(true, {nullptr,
        [](void*, void* owner, void* p, size_t n, ReadLocalRetireSink::ReclaimFn reclaim) {
            reclaim({}, owner, p, n);
        }, nullptr});
    require(s.read_local_enabled(), "read-local lane armed");
}
static KvObj* string_object(const std::string& key, const std::string& value = "value",
                            int64_t deadline = -1) {
    KvObj* o = kvobj_new_string(slice(key), slice(value), deadline);
    require(o != nullptr, "fixture string allocation");
    return o;
}
static KvObj* put(FlatStore& s, const std::string& key, const std::string& value = "value",
                  int64_t deadline = -1) {
    KvObj* o = string_object(key, value, deadline);
    require(s.insert(hash(key), o) == FlatStore::InsertResult::Inserted, "fixture insertion");
    return o;
}
static std::vector<std::string> keys_at_slots(uint32_t cap, uint32_t first, uint32_t count,
                                           const std::string& prefix) {
    std::vector<std::string> keys(count);
    uint32_t found = 0;
    for (uint32_t i = 0; i < 10000000 && found < count; i++) {
        const std::string key = prefix + std::to_string(i);
        const uint32_t home = mix64(hash(key)) & (cap - 1);
        if (home >= first && home - first < count && keys[home - first].empty()) {
            keys[home - first] = key; found++;
        }
    }
    require(found == count, "bounded exact-slot geometry construction");
    return keys;
}

struct Group {
    ScatterState state;
    std::atomic<uint64_t> epoch{0};
    std::atomic<uint32_t> refs{0};
    std::atomic<bool> aborted{false};
    void* entry = nullptr;
    size_t parked = 0, installed = 0;
    explicit Group(std::vector<std::string> keys) { state.keys = std::move(keys); }
    void prepare(FlatStore& s) {
        uint64_t bytes = 0, membership = 0;
        for (const auto& key : state.keys) { bytes += key.size(); membership |= atomic_membership_bit(hash(key)); }
        entry = s.atomic_prepare_group(&state, 0, state.keys.size(), &epoch, &refs, &aborted,
                                        42, nullptr, membership, true, bytes);
        require(entry != nullptr, "group preparation");
    }
    void install(FlatStore& s, uint32_t i, KvObj* value) {
        const auto& key = state.keys.at(i);
        if (KvObj* old = s.atomic_install_group(entry, hash(key), slice(key), value)) parked += kvobj_size(old);
        if (value) installed += kvobj_size(value);
    }
    void publish(FlatStore& s) {
        s.atomic_finish_group_install(parked, installed);
        s.atomic_publish_group(entry);
        refs.fetch_add(1); // The scatter caller pins its arena after the owner installs a record.
        require(s.atomic_pending_entries() != 0, "published held group");
    }
    void finish(FlatStore& s, bool abort = false) {
        aborted.store(abort); epoch.store(1);
        for (uint32_t i = 0; i < 10000 && s.atomic_pending_entries(); i++)
            s.atomic_sweep(UINT64_MAX, UINT64_MAX, 1);
        require(s.atomic_pending_entries() == 0, "bounded group cleanup completed");
        require(refs.load() == 0, "group arena reference released exactly once");
        entry = nullptr;
    }
};

static void unlinked() {
    for (bool armed : {false, true}) {
        Group g({"candidate-a", "candidate-b"}); FlatStore s(64); arm(s, armed);
        KvObj* a = string_object(g.state.keys[0]); KvObj* b = string_object(g.state.keys[1]);
        s.configure_maxmemory(true, kvobj_size(a) + 12, MaxmemoryPolicy::AllKeysRandom, 64);
        require(s.atomic_prepare_capacity(2), "candidate pass capacity"); g.prepare(s);
        require(s.atomic_admit(slice(g.state.keys[0]), a), "first candidate admitted");
        g.install(s, 0, a);
        require(!s.atomic_has_record(hash(g.state.keys[0]), slice(g.state.keys[0])) &&
                s.find_resident(hash(g.state.keys[0]), slice(g.state.keys[0])) == a,
                "installed candidate is unlinked and resident");
        // Establish the sampler can select a, independently of the admission guard.
        require(FlatStoreRegressionTest::candidate(s, g.state.keys[1]) == a, "candidate is sampleable");
        const size_t before = s.object_bytes();
        require(!s.atomic_admit(slice(g.state.keys[1]), b), "refuse eviction during an unlinked span");
        require(s.object_bytes() == before && s.find_resident(hash(g.state.keys[0]), slice(g.state.keys[0])) == a,
                "unlinked candidate survives pressure");
        kvobj_free(b); g.publish(s); g.finish(s, true);
        require(s.size() == 0, "aborted fresh candidate removed");
    }
}

static void randomkey() {
    for (bool armed : {false, true}) {
        {
            FlatStore s; arm(s, armed); s.set_cached_now_ms(100);
            const std::string value(128 * 1024, 'x');
            KvObj* o = put(s, "snapshot-ttl", value, 200);
            require(s.snapshot_prepare(1, 100) == FlatStore::SnapshotWriteResult::Ready &&
                    s.snapshot_mark(0, 100), "snapshot frozen");
            s.snapshot_progress(1, 2048);
            require(FlatStoreRegressionTest::capturing_record(s, o), "record held partway through serialization");
            const size_t before = s.object_bytes(); s.set_cached_now_ms(201);
            require(s.random_live() == nullptr, "RANDOMKEY logically excludes expired snapshot key");
            require(s.size() == 1 && s.object_bytes() == before &&
                    s.find_resident(hash("snapshot-ttl"), slice(std::string("snapshot-ttl"))) == o,
                    "RANDOMKEY retains serializer's object");
            for (unsigned i = 0; i < 1000 && s.snapshot_active(); i++) {
                s.snapshot_progress(4096, 2048);
                if (s.snapshot_take_chunk()) s.snapshot_handoff_complete();
            }
            require(!s.snapshot_active() && !s.snapshot_failed(), "snapshot resumed after expiry");
        }
        {
            Group g({"private"}); FlatStore s; arm(s, armed); g.prepare(s);
            KvObj* value = string_object("private"); require(s.atomic_admit(slice(g.state.keys[0]), value), "admit private key");
            g.install(s, 0, value); g.publish(s);
            require(s.size() == 1 && g.epoch.load() == 0, "fresh undecided physical candidate");
            require(s.random_live() == nullptr, "RANDOMKEY hides undecided candidate"); g.finish(s);
            require(s.random_live() == value, "RANDOMKEY sees committed candidate");
        }
        {
            Group g({"deleted"}); FlatStore s; arm(s, armed); KvObj* old = put(s, "deleted");
            g.prepare(s); g.install(s, 0, nullptr); g.publish(s);
            require(s.size() == 0 && s.atomic_pending_entries() == 1, "only visible key is parked");
            require(s.random_live() == old, "RANDOMKEY samples parked predecessor"); g.finish(s, true);
        }
    }
}

static void rehash_capacity() {
    for (bool armed : {false, true}) {
        FlatStore s; arm(s, armed);
        const auto live = keys_at_slots(1024, 424, 600, "old-");
        const auto tombs = keys_at_slots(1024, 0, 100, "tomb-");
        for (const auto& key : live) FlatStoreRegressionTest::raw_insert(s, string_object(key));
        for (const auto& key : tombs) FlatStoreRegressionTest::raw_insert(s, string_object(key));
        for (const auto& key : tombs) FlatStoreRegressionTest::raw_erase(s, key);
        require(s.atomic_prepare_capacity(100) && s.rehashing() && FlatStoreRegressionTest::cap(s) == 1024,
                "600 live + 100 tombstones starts same-size atomic rehash");
        std::vector<std::string> added;
        for (unsigned pass = 0; pass < 12; pass++) {
            const bool admitted = pass == 0 || s.atomic_prepare_capacity(100);
            require(admitted, "room for an atomic pass without a spurious OOM");
            for (unsigned i = 0; i < 100; i++) {
                added.push_back("fresh-" + std::to_string(pass * 100 + i));
                FlatStoreRegressionTest::raw_insert(s, string_object(added.back()));
            }
            require(!s.rehashing() || s.size() < FlatStoreRegressionTest::cap(s),
                    "destination reserves every unmoved old key");
        }
        for (unsigned i = 0; i < 10000 && s.rehashing(); i++) FlatStoreRegressionTest::step(s);
        require(!s.rehashing() && s.size() == live.size() + added.size(), "rehash drains without loss");
        for (const auto& key : live) require(s.find(hash(key), slice(key)) != nullptr, "old key survived rehash");
        for (const auto& key : added) require(s.find(hash(key), slice(key)) != nullptr, "new key survived rehash");
    }
}

static void rollback() {
    for (bool armed : {false, true}) {
        auto keys = keys_at_slots(1024, 0, 700, "rollback-");
        Group g(keys); FlatStore s; arm(s, armed);
        for (const auto& key : keys) FlatStoreRegressionTest::raw_insert(s, string_object(key));
        require(s.atomic_prepare_capacity(700) && FlatStoreRegressionTest::cap(s) == 2048, "delete preflight grows table");
        g.prepare(s);
        for (unsigned i = 0; i < keys.size(); i++) g.install(s, i, nullptr);
        g.publish(s);
        require(s.size() == 0 && s.atomic_pending_entries() == 1, "700 predecessors held by undecided deletion");
        for (unsigned i = 0; i < 1000 && s.rehashing(); i++) FlatStoreRegressionTest::step(s);
        const auto unrelated = keys_at_slots(2048, 0, 1400, "unrelated-");
        for (const auto& key : unrelated) put(s, key);
        require(s.size() == 1400 && FlatStoreRegressionTest::cap(s) == 2048 && !s.rehashing(),
                "rollback needs 2100 slots in a 2048-slot destination");
        g.aborted.store(true);
        const size_t before = s.object_bytes();
        flatstore_debug_fail_table_allocations(1);
        require(s.atomic_sweep(UINT64_MAX, UINT64_MAX, 1) == 0, "rollback allocation failure defers cleanup");
        require(g_flatstore_table_alloc_failures.load() == 0 && s.atomic_pending_entries() == 1 &&
                s.object_bytes() == before && s.size() == 1400, "failed preflight retains all ownership/accounting");
        g.finish(s, true);
        require(s.size() == 2100, "all rollback and unrelated keys remain");
        for (const auto& key : keys) require(s.find(hash(key), slice(key)) != nullptr, "predecessor restored");
        for (const auto& key : unrelated) require(s.find(hash(key), slice(key)) != nullptr, "unrelated write retained");
    }
}

static void snapshot_eviction() {
    FlatStore s(64); put(s, "frozen");
    require(s.snapshot_prepare(1, 100) == FlatStore::SnapshotWriteResult::Ready && s.snapshot_mark(0, 100),
            "snapshot eviction fixture frozen");
    put(s, "postcut", "abcde");
    s.configure_maxmemory(true, 1, MaxmemoryPolicy::AllKeysRandom, 64);
    require(s.accounted_bytes() > 1 && FlatStoreRegressionTest::candidate(s, "postcut") != nullptr,
            "over-budget store has a frozen eviction victim");
    const size_t before = s.object_bytes(); deletes.clear();
    require(s.try_overwrite(hash("postcut"), slice(std::string("postcut")), slice(std::string("vwxyz"))) ==
                FlatStore::OverwriteResult::Updated, "same-class overwrite runs during capture");
    require(s.size() == 2 && s.object_bytes() == before && deletes.empty(), "overwrite cannot evict frozen image");
    s.snapshot_cancel();
}

static void flags() {
    // Replacing the synchronized byte with a plain byte must fail even without a race detector.
    static_assert(std::is_same_v<decltype(KvObj::flags), KvObjFlagByte>);
    static_assert(!std::is_copy_assignable_v<KvObjFlagByte>);
    const std::string key(300, 'k'); KvObj* o = string_object(key, "value", 1000);
    const size_t bytes = kvobj_size(o); std::barrier start(2);
    std::atomic<uint32_t> touches{0};
    std::thread reader([&] {
        start.arrive_and_wait();
        for (uint32_t i = 0; i < 100000; i++) {
            o->touch_eviction_meta_foreign(o->read_local_flags(), i & 31); touches++;
        }
    });
    start.arrive_and_wait();
    for (uint32_t i = 0; i < 100000; i++) {
        require(o->has_ttl_slot() && o->klen() == key.size() && o->key() == slice(key) &&
                o->expire_at_ms() == 1000 && kvobj_size(o) == bytes, "layout reads coexist with foreign metadata CAS");
        o->set_eviction_meta(i & 31);
    }
    reader.join(); require(touches == 100000, "foreign touches executed"); kvobj_free(o);
}

static void aof_eviction() {
    for (bool armed : {false, true}) {
        FlatStore s(64); arm(s, armed); KvObj* old = put(s, "victim");
        KvObj* incoming = string_object("new");
        s.configure_maxmemory(true, kvobj_size(old) + 12, MaxmemoryPolicy::AllKeysRandom, 64);
        uint64_t evicted = 0; s.bind_evicted_counter(&evicted); deletes.clear();
        require(s.atomic_admit(slice(std::string("new")), incoming), "atomic admission succeeds through eviction");
        require(evicted == 1 && s.size() == 0, "unrelated victim actually evicted");
        require(deletes == std::vector<std::string>{"victim"}, "eviction emits exactly one independent AOF DEL");
        s.atomic_unadmit(incoming); kvobj_free(incoming);
    }
}

static void intents() {
    for (bool armed : {false, true}) {
        FlatStore s(64); arm(s, armed); KvObj* o = put(s, "pinned");
        s.configure_maxmemory(true, 1, MaxmemoryPolicy::AllKeysRandom, 64);
        require(FlatStoreRegressionTest::candidate(s) == o, "victim sampling positive control");
        require(s.atomic_script_pin(hash("pinned"), slice(std::string("pinned"))), "script pin created");
        require(s.atomic_has_script_intent(hash("pinned"), slice(std::string("pinned"))) &&
                !s.atomic_has_record(hash("pinned"), slice(std::string("pinned"))), "intent exists without version record");
        require(FlatStoreRegressionTest::candidate(s) == nullptr && !s.budget_admit(slice(std::string("other"))),
                "ordinary eviction excludes intent-only key");
        require(s.size() == 1, "pinned predecessor retained");
        s.atomic_script_unpin(hash("pinned"), slice(std::string("pinned")));
        require(s.budget_admit(slice(std::string("other"))) && s.size() == 0, "unpin restores ordinary eviction");
    }
}

static KvObj* hash_object(const std::string& key) {
    auto* value = new HashVal;
    const char pair[] = {1, 'f', '1', '0'};
    require(value->append({pair, sizeof(pair)}), "hash field seeded");
    value->compact_payload_bytes = 3;
    value->ttls = new HashFieldTtl;
    require(value->ttls->set(slice(std::string("f")), 200), "hash field deadline installed");
    KvObj* object = kvobj_new_hash(slice(key), value);
    require(object != nullptr, "external hash allocation"); hash_ttl_note_bytes(object);
    require(hash_ttl_field_count(object) == 1, "one field in real hash representation");
    return object;
}

static void imported_hash() {
    for (bool armed : {false, true}) {
        Group g({"imported"}); FlatStore s; arm(s, armed); s.set_cached_now_ms(100);
        KvObj* o = hash_object("imported");
        require(s.field_expire_count() == 0, "destination has never registered a field deadline");
        require(s.atomic_admit(slice(g.state.keys[0]), o), "import admitted");
        g.prepare(s); g.install(s, 0, o); g.publish(s); g.finish(s);
        require(s.field_expire_count() != 0 && FlatStoreRegressionTest::field_index_size(s) == 1,
                "atomic APPLY registered imported field deadlines");
        s.set_cached_now_ms(201);
        require(s.active_expire_fields(1024) == 1 && s.size() == 0, "imported field expires on destination");
    }
}

static void field_index_failure() {
    FlatStore s; s.set_cached_now_ms(100); KvObj* o = hash_object("missed");
    require(s.insert(hash("missed"), o) == FlatStore::InsertResult::Inserted, "TTL-bearing hash seeded");
    flatstore_debug_fail_table_allocations(1); s.note_field_ttl(hash("missed"));
    require(g_flatstore_table_alloc_failures.load() == 0 && FlatStoreRegressionTest::field_index_size(s) == 0,
            "first field-index allocation really failed");
    require(s.field_expire_count() != 0, "missed index must keep the lazy gate armed");
    // A later successful registration/removal must not erase knowledge of the unindexed key.
    s.note_field_ttl(hash("stale")); s.active_expire_fields(1024);
    require(FlatStoreRegressionTest::field_index_size(s) == 0 && s.field_expire_count() != 0,
            "stale-entry cleanup cannot disarm missed registration");
    s.set_cached_now_ms(201); uint32_t reaped = 0;
    const size_t before = kvobj_size(o);
    require(hash_ttl_active_reap(s, o, 201, reaped) && reaped == 1, "real lazy-reap dependency can collect missed field");
    s.note_object_size_change(before, kvobj_size(o));
    // This case verifies the gate, not the separate command-side shrink-accounting finding.
    s.clear(); require(s.field_expire_count() == 0, "FLUSH resets sticky attention");
}

static void hash_bytes() {
    for (bool armed : {false, true}) {
        FlatStore s; arm(s, armed);
        for (unsigned i = 0; i < 12; i++) {
            s.set_cached_now_ms(100); KvObj* o = hash_object("one-field");
            require(s.insert(hash("one-field"), o) == FlatStore::InsertResult::Inserted, "accounted hash seeded");
            s.note_field_ttl(hash("one-field"));
            require(s.object_bytes() == kvobj_size(o), "TTL bytes charged before reap");
            s.set_cached_now_ms(201);
            require(s.active_expire_fields(1024) == 1 && s.size() == 0, "last hash field actively reaped");
            require(s.object_bytes() == 0, "last-field reaping leaves no phantom bytes");
        }
    }
}

static void deadline_sidecar() {
    require(kTtlDeadlineSidecar, "deadline case must run a sidecar-enabled build");
    FlatStore s; s.set_cached_now_ms(100);
    for (unsigned i = 0; i < 11; i++) put(s, "ttl-" + std::to_string(i), "value", 200);
    require(s.expire_count() == 11, "index at allocation-triggering occupancy");
    flatstore_debug_fail_table_allocations(1);
    require(s.set_expire(hash("ttl-0"), slice(std::string("ttl-0")), 400) == FlatStore::TtlResult::Updated,
            "deadline extension completes despite attention allocation failure");
    require(g_flatstore_table_alloc_failures.load() == 0, "sidecar growth allocation failed");
    s.set_cached_now_ms(201); KvObj* o = s.find(hash("ttl-0"), slice(std::string("ttl-0")));
    require(o && s.deadline(hash("ttl-0"), o) == 400, "stale sidecar cannot expire extended deadline");
    s.set_cached_now_ms(401);
    require(s.find(hash("ttl-0"), slice(std::string("ttl-0"))) == nullptr, "extended deadline still expires");
}

int main(int argc, char** argv) {
    require(argc == 2, "select one named regression");
    struct Case { const char* name; void (*run)(); };
    const Case cases[] = {{"unlinked", unlinked}, {"randomkey", randomkey}, {"rehash", rehash_capacity},
        {"rollback", rollback}, {"snapshot-eviction", snapshot_eviction}, {"flags", flags},
        {"aof-eviction", aof_eviction}, {"intents", intents}, {"imported-hash", imported_hash},
        {"field-index-failure", field_index_failure}, {"hash-bytes", hash_bytes}, {"deadline-sidecar", deadline_sidecar}};
    for (const auto& c : cases) if (std::strcmp(argv[1], c.name) == 0) {
        c.run(); std::printf("PASS storage %s\n", c.name); return 0;
    }
    require(false, "unknown regression");
}
