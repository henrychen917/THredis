// Unified generalized-thread runtime. This translation unit is the boot-time architecture wall:
// only it instantiates ExLoopT<true> and IoLoop's Fused=true loop methods for pipelines 0, 1, and 2.
#include "genthread.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
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

namespace tomo {
namespace {

void pin_fused_thread(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

}  // namespace

void IoLoop::run_fused() {
    if (!fused_executor_) std::abort();
    const bool has_unix = unix_listen_fd_ >= 0 ||
                          (srv_->cfg().unixsocket && *srv_->cfg().unixsocket);
    auto run_pipeline = [&](auto pipeline_tag) {
        constexpr uint8_t Pipeline = decltype(pipeline_tag)::value;
        if (epoll_) {
            if (tls_context_) {
                if (has_unix) run_loop<true, true, true, true, Pipeline>();
                else run_loop<false, true, true, true, Pipeline>();
            } else {
                if (has_unix) run_loop<true, false, true, true, Pipeline>();
                else run_loop<false, false, true, true, Pipeline>();
            }
            return;
        }
        if (tls_context_) {
            if (has_unix) run_loop<true, true, false, true, Pipeline>();
            else run_loop<false, true, false, true, Pipeline>();
        } else {
            if (has_unix) run_loop<true, false, false, true, Pipeline>();
            else run_loop<false, false, false, true, Pipeline>();
        }
    };
    switch (srv_->cfg().overlap) {
        case 0: run_pipeline(std::integral_constant<uint8_t, 0>{}); break;
        case 1: run_pipeline(std::integral_constant<uint8_t, 1>{}); break;
        case 2: run_pipeline(std::integral_constant<uint8_t, 2>{}); break;
        default: std::abort();
    }
}

int run_fused_server(Server& srv, const SnapshotLoadPlan* aof_base_plan,
                     const std::vector<std::unique_ptr<AofReplayPlan>>& aof_plans,
                     const SnapshotLoadPlan* load_plan, TlsContext* tls_context,
                     LateUnixListener& unix_listener,
                     ShutdownReportFinalLine& final_report) {
    const Config& cfg = srv.cfg();
    const uint32_t nthreads = srv.nthreads();
    std::printf("tomokv-cpp: %u unified threads, %u shard(s), thread-mode=1s,"
                " overlap=%u, %s, alloc=%s\n", nthreads, cfg.shards,
                cfg.overlap,
                cfg.net_io == NetIoEngine::Epoll ? "epoll" : "io_uring", alloc_backend());
    for (const ThreadPlacement& placement : srv.placement().threads())
        std::printf("  thread t%u: role=unified cpu=%d L3=%u shards=%zu send=self\n",
                    placement.id, placement.cpu, placement.domain,
                    srv.thread(placement.id).shards().size());
    std::fflush(stdout);

    std::vector<std::thread> pool;
    std::vector<IoLoop> ios(nthreads);
    std::vector<FusedExLoop> executors(nthreads);
    // ONE boot gate. The ad-hoc mutex/cv gate this replaced needed a separate `runners_stopped`
    // counter so a thread that saw shutdown while parked still reported at the gate (without it a
    // SIGTERM during a long --load hung main forever). FusedBootGate keeps that property as a
    // first-class `gave_up` arrival, and every wait/advance also takes the stop edge, so the two
    // predicates cannot disagree.
    FusedBootGate boot(nthreads);
    // Identical to placement().ifid_threads().front() whenever a unixsocket is configured, but
    // read from the server so the LB's unix_owner_tid_ and the actual attach point cannot drift.
    const uint32_t unix_owner = srv.unix_owner_tid();
    auto report_graceful_shutdown = [&] {
        if (srv.read_local_enabled()) {
            // Joined fused readers cannot run another grace callback. Empty their private
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
            if (cfg.pin_threads) pin_fused_thread(srv.placement().cpu_of_thread(tid));
            ThreadCtx& self = srv.thread(tid);
            self.latch_placement(srv.topo());
            bind_thread_arena();
            bool ok = self.init_task_inbox_local_fused();
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
                if (cfg.overlap == 0)
                    executors[tid].bind_fused_completion(
                        &ios[tid],
                        [](void* p, Client* client) {
                            static_cast<IoLoop*>(p)->fused_executor_completion<false>(client);
                        });
                else
                    executors[tid].bind_fused_completion(
                        &ios[tid],
                        [](void* p, Client* client) {
                            static_cast<IoLoop*>(p)->fused_executor_completion<true>(client);
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
                if (cfg.overlap == 0)
                    self.bind_fused_executor_hooks(
                        &executors[tid],
                        [](void* p) {
                            return static_cast<FusedExLoop*>(p)->fused_baseline_pass();
                        },
                        [](void* p, SnapshotManager* manager) {
                            static_cast<FusedExLoop*>(p)->fused_snapshot_start(manager);
                        });
                else if (cfg.overlap == 1)
                    self.bind_fused_executor_hooks(
                        &executors[tid],
                        [](void* p) {
                            return static_cast<FusedExLoop*>(p)->fused_coarse_pass();
                        },
                        [](void* p, SnapshotManager* manager) {
                            static_cast<FusedExLoop*>(p)->fused_snapshot_start(manager);
                        });
                else
                    self.bind_fused_executor_hooks(
                        &executors[tid],
                        [](void* p) {
                            // Snapshot's blocking progress loop has no WB filler to interleave.
                            // Use overlap 1's private-lane coarse turn; the main overlap-2 loop
                            // supplies the three-way callback only at its ordinary batch seam.
                            return static_cast<FusedExLoop*>(p)->fused_coarse_pass();
                        },
                        [](void* p, SnapshotManager* manager) {
                            static_cast<FusedExLoop*>(p)->fused_snapshot_start(manager);
                        });
            }
            if (!boot.arrive_loaded(tid, ok, local_error)) return;
            if (!boot.wait_until_ready(tid, self.stop_flag())) return;
            if (!ios[tid].activate()) {
                boot.give_up(tid, "unified listener activation failed");
                return;
            }
            // arrive_ready() returns false on the stop edge: a worker can leave wait_until_ready(),
            // spend time arming its listeners, and come back after main already saw a signal. That
            // arrival is recorded as gave_up so main's wait still sees every thread report.
            if (!boot.arrive_ready(tid)) return;
            if (!boot.wait_until_running(tid, self.stop_flag())) return;
            self.publish_ready_role(Role::Ifid);
            ios[tid].run_fused();
            if (srv.read_local_enabled())
                self.publish_read_local_parked(srv.read_local_epoch());
            self.publish_ready_role(Role::Idle);
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
        if (!error.empty()) std::fprintf(stderr, "unified boot failed: %s\n", error.c_str());
        const bool interrupted = srv.shutting_down().load(std::memory_order_relaxed);
        if (interrupted) report_graceful_shutdown();
        return interrupted ? 0 : 1;
    }

    // Reached only when advance_running() succeeded, i.e. no stop edge was taken; the old
    // `if (!stopping)` guard around these lines is now the gate's own postcondition.
    if (cfg.port) std::printf("listening on %s:%u\n", cfg.bind_addr, cfg.port);
    if (cfg.tls_port) std::printf("listening with TLS on %s:%u\n", cfg.bind_addr, cfg.tls_port);
    if (unix_listener.bound()) std::printf("listening on unix:%s\n", cfg.unixsocket);
    std::fflush(stdout);

    for (std::thread& worker : pool) worker.join();
    // The unix socket file is unlinked by its RAII owner in main, for every return path.
    report_graceful_shutdown();
    return 0;
}

}  // namespace tomo
