// Real FlatStore + real QSBR queue, without worker threads or sockets. Each child pins one
// non-parked participant and fills all 4096 entries. A one-second alarm bounds the attempted
// read; the parent treats that deadline as FAILURE except in the explicit blocking control.
// `lookups` isolates the cx-waits fix; `retirement` also asserts the broader law for lazy expiry.
// No mock reclaim policy, probabilistic timing window, retrying reader, or successful skip.
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "src/core/read_local.h"
#include "src/core/ex_loop.h"
#include "src/cmd/t_hash_ttl.h"

using namespace tomo;

static void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL rehash waits: %s\n", why); std::_Exit(1); }
}
static Slice slice(const std::string& s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
static uint64_t hash(const std::string& s) { return FlatStore::hash_key(slice(s)); }

namespace tomo {
// Reuse the tree's existing serverless-fixture access. Only real QSBR participants are needed;
// no scheduler, IO ring, topology or CONFIG publisher runs in this queue/lookup test.
struct CoreConcurrencyTest {
    static void idle_visit_bound();
    static void participants(Server& server) {
        server.read_local_state_ = std::make_unique<ReadLocalServerState>();
        for (uint32_t tid = 0; tid < 8; tid++) {
            auto thread = std::make_unique<ThreadCtx>();
            require(thread->init_read_local_state(), "participant allocation");
            thread->publish_read_local_parked(server.read_local_epoch());
            server.threads_.push_back(std::move(thread));
        }
        require(server.nthreads() == 8, "eight real QSBR participants");
    }
};
} // namespace tomo

struct Fixture {
    static constexpr uint32_t owner = 6, pinned = 0;
    Server server;
    ReadLocalDeferredQueue queue;
    FlatStore store{64};
    uint64_t starts = 0;
    uint32_t reclaimed = 0;
    std::array<unsigned char, ReadLocalDeferredQueue::kCapacity + 1> payloads{};
    std::string old_key, new_key;

    Fixture() {
        CoreConcurrencyTest::participants(server);
        require(queue.init(&server, &server.thread(owner)), "real retire queue allocation");
        require(store.prepare_read_local(), "armed store allocation");
        store.configure_read_local(true, *queue.sink());
        require(store.read_local_enabled(), "read-local must be armed");
        store.bind_rehash_counter(&starts);
        store.set_cached_now_ms(1000);
    }
    ~Fixture() {
        // No readers were started. Release the artificial pin only AFTER the read and assertions.
        server.thread(pinned).publish_read_local_parked(server.read_local_epoch());
        server.thread(owner).publish_read_local_parked(server.read_local_epoch());
        queue.drain_shutdown();
    }
    void put(const std::string& key, int64_t deadline = -1) {
        KvObj* value = kvobj_new_string(slice(key), Slice("value"), deadline);
        require(value != nullptr, "object allocation");
        require(store.insert(hash(key), value) == FlatStore::InsertResult::Inserted,
                "normal grow-triggering insertion");
    }
    void resizing(bool tracked) {
        // Place the first key in the last rehash block. It cannot have moved at cursor 56.
        for (unsigned n = 0; n < 10000; n++) {
            old_key = "tail-" + std::to_string(n);
            if ((mix64(hash(old_key)) & 63) >= 56) break;
        }
        require((mix64(hash(old_key)) & 63) >= 56, "bounded last-block key construction");
        put(old_key);
        if (tracked) {
            const std::string key = "tracked-record";
            void* entry = nullptr;
            require(store.atomic_prepare_plain(slice(key), hash(key), 42, entry), "MVCC entry");
            KvObj* value = kvobj_new_string(slice(key), Slice("value"));
            require(value != nullptr, "MVCC value");
            store.atomic_install_plain(hash(key), slice(key), entry, value, 2);
            store.atomic_finish_plain();
            require(store.atomic_has_records(), "tracked lookup really has an MVCC record");
        }
        for (unsigned n = 0; n < 128 && !store.rehashing(); n++) {
            new_key = "grow-" + std::to_string(n);
            put(new_key);
        }
        require(starts == 1 && store.rehashing(), "normal insertion must START one resize");
        for (unsigned n = 0; n < 7; n++) store.active_expire(1);
        const auto progress = store.rehash_progress();
        require(progress.old_capacity == 64 && progress.current_capacity == 128 &&
                progress.cursor == 56 && progress.old_live > 0,
                "one rehash step from table retirement, with a live key in the old table");
        require(store.expire_count() == 0, "lookup arm must have no TTL keys");
    }
    void full() {
        require(queue.empty(), "fresh queue before capacity arm");
        const uint64_t stamp = server.read_local_epoch();
        server.thread(pinned).publish_read_local_tick(stamp); // deliberately NEVER refreshed
        for (uint32_t i = 0; i < ReadLocalDeferredQueue::kCapacity; i++)
            queue.defer(this, &payloads[i], 1, [](void* owner, void*, size_t) {
                static_cast<Fixture*>(owner)->reclaimed++;
            });
        require(queue.size() == 4096 && queue.drain_ready() == 0, "full, sealed, unreclaimable ring");
        server.thread(owner).publish_read_local_tick(server.read_local_epoch());
        uint32_t hint = pinned;
        require(server.read_local_grace_floor(stamp, hint) == stamp && hint == pinned &&
                queue.drain_ready() == 0 && reclaimed == 0,
                "the ONE stale non-parked participant must pin the oldest batch");
        require(!ThreadCtx::read_local_publication_parked(server.thread(pinned).read_local_publication()),
                "stale participant cannot be parked");
    }
};

static volatile sig_atomic_t read_armed = 0;
static void deadline(int) {
    const char message[] = "DEADLINE: armed operation did not return within 1.000s\n";
    const ssize_t written = ::write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    std::_Exit(read_armed ? 42 : 2);
}

static const KvObj* lookup(FlatStore& store, const char* kind, const std::string& key) {
    const auto h = hash(key);
    const auto k = slice(key);
    if (!std::strcmp(kind, "find") || !std::strcmp(kind, "expiry")) return store.find(h, k);
    if (!std::strcmp(kind, "no-touch")) return store.find_no_touch(h, k);
    if (!std::strcmp(kind, "notify")) return store.find_notify(h, k, nullptr);
    if (!std::strcmp(kind, "resident")) return store.find_resident(h, k);
    if (!std::strcmp(kind, "tracked")) return store.atomic_find_tracked(h, k);
    if (!std::strcmp(kind, "resolve")) return store.atomic_resolve(h, k, UINT64_MAX);
    if (!std::strcmp(kind, "foreign")) {
        const auto result = store.read_local_probe(h, k);
        require(result.result == FlatStore::ReadLocalProbeResult::Hit ||
                result.result == FlatStore::ReadLocalProbeResult::Missing,
                "foreign lookup must execute, never decline/skip");
        return result.object;
    }
    require(false, "unknown lookup");
    return nullptr;
}

static void child(const char* kind) {
    std::signal(SIGALRM, deadline);
    alarm(10); // arming failure/hang is never the expected read deadline
    Fixture f;
    const bool expiry = !std::strcmp(kind, "expiry");
    const bool control = !std::strcmp(kind, "blocking-control");
    if (expiry) {
        f.put("elapsed", 100);
        const KvObj* value = f.store.find_resident(hash("elapsed"), Slice("elapsed"));
        require(value && f.store.deadline(hash("elapsed"), value) == 100 &&
                f.store.expire_count() == 1 && !f.store.rehashing(),
                "physically resident expired key, with NO resize to confound the result");
    } else if (!control) {
        f.resizing(!std::strcmp(kind, "tracked") || !std::strcmp(kind, "resolve"));
    }
    f.full();
    const auto before = f.store.rehash_progress();
    const uint64_t publication = f.server.thread(Fixture::pinned).read_local_publication();
    std::printf("ARMED %s ring=%u reclaims=%u stale_tick=%llu old_capacity=%u cursor=%u old_live=%u\n",
                kind, f.queue.size(), f.reclaimed, static_cast<unsigned long long>(publication),
                before.old_capacity, before.cursor, before.old_live);
    std::fflush(stdout);
    read_armed = 1;
    alarm(1);
    if (control) {
        // Establish that one additional retirement REALLY cannot return under this pin.
        f.queue.defer(&f, &f.payloads.back(), 1, [](void*, void*, size_t) {});
    } else if (expiry) {
        require(lookup(f.store, kind, "elapsed") == nullptr, "expired read returns logical absence");
    } else {
        for (const std::string& key : {f.old_key, f.new_key}) {
            const KvObj* value = lookup(f.store, kind, key);
            require(value && value->str_value() == Slice("value"), "old/new table lookup value");
        }
        require(lookup(f.store, kind, "absent-key") == nullptr, "miss searches both tables");
        const auto after = f.store.rehash_progress();
        require(after.current_capacity == before.current_capacity && after.old_capacity == before.old_capacity &&
                after.cursor == before.cursor && after.old_live == before.old_live,
                "lookup must not advance or retire the rehash table");
    }
    alarm(0);
    require(f.queue.size() == 4096 && f.reclaimed == 0 &&
            f.server.thread(Fixture::pinned).read_local_publication() == publication,
            "read returned without reclamation or releasing the pin");
    std::printf("RETURNED %s ring=4096 reclaims=0 stale_tick_unchanged=1\n", kind);
}

static bool run(const char* kind, bool expect_block = false) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    require(pid >= 0, "fork failed (no skip)");
    if (pid == 0) { child(kind); std::fflush(nullptr); std::_Exit(0); }
    int status = 0;
    pid_t result;
    do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    require(result == pid, "wait for our own child");
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == (expect_block ? 42 : 0);
    std::printf("%s %s%s (status=%d)\n", ok ? "PASS" : "FAIL", kind,
                expect_block ? " proves pinned retirement blocks" : " read must not wait for quiescence", status);
    return ok;
}

// Additional resize-fix checks; the gate's original `retirement` selection above is unchanged.
static void maintenance_child(bool expiry) {
    std::signal(SIGALRM, deadline);
    alarm(10);
    Fixture f;
    uint64_t expired_count = 0;
    f.store.bind_expired_counter(&expired_count);
    if (expiry) f.put("elapsed", 100);
    else f.resizing(false);
    const auto capture = expiry ? FlatStore::ReadLocalPrefetchCapture{}
                               : f.store.read_local_prefetch_capture(hash(f.old_key), slice(f.old_key));
    if (!expiry)
        require(capture.result == FlatStore::ReadLocalProbeResult::Hit && capture.slot &&
                capture.object, "capture an actual old-table slot before its retirement");
    f.full();
    const uint64_t pin = f.server.thread(Fixture::pinned).read_local_publication();
    read_armed = 1;
    alarm(1);
    if (expiry) {
        struct Events { uint32_t expired = 0, missed = 0; } events;
        FlatNotifySink sink{&events, [](void*, uint32_t) { return true; },
            [](void* p, uint32_t cls, NotifyEventId, Slice) {
                auto& e = *static_cast<Events*>(p);
                if (cls == NOTIFY_EXPIRED) e.expired++;
                if (cls == NOTIFY_KEY_MISS) e.missed++;
            }};
        notify_bind_flat_store(&f.store, &sink);
        for (unsigned i = 0; i < 3; i++) {
            require(!f.store.find_notify(hash("elapsed"), Slice("elapsed"), &sink),
                    "full-ring notified expiry returns logical absence");
            require(f.store.active_expire(64) != 0, "capacity-blocked maintenance remains work");
        }
        require(events.expired == 0 && events.missed == 3 && expired_count == 0 &&
                f.store.expire_count() == 1 && f.queue.size() == 4096 && f.reclaimed == 0 &&
                f.store.find_resident(hash("elapsed"), Slice("elapsed")),
                "no duplicate effects, unlink, retirement or lost expiry attention under pressure");
        require(f.server.thread(Fixture::pinned).read_local_publication() == pin,
                "maintenance did not release the stale participant");
        f.server.thread(Fixture::pinned).publish_read_local_parked(f.server.read_local_epoch());
        f.server.thread(Fixture::owner).publish_read_local_parked(f.server.read_local_epoch());
        require(f.queue.drain_ready() == 4096 && f.reclaimed == 4096, "release the real pin");
        require(f.store.active_expire(64) != 0 && !f.store.expire_count() &&
                !f.store.find_resident(hash("elapsed"), Slice("elapsed")) &&
                events.expired == 1 && expired_count == 1 && f.queue.size() == 1,
                "owner reaps and accounts exactly once after capacity becomes available");
        require(!f.store.find_notify(hash("elapsed"), Slice("elapsed"), &sink) &&
                events.expired == 1 && expired_count == 1, "later misses cannot re-emit expiry");
    } else {
        // Complete several distinct resizes with the SAME full ring and stale tick. This rules
        // out a one-table emergency slot that merely postpones the next capacity deadlock.
        for (unsigned generation = 0; generation < 4; generation++) {
            if (generation) {
                for (unsigned n = 0; n < 1024 && !f.store.rehashing(); n++)
                    f.put("later-" + std::to_string(generation) + "-" + std::to_string(n));
                require(f.store.rehashing(), "normal mutation must start the next resize");
            }
            const auto armed = f.store.rehash_progress();
            for (uint32_t step = 0; step < armed.old_capacity / 8 && f.store.rehashing(); step++)
                require(f.store.active_expire(0) != 0, "every resize step counts as owner work");
            require(!f.store.rehashing() && f.store.rehash_progress().cursor == 0 &&
                    f.queue.size() == 4097 + generation && f.queue.resize_pending() &&
                    f.queue.drain_ready() == 0 && f.reclaimed == 0 &&
                    f.server.thread(Fixture::pinned).read_local_publication() == pin,
                    "resize completes; all retired tables stay protected behind the stale pin");
            const auto* value = f.store.find(hash(f.old_key), slice(f.old_key));
            require(value && value->str_value() == Slice("value"), "value survives every resize");
        }
        // A captured foreign pointer remains readable until grace. ASAN also checks this load.
        require(__atomic_load_n(capture.slot, __ATOMIC_ACQUIRE) != 0 &&
                capture.object->str_value() == Slice("value"), "old table/object retained until grace");
        f.server.thread(Fixture::pinned).publish_read_local_parked(f.server.read_local_epoch());
        f.server.thread(Fixture::owner).publish_read_local_parked(f.server.read_local_epoch());
        require(f.queue.drain_ready() == 4100 && f.queue.empty() && f.reclaimed == 4096,
                "all four table records and the full object ring reclaim after grace");
    }
    alarm(0);
    std::printf("PASS maintenance %s\n", expiry ? "expiry effects" : "four pinned-ring resizes");
}

static void hash_expiry_child() {
    std::signal(SIGALRM, deadline);
    alarm(10);
    Fixture f;
    Shard shard;
    shard.init_private(&f.server, 0, TypeLimits{}, StreamLimits{});
    require(shard.store().prepare_read_local(), "hash store arm");
    shard.store().configure_read_local(true, *f.queue.sink());
    shard.set_cached_now_ms(1000);
    auto put_hash = [&](const char* key, bool partial) {
        auto* hash_value = new HashVal;
        const char pair[] = {1, 'f', '1', '0'};
        require(hash_value->append({pair, sizeof(pair)}), "real hash field");
        hash_value->compact_payload_bytes = 3;
        if (partial) {
            const char live[] = {1, 'g', '1', '0'};
            require(hash_value->append({live, sizeof(live)}), "permanent field");
            hash_value->compact_payload_bytes += 3;
        }
        hash_value->ttls = new HashFieldTtl;
        require(hash_value->ttls->set(Slice("f"), 200), "expired field deadline");
        KvObj* value = kvobj_new_hash(Slice(key), hash_value);
        require(value != nullptr, "real hash allocation");
        hash_ttl_note_bytes(value);
        require(shard.store().insert(hash(key), value) == FlatStore::InsertResult::Inserted,
                "install hash and field-expiry attention");
        shard.store().note_field_ttl(hash(key)); // the production HEXPIRE registration
        return value;
    };
    KvObj* elapsed = put_hash("elapsed-hash", false);
    KvObj* partial = put_hash("partial-hash", true);
    require(shard.store().field_expire_count() == 2, "both real hash attention entries armed");
    f.full();
    read_armed = 1;
    alarm(1);
    Op op;
    for (unsigned i = 0; i < 3; i++)
        require(!hash_ttl_on_access(shard, op, elapsed, false),
                "all-expired hash read returns absence without retirement");
    require(shard.store().field_expired() == 0 && hash_ttl_field_count(elapsed) == 1 &&
            shard.store().field_expire_count() == 2 && f.queue.size() == 4096 && f.reclaimed == 0,
            "deferred hash retains its fields, attention and zero expiry side effects");
    require(hash_ttl_on_access(shard, op, partial, false) == partial &&
            hash_ttl_field_count(partial) == 1 && hash_ttl_field_exists(partial, Slice("g")) &&
            !hash_ttl_field_exists(partial, Slice("f")) && shard.store().field_expired() == 1 &&
            f.queue.size() == 4096, "partial expiry preserves the live field without header retirement");
    f.server.thread(Fixture::pinned).publish_read_local_parked(f.server.read_local_epoch());
    f.server.thread(Fixture::owner).publish_read_local_parked(f.server.read_local_epoch());
    require(f.queue.drain_ready() == 4096, "hash release of pinned capacity");
    require(shard.store().active_expire(64) != 0 && shard.store().field_expired() == 2 &&
            !shard.store().find_resident(hash("elapsed-hash"), Slice("elapsed-hash")) &&
            shard.store().find_resident(hash("partial-hash"), Slice("partial-hash")) == partial,
            "owner eventually reaps only the all-expired hash");
    f.queue.drain_shutdown(); // the callbacks name this shard, which dies before Fixture
    alarm(0);
    std::puts("PASS maintenance hash expiry under pinned capacity");
}

static void snapshot_retirement_child() {
    std::signal(SIGALRM, deadline);
    alarm(10);
    Fixture f;
    f.resizing(false);
    require(f.store.active_expire(0) != 0 && f.queue.drain_ready() == 1,
            "consume the initial reserved record and reclaim its resize");
    require(f.store.snapshot_prepare(2, 1000) == FlatStore::SnapshotWriteResult::Ready &&
            f.store.snapshot_mark(0, 1000), "snapshot must reserve its own table retirement");
    f.store.snapshot_cancel(); // cancellation leaves an ordinary merge of both live tables
    f.full();
    read_armed = 1;
    alarm(1);
    const auto armed = f.store.rehash_progress();
    for (uint32_t step = 0; step < armed.old_capacity / 8 && f.store.rehashing(); step++)
        require(f.store.active_expire(0) != 0, "post-snapshot merge work");
    require(!f.store.rehashing() && f.queue.size() == 4097 && f.queue.drain_ready() == 0 &&
            f.store.find(hash(f.old_key), slice(f.old_key)),
            "snapshot merge completes behind a pinned, full ring without losing values");
    alarm(0);
    std::puts("PASS maintenance snapshot table retirement");
}

void CoreConcurrencyTest::idle_visit_bound() {
    std::signal(SIGALRM, deadline);
    alarm(10);
    Server server;
    participants(server);
    std::array<Shard, 32> shards;
    ThreadCtx& owner = server.thread(Fixture::owner);
    for (auto& shard : shards) owner.shards().push_back(&shard);
    FlatStore& store = shards.back().store();
    for (unsigned n = 0; n < 2048 && !store.rehashing(); n++) {
        const std::string key = "unsampled-" + std::to_string(n);
        KvObj* value = kvobj_new_string(slice(key), Slice("value"));
        require(value && store.insert(hash(key), value) == FlatStore::InsertResult::Inserted,
                "start resize on a shard outside the first twenty visits");
    }
    require(store.rehashing(), "late owned shard really resizing");
    const auto before = store.rehash_progress();
    ExLoop loop;
    loop.srv_ = &server;
    loop.self_ = &owner;
    loop.cached_now_ms_ = 1000;
    read_armed = 1;
    alarm(1);
    require(loop.active_expire_cycle() != 0 && store.rehash_progress().cursor == before.cursor,
            "an unvisited resize prevents parking, even before its first maintenance visit");
    const uint32_t rounds = 2 * (before.old_capacity / 8);
    for (uint32_t n = 0; n < rounds && store.rehashing(); n++)
        require(loop.active_expire_cycle() != 0, "round-robin resize debt keeps idle owner awake");
    require(!store.rehashing(), "idle maintenance reaches every owned shard within its visit bound");
    owner.shards().clear();
    alarm(0);
    std::puts("PASS maintenance idle owner with more shards than the visit budget");
}

static int maintenance_checks() {
    for (unsigned which = 0; which < 5; which++) {
        std::fflush(nullptr);
        const pid_t pid = fork();
        require(pid >= 0, "maintenance fork");
        if (!pid) {
            if (which < 2) maintenance_child(which == 1);
            else if (which == 2) hash_expiry_child();
            else if (which == 3) snapshot_retirement_child();
            else CoreConcurrencyTest::idle_visit_bound();
            std::fflush(nullptr);
            std::_Exit(0);
        }
        int status = 0;
        pid_t result;
        do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
        require(result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "maintenance must return without a quiescence deadline");
    }
    return 0;
}

int main(int argc, char** argv) {
    require(argc == 2, "usage: rehash-waits-unit lookups|retirement|expiry|<lookup>");
    const std::string selection = argv[1];
    if (selection == "maintenance") return maintenance_checks();
    bool ok = run("blocking-control", true);
    if (selection == "lookups" || selection == "retirement") {
        for (const char* kind : {"find", "no-touch", "notify", "resident", "tracked", "resolve", "foreign"})
            ok = run(kind) && ok;
        if (selection == "retirement") ok = run("expiry") && ok;
    } else {
        ok = run(argv[1]) && ok;
    }
    return ok ? 0 : 1;
}
