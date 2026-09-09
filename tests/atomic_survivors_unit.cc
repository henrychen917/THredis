// Directed regressions for SURVIVING.md's atomic/transaction/script lane.
// Includes the real owner implementation so phases and allocation failures can be interleaved
// deterministically. No worker is started, no socket is opened, and no clock race is required.
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#include "src/cmd/xshard.cc"
#include <cstdio>
#include <type_traits>

namespace {
thread_local int fail_new_after = -1;
thread_local bool allocation_failed = false;
}
__attribute__((noinline)) void* operator new(std::size_t size) {
    if (fail_new_after >= 0 && fail_new_after-- == 0) {
        fail_new_after = -1;
        allocation_failed = true;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace tomo;
namespace {
void require(bool yes, const char* why) {
    if (!yes) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
Slice slice(const std::string& s) { return Slice(s.data(), s.size()); }
struct Request {
    std::vector<std::string> args;
    Op op;
    explicit Request(std::vector<std::string> a) : args(std::move(a)) {
        op.reset();
        for (const auto& s : args) require(op.push_arg(slice(s)), "argument allocation");
        op.spec = command_lookup(op.arg(0));
        require(op.spec != nullptr, "registered command");
    }
    std::string reply() const { return std::string(op.reply.data(), op.reply.size()); }
};
int sid(Server& server, const std::string& key) {
    return server.router().shard_of(FlatStore::hash_key(slice(key)));
}
std::string key_on(Server& server, int target, const char* prefix) {
    for (unsigned i = 0; i < 100000; i++) {
        std::string key = std::string(prefix) + std::to_string(i);
        if (sid(server, key) == target) return key;
    }
    require(false, "find a key on the required shard");
    return {};
}
std::string local(Server& server, std::vector<std::string> args, uint64_t conn = 91,
                  bool local_multi = false) {
    Request r(std::move(args));
    const int shard_id = r.args.size() > 1 ? sid(server, r.args[1]) : 0;
    Shard& shard = server.shard(shard_id);
    shard.set_cached_now_ms(now_realtime_ms(), 0);
    r.op.shard = shard_id;
    r.op.hash = r.args.size() > 1 ? FlatStore::hash_key(r.op.arg(1)) : 0;
    if (local_multi) r.op.mark_local_xshard();
    PlainForeignReadScope scope;
    require(xshard_plain_prepare(server, shard, r.op, conn, scope), "plain preparation");
    r.op.spec->handler(shard, r.op);
    xshard_plain_finish(shard, scope);
    return r.reply();
}
void release_records(Server& server) {
    for (unsigned s = 0; s < server.nshards(); s++)
        server.shard(s).store().atomic_shutdown_release_records();
}
std::string multi_io(Server& server, Client& client, std::vector<std::string> args) {
    Request r(std::move(args));
    MultiExecState* state = nullptr;
    const auto action = multi_handle_io(server, client, r.op, 0, state);
    if (action == MultiIoAction::LocalDone) return r.reply();
    require(action == MultiIoAction::Dispatch && state, "MULTI owner dispatch");
    state->now_cut_ms = now_realtime_ms();
    for (auto& command : state->commands)
        if (command->scatter) command->scatter->now_cut_ms = state->now_cut_ms;
    initialize_multi_owner_record_refs(*state);
    multi_dispatch_started(client, state);
    auto* carrier = client.rob().acquire();
    require(carrier != nullptr, "public transaction carrier allocated");
    carrier->spec = r.op.spec;
    std::vector<bool> done(state->shards.size(), false);
    unsigned remaining = done.size();
    for (unsigned pass = 0; remaining && pass < 1000; pass++) {
        for (unsigned i = 0; i < done.size(); i++) {
            if (done[i]) continue;
            const int s = state->shards[i];
            const auto result = multi_execute_task(server, multi_make_task(&client, 0, s, state),
                server.shard(s), server.worker_of_shard(s), 0, nullptr);
            if (result != MultiTaskResult::Retry) { done[i] = true; remaining--; }
        }
    }
    require(!remaining, "transaction owner phases completed (no reservation cycle)");
    set_state_reply(*state, r.op);
    if (state->reset_watch) { client.next_watch_generation(); client.clear_watch_dirty(); }
    release_records(server);
    destroy_multi_state(state);
    return r.reply();
}

struct ScatterRun {
    Server& server;
    Client client{-1};
    Request request;
    ScatterArenaPool pool;
    ScatterState* state = nullptr;
    ScatterRun(Server& s, std::vector<std::string> args) : server(s), request(std::move(args)) {
        client.set_id(777);
        ScatterDispatch dispatch;
        require(xshard_prepare(server, request.op, pool, 0, client.id(), dispatch, true) ==
                ScatterPrepare::Ready, "cross-owner scatter prepared");
        state = dispatch.state;
        state->now_cut_ms = now_realtime_ms();
        require(state->nsub > 1, "scatter spans multiple shards");
    }
    void phase(ScriptPhase p) {
        state->script_phase = p;
        state->phase = p == ScriptPhase::Apply ? 2 : 1;
        if (p == ScriptPhase::Run) {
            const int s = state->script_coordinator_shard;
            require(xshard_execute(Task{&client, 0, s, state}, server.shard(s), request.op,
                    server.worker_of_shard(s)) == ScatterTaskResult::Complete, "script RUN");
            return;
        }
        const auto count = script_select_positions(*state, p);
        reset_groups(*state);
        build_groups(*state, state->hop2, count);
        if (p == ScriptPhase::Apply) initialize_owner_completion(*state);
        for (unsigned i = 0; i < state->nsub; i++) {
            const int s = state->groups[i].shard;
            require(xshard_execute(Task{&client, 0, s, state}, server.shard(s), request.op,
                    server.worker_of_shard(s)) == ScatterTaskResult::Complete, "script owner phase");
        }
    }
    ~ScatterRun() {
        if (!state->epoch.load()) state->aborted.store(true);
        for (unsigned i = 0; i < state->owner_slots; i++)
            while (state->owner_record_refs[i].remaining)
                complete_owner_record_wave(*state, i);
        release_records(server);
        xshard_destroy(state, pool, 0);
        pool.reap_deferred();
    }
};

void admission(Server& server) {
    const auto a = key_on(server, 0, "admit-a"), b = key_on(server, 0, "admit-b");
    const auto c = key_on(server, 1, "admit-c");
    const std::string value(65536, 'x');
    ScatterRun run(server, {"MSET", a, value, b, value, c, value});
    auto& store = server.shard(0).store();
    require(store.size() == 0 && !store.read_local_enabled(), "empty unborrowed admission shard");
    const auto baseline = store.accounted_bytes();
    store.configure_maxmemory(true, baseline + 100000, MaxmemoryPolicy::AllKeysRandom, 64);
    auto* group = group_for(*run.state, 0);
    require(group && group->count == 2, "two candidates share the admission owner");
    require(xshard_execute(Task{&run.client, 0, 0, run.state}, server.shard(0), run.request.op,
            server.worker_of_shard(0)) == ScatterTaskResult::Complete, "admission owner ran");
    require(group->error == WorkError::Maxmemory && run.state->aborted.load(),
            "second admission exceeded capacity");
    require(run.state->keys[run.state->key_order[group->begin]].key_anchor != nullptr,
            "first candidate was materialized but remains unlinked");
    require(store.size() == 0 && store.atomic_pending_entries() == 0,
            "failed admission installed no evictable candidate");
    store.configure_maxmemory(false, 0, MaxmemoryPolicy::NoEviction, 5);
}
void script_keys(Server& server) {
    const auto a = key_on(server, 0, "aof-key"), arg = key_on(server, 1, "aof-arg");
    require(server.worker_of_shard(0) != server.worker_of_shard(1), "distinct AOF owners");
    for (const char* verb : {"EVAL", "EVALSHA", "FCALL"}) {
        Request r({verb, "return redis.call('SET',KEYS[1],ARGV[1])", "1", a, arg});
        MultiQueuedCommand queued;
        require(queued_copy(r.op, queued), "copy queued script");
        std::vector<uint32_t> keys;
        command_key_args(queued, keys);
        require(keys == std::vector<uint32_t>{3}, "AOF participant enumeration excludes ARGV");
    }
    Request r({"EVAL", "return 42", "0", arg});
    MultiQueuedCommand queued;
    require(queued_copy(r.op, queued), "zero-key script queued");
    std::vector<uint32_t> keys;
    command_key_args(queued, keys);
    require(keys.empty(), "zero-key script creates no phantom write participant");
}
void rename(Server& server, bool watch) {
    const auto a = key_on(server, 0, "rename-a"), b = key_on(server, 1, "rename-b");
    require(server.worker_of_shard(0) != server.worker_of_shard(1), "RENAME crosses owners");
    require(local(server, {"SET", a, "old"}) == "+OK\r\n", "seed RENAME source");
    Client client(-1); client.set_id(81);
    if (watch) require(multi_io(server, client, {"WATCH", a}) == "+OK\r\n", "WATCH registered");
    require(multi_io(server, client, {"MULTI"}) == "+OK\r\n", "MULTI opened");
    if (!watch) require(multi_io(server, client, {"SET", a, "new"}) == "+QUEUED\r\n", "SET queued");
    require(multi_io(server, client, {"RENAME", a, b}) == "+QUEUED\r\n", "RENAME queued");
    const auto reply = multi_io(server, client, {"EXEC"});
    require(reply == (watch ? "*1\r\n+OK\r\n" : "*2\r\n+OK\r\n+OK\r\n"), "EXEC reply");
    require(local(server, {"GET", a}) == "$-1\r\n", "RENAME removes source");
    require(local(server, {"GET", b}) == (watch ? "$3\r\nold\r\n" : "$3\r\nnew\r\n"),
            "RENAME consumes the transaction's own source version");
}
void write_latest(Server& server) {
    const auto key = key_on(server, 0, "increment");
    require(local(server, {"SET", key, "0"}) == "+OK\r\n", "seed counter");
    auto& store = server.shard(0).store();
    const auto hash = FlatStore::hash_key(slice(key));
    require(store.atomic_script_pin(hash, slice(key)), "counter intent pinned");
    const auto cut = server.atomic_snapshot();
    require(server.atomic_commit_reserve() > cut, "unrelated reservation drawn");
    require(local(server, {"INCR", key}, 101) == ":1\r\n", "first connection increment");
    require(server.atomic_snapshot() == cut && store.atomic_has_record(hash, slice(key)),
            "safe watermark held behind a completed write");
    require(local(server, {"INCR", key}, 102) == ":2\r\n", "second connection sees first increment");
    const auto fresh = key_on(server, 0, "increment-fresh");
    const auto fresh_hash = FlatStore::hash_key(slice(fresh));
    require(store.atomic_script_pin(fresh_hash, slice(fresh)), "fresh-key intent pinned");
    require(local(server, {"SET", fresh, "new"}, 103) == "+OK\r\n", "fresh key committed after safe cut");
    require(local(server, {"MSETNX", fresh, "wrong"}, 104, true) == ":0\r\n",
            "local multi-key conditional writer sees the newest committed key");
    store.atomic_script_unpin(fresh_hash, slice(fresh));
    store.atomic_script_unpin(hash, slice(key));
    server.atomic_commit_publish();
    release_records(server);
}
void script_apply(Server& server) {
    const auto x = key_on(server, 0, "apply-x"), y = key_on(server, 1, "apply-y");
    require(local(server, {"SET", x, "B"}) == "+OK\r\n", "seed script image");
    ScatterRun run(server, {"EVAL", "redis.call('GET',KEYS[2]); return redis.call('APPEND',KEYS[1],'S')",
                           "2", x, y});
    // PIN is already grouped by xshard_prepare; the remaining phases use the production planner.
    for (unsigned i = 0; i < run.state->nsub; i++) {
        const int s = run.state->groups[i].shard;
        require(xshard_execute(Task{&run.client, 0, s, run.state}, server.shard(s), run.request.op,
                server.worker_of_shard(s)) == ScatterTaskResult::Complete, "PIN ran");
    }
    run.state->script_cut = server.atomic_snapshot();
    run.phase(ScriptPhase::Read);
    run.phase(ScriptPhase::Run);
    require(run.request.reply() == ":2\r\n" && run.state->status[0] == 3,
            "script produced the saved BS image from a read-modify-write");
    run.state->script_ticket = server.atomic_commit_reserve();
    run.phase(ScriptPhase::Validate);
    require(!run.state->script_conflict.load(), "initial validation succeeded");
    run.state->script_apply_after_unpin = true;
    run.phase(ScriptPhase::Unpin);
    auto& store = server.shard(0).store();
    const auto hash = FlatStore::hash_key(slice(x));
    require(store.atomic_has_script_intent(hash, slice(x)), "write intent survives UNPIN");
    require(!server.shard(1).store().atomic_has_script_intent(FlatStore::hash_key(slice(y)), slice(y)),
            "read-only intent actually released in the same UNPIN wave");
    require(local(server, {"APPEND", x, "W"}, 222) == ":2\r\n", "foreign APPEND completed in the window");
    require(store.atomic_has_record(hash, slice(x)), "intervening writer drew a version");
    run.phase(ScriptPhase::Apply);
    require(run.state->script_conflict.load() && run.state->aborted.load(), "APPLY rejects stale BS");
    require(!store.atomic_has_script_intent(hash, slice(x)), "APPLY released the retained intent");
    server.atomic_commit_publish();
    require(local(server, {"GET", x}) == "$2\r\nBW\r\n", "foreign APPEND survives rejected APPLY");
}
void watch_cycle(Server& server) {
    Client a(-1), b(-1); a.set_id(201); b.set_id(202);
    Shard first, second;
    first.init_private(&server, 0, {}, {}); second.init_private(&server, 1, {}, {});
    const Slice key("watched", 7);
    for (Shard* s : {&first, &second})
        for (Client* c : {&a, &b}) require(s->watch_add(key, c, c->watch_generation()), "WATCH armed");
    std::atomic<uint64_t> ae{0}, be{0};
    std::atomic<bool> aa{false}, ba{false};
    std::atomic<uint32_t> ar{0}, br{0};
    auto reserve = [&](Shard& s, Client& c, auto& e, auto& abort, auto& refs) {
        return s.watch_validate_and_reserve(key, &c, c.watch_generation(), &c, &e, &abort, &refs, false);
    };
    require(reserve(first, a, ae, aa, ar) && reserve(second, b, be, ba, br), "opposing first reservations");
    require(ar == 1 && br == 1 && !first.watch_write_ready(key) && !second.watch_write_ready(key),
            "both blocking reservations are live");
    require(reserve(second, a, ae, aa, ar) && reserve(first, b, be, ba, br), "opposing validation completes");
    require(ar == 2 && br == 2 && !a.watch_dirty() && !b.watch_dirty(), "read-only claims coexist cleanly");
    ae.store(1); be.store(1);
    for (Shard* s : {&first, &second}) {
        require(s->watch_write_ready(key), "decided claims release writers");
        for (Client* c : {&a, &b}) s->watch_remove(key, c, c->watch_generation());
    }
    require(ar == 0 && br == 0, "reservation references drained");
}

// Diagnostic for a separate, still-unfixed interval found while reviewing C atomics 6.
// Deliberately NOT a green gate row: run explicitly as `post_apply_probe`. See FIXES.md.
void post_apply_probe(Server& server) {
    const auto x = key_on(server, 0, "post-apply-x"), y = key_on(server, 1, "post-apply-y");
    require(server.worker_of_shard(0) != server.worker_of_shard(1), "two APPLY owners");
    require(local(server, {"SET", x, "B"}) == "+OK\r\n" &&
            local(server, {"SET", y, "B"}) == "+OK\r\n", "seed both APPLY owners");
    ScatterRun run(server, {"EVAL", "redis.call('APPEND',KEYS[2],'T'); return redis.call('APPEND',KEYS[1],'S')",
                           "2", x, y});
    for (unsigned i = 0; i < run.state->nsub; i++) {
        const int s = run.state->groups[i].shard;
        require(xshard_execute(Task{&run.client, 0, s, run.state}, server.shard(s), run.request.op,
                server.worker_of_shard(s)) == ScatterTaskResult::Complete, "PIN ran");
    }
    run.state->script_cut = server.atomic_snapshot();
    run.phase(ScriptPhase::Read); run.phase(ScriptPhase::Run);
    run.state->script_ticket = server.atomic_commit_reserve();
    run.phase(ScriptPhase::Validate);
    require(!run.state->script_conflict.load(), "validation succeeded before either APPLY");
    run.state->script_apply_after_unpin = true;
    run.phase(ScriptPhase::Unpin);
    run.state->script_phase = ScriptPhase::Apply; run.state->phase = 2;
    const auto count = script_select_positions(*run.state, ScriptPhase::Apply);
    reset_groups(*run.state); build_groups(*run.state, run.state->hop2, count);
    initialize_owner_completion(*run.state);
    require(run.state->nsub == 2, "both owners must install script writes");
    require(xshard_execute(Task{&run.client, 0, 0, run.state}, server.shard(0), run.request.op,
            server.worker_of_shard(0)) == ScatterTaskResult::Complete, "first APPLY completed");
    require(!run.state->epoch.load() && server.shard(0).store().atomic_pending_entries() != 0,
            "first script image is installed and still undecided");
    const auto write_reply = local(server, {"APPEND", x, "W"}, 888);
    require(xshard_execute(Task{&run.client, 0, 1, run.state}, server.shard(1), run.request.op,
            server.worker_of_shard(1)) == ScatterTaskResult::Complete, "second APPLY completed");
    require(!run.state->aborted.load(), "both owners accepted their images");
    run.state->epoch.store(run.state->script_ticket, std::memory_order_release);
    server.atomic_commit_publish();
    const auto value = local(server, {"GET", x});
    std::printf("script reply=%sforeign reply=%sfinal x=%s", run.request.reply().c_str(),
                write_reply.c_str(), value.c_str());
    require((run.request.reply() == ":2\r\n" && write_reply == ":3\r\n" && value == "$3\r\nBSW\r\n") ||
            (run.request.reply() == ":3\r\n" && write_reply == ":2\r\n" && value == "$3\r\nBWS\r\n"),
            "completed APPENDs have a legal serial outcome after first-owner APPLY");
}
void mset_arity(Server& server) {
    const auto a = key_on(server, 0, "arity-a"), b = key_on(server, 1, "arity-b");
    for (const char* verb : {"MSET", "MSETNX"}) {
        Client c(-1); c.set_id(404);
        require(multi_io(server, c, {"MULTI"}) == "+OK\r\n", "fresh MULTI");
        require(multi_io(server, c, {verb, a, "one", b}) == "+QUEUED\r\n", "malformed pair list queued");
        const auto reply = multi_io(server, c, {"EXEC"});
        require(reply.starts_with("*1\r\n-ERR wrong number"), "EXEC has a command error element");
        require(local(server, {"GET", a}) == "$-1\r\n" && local(server, {"GET", b}) == "$-1\r\n",
                "malformed MSET/MSETNX changed no key");
    }
}
void watch_oom(Server& server) {
    const std::string key(128, 'w');
    unsigned failures = 0; bool saw_rollback = false, reached_success = false;
    for (int budget = 0; budget < 100; budget++) {
        Client c(-1); c.set_id(505);
        Request watch({"WATCH", key});
        MultiExecState* state = nullptr;
        allocation_failed = false; fail_new_after = budget;
        const auto action = multi_handle_io(server, c, watch.op, 0, state);
        fail_new_after = -1;
        if (allocation_failed) {
            failures++;
            require(action == MultiIoAction::LocalDone && watch.reply().starts_with("-ERR out of memory"),
                    "injected WATCH preparation failure reported");
            auto* session = c.multi_session();
            saw_rollback |= session && session->watched.capacity() != 0;
            require(!session || session->watched.empty(), "failed WATCH did not leave a phantom session key");
            watch.op.clear_reply();
            require(multi_handle_io(server, c, watch.op, 0, state) == MultiIoAction::Dispatch,
                    "successful retry posts real WATCH work");
        } else {
            require(action == MultiIoAction::Dispatch, "WATCH success reached");
            reached_success = true;
        }
        require(state && state->watched.size() == 1 && state->shards.size() == 1, "retry owns one key");
        auto& shard = server.shard(state->shards[0]);
        require(execute_watch_phase(*state, shard), "retry registered owner watcher");
        shard.watch_write_committed(slice(key));
        require(c.watch_dirty(), "foreign mutation dirties the successfully retried WATCH");
        shard.watch_remove(slice(key), &c, c.watch_generation());
        c.multi_session()->pending = nullptr;
        c.multi_session()->watched.clear();
        destroy_multi_state(state);
        if (reached_success) break;
    }
    require(failures >= 10 && saw_rollback && reached_success, "swept through the post-append OOM window");
}

void lua_case(Server& server, const std::string& name) {
    Shard& shard = server.shard(0);
    auto call = [&](std::vector<std::string> args) {
        Request r(std::move(args)); r.op.spec->handler(shard, r.op); return r.reply();
    };
    if (name == "closure") {
        require(call({"FUNCTION", "LOAD", "#!lua name=retained\nlocal reg=redis.register_function\n"
             "reg('retained',function() reg('late',function() return 1 end); return 42 end)"}) ==
             "$8\r\nretained\r\n", "retained closure library loaded");
        const auto reply = call({"FCALL", "retained", "0"});
        require(reply.starts_with("-") && reply.find("not available here") != std::string::npos,
                "escaped registration closure is revoked after materialization");
    } else if (name == "lua_conversion") {
        for (const char* body : {"error('conversion failed')", "while true do end"}) {
            const std::string table = std::string("setmetatable({}, {__index=function() ") + body + " end})";
            require(call({"EVAL", "return " + table, "0"}) == "*0\r\n", "reply conversion never invokes __index");
            require(call({"EVAL", "error(" + table + ")", "0"}).starts_with("-ERR"),
                    "error conversion never invokes __index");
        }
    } else if (name == "lua_lines") {
        require(call({"EVAL", "return {ok='OK\\r\\n:42'}", "0"}) == "+OK  :42\r\n", "status has exactly one frame");
        require(call({"EVAL", "return {err='ERR bad\\r\\n:42'}", "0"}) == "-ERR bad  :42\r\n", "error has exactly one frame");
    } else if (name == "library_limit") {
        for (const char* body : {"while true do end", "pcall(function() while true do end end); "
                               "redis.register_function('late',function() return 1 end)"}) {
            const auto before = function_stats().libraries;
            const auto reply = call({"FUNCTION", "LOAD", std::string("#!lua name=spin\n") + body});
            require(reply.starts_with("-") && reply.find("instruction limit") != std::string::npos,
                    "FUNCTION LOAD budget failure is terminal even when caught");
            require(function_stats().libraries == before, "timed out library was not registered");
        }
    } else if (name == "instruction_limit") {
        require(call({"EVAL", "local ok=pcall(function() while true do end end); return 42", "0"}).starts_with("-BUSY"),
                "caught instruction-limit error still fails the activation");
    }
    require(call({"EVAL", "return 7", "0"}) == ":7\r\n", "interpreter remains usable");
}
void stage_flag(Server& server) {
    static_assert(std::is_same_v<decltype(ScatterState::script_stage_limit), std::atomic<bool>>,
                  "owner tasks concurrently publish the staging-failure flag");
    const auto a = key_on(server, 0, "stage-a"), b = key_on(server, 1, "stage-b");
    const std::string big(5 * 1024 * 1024, 's');
    require(local(server, {"SET", a, big}) == "+OK\r\n", "oversized owner image seeded");
    ScatterRun run(server, {"EVAL", "return redis.call('GET',KEYS[1])", "2", a, b});
    run.state->script_cut = server.atomic_snapshot();
    run.phase(ScriptPhase::Read);
    require(run.state->script_staged_bytes > server.script_crossshard_max_bytes() &&
            run.state->script_stage_limit.load() && run.state->aborted.load(), "derived staging cap fired");
    script_engine_error_reply(run.request.op, *run.state, WorkError::Oom);
    require(run.request.reply() == "-ERR cross-shard script staging limit exceeded\r\n", "staging error retained");
}
}

int main(int argc, char** argv) {
    require(argc == 2, "one named regression is required");
    require(command_registry_init(false), "command registry");
    Config cfg;
    cfg.shards = 16; cfg.even_ifid = 6; cfg.even_ex = 2;
    cfg.key_lb = cfg.client_lb = 0; cfg.flip_auto = 0;
    // Exercise the fixed production instruction budget, including caught-limit failure.
    Server server;
    require(server.prepare_boot(cfg) && server.init(cfg), "16-shard, 6:2 owner geometry initialized");
    command_bind_server(&server);
    std::string acl_error;
    require(acl_initialize(server, cfg, acl_error), "default ACL initialized");
    const std::string name = argv[1];
    if (name == "admission") admission(server);
    else if (name == "script_keys") script_keys(server);
    else if (name == "rename_overlay" || name == "watch_parent") rename(server, name == "watch_parent");
    else if (name == "write_latest") write_latest(server);
    else if (name == "script_apply") script_apply(server);
    else if (name == "watch_cycle") watch_cycle(server);
    else if (name == "post_apply_probe") post_apply_probe(server);
    else if (name == "mset_arity") mset_arity(server);
    else if (name == "watch_oom") watch_oom(server);
    else if (name == "stage_flag") stage_flag(server);
    else if (name == "closure" || name == "lua_conversion" || name == "lua_lines" ||
             name == "library_limit" || name == "instruction_limit") lua_case(server, name);
    else require(false, "known regression name");
    release_records(server);
    std::printf("PASS %s\n", name.c_str());
}
