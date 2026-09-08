#include "netcmd_unit.h"
// Include the actual cold command implementation to exercise its anonymous notification helpers.
// The unit link omits the corresponding production object.
#include "src/cmd/xshard.cc"
#include <filesystem>

long netcmd_fail_after = -1;
size_t netcmd_fail_size = 0;
unsigned netcmd_allocations = 0, netcmd_failures = 0;
void* operator new(size_t size) {
    if (netcmd_fail_after >= 0 && (!netcmd_fail_size || netcmd_fail_size == size)) {
        ++netcmd_allocations;
        if (netcmd_fail_after-- == 0) { ++netcmd_failures; throw std::bad_alloc(); }
    }
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { std::free(ptr); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { std::free(ptr); }

namespace tomo {
struct NetcmdRegression {
    static void notify_oom() {
        Server server;
        auto chain = std::make_unique<PubSubNotificationChain>();
        chain->items.push_back(PubSubNotificationItem{"channel", "message"});
        fault(0);
        bool threw = false;
        try { (void)notify_reserve_chain(server, std::move(chain)); }
        catch (const std::bad_alloc&) { threw = true; }
        const auto failed = netcmd_failures; fault(-1);
        check(threw && failed == 1, "shared_ptr control block allocation failed");
        check(server.pubsub_inflight() == 0, "reservation released exactly once on failure");
        auto good = std::make_unique<PubSubNotificationChain>();
        good->items.push_back(PubSubNotificationItem{"channel", "message"});
        auto reserved = notify_reserve_chain(server, std::move(good));
        check(reserved && server.pubsub_inflight() == 1, "successful chain owns reservation");
        reserved.reset(); check(server.pubsub_inflight() == 0, "successful chain releases reservation");
    }

    static void notify_retry() {
        Server server;
        Config cfg; cfg.even_ifid = 1; cfg.even_ex = 1; cfg.shards = 16;
        check(server.prepare_boot(cfg), "notification topology");
        server.threads_.resize(server.placement().total_threads());
        for (uint32_t i = 0; i < server.threads_.size(); ++i) {
            server.threads_[i] = std::make_unique<ThreadCtx>();
            server.threads_[i]->init(i, Role::Ifid, server.threads_.size(), 0, 0);
        }
        const uint32_t io = server.placement().ifid_threads().front();
        ThreadCtx& thread = server.thread(io);
        server.climon_tracking_io_mask_.store(uint64_t{1} << io);
        Ring ring; LoopSignals signals;
        uint32_t queued = 0;
        while (thread.post_client(0, nullptr, ring, signals)) ++queued;
        check(queued > 0, "producer marker lane filled");
        Shard shard; shard.init_private(&server, 0, TypeLimits{}, StreamLimits{});
        shard.notify_state_slot() = std::make_unique<NotifyShardState>();
        NotifyOut out; out.routes = NOTIFY_TRACKING; out.key = "expired";
        shard.notify_state_slot()->keyless.push_back(std::move(out));
        check(notify_ex_pass_entry(server, shard, 0, thread, ring, signals) == 0,
              "failed marker post is incomplete work");
        check(shard.notify_state_slot()->keyless.size() == 1, "keyless invalidation retained");
        std::deque<PubSubEvent*> events; thread.take_pubsub_events(events);
        check(events.empty(), "failed post enqueued no payload");
        check(thread.drain_clients([](Client*) {}) == queued, "producer lane drained");
        check(notify_ex_pass_entry(server, shard, 0, thread, ring, signals) == 1, "retry completes record");
        thread.take_pubsub_events(events);
        check(events.size() == 1 && events.front()->kind == PubSubEventKind::TrackingInvalidate &&
              events.front()->items.size() == 1 && events.front()->items.front().value == "expired",
              "exact invalidation delivered after retry");
        server.pubsub_event_retired(); delete events.front();
        thread.drain_clients([](Client*) {});
    }

    static void flush() {
        Server server; Config cfg; cfg.even_ifid = 2; cfg.even_ex = 1; cfg.shards = 16;
        check(server.prepare_boot(cfg), "flush topology");
        const auto& ios = server.placement().ifid_threads();
        check(ios.size() == 2, "foreign tracking IO exists");
        server.threads_.resize(server.placement().total_threads());
        for (uint32_t i = 0; i < server.threads_.size(); ++i) {
            server.threads_[i] = std::make_unique<ThreadCtx>();
            server.threads_[i]->init(i, Role::Ifid, server.threads_.size(), 0, 0);
        }
        IoLoop loop; loop.srv_ = &server; loop.self_ = &server.thread(ios[0]);
        loop.climon_armed_cached_ = Server::kClimonTracking;
        server.climon_tracking_io_mask_.store(uint64_t{1} << ios[1]);
        Client writer(-1); writer.set_id(9); Op op; args(op, {"FLUSHALL"});
        check(!loop.climon_armed_gate(&writer, op), "FLUSH passed dispatch gate");
        std::deque<PubSubEvent*> events;
        server.thread(ios[1]).take_pubsub_events(events);
        check(events.empty(), "no flush invalidation before owner work was published");
        op.state.store(OpState::Done);
        loop.climon_flush_completed(op);
        server.thread(ios[1]).take_pubsub_events(events);
        check(events.size() == 1 && events.front()->kind == PubSubEventKind::TrackingFlush,
              "completed flush invalidates foreign tracking IO");
        delete events.front(); server.pubsub_event_retired();
        server.thread(ios[1]).drain_clients([](Client*) {});
    }

    static void output() {
        Client client(-1); WbEngine wb; std::atomic<bool> limits{false};
        wb.bind(nullptr, nullptr, nullptr, nullptr, nullptr, &limits);
        client.start_obuf_tracking();
        check(client.obuf_bytes() == 0, "tracked balance drained before disable");
        Op* op = client.rob().acquire(); check(op, "reply slot");
        op->reply.append("+PONG\r\n", 7); op->state.store(OpState::Done); client.rob().publish();
        bool allowed; wb.prepare(client, allowed);
        check(allowed && client.fill_buf().size() == 7, "disabled specialization staged reply");
        client.swap_buffers(); client.commit_write(7);
        check(client.write_drained(), "uncounted reply send completion did not underflow");

        Server server; ThreadCtx self; IoLoop loop;
        loop.srv_ = &server; loop.self_ = &self;
        server.live_obuf_normal_hard_.store(8); server.client_obuf_armed_.store(true);
        Client held(-1); held.set_id(1);
        check(held.rob().acquire(), "held ordinary operation"); held.rob().publish();
        check(!held.blocked(), "ordinary operation, not blocking exception");
        check(loop.wb_.defer_oob(held, "012345678", 9), "push entered deferred queue");
        check(held.buffered_output_bytes() == 0 && loop.wb_.deferred_output_bytes(held) == 9,
              "limit bytes exist only in deferred storage");
        check(loop.client_obuf_check(&held, false), "hard limit fired before Done");
        check(held.closing(), "deferred output scheduled connection close");
        loop.wb_.teardown(held);
    }

    static void pubsub() {
        for (auto kind : {IoLoop::PubSubPendingKind::Channels, IoLoop::PubSubPendingKind::Numsub,
                          IoLoop::PubSubPendingKind::Numpat, IoLoop::PubSubPendingKind::Modify}) {
            for (bool suppressed : {false, true}) {
                Server server; ThreadCtx self; IoLoop loop; Client client(-1); client.set_id(42);
                loop.srv_ = &server; loop.self_ = &self; loop.wb_.bind(nullptr);
                Op* op = client.rob().acquire(); check(op, "pending pubsub operation");
                if (suppressed) op->mark_reply_skip();
                client.rob().publish();
                auto& local = loop.pubsub_local_[client.id()]; local.client = &client;
                local.channels.insert("old"); local.pending = 1;
                auto& pending = loop.pubsub_pending_[client.id()];
                pending.kind = kind; pending.op_id = client.rob().flush_id(); pending.subscribe = true;
                if (kind == IoLoop::PubSubPendingKind::Modify) pending.items.push_back("new");
                server.pubsub_pending_started();
                const std::string message = "*3\r\n$7\r\nmessage\r\n$3\r\nold\r\n$4\r\ndata\r\n";
                loop.pubsub_emit(&client, message.data(), message.size(), true);
                check(!pending.deferred.empty(), "publication arrived while control was pending");
                loop.pubsub_finish_pending(client.id());
                check(loop.pubsub_pending_.empty(), "pending control completed");
                bool allowed;
                if (suppressed) loop.wb_.prepare_suppressing(client, allowed);
                else loop.wb_.prepare(client, allowed);
                const std::string reply(client.fill_buf().data(), client.fill_buf().size());
                std::string push = message; push[0] = '>';
                check(reply.find(push) != std::string::npos, "independent publication survives retirement");
                if (suppressed) check(reply == push, "only command reply was suppressed");
            }
        }
    }

    static void receive_segments() {
        SegmentQueue<4, 8> segments;
        segments.append_buf("abcdefghijk", 11, "lmnopqrs", 8);
        check(segments.size() == 3 && segments.byte_size() == 19, "two-piece aggregate split into representable segments");
        iovec iov[4]; bool borrow = false; uint32_t bytes = 0;
        const auto n = segments.build_iov(iov, 4, 64, borrow, bytes);
        std::string actual;
        for (uint32_t i = 0; i < n; ++i) {
            check(iov[i].iov_len <= 8, "segment length cap honored");
            actual.append(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
        }
        check(bytes == 19 && actual == "abcdefghijklmnopqrs", "aggregate contents preserved");
        Client input(-1); size_t avail = 0;
        check(input.read_space(2 * 1024 * 1024, avail, true, 1024 * 1024) && avail >= 2 * 1024 * 1024,
              "whole command can exceed individual bulk limit");
        const auto before = input.rcap();
        input.commit_read(before - 1);
        check(!input.read_space(4096, avail, false, 1024 * 1024) && input.rcap() == before,
              "receive growth remains forbidden with outstanding argv slices");
        // Allocation reserves virtual space; no 600 MiB payload or benchmark is needed.
        Client large(-1);
        check(large.read_space(600ull * 1024 * 1024, avail, true) &&
              avail >= 600ull * 1024 * 1024, "default receive allowance has no one-bulk ceiling");
        check(large.read_space(600ull * 1024 * 1024, avail, true, 1024ull * 1024 * 1024) &&
              avail >= 600ull * 1024 * 1024, "raised bulk limit fits epoll receive geometry");
    }

    // Diagnostic for the still-open cross-family partial-mutation finding; deliberately absent
    // from the passing gate rows until a failure-atomic collection update is implemented.
    static void collection_oom() {
        Shard shard; TypeLimits limits; limits.hash.max_entries = 1;
        shard.init_private(nullptr, 0, limits, StreamLimits{}); shard.set_cached_now_ms(1000);
        check(execute(shard, {"HSET", "h", "a", "old-a", "b", "old-b"}) == ":2\r\n", "expanded hash seeded");
        auto* object = shard.store().find(FlatStore::hash_key(Slice("h", 1)), Slice("h", 1));
        check(CollectionRef(object).encoding() == CollectionEncoding::Hashtable, "expanded hash arm entered");
        check(execute(shard, {"HPEXPIRE", "h", "60000", "FIELDS", "1", "a"}) == "*1\r\n:1\r\n", "field TTL armed");
        const std::string replacement(256, 'x'); Op op;
        args(op, {"HSET", "h", "a", replacement.c_str(), "b", replacement.c_str()});
        fault(1, replacement.size() + 1); op.spec->handler(shard, op);
        const unsigned failures = netcmd_failures; fault(-1);
        check(failures == 1, "second replacement allocation failed");
        check(std::string(op.reply.data(), op.reply.size()).starts_with("-ERR"), "HSET reported allocation failure");
        check(execute(shard, {"HGET", "h", "a"}) == "$5\r\nold-a\r\n",
              "OPEN F05: failed multi-field HSET retained a changed prefix (and its old TTL)");
    }

    static void config() {
        Server server;
        const std::string password = "pass with spaces\n\r\t\"'\\tail";
        char directory[] = "build/netcmd-config-XXXXXX";
        check(::mkdtemp(directory), "private config directory");
        const std::string path = std::string(directory) + "/tomo.conf";
        server.cfg_.conf_path = path.c_str(); server.cfg_.requirepass = password.c_str();
        command_bind_server(&server);
        test_config_rewrite();
        command_bind_server(nullptr);
        std::filesystem::remove_all(directory);
    }
};
}

int main(int argc, char** argv) {
    check(argc == 2, "one regression section required");
    check(tomo::command_registry_init(false), "command registry initialized");
    const std::string mode = argv[1];
    using R = tomo::NetcmdRegression;
    if (mode == "streams") test_stream_faults();
    else if (mode == "zpop") test_zpop_faults();
    else if (mode == "notify-oom") R::notify_oom();
    else if (mode == "notify-retry") R::notify_retry();
    else if (mode == "flush") R::flush();
    else if (mode == "output") R::output();
    else if (mode == "pubsub") R::pubsub();
    else if (mode == "receive") R::receive_segments();
    else if (mode == "collection-oom") R::collection_oom();
    else if (mode == "config") R::config();
    else check(false, "unknown regression section");
    std::printf("ok: netcmd %s\n", argv[1]);
}
