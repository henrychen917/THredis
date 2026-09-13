// Serverless L4 checks: drive the real scatter prepare/owner phases on pinned threads, inspect
// jemalloc's allocation arena, and fail on the old IO-prebuild implementation. No listener,
// io_uring initialization, workload generator, sleeps, or performance measurements.
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#include "src/cmd/xshard.cc"
#include <condition_variable>
#include <functional>

#ifndef TOMO_JEMALLOC
#error "owner-arena-unit checks the jemalloc arena contract; build with JE=1"
#endif

namespace {
void require(bool yes, const char* why) {
    if (!yes) { std::fprintf(stderr, "FAIL owner arena: %s\n", why); std::_Exit(1); }
}
thread_local size_t fail_size = 0;
thread_local unsigned fail_occurrence = 0;
thread_local bool allocation_failed = false;
thread_local bool audit_allocations = false;

// Track only the two record size classes while an owner executes. Long fixture keys keep the
// header class distinct from scatter/entry metadata. A fixed ledger avoids allocating from the
// allocation wrapper itself, and keeps observing frees during owner rollback AND IO destruction.
struct AllocationAudit {
    struct Block { void* memory = nullptr; size_t bytes = 0; };
    std::mutex mutex;
    Block live[16]{};
    size_t header_bytes = 0, payload_bytes = 0;
    unsigned headers = 0, payloads = 0;

    void start(size_t header, size_t payload) {
        std::lock_guard lock(mutex);
        require(!header_bytes && header && payload && header != payload, "distinct audit classes");
        header_bytes = header; payload_bytes = payload;
        headers = payloads = 0;
    }
    void allocated(void* memory, size_t bytes) {
        if (!audit_allocations || !memory) return;
        std::lock_guard lock(mutex);
        if (!header_bytes || (bytes != header_bytes && bytes != payload_bytes)) return;
        for (const auto& block : live)
            require(block.memory != memory, "tracked live blocks cannot be reused");
        for (auto& block : live) {
            if (block.memory) continue;
            block = {memory, bytes};
            (bytes == header_bytes ? headers : payloads)++;
            return;
        }
        require(false, "bounded allocation audit has capacity");
    }
    void freed(void* memory, size_t bytes) {
        if (!memory) return;
        std::lock_guard lock(mutex);
        for (auto& block : live) {
            if (block.memory != memory) continue;
            require(tomo::good_size(bytes) == block.bytes, "record uses its allocated size class at free");
            block = {};
            return;
        }
    }
    void finish(unsigned expected_headers, unsigned expected_payloads) {
        std::lock_guard lock(mutex);
        require(headers == expected_headers && payloads == expected_payloads,
                "allocation audit observed the complete materialization window");
        for (const auto& block : live)
            require(!block.memory, "every aborted record header and payload was freed");
        header_bytes = payload_bytes = 0;
    }
} allocation_audit;
}
extern "C" void* __real_mallocx(size_t, int);
extern "C" void __real_sdallocx(void*, size_t, int);
extern "C" void* __wrap_mallocx(size_t n, int flags) {
    if (fail_size == n && fail_occurrence && --fail_occurrence == 0) {
        fail_size = 0;
        allocation_failed = true;
        return nullptr;
    }
    void* memory = __real_mallocx(n, flags);
    allocation_audit.allocated(memory, n);
    return memory;
}
extern "C" void __wrap_sdallocx(void* memory, size_t n, int flags) {
    allocation_audit.freed(memory, n);
    __real_sdallocx(memory, n, flags);
}

using namespace tomo;
namespace {
Slice slice(const std::string& s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
unsigned arena_of(const void* memory) {
    unsigned arena = UINT32_MAX;
    size_t bytes = sizeof(arena);
    void* address = const_cast<void*>(memory);
    require(memory && mallctl("arenas.lookup", &arena, &bytes, &address, sizeof(address)) == 0,
            "allocation has an inspectable arena");
    return arena;
}

// Each worker binds exactly once, just as the real boot paths do. The caller owns the callable
// until it finishes, so this task handoff does not free caller-allocated std::function storage
// into the worker's tcache and accidentally turn the fixture into an allocator experiment.
class Worker {
public:
    explicit Worker(int cpu) : thread_([this, cpu] {
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(cpu, &cpus);
        require(sched_setaffinity(0, sizeof(cpus), &cpus) == 0, "pin unit worker");
        require(bind_thread_arena(), "production worker arena binding succeeds");
        size_t bytes = sizeof(arena);
        require(mallctl("thread.arena", &arena, &bytes, nullptr, 0) == 0, "read worker arena");
        std::unique_lock lock(mutex_);
        ready_ = true; cv_.notify_all();
        for (;;) {
            cv_.wait(lock, [&] { return action_ || stop_; });
            if (stop_) break;
            auto* action = action_;
            lock.unlock(); (*action)(); lock.lock();
            action_ = nullptr; cv_.notify_all();
        }
    }) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return ready_; });
    }
    ~Worker() {
        { std::lock_guard lock(mutex_); stop_ = true; cv_.notify_all(); }
        thread_.join();
    }
    void call(std::function<void()> action) {
        std::unique_lock lock(mutex_);
        require(!action_, "one task per fixture worker");
        action_ = &action; cv_.notify_all();
        cv_.wait(lock, [&] { return !action_; });
    }
    unsigned arena = UINT32_MAX;
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::function<void()>* action_ = nullptr;
    bool ready_ = false, stop_ = false;
    std::thread thread_;
};

struct Fixture {
    // Clear stores while their retire queues still exist; explicit teardown below handles this.
    Server server;
    ReadLocalDeferredQueue queues[8];
    bool notify_pending[8]{};
    std::unique_ptr<Worker> workers[8];
    uint32_t source, destination;
    int32_t sid_a, sid_b;
    bool armed;
    explicit Fixture(bool fused, bool read_local) : armed(read_local) {
        Config config;
        config.shards = 16; config.even_ifid = 6; config.even_ex = 2;
        config.thread_mode = fused ? ThreadMode::Fused : ThreadMode::Split;
        config.read_local = read_local; config.atomic = 1;
        config.key_lb = config.client_lb = 1; config.flip_auto = 0; config.save.clear();
        require(server.prepare_boot(config) && server.init(config), "16-shard fixture initialized");
        require(server.nthreads() == 8, "fixture needs exactly eight allowed CPUs");
        source = fused ? 0 : 6; destination = fused ? 1 : 7;
        require(!server.thread(source).shards().empty() &&
                !server.thread(destination).shards().empty(), "two owners have shards");
        sid_a = server.thread(source).shards().front()->id();
        sid_b = server.thread(destination).shards().front()->id();
        command_bind_server(&server);
        for (unsigned tid = 0; tid < 8; tid++) {
            server.bind_owner_notify_pending(tid, &notify_pending[tid]);
            if (armed) {
                require(queues[tid].init(&server, &server.thread(tid)), "owner retire queue");
                server.thread(tid).bind_read_local_retire_sink(*queues[tid].sink());
                server.thread(tid).publish_read_local_parked(server.read_local_epoch());
            }
            workers[tid] = std::make_unique<Worker>(server.placement().cpu_of_thread(tid));
        }
        for (unsigned sid = 0; sid < server.nshards(); sid++) {
            auto& store = server.shard(sid).store();
            if (armed) store.configure_read_local(true, *queues[server.worker_of_shard(sid)].sink());
        }
        require(workers[source]->arena != workers[destination]->arena, "owners have distinct arenas");
    }
    void on(int32_t sid, std::function<void()> action) {
        workers[server.worker_of_shard(sid)]->call(std::move(action));
    }
    std::string key(int32_t sid, const std::string& prefix) {
        for (unsigned i = 0; i < 100000; i++) {
            auto candidate = prefix + std::to_string(i);
            if (server.router().shard_of(FlatStore::hash_key(slice(candidate))) == sid) return candidate;
        }
        require(false, "bounded key routing search"); return {};
    }
    void release_records() {
        for (unsigned sid = 0; sid < server.nshards(); sid++)
            on(sid, [&] {
                // All fixture groups are decided and no independent read cut survives. Use live
                // cleanup: the shutdown-only helper destroys the armed sidecar itself.
                xshard_cleanup_shard_at(server.shard(sid), UINT64_MAX, UINT64_MAX, UINT32_MAX);
                require(server.shard(sid).store().atomic_pending_entries() == 0,
                        "decided records cleaned without destroying the armed store");
            });
    }
    void drain() {
        if (armed) for (unsigned tid = 0; tid < 8; tid++)
            workers[tid]->call([&] { queues[tid].drain_shutdown(); });
    }
    ~Fixture() {
        release_records(); drain();
        for (unsigned sid = 0; sid < server.nshards(); sid++)
            on(sid, [&] { server.shard(sid).store().clear(); });
        drain();
    }
};

struct Request {
    Fixture& f;
    std::vector<std::string> args;
    Op op;
    Client client{-1};
    ScatterArenaPool pool;
    ScatterState* state = nullptr;
    unsigned executed = 0;
    Request(Fixture& fixture, std::vector<std::string> argv) : f(fixture), args(std::move(argv)) {
        op.reset(); client.set_id(777);
        for (const auto& arg : args) require(op.push_arg(slice(arg)), "stable request argv");
        op.spec = command_lookup(op.arg(0));
        require(op.spec, "registered command");
        ScatterDispatch dispatch;
        require(xshard_prepare(f.server, op, pool, 0, client.id(), dispatch, true) ==
                ScatterPrepare::Ready, "scatter prepared");
        state = dispatch.state;
        require(state && state->atomic_write && state->nsub >= 2, "real atomic multi-shard path");
        for (unsigned i = 0; i < state->key_count; i++)
            require(!state->keys[i].stable_object && !state->keys[i].key_anchor,
                    "prepare must leave record allocation to the shard owner");
    }
    void group(unsigned index, size_t fail_bytes = 0, unsigned occurrence = 0) {
        require(index == executed && index < state->nsub, "execute every owner group once");
        auto& group = state->groups[index];
        const int32_t sid = group.shard;
        const auto tid = f.server.worker_of_shard(sid);
        f.on(sid, [&] {
            fail_size = fail_bytes; fail_occurrence = occurrence; allocation_failed = false;
            audit_allocations = true;
            require(xshard_execute(Task{&client, 0, sid, state}, f.server.shard(sid), op, tid) ==
                    ScatterTaskResult::Complete, "owner pass completed");
            audit_allocations = false;
            require(!fail_bytes || allocation_failed, "requested allocation failure actually fired");
            fail_size = 0; fail_occurrence = 0;
            complete_owner_record_wave(*state, tid);
        });
        executed++;
    }
    void finish() {
        while (executed < state->nsub) group(executed);
        if (!state->aborted.load()) state->epoch.store(f.server.atomic_commit(), std::memory_order_release);
    }
    void expect(const std::string& key, const std::string* value, bool check_arena = true) {
        const auto hash = FlatStore::hash_key(slice(key));
        const auto sid = f.server.router().shard_of(hash);
        f.on(sid, [&] {
            auto& store = f.server.shard(sid).store();
            store.atomic_set_read_context(f.server.atomic_snapshot(), 999);
            auto* object = store.atomic_find_tracked(hash, slice(key));
            require(bool(object) == bool(value), "committed presence matches whole-command result");
            if (object) {
                KvObjRawReadBuffer raw;
                const Slice actual = kvobj_string_value(object, raw);
                require(actual == slice(*value), "committed bytes match last supplied value");
                if (check_arena) {
                    const auto owner = f.workers[f.server.worker_of_shard(sid)]->arena;
                    require(arena_of(object) == owner, "record header allocated in current owner arena");
                    if (static_cast<Enc>(object->enc) == Enc::Extern)
                        require(arena_of(object->str_data()) == owner,
                                "external payload allocated in current owner arena");
                }
            }
            store.atomic_clear_read_epoch();
        });
    }
    ~Request() {
        require(executed == state->nsub, "every completion sentinel consumed");
        f.release_records(); f.drain();
        require(state->record_refs.load() == 0, "no retained scatter records after drain");
        xshard_destroy(state, pool, 0); pool.reap_deferred();
    }
};

void placement_and_duplicates(Fixture& f) {
    for (unsigned length : {64u, 192u, 193u, 256u, 1024u, 8193u}) {
        const auto a = f.key(f.sid_a, "arena-a-" + std::to_string(length) + "-");
        const auto b = f.key(f.sid_b, "arena-b-" + std::to_string(length) + "-");
        const std::string first(length, 'a'), last(length, 'b');
        for (const char* verb : {"MSET", "MSETNX"}) {
            const auto ka = a + verb, kb = b + verb;
            // Appending changes the route: derive the complete key on its intended owner again.
            const auto x = f.key(f.sid_a, ka), y = f.key(f.sid_b, kb);
            Request request(f, {verb, x, first, y, first, x, last});
            request.group(0);
            require(request.state->epoch.load() == 0, "first owner leaves the group undecided");
            request.expect(x, nullptr); request.expect(y, nullptr);
            request.finish();
            require(!request.state->aborted.load(), "duplicate keys do not fail NX validation");
            request.expect(x, &last); request.expect(y, &first);
        }
    }
}

void aborts(Fixture& f) {
    const std::string prefix(512, 'k');
    const auto a = f.key(f.sid_a, prefix + "oom-a-"), b = f.key(f.sid_a, prefix + "oom-b-");
    const auto c = f.key(f.sid_b, prefix + "oom-c-"), d = f.key(f.sid_b, prefix + "oom-d-");
    const std::string old(256, 'o'), fresh(8193, 'n');
    const size_t header_bytes = good_size(kvobj_alloc_size(a.size(), fresh.size(), false, Enc::Extern));
    const size_t payload_bytes = good_size(fresh.size());
    for (const auto& key : {a, b, c, d})
        require(good_size(kvobj_alloc_size(key.size(), fresh.size(), false, Enc::Extern)) == header_bytes,
                "all failure targets share the audited header class");
    { Request seed(f, {"MSET", a, old, b, old, c, old, d, old}); seed.finish(); }
    // Fail each of the first/second headers/payloads on each owner. A later-owner failure must
    // roll back already installed private values as well as reclaim the unlinked candidate span.
    // Inspect the ledger AFTER Request destruction so IO-owned anchors cannot hide a leak.
    for (bool payload_failure : {false, true}) {
        for (unsigned failing_group : {0u, 1u}) {
            for (unsigned occurrence : {1u, 2u}) {
                allocation_audit.start(header_bytes, payload_bytes);
                {
                    Request request(f, {"MSET", a, fresh, b, fresh, c, fresh, d, fresh});
                    require(request.state->nsub == 2, "two owner fragments for the failure window");
                    for (unsigned group = 0; group < request.state->nsub; group++) {
                        require(request.state->groups[group].count == 2, "two values per owner fragment");
                        request.group(group, group == failing_group
                            ? (payload_failure ? payload_bytes : header_bytes) : 0,
                            group == failing_group ? occurrence : 0);
                    }
                    request.finish();
                    require(request.state->aborted.load() &&
                            request.state->groups[failing_group].error == WorkError::Oom,
                            "header or payload OOM aborts the group");
                    request.expect(a, &old); request.expect(b, &old);
                    request.expect(c, &old); request.expect(d, &old);
                }
                const unsigned complete_values = 2 * failing_group + occurrence - 1;
                allocation_audit.finish(complete_values + payload_failure, complete_values);
            }
        }
    }
    const auto absent = f.key(f.sid_a, prefix + "nx-absent-");
    require(good_size(kvobj_alloc_size(absent.size(), fresh.size(), false, Enc::Extern)) == header_bytes,
            "NX candidate uses the audited header class");
    allocation_audit.start(header_bytes, payload_bytes);
    {
        Request nx(f, {"MSETNX", absent, fresh, c, fresh});
        nx.group(0);
        require(!nx.state->aborted.load(), "NX first owner installed its private candidate");
        nx.expect(absent, nullptr);
        nx.finish();
        require(nx.state->aborted.load(), "NX observes existing key on another owner");
        nx.expect(absent, nullptr); nx.expect(c, &old);
    }
    allocation_audit.finish(1, 1);
}

void immutable_retirement(Fixture& f) {
    if (!f.armed) return; // This case exercises a feature absent in the disarmed fixture.
    const auto a = f.key(f.sid_a, "grace-a-"), b = f.key(f.sid_b, "grace-b-");
    const std::string old(1024, 'r'), fresh(1024, 's');
    { Request seed(f, {"MSET", a, old, b, old}); seed.finish(); }
    KvObj* captured = nullptr;
    f.on(f.sid_a, [&] { captured = f.server.shard(f.sid_a).store().find(
        FlatStore::hash_key(slice(a)), slice(a)); });
    require(captured, "capture a live predecessor");
    f.server.thread(5).publish_read_local_tick(f.server.read_local_epoch());
    Request request(f, {"MSET", a, fresh, b, fresh});
    request.finish(); request.expect(a, &fresh); request.expect(b, &fresh);
    f.release_records();
    const auto owner = f.server.worker_of_shard(f.sid_a);
    require(f.queues[owner].size() != 0, "replacement actually entered QSBR retirement");
    f.workers[owner]->call([&] {
        require(f.queues[owner].drain_ready() == 0, "active reader holds the grace period open");
    });
    require(captured->str_value() == slice(old), "retained predecessor remains immutable");
    f.server.thread(5).publish_read_local_parked(f.server.read_local_epoch());
    f.workers[owner]->call([&] {
        require(f.queues[owner].drain_ready() != 0, "retirement progresses after the reader leaves");
    });
}

void migration(Fixture& f) {
    const auto a = f.key(f.sid_a, "move-a-"), b = f.key(f.sid_b, "move-b-");
    const std::string old(193, 'p'), fresh(4096, 'q');
    { Request seed(f, {"MSET", a, old, b, old}); seed.finish(); }
    // A live atomic apply wave excludes migration in production. Transfer at the quiescent edge
    // between requests, then prepare a new wave; do not manufacture a forbidden mid-wave move.
    require(f.server.reserve_shard_capacity(f.destination, 1), "reserve destination shard capacity");
    require(f.server.transfer_shard_quiesced(f.sid_a, f.source, f.destination), "shard transfer fired");
    require(f.server.worker_of_shard(f.sid_a) == f.destination, "new owner published");
    Request request(f, {"MSET", a, fresh, b, fresh});
    request.finish(); request.expect(a, &fresh); request.expect(b, &fresh);
    f.release_records();
    if (f.armed)
        require(f.queues[f.source].size() == 0 && f.queues[f.destination].size() != 0,
                "post-handoff retirement reaches only the new owner queue");
}
}

int main(int argc, char** argv) {
    require(argc == 3, "usage: owner-arena-unit 1s|2s read-local-0|read-local-1");
    const std::string mode = argv[1], lane = argv[2];
    require(mode == "1s" || mode == "2s", "known mode");
    require(lane == "read-local-0" || lane == "read-local-1", "known read-local arm");
    require(command_registry_init(false), "command registry initialized");
    Fixture fixture(mode == "1s", lane == "read-local-1");
    placement_and_duplicates(fixture); aborts(fixture); immutable_retirement(fixture); migration(fixture);
    std::printf("PASS owner arena %s %s: placement, atomic visibility, duplicates, OOM, NX, QSBR, migration\n",
                mode.c_str(), lane.c_str());
}
