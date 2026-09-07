// rl2s.cc — split placement with a shard-less fused-capable IO tier.
// All reader-lane scheduling and split role-tenure wiring for this feature live here.
// The ordinary split runtime and the 1s boot sequence remain independent.
#include "genthread.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#include "../base/alloc.h"
#include "../cmd/acl.h"
#include "../net/conn.h"
#include "../net/unix_listener.h"
#include "../persist/aof.h"
#include "../snapshot/snapshot.h"
#include "ex_loop.h"
#include "fused_boot_gate.h"
#include "io_loop.h"
#include "server.h"
#include "shutdown_report.h"
#include "signal_doorbell.h"

namespace tomo {
namespace {

void pin_read_local_thread(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

}  // namespace

// Same copy/validate/demote lane as 1s, with no owner work. In particular, do not
// inspect self_->shards(): FLIP may be installing a future owner's vector while
// this IO tenure is still returning its final acknowledgements. Live CONFIG's
// reader fields are independent of that vector. Foreign pointers die before
// publication, including when a local read is lowered to an owner task.
template <>
uint32_t ExLoopT<true>::split_read_local_pass() {
    if (!read_local_enabled()) std::abort();
    cached_now_ms_ = realtime_ms();
    refresh_live_config<false>();
    if (maxmemory_enabled_)
        cached_lru_clock_ = static_cast<uint8_t>(
            (static_cast<uint64_t>(cached_now_ms_ / 1000) >> lru_clock_shift_) & 0x1f);
    uint32_t did = drain_local_reads();
    // The shared fused drain books execution as well as parsing in LoopSignals::ops.
    // Split IO's counter means parsed/dispatched commands (FLIP consumes that rate),
    // so cancel that second charge once per pass. Successful reads remain visible
    // in ReadLocalStats and command counts; EX still counts only owner work.
    self_->sig().ops -= did;
    did += read_local_impl().deferred.drain_ready();
    auto& lane = read_local_impl();
    if (__builtin_expect(lane.lane_pressure != 0, false)) lane.lane_pressure--;
    const uint32_t cap = srv_->debug_read_local_lane_cap();
    const uint16_t want = cap == 0 ? static_cast<uint16_t>(kInboxSlots)
        : static_cast<uint16_t>(std::min<uint32_t>(cap, kInboxSlots));
    if (__builtin_expect(lane.lane_admit_cap != want, false)) lane.lane_admit_cap = want;
    self_->publish_read_local_tick(srv_->read_local_epoch());
    return did;
}

int run_split_read_local_server(Server& srv, const SnapshotLoadPlan* aof_base_plan,
                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
                     LateUnixListener& unix_listener,
                     ShutdownReportFinalLine& final_report) {
    const Config& cfg = srv.cfg();
    const uint32_t nthreads = srv.nthreads();
    if (cfg.thread_mode != ThreadMode::Split || !srv.read_local_enabled()) std::abort();
    std::printf("tomokv-cpp: %u threads (%zu io + %zu ex), %u shard(s),"
                " thread-mode=2s, overlap=0, read-local=1, %s, alloc=%s\n",
                nthreads, srv.placement().ifid_threads().size(),
                srv.placement().ex_threads().size(), cfg.shards,
                cfg.net_io == NetIoEngine::Epoll ? "epoll" : "io_uring", alloc_backend());
    for (const ThreadPlacement& placement : srv.placement().threads())
        std::printf("  thread t%u: role=%s cpu=%d L3=%u shards=%zu read-local=%u\n",
                    placement.id, placement.role == Role::Ifid ? "ifid" : "ex",
                    placement.cpu, placement.domain, srv.thread(placement.id).shards().size(),
                    placement.role == Role::Ifid ? 1u : 0u);
    std::fflush(stdout);

    std::vector<std::thread> pool;
    std::vector<IoLoop> ios(nthreads);
    std::vector<FusedExLoop> executors(nthreads);
    // Reuse the 1s boot gate's stop-aware loading/listener barriers. Every physical thread binds
    // its permanent sink and every owner arms its stores before any reader can enter the lane.
    FusedBootGate boot(nthreads);
    // Identical to placement().ifid_threads().front() whenever a unixsocket is configured, but
    // read from the server so the LB's unix_owner_tid_ and the actual attach point cannot drift.
    const uint32_t unix_owner = srv.unix_owner_tid();
    auto report_graceful_shutdown = [&] {
        if (srv.read_local_enabled()) {
            // Joined readers cannot run another grace callback. Empty their private
            // retirement queues and disarm every store hook before taking the immutable report;
            // the subsequent atomic-record drain may detach more than a bounded callback list.
            for (FusedExLoop& executor : executors) executor.read_local_shutdown_drain();
            for (uint32_t sid = 0; sid < srv.nshards(); sid++)
                srv.shard(static_cast<int32_t>(sid)).store().configure_read_local(false, {});
        }
        // All owners and readers are quiescent. Release pending-entry references before IoLoop
        // destruction, then return their deferred ScatterState arenas to the correct IO-owned
        // pools. Server normally outlives those pools, so leaving this to FlatStore destructors
        // would leak the retained arenas.
        for (uint32_t sid = 0; sid < srv.nshards(); sid++)
            srv.shard(static_cast<int32_t>(sid)).store().atomic_shutdown_release_records();
        for (IoLoop& io : ios) io.reap_atomic_deferred();
        ShutdownReport report = collect_shutdown_report(srv, ios, executors);
        print_shutdown_report_human(report);
        final_report.arm(std::move(report));
        acl_shutdown();
    };

    for (uint32_t tid = 0; tid < nthreads; tid++)
        pool.emplace_back([&, tid] {
            if (cfg.pin_threads) pin_read_local_thread(srv.placement().cpu_of_thread(tid));
            ThreadCtx& self = srv.thread(tid);
            self.latch_placement(srv.topo());
            bind_thread_arena();
            bool ok = self.init_task_inbox_local(srv.placement().ifid_threads(),
                                                 srv.placement().ex_threads());
            if (ok) ok = executors[tid].init(&srv, &self, true);
            std::string local_error;
            if (ok && aof_base_plan)
                ok = snapshot_load_owned(*aof_base_plan, srv, self, local_error);
            if (ok && !aof_plans.empty()) {
                for (const auto& plan : aof_plans) {
                    if (!aof_load_owned(*plan, srv, self, local_error)) {
                        ok = false;
                        break;
                    }
                }
            } else if (ok && load_plan) {
                ok = snapshot_load_owned(*load_plan, srv, self, local_error);
            }
            if (ok)
                ok = ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
                                   tls_context, true);
            if (ok)
                self.bind_io_role_hooks(
                    &ios[tid],
                    [](void* p) { return static_cast<IoLoop*>(p)->prepare_activation(); },
                    [](void* p) { static_cast<IoLoop*>(p)->cancel_prepared_activation(); });
            if (ok)
                self.bind_client_registration_hooks(
                    [](void* p, Client* client) {
                        return static_cast<IoLoop*>(p)->prepare_client_registration(client);
                    },
                    [](void* p, Client* client) {
                        static_cast<IoLoop*>(p)->cancel_client_registration(client);
                    });
            if (ok)
                self.bind_client_capacity_hook(
                    [](void* p, uint32_t incoming) {
                        return static_cast<IoLoop*>(p)->prepare_client_transfer_capacity(incoming);
                    });
            if (ok) {
                executors[tid].activate_fused(&ios[tid].ring());
                ios[tid].bind_fused_executor(&executors[tid]);
                executors[tid].bind_fused_completion(
                    &ios[tid],
                    [](void* p, Client* client) {
                        static_cast<IoLoop*>(p)->fused_executor_completion<false>(client);
                    });
                if (srv.read_local_enabled())
                    executors[tid].bind_read_local_demotion(
                        &ios[tid],
                        [](void* p, Client* client, const uint64_t* probed,
                           const ReadLocalFallbackReason* fallbacks,
                           uint32_t probed_count, uint32_t& demoted) {
                            return static_cast<IoLoop*>(p)->fused_demote_local_read_batch(
                                client, probed, fallbacks, probed_count, demoted);
                        });
                self.bind_fused_executor_hooks(
                    &executors[tid],
                    [](void* p) {
                        return static_cast<FusedExLoop*>(p)->split_read_local_pass();
                    },
                    [](void*, SnapshotManager*) {
                        // Snapshot broadcasts target EX only in split placement. The writer's
                        // progress hook above still advances QSBR during blocking SAVE.
                        std::abort();
                    });
            }
            if (!boot.arrive_loaded(tid, ok, local_error)) return;
            if (!boot.wait_until_ready(tid, self.stop_flag())) return;
            if (self.role() == Role::Ifid) {
                if (!self.shards().empty()) std::abort();
                if (!ios[tid].activate()) {
                    boot.give_up(tid, "read-local IO listener activation failed");
                    return;
                }
            } else {
                executors[tid].activate();
            }
            if (!boot.arrive_ready(tid)) return;
            if (!boot.wait_until_running(tid, self.stop_flag())) return;
            for (;;) {
                if (self.stop_flag().load(std::memory_order_relaxed)) break;
                const Role role = self.role();
                if (role == Role::Ifid) {
                    if (!ios[tid].activate()) std::abort();
                    self.publish_ready_role(Role::Ifid);
                    ios[tid].run_fused();
                    self.publish_ready_role(Role::Idle);
                    if (!self.stop_flag().load(std::memory_order_relaxed)) ios[tid].deactivate();
                } else if (role == Role::Ex) {
                    executors[tid].activate();
                    self.publish_ready_role(Role::Ex);
                    executors[tid].run();
                    self.publish_ready_role(Role::Idle);
                } else {
                    std::this_thread::yield();
                }
            }
            self.publish_read_local_parked(srv.read_local_epoch());
        });

    auto stop_workers = [&] {
        for (uint32_t tid = 0; tid < nthreads; tid++)
            srv.thread(tid).stop_flag().store(true, std::memory_order_relaxed);
        boot.stop();
        for (std::thread& worker : pool)
            if (worker.joinable()) worker.join();
    };
    if (!boot.wait_loaded(srv.shutting_down())) {
        stop_workers();
        const std::string error = boot.error();
        if (!error.empty()) std::fprintf(stderr, "persistence load failed: %s\n", error.c_str());
        const bool interrupted = srv.shutting_down().load(std::memory_order_relaxed);
        if (interrupted) report_graceful_shutdown();
        return interrupted ? 0 : 1;
    }
    srv.set_loading(false);
    if (srv.shutting_down().load(std::memory_order_relaxed)) {
        stop_workers();
        report_graceful_shutdown();
        return 0;
    }

    auto probe_listener = [&](uint32_t port, bool tls) {
        if (!port) return true;
        const int probe = IoLoop::make_reuseport_listener(
            cfg.bind_addr, port, cfg.tcp_backlog, tls);
        if (probe < 0) return false;
        ::close(probe);
        return true;
    };
    const bool port_ok = probe_listener(cfg.port, false);
    const bool tls_port_ok = port_ok && probe_listener(cfg.tls_port, true);
    if (!port_ok || !tls_port_ok) {
        std::perror(port_ok ? "bind tls-port" : "bind");
        stop_workers();
        return 1;
    }
    std::string unix_error;
    if (!unix_listener.open(cfg.tcp_backlog, unix_error)) {
        std::fprintf(stderr, "%s\n", unix_error.c_str());
        stop_workers();
        return 1;
    }
    if (unix_listener.fd() >= 0) {
        if (!ios[unix_owner].attach_listener(unix_listener.fd())) {
            std::fprintf(stderr, "unix listener attach failed on t%u\n", unix_owner);
            stop_workers();
            return 1;
        }
        (void)unix_listener.release_fd();
    }
    if (!boot.advance_ready(srv.shutting_down()) ||
        !boot.wait_ready(srv.shutting_down()) ||
        !boot.advance_running(srv.shutting_down())) {
        stop_workers();
        const std::string error = boot.error();
        if (!error.empty()) std::fprintf(stderr, "split read-local boot failed: %s\n", error.c_str());
        const bool interrupted = srv.shutting_down().load(std::memory_order_relaxed);
        if (interrupted) report_graceful_shutdown();
        return interrupted ? 0 : 1;
    }

    // Reached only after every ring, sink and listener is ready and no stop edge was taken.
    if (cfg.port) std::printf("listening on %s:%u\n", cfg.bind_addr, cfg.port);
    if (cfg.tls_port) std::printf("listening with TLS on %s:%u\n", cfg.bind_addr, cfg.tls_port);
    if (unix_listener.bound()) std::printf("listening on unix:%s\n", cfg.unixsocket);
    std::fflush(stdout);

    if (srv.flipctl_enabled()) {
        while (!srv.shutting_down().load(std::memory_order_relaxed)) {
            (void)srv.flipctl_tick(now_ns() / 1000000ull);
            if (srv.shutting_down().load(std::memory_order_relaxed)) break;
            (void)signal_doorbell_wait(srv.flipctl_wait_ms());
        }
    }

    for (std::thread& worker : pool) worker.join();
    // The unix socket file is unlinked by its RAII owner in main, for every return path.
    report_graceful_shutdown();
    return 0;
}

// Keep the scratch arrays out of cmd_info's frame, including with LTO. Only an armed INFO SERVER
// request reaches this helper; the boot/runtime paths install no reporting callback or sidecar.
__attribute__((noinline, cold))
void append_read_local_thread_info(std::string& body, const Server& srv) {
    uint32_t active_readers = 0;
    uint32_t owned[kMaxThreads] = {};
    // Read the published ownership directory, never a vector FLIP may be rewriting.
    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
        owned[srv.worker_of_shard(static_cast<int32_t>(sid))]++;
    char row[192];  // fixed grammar, including full-width uint32/uint64 counters
    for (uint32_t tid = 0; tid < srv.nthreads(); tid++) {
        const ThreadCtx& thread = srv.thread(tid);
        const bool active = thread.read_local_lane_active();
        active_readers += active;
        const Role role = thread.role();
        const char* name = role == Role::Idle ? "idle"
            : srv.thread_mode() == ThreadMode::Fused ? "unified"
            : role == Role::Ifid ? "ifid" : "ex";
        const ReadLocalStats& stats = thread.read_local_stats();
        // Lifetime counters survive FLIP and RESETSTAT; INFO stats retains its existing
        // RESETSTAT-relative aggregate counters.
        const int n = std::snprintf(row, sizeof(row),
            "read_local_thread_%u:role=%s,shards=%u,active=%u,"
            "hits_total=%llu,mget_hits_total=%llu\r\n",
            tid, name, owned[tid], active ? 1u : 0u,
            static_cast<unsigned long long>(stats.hits),
            static_cast<unsigned long long>(stats.mget_local_hits));
        if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
        body.append(row, static_cast<size_t>(n));
    }
    const int n = std::snprintf(row, sizeof(row), "read_local_active_threads:%u\r\n",
                              active_readers);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(row)) std::abort();
    body.append(row, static_cast<size_t>(n));
}

}  // namespace tomo
