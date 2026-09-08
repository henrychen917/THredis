// main.cc — boot, thread launch, pinning, shutdown.
//
// Split mode keeps the pure 2s design. Fused startup is isolated in genthread.cc so boot mode
// selection cannot change the split loop translation unit's optimization or object code.
#include <pthread.h>
#include <sched.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/server.h"
#include "base/alloc.h"
#include "core/io_loop.h"
#include "core/ex_loop.h"
#include "core/genthread.h"
#include "core/signal_doorbell.h"
#include "core/shutdown_report.h"
#include "cmd/command.h"
#include "cmd/acl.h"
#include "net/unix_listener.h"
#include "persist/aof.h"

using namespace tomo;

// Signal-handler state. A handler may only touch lock-free atomics, so the thread table is a
// fixed array of atomics published through an atomic count: arm() fills the slots, then stores the
// count (release); the handler loads the count (acquire) and walks exactly that many entries. No
// vector, no reallocation, no torn size/data pair. Everything is cleared again before Server is
// destroyed, so a late signal finds nothing to poke instead of a dead object.
//
// Publication happens with SIGINT/SIGTERM blocked on this thread and the handlers are installed
// only afterwards. Until arm() returns, SIGINT/SIGTERM keep the default action (terminate), which
// is the right answer while Server::init is still allocating tables and no thread exists to stop
// -- an earlier revision installed the handlers first and a signal in that window was silently
// swallowed. disarm() restores SIG_IGN, so a signal after teardown cannot re-enter the handler.
static std::atomic<Server*> g_signal_server{nullptr};
static std::array<std::atomic<ThreadCtx*>, kMaxThreads> g_signal_threads{};
static std::atomic<uint32_t> g_signal_thread_count{0};
static std::atomic<bool> g_signal_armed{false};

static_assert(std::atomic<Server*>::is_always_lock_free);
static_assert(std::atomic<ThreadCtx*>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

static void on_signal(int) {
    if (!g_signal_armed.load(std::memory_order_acquire)) return;
    if (Server* server = g_signal_server.load(std::memory_order_acquire))
        server->shutting_down().store(true, std::memory_order_relaxed);
    const uint32_t count = g_signal_thread_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; i++) {
        ThreadCtx* thread = g_signal_threads[i].load(std::memory_order_relaxed);
        if (thread) thread->stop_flag().store(true, std::memory_order_relaxed);
    }
    signal_doorbell_notify();
}

class ScopedSignalHandlers {
public:
    bool arm(Server& server) {
        if (server.nthreads() > kMaxThreads) return false;
        sigset_t blocked{}, previous{};
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGINT);
        sigaddset(&blocked, SIGTERM);
        if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) return false;

        for (uint32_t i = 0; i < server.nthreads(); i++)
            g_signal_threads[i].store(&server.thread(i), std::memory_order_relaxed);
        g_signal_server.store(&server, std::memory_order_release);
        g_signal_thread_count.store(server.nthreads(), std::memory_order_release);

        struct sigaction action{};
        action.sa_handler = on_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        const bool installed = ::sigaction(SIGINT, &action, nullptr) == 0 &&
                               ::sigaction(SIGTERM, &action, nullptr) == 0;
        if (installed) g_signal_armed.store(true, std::memory_order_release);
        (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        if (!installed) disarm();
        return installed;
    }

    ~ScopedSignalHandlers() { disarm(); }

private:
    void disarm() {
        if (!g_signal_server.load(std::memory_order_relaxed) &&
            !g_signal_armed.load(std::memory_order_relaxed)) return;
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        (void)::sigaction(SIGINT, &ignore, nullptr);
        (void)::sigaction(SIGTERM, &ignore, nullptr);
        g_signal_armed.store(false, std::memory_order_release);
        g_signal_thread_count.store(0, std::memory_order_release);
        g_signal_server.store(nullptr, std::memory_order_release);
        for (auto& thread : g_signal_threads)
            thread.store(nullptr, std::memory_order_relaxed);
    }
};

// Pins to one cpu. Relative to the process's ALLOWED set by construction, because the caller takes
// the cpu from Topology, which intersects with sched_getaffinity. A pin to a cpu outside the mask
// silently leaves the thread floating rather than erroring — that was a real bug in the fork.
static void pin_to(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

int main(int argc, char** argv) {
    // Declared before every other automatic: once armed on a clean runtime shutdown, this emits
    // only after all later-declared objects (including server/loops/listeners/signals) destruct.
    ShutdownReportFinalLine final_shutdown_line;

    // Hash key material, before anything hashes. getrandom never fails for 24 bytes on any kernel
    // we run; if it somehow does, a zero seed degrades to the old deterministic behavior rather
    // than refusing to boot.
    {
        uint64_t buf[3] = {};
        if (getrandom(buf, sizeof(buf), 0) == sizeof(buf)) {
            g_hash_seed = buf[0]; g_sip_k0 = buf[1]; g_sip_k1 = buf[2];
        }
    }

    Config cfg;
    ConfigParseState parse_state;
    std::vector<std::string> token_store;      // owns conf-file tokens; Config keeps views into it
    std::vector<const char*> conf_tokens, cli_tokens;

    // Redis-style optional first argument: ./tomokv tomokv.conf. Flags override that file.
    const char* conf_path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (i == 1 && argv[i][0] != '-') {
            conf_path = argv[i];
        } else {
            cli_tokens.push_back(argv[i]);
        }
    }
    cfg.conf_path = conf_path;   // retained for CONFIG REWRITE; points into argv, which outlives us
    if (conf_path) {
        if (!load_conf_file(conf_path, token_store)) return 1;
        for (const std::string& t : token_store) conf_tokens.push_back(t.c_str());
        const int rc = parse_config_args(conf_tokens, cfg, parse_state, 1, argv[0]);
        if (rc != kConfigParsed) {
            if (rc == kConfigError) std::fprintf(stderr, "(while parsing %s)\n", conf_path);
            return rc == kConfigHelp ? 0 : 1;
        }
    }
    // CLI parses second so flags override the file.
    {
        const int rc = parse_config_args(cli_tokens, cfg, parse_state, 2, argv[0]);
        if (rc != kConfigParsed) return rc == kConfigHelp ? 0 : 1;
    }
    if (validate_config(cfg) != kConfigParsed) return 1;
    // THE ENGINE IS LATCHED HERE, once, before anything that reads it exists. Every Ring in the
    // process must agree (a uring ring cannot receive an eventfd doorbell and vice versa), and no
    // thread has been spawned yet, so this store needs no synchronisation.
    if (cfg.net_io == NetIoEngine::Epoll) {
        g_ring_epoll_mode = true;
        std::fprintf(stderr, "--net-io epoll: using syscall persistence (no io_uring ring)\n");
    }
    std::unique_ptr<TlsContext> tls_context;
    if (cfg.tls_port) {
        std::string tls_error;
        tls_context = TlsContext::create(cfg, tls_error);
        if (!tls_context) {
            std::fprintf(stderr, "TLS configuration failed: %s\n", tls_error.c_str());
            return 1;
        }
    }
    if (!command_registry_init(cfg.tls_port != 0, cfg.thread_mode == ThreadMode::Fused,
                               Server::read_local_enabled(cfg))) {
        std::fprintf(stderr, "command registry init failed\n");
        return 1;
    }

    Server srv;
    if (!srv.prepare_boot(cfg)) {
        std::fprintf(stderr, "placement/geometry resolution failed\n");
        return 1;
    }

    std::unique_ptr<SnapshotLoadPlan> aof_base_plan;
    std::vector<std::unique_ptr<AofReplayPlan>> aof_plans;
    if (cfg.appendonly) {
        std::string warning, error;
        if (!aof_read_recovery(cfg, cfg.shards, aof_base_plan, aof_plans, warning, error)) {
            std::fprintf(stderr, "AOF load plan failed: %s\n", error.c_str());
            return 1;
        }
        if (!warning.empty()) std::fprintf(stderr, "AOF warning: %s\n", warning.c_str());
        if (aof_base_plan) {
            g_hash_kind = static_cast<HashKind>(aof_base_plan->hash_kind);
            g_hash_seed = aof_base_plan->hash_seed;
            g_sip_k0 = aof_base_plan->sip_k0;
            g_sip_k1 = aof_base_plan->sip_k1;
        } else if (!aof_plans.empty()) {
            const AofReplayPlan& plan = *aof_plans.front();
            g_hash_kind = static_cast<HashKind>(plan.hash_kind);
            g_hash_seed = plan.hash_seed;
            g_sip_k0 = plan.sip_k0;
            g_sip_k1 = plan.sip_k1;
        }
    }

    std::unique_ptr<SnapshotLoadPlan> load_plan;
    if (!aof_base_plan && aof_plans.empty()) {
        // The recovery input derives from the existing persistence destination. AOF recovery
        // keeps precedence; a missing dump boots empty, while an unreadable/corrupt dump fails.
        // Path construction and filesystem work remain entirely behind this boot-only gate.
        const std::string load_path = std::string(cfg.dir) + "/" + cfg.dbfilename;
        struct stat dump_stat;
        if (::stat(load_path.c_str(), &dump_stat) == 0) {
            std::string error;
            load_plan = snapshot_read_plan(load_path.c_str(), cfg.shards, error);
            if (!load_plan) {
                std::fprintf(stderr, "snapshot load plan failed: %s\n", error.c_str());
                return 1;
            }
        } else if (errno != ENOENT) {
            std::fprintf(stderr, "snapshot stat failed for '%s': %s\n",
                         load_path.c_str(), std::strerror(errno));
            return 1;
        }
    }
    if (load_plan) {
        // The router consumes the keyed hash, so its key material is part of the persisted format.
        // Restore it before Server::init builds shard ownership and before any loaded key is hashed.
        g_hash_kind = static_cast<HashKind>(load_plan->hash_kind);
        g_hash_seed = load_plan->hash_seed;
        g_sip_k0 = load_plan->sip_k0;
        g_sip_k1 = load_plan->sip_k1;
    }

    std::signal(SIGPIPE, SIG_IGN);      // send() errors arrive as -EPIPE on the CQE instead
    // SIGINT/SIGTERM handlers are armed later, once the threads they stop exist.

    if (!good_size_matches_allocator()) {
        std::fprintf(stderr, "good_size() disagrees with the allocator's size classes\n");
        return 1;
    }
    if (!FlatStore::pointer_encoding_supported()) {
        std::fprintf(stderr,
                     "fatal: FlatStore requires KvObj allocations below 2^48; "
                     "this virtual-address layout is unsupported\n");
        return 1;
    }
    const AofReplayPlan* active_aof_plan = aof_plans.empty() ? nullptr : aof_plans.back().get();
    try {
        if (!srv.init(cfg, active_aof_plan)) {
            std::fprintf(stderr, "server init failed\n");
            return 1;
        }
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "server init failed: out of memory allocating shard tables\n");
        return 1;
    }
    srv.set_loading(true);
    command_bind_server(&srv);
    {
        std::string acl_error;
        if (!acl_initialize(srv, cfg, acl_error)) {
            std::fprintf(stderr, "%s\n", acl_error.c_str());
            return 1;
        }
    }

    // ONE RAII owner for the AF_UNIX pathname and its untransferred fd (LateUnixListener also
    // carries the non-socket / already-accepting probes the inline version used to do here), and
    // open() is deliberately deferred until after the persistence-load barrier in the selected
    // runtime below, so no unix client can connect before every owner has decoded its shards.
    // The pathname and any not-yet-transferred fd have one lifetime owner. open() is deliberately
    // called only after the persistence-load barrier in the selected runtime below.
    LateUnixListener unix_listener(cfg.unixsocket);
    SignalDoorbell signal_doorbell;
    if (!signal_doorbell.init()) {
        std::perror("eventfd shutdown doorbell");
        return 1;
    }
    ScopedSignalHandlers signal_handlers;
    if (!signal_handlers.arm(srv)) {
        std::perror("install signal handlers");
        return 1;
    }

    if (cfg.thread_mode == ThreadMode::Fused) {
        srv.topo().dump(stdout);
        return run_fused_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
                                tls_context.get(), unix_listener, final_shutdown_line);
    }
    if (srv.read_local_enabled()) {
        srv.topo().dump(stdout);
        return run_split_read_local_server(srv, aof_base_plan.get(), aof_plans, load_plan.get(),
                                           tls_context.get(), unix_listener, final_shutdown_line);
    }

    srv.topo().dump(stdout);
    std::printf("tomokv-cpp: %u threads (%zu io + %zu ex), %u shard(s),"
                " thread-mode=2s, overlap=%u, %s, alloc=%s\n", srv.nthreads(),
                srv.placement().ifid_threads().size(), srv.placement().ex_threads().size(),
                cfg.shards, cfg.overlap,
                cfg.net_io == NetIoEngine::Epoll ? "epoll" : "io_uring", alloc_backend());
    for (const ThreadPlacement& p : srv.placement().threads()) {
        const char* role = p.role == Role::Ifid ? "ifid" : p.role == Role::Ex ? "ex" : "idle";
        std::printf("  thread t%u: role=%s cpu=%d L3=%u shards=%zu\n", p.id, role, p.cpu,
                    p.domain, srv.thread(p.id).shards().size());
    }
    std::fflush(stdout);

    // Placement decides every cpu directly. Pinning is relative to the process's ALLOWED set by
    // construction because both discovery and --place validation intersect with sched affinity.
    const uint32_t nthreads = srv.nthreads();
    std::vector<std::thread> pool;
    std::vector<IoLoop> ios(nthreads);
    std::vector<ExLoop> exs(nthreads);
    std::mutex load_mu;
    std::condition_variable load_cv;
    uint32_t loaders_done = 0;
    bool load_ok = true;
    std::string load_error;
    // An io thread that fails to provision after the listeners were probed cannot report through
    // load_ok (that barrier has passed); it stops every thread and records the failure here so
    // the exit status says what happened instead of a silent 0.
    std::atomic<bool> io_boot_failed{false};

    auto pin_for = [&](uint32_t tid) {
        if (cfg.pin_threads) pin_to(srv.placement().cpu_of_thread(tid));
    };
    auto report_graceful_shutdown = [&] {
        // All owners and readers are quiescent. Release pending-entry references before IoLoop
        // destruction, then return their deferred ScatterState arenas to the correct IO-owned
        // pools. Server normally outlives those pools, so leaving this to FlatStore destructors
        // would leak the retained arenas.
        for (uint32_t sid = 0; sid < srv.nshards(); sid++)
            srv.shard(static_cast<int32_t>(sid)).store().atomic_shutdown_release_records();
        for (IoLoop& io : ios) io.reap_atomic_deferred();
        ShutdownReport report = collect_shutdown_report(srv, ios, exs);
        print_shutdown_report_human(report);
        final_shutdown_line.arm(std::move(report));
        acl_shutdown();
    };

    // Workers and senders BEFORE io: an io thread that dispatches or hands off to a thread whose
    // ring does not exist yet would find nothing to wake, and the work would sit until something
    // unrelated poked the peer.
    for (uint32_t tid : srv.placement().ex_threads())
        pool.emplace_back([&, tid] {
            pin_for(tid);
            ThreadCtx& self = srv.thread(tid);
            self.latch_placement(srv.topo());   // after pinning: sched_getcpu is only now truthful
            bind_thread_arena();                // per-worker jemalloc arena; no-op without it
            bool ok = self.init_task_inbox_local(srv.placement().ifid_threads(),
                                                 srv.placement().ex_threads());
            if (ok) ok = exs[tid].init(&srv, &self);
            std::string local_error;
            if (ok && aof_base_plan)
                ok = snapshot_load_owned(*aof_base_plan, srv, self, local_error);
            if (ok && !aof_plans.empty()) {
                for (const auto& plan : aof_plans) {
                    if (!aof_load_owned(*plan, srv, self, local_error)) { ok = false; break; }
                }
            } else if (ok && load_plan) {
                ok = snapshot_load_owned(*load_plan, srv, self, local_error);
            }
            // Provision the opposite loop before runtime mutation is possible. Dormant IO creates
            // its ring/epoll/WB state but no listener and does not bind AOF, so boot loading still
            // happens before any connection can arrive and before the initial AOF writer exists.
            if (ok)
                ok = ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
                                   tls_context.get(), true);
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
            {
                std::lock_guard<std::mutex> lock(load_mu);
                if (!ok) {
                    load_ok = false;
                    if (load_error.empty())
                        load_error = local_error.empty() ? "executor initialization failed" : local_error;
                }
                loaders_done++;
            }
            load_cv.notify_one();
            if (!ok) return;
            for (;;) {
                if (self.stop_flag().load(std::memory_order_relaxed)) break;
                const Role role = self.role();
                if (role == Role::Ex) {
                    exs[tid].activate();
                    self.publish_ready_role(Role::Ex);
                    exs[tid].run();
                    self.publish_ready_role(Role::Idle);
                } else if (role == Role::Ifid) {
                    if (!ios[tid].activate()) std::abort();
                    self.publish_ready_role(Role::Ifid);
                    ios[tid].run();
                    self.publish_ready_role(Role::Idle);
                    if (!self.stop_flag().load(std::memory_order_relaxed)) ios[tid].deactivate();
                } else {
                    std::this_thread::yield();
                }
            }
        });

    // Main performed every read(2); the real owning executor threads now deserialize their own
    // shard sections in parallel. No TCP listener exists until all owners report success.
    {
        std::unique_lock<std::mutex> lock(load_mu);
        load_cv.wait(lock, [&] {
            return loaders_done == static_cast<uint32_t>(srv.placement().ex_threads().size());
        });
    }
    if (!load_ok) {
        for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
        for (auto& thread : pool) thread.join();
        if (srv.shutting_down().load(std::memory_order_relaxed)) {
            srv.set_loading(false);
            report_graceful_shutdown();
            return 0;
        }
        std::fprintf(stderr, "persistence load failed: %s\n", load_error.c_str());
        return 1;
    }
    srv.set_loading(false);
    if (srv.shutting_down().load(std::memory_order_relaxed)) {
        for (uint32_t i = 0; i < nthreads; i++)
            srv.thread(i).stop_flag().store(true, std::memory_order_relaxed);
        for (auto& thread : pool)
            if (thread.joinable()) thread.join();
        report_graceful_shutdown();
        return 0;
    }

    // Probe only after boot load. Each io thread then opens its own SO_REUSEPORT listener.
    if (cfg.port) {
        const int probe = IoLoop::make_reuseport_listener(
            cfg.bind_addr, cfg.port, srv.cfg().tcp_backlog);
        if (probe < 0) {
            std::perror("bind");
            for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
            for (auto& thread : pool) thread.join();
            return 1;
        }
        ::close(probe);
    }
    if (cfg.tls_port) {
        const int probe = IoLoop::make_reuseport_listener(
            cfg.bind_addr, cfg.tls_port, srv.cfg().tcp_backlog, true);
        if (probe < 0) {
            std::perror("bind tls-port");
            for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
            for (auto& thread : pool) thread.join();
            return 1;
        }
        ::close(probe);
    }
    std::string unix_error;
    if (!unix_listener.open(cfg.tcp_backlog, unix_error)) {
        std::fprintf(stderr, "%s\n", unix_error.c_str());
        for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
        for (auto& thread : pool) thread.join();
        return 1;
    }

    const uint32_t unix_owner = srv.unix_owner_tid();
    for (uint32_t tid : srv.placement().ifid_threads())
        pool.emplace_back([&, tid] {
            pin_for(tid);
            ThreadCtx& self = srv.thread(tid);
            self.latch_placement(srv.topo());
            bind_thread_arena();
            auto boot_failed = [&](const char* what) {
                std::fprintf(stderr, "%s failed on t%u\n", what, tid);
                io_boot_failed.store(true, std::memory_order_relaxed);
                for (uint32_t i = 0; i < nthreads; i++)
                    srv.thread(i).stop_flag().store(true, std::memory_order_relaxed);
            };
            if (!self.init_task_inbox_local(srv.placement().ifid_threads(),
                                            srv.placement().ex_threads())) {
                boot_failed("task inbox initialization");
                return;
            }
            // Provision dormant EX first; active IO then republishes its own ring as the current
            // role endpoint. No runtime conversion can fail later for lack of a ring. A failure
            // here must not let the thread vanish silently: the join below waits for it, and the
            // other threads would keep serving with one reuseport listener missing.
            if (!exs[tid].init(&srv, &self, true)) {
                boot_failed("executor loop provisioning");
                return;
            }
            // Dormant, exactly like the executor pool above: the SO_REUSEPORT listeners and the
            // AOF writer binding are opened by activate() in the role loop below instead of here,
            // which is what leaves the loop cold enough to accept the transferred unix fd.
            if (!ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, -1,
                               tls_context.get(), true)) {
                boot_failed("io loop provisioning");
                return;
            }
            if (tid == unix_owner && unix_listener.fd() >= 0) {
                if (!ios[tid].attach_listener(unix_listener.fd())) {
                    boot_failed("unix listener attach");
                    return;
                }
                (void)unix_listener.release_fd();
            }
            self.bind_io_role_hooks(
                &ios[tid],
                [](void* p) { return static_cast<IoLoop*>(p)->prepare_activation(); },
                [](void* p) { static_cast<IoLoop*>(p)->cancel_prepared_activation(); });
            self.bind_client_registration_hooks(
                [](void* p, Client* client) {
                    return static_cast<IoLoop*>(p)->prepare_client_registration(client);
                },
                [](void* p, Client* client) {
                    static_cast<IoLoop*>(p)->cancel_client_registration(client);
                });
            self.bind_client_capacity_hook(
                [](void* p, uint32_t incoming) {
                    return static_cast<IoLoop*>(p)->prepare_client_transfer_capacity(incoming);
                });
            for (;;) {
                if (self.stop_flag().load(std::memory_order_relaxed)) break;
                const Role role = self.role();
                if (role == Role::Ifid) {
                    if (!ios[tid].activate()) std::abort();
                    self.publish_ready_role(Role::Ifid);
                    ios[tid].run();
                    self.publish_ready_role(Role::Idle);
                    if (!self.stop_flag().load(std::memory_order_relaxed)) ios[tid].deactivate();
                } else if (role == Role::Ex) {
                    exs[tid].activate();
                    self.publish_ready_role(Role::Ex);
                    exs[tid].run();
                    self.publish_ready_role(Role::Idle);
                } else {
                    std::this_thread::yield();
                }
            }
        });

    if (cfg.port) std::printf("listening on %s:%u\n", cfg.bind_addr, cfg.port);
    if (cfg.tls_port) std::printf("listening with TLS on %s:%u\n", cfg.bind_addr, cfg.tls_port);
    if (unix_listener.bound()) std::printf("listening on unix:%s\n", cfg.unixsocket);
    std::fflush(stdout);

    // The automatic split controller has exactly one writer: this main/monitor thread. Worker
    // loops only publish owner-local counters and execute the unchanged FLIP stage machine. With
    // the default --flip-auto 0 this block does not run and allocates/schedules nothing.
    if (srv.flipctl_enabled()) {
        while (!srv.shutting_down().load(std::memory_order_relaxed)) {
            (void)srv.flipctl_tick(now_ns() / 1000000ull);
            if (srv.shutting_down().load(std::memory_order_relaxed)) break;
            (void)signal_doorbell_wait(srv.flipctl_wait_ms());
        }
    }

    for (auto& t : pool) t.join();
    report_graceful_shutdown();
    return io_boot_failed.load(std::memory_order_relaxed) ? 1 : 0;
}
