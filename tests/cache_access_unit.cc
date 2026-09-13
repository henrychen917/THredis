// Serverless access-order regressions. Record the real WB helper's prefetch calls, including
// their order, so restoring the whole-batch walk fails the locality witness. No clocks or rates.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

struct CacheHint { const void* address; int write; int locality; };
static std::vector<CacheHint> hints;
static void record_prefetch(const void* address, int write = 0, int locality = 3) {
    hints.push_back({address, write, locality});
}
#define __builtin_prefetch(...) ::record_prefetch(__VA_ARGS__)
#include "src/core/io_loop.h"
#undef __builtin_prefetch

namespace tomo {
// The fixture never opens a transaction or a listener.
void multi_session_destroy(MultiSession* session) { if (session) std::abort(); }

struct CoreConcurrencyTest {
    static void require(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "FAIL cache access: %s\n", what);
            std::exit(1);
        }
    }

    static Op& publish(Client& client, OpState state, const char* payload,
                       uint32_t length, int32_t shard = 0) {
        Op* op = client.rob().acquire();
        require(op != nullptr, "ROB fixture full");
        op->zc_ptr = payload;
        op->zc_len = length;
        op->zc_shard = shard;
        op->state.store(state, std::memory_order_release);
        client.rob().publish();
        return *op;
    }

    static void wb() {
        Client a(-1), b(-1), empty(-1), dead(-1);
        std::array<char, 1024> payload{};
        publish(a, OpState::Done, payload.data(), 65);
        Op& pending = publish(a, OpState::Issued, payload.data() + 128, 64);
        publish(a, OpState::Done, payload.data() + 256, 64);
        publish(b, OpState::Done, payload.data() + 384, 640);
        publish(b, OpState::Done, payload.data(), 64, -1); // local reply, no borrow
        publish(b, OpState::Done, nullptr, 64);
        publish(b, OpState::Done, payload.data(), 0);
        publish(dead, OpState::Done, payload.data(), 64);
        dead.mark_dead();
        IoLoop::WbBatch batch{};
        batch.clients[0] = &a;
        batch.clients[1] = &empty;
        batch.clients[2] = &dead;
        batch.clients[3] = &b;
        batch.count = 4;

        // A premature metadata load must fail even if its eventual prefetch would be harmless.
#if defined(__SANITIZE_ADDRESS__)
        __asan_poison_memory_region(&pending.zc_ptr, sizeof(pending.zc_ptr));
        __asan_poison_memory_region(&pending.zc_len, sizeof(pending.zc_len) +
                                                  sizeof(pending.zc_shard));
#endif
        hints.clear();
        IoLoop::wb_prefetch(batch);
#if defined(__SANITIZE_ADDRESS__)
        __asan_unpoison_memory_region(&pending.zc_ptr, sizeof(pending.zc_ptr));
        __asan_unpoison_memory_region(&pending.zc_len, sizeof(pending.zc_len) +
                                                    sizeof(pending.zc_shard));
#endif
        std::vector<CacheHint> expected;
        for (unsigned i = 0; i < 3; i++) expected.push_back({&a.rob().at(i).state, 0, 3});
        for (unsigned i = 0; i < 2; i++) expected.push_back({payload.data() + i * 64, 0, 1});
        for (unsigned i = 0; i < 4; i++) expected.push_back({&b.rob().at(i).state, 0, 3});
        for (unsigned i = 0; i < 8; i++) expected.push_back({payload.data() + 384 + i * 64, 0, 1});
        require(hints.size() == expected.size(), "hint count / first-pending / borrow limits");
        for (size_t i = 0; i < hints.size(); i++)
            require(hints[i].address == expected[i].address &&
                    hints[i].write == expected[i].write &&
                    hints[i].locality == expected[i].locality,
                    "connection's state and borrow hints must be adjacent, with acquire safety");
        require(a.rob().flush_id() == 0 && a.rob().dispatch_id() == 3 &&
                pending.state.load() == OpState::Issued && batch.count == 4,
                "hinting cannot retire, execute or change the batch");

        // Wrap the physical ROB slots, then exercise the same helper used by both fused arms.
        Client wrapped(-1);
        for (unsigned i = 0; i < 60; i++) publish(wrapped, OpState::Done, nullptr, 0);
        require(wrapped.rob().drain([](Op&) {}) == 60, "advance real flush frontier");
        for (unsigned i = 0; i < kRobWindow; i++)
            publish(wrapped, OpState::Done, payload.data(), 1);
        hints.clear();
        IoLoop::prefetch_wb_client<kGenthreadWbPrefetchOpsPerConn,
                                  kGenthreadWbBorrowPrefetchBytes,
                                  kGenthreadCacheLineBytes>(wrapped);
        require(hints.size() == 2 * kRobWindow, "full wrapped ROB hinted exactly once");
        for (unsigned i = 0; i < kRobWindow; i++)
            require(hints[i].address == &wrapped.rob().at(60 + i).state &&
                    hints[kRobWindow + i].address == payload.data(),
                    "wrapped IDs address the current ROB generation");
        std::puts("PASS cache access: WB connection window, pending acquire, borrow bounds, ROB wrap");
    }

    // An opaque callback mutates ThreadCtx's command line, as real execution does, and can append
    // work on the same lane. A transport snapshot must preserve visibility of those fresh tails.
    struct DrainState {
        ThreadCtx* thread;
        LoopSignals* sig;
        bool append;
        bool fused;
        std::vector<uint64_t> seen;
    };
    static __attribute__((noinline)) void consume(DrainState& state, const Task& task) {
        state.thread->note_command(0);
        state.seen.push_back(task.op_id);
        require(!state.thread->ex_inbound_quiesced(), "retirement must follow callback");
        if (state.append) {
            state.append = false;
            Task next(nullptr, 3, 0, nullptr);
            const bool ok = state.fused
                ? state.thread->post_iofused_task_quiet(0, next, *state.sig)
                : state.thread->post_task_quiet(0, next, *state.sig);
            require(ok, "callback enqueued a fresh tail");
        }
    }

    template <bool Fused>
    static void transport() {
        auto owner = std::make_unique<ThreadCtx>();
        owner->init(6, Role::Ex, 8, 0, 0);
        const std::vector<uint32_t> io{0, 1, 2, 3, 4, 5}, ex{6, 7};
        require(Fused ? owner->init_task_inbox_local_fused()
                      : owner->init_task_inbox_local(io, ex), "init 16-shard gate's 6:2 transport");
        Ring unopened;
        LoopSignals sig;
        auto post = [&](uint32_t producer, uint64_t id, bool quiet = false) {
            Task task(nullptr, id, 0, nullptr);
            const bool ok = Fused
                ? owner->post_iofused_task_quiet(producer, task, sig)
                : owner->post_task_quiet(producer, task, sig);
            require(ok, "post fixture task");
            if (!quiet) owner->flush_task_notify(producer, unopened, sig);
        };
        DrainState state{owner.get(), &sig, true, Fused, {}};
        for (unsigned i = 0; i < 3; i++) post(0, i);
        require(owner->drain_tasks<Fused>([&](const Task& t) { consume(state, t); }) == 4,
                "masked drain sees callback publication");
        require(state.seen == std::vector<uint64_t>({0, 1, 2, 3}) &&
                owner->ex_inbound_quiesced(), "masked FIFO and retired frontier");
        post(1, 4, true);
        require(owner->drain_tasks<Fused>([&](const Task&) { require(false, "quiet lane masked"); }) == 0,
                "missing notification witness");
        require(owner->drain_tasks_unmasked<Fused>([&](const Task& t) { consume(state, t); }) == 1 &&
                state.seen.back() == 4 && owner->ex_inbound_quiesced(), "idle sweep recovers quiet task");

        for (unsigned i = 0; i < 5; i++) post(0, 10 + i);
        for (unsigned i = 0; i < 3; i++) post(1, 20 + i);
        unsigned boundaries = 0, per_batch = 0;
        auto take = [&](const Task& t) {
            consume(state, t);
            return ++per_batch % 2 == 0;
        };
        auto boundary = [&] { boundaries++; };
        require(owner->drain_task_producer_chunks<Fused>(2, take, boundary) == 4 && boundaries == 2,
                "one producer quantum, unchanged batch boundary");
        require(owner->drain_task_producer_chunks<Fused>(2, take, boundary) == 3,
                "capped producer is re-notified");
        require(owner->drain_task_producer_chunks<Fused>(2, take, boundary) == 1 &&
                owner->ex_inbound_quiesced(), "suffix conserved across bounded drains");

        // A remask is forbidden while popped-but-unretired work exists. The hoisted pointer must
        // expire before the legal edge, and the next drain must observe the new lane geometry.
        post(0, 30);
        post(1, 31);
        Task gathered[2];
        uint32_t lanes[2], counts[2], lane_count = 0;
        require(owner->gather_tasks_unretired(gathered, lanes, counts, lane_count, 2) == 2 &&
                lane_count == 2 && !owner->ex_inbound_quiesced(), "gather retains retirement debt");
        require(!owner->remask_task_inbox_quiesced(io, ex), "remask rejects active cursor");
        owner->retire_task_lanes(lanes, counts, lane_count);
        require(owner->ex_inbound_quiesced(), "batched retire clears all gathered debt");
        if constexpr (!Fused) {
            require(owner->remask_task_inbox_quiesced({0, 1, 2, 3}, {4, 5, 6, 7}), "quiesced remask");
            post(4, 32);
            require(owner->drain_tasks<>([&](const Task& t) { consume(state, t); }) == 1 &&
                    state.seen.back() == 32 && owner->ex_inbound_quiesced(), "drain uses new geometry");
        }
        std::printf("PASS cache access: %s transport, callback tail, idle sweep, bounded drain, remask\n",
                    Fused ? "1s" : "2s");
    }

    // The other hoisted transports carry borrow lifetimes and client ownership. Exercise every
    // real drain with a callback that publishes a fresh tail, including an unnotified idle sweep.
    // At the last callback head == tail; only the separate retired frontier may authorize teardown.
    static void control_channels() {
        auto owner = std::make_unique<ThreadCtx>();
        owner->init(6, Role::Ifid, 8, 0, 0);
        require(owner->init_task_inbox_local({0, 1, 2, 3, 4, 5}, {6, 7}),
                "control-channel fixture inbox");
        Ring unopened;
        LoopSignals sig;
        Client a(-1), b(-1), c(-1), d(-1);
        std::array<Client*, 4> clients{&a, &b, &c, &d};
        auto exercise = [&](auto post, auto drain, auto id, auto quiesced) {
            for (bool unmasked : {false, true}) {
                require(quiesced(), "control channel begins quiesced");
                require(post(0, 0, unmasked) && post(0, 1, unmasked) &&
                        post(1, 2, unmasked), "control-channel publication");
                std::vector<uint32_t> seen;
                auto take = [&](auto value) {
                    require(!quiesced(), "control callback must precede retirement");
                    owner->note_command(0);
                    const uint32_t current = id(value);
                    seen.push_back(current);
                    if (current == 0)
                        require(post(0, 3, unmasked), "control callback publishes fresh tail");
                };
                if (unmasked)
                    require(drain(take, false) == 0, "control idle sweep has no notification");
                require(drain(take, unmasked) == 4 &&
                        seen == std::vector<uint32_t>({0, 1, 3, 2}),
                        "control drain preserves producer FIFO and observes fresh tail");
                require(quiesced(), "control drain retires every callback exactly once");
                require(drain(take, false) == 0 && drain(take, true) == 0,
                        "control drain leaves no duplicate or hidden work");
            }
        };
        exercise([&](uint32_t p, uint32_t i, bool quiet) {
                     return quiet ? owner->client_in_[p].push(clients[i], sig)
                                  : owner->post_client(p, clients[i], unopened, sig);
                 }, [&](auto take, bool unmasked) {
                     return unmasked ? owner->drain_clients_unmasked(take)
                                     : owner->drain_clients(take);
                 }, [&](Client* client) {
                     const auto found = std::find(clients.begin(), clients.end(), client);
                     require(found != clients.end(), "completion names the published client");
                     return static_cast<uint32_t>(found - clients.begin());
                 }, [&] { return owner->io_inbound_quiesced(); });
        exercise([&](uint32_t p, uint32_t i, bool quiet) {
                     BorrowRelease release{static_cast<int32_t>(i), nullptr};
                     return quiet ? owner->release_in_[p].push(release, sig)
                                  : owner->post_release(p, release, unopened, sig);
                 }, [&](auto take, bool unmasked) {
                     return unmasked ? owner->drain_releases_unmasked(take)
                                     : owner->drain_releases(take);
                 }, [](const BorrowRelease& release) { return uint32_t(release.shard); },
                 [&] { return owner->ex_inbound_quiesced(); });
        exercise([&](uint32_t p, uint32_t i, bool quiet) {
                     ClientTransfer transfer{clients[i], nullptr, nullptr, i};
                     return quiet ? owner->transfer_in_[p].push(transfer, sig)
                                  : owner->post_client_transfer(p, transfer, unopened, sig);
                 }, [&](auto take, bool unmasked) {
                     return unmasked ? owner->drain_client_transfers_unmasked(take)
                                     : owner->drain_client_transfers(take);
                 }, [&](const ClientTransfer& transfer) {
                     require(transfer.source < clients.size() &&
                             transfer.client == clients[transfer.source],
                             "transfer retains client ownership metadata");
                     return transfer.source;
                 }, [&] { return owner->client_transfers_quiesced(); });
        std::puts("PASS cache access: completion/release/transfer FIFO, callback retirement, idle sweeps");
    }
};
} // namespace tomo

int main() {
    tomo::CoreConcurrencyTest::wb();
    tomo::CoreConcurrencyTest::transport<false>();
    tomo::CoreConcurrencyTest::transport<true>();
    tomo::CoreConcurrencyTest::control_channels();
}
