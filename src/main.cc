// main.cc — boot, thread launch, pinning, shutdown.
//
// PURE 2s (owner ruling 2026-08-24): io threads receive, parse, dispatch, retire and send;
// executors execute. The 3s posture was measured exhaustively and deleted -- see wb.h's header
// for the evidence. --mode/--wb survive only to reject scripts that still ask for 3s.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/random.h>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "core/server.h"
#include "base/alloc.h"
#include "core/io_loop.h"
#include "core/ex_loop.h"
#include "cmd/command.h"
#include "cmd/acl.h"
#include "persist/aof.h"

using namespace tomo;

static Server*                   g_srv = nullptr;
static std::vector<ThreadCtx*>   g_threads;

static void on_signal(int) {
    if (g_srv) g_srv->shutting_down().store(true, std::memory_order_relaxed);
    for (auto* t : g_threads) t->stop_flag().store(true, std::memory_order_relaxed);
}

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

    // Pre-scan: --conf FILE anywhere, or a bare first argument (redis-style ./tomokv tomokv.conf).
    const char* conf_path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--conf")) {
            if (i + 1 >= argc) { std::fprintf(stderr, "--conf wants a file path\n"); return 1; }
            conf_path = argv[++i];
        } else if (i == 1 && argv[i][0] != '-') {
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
        // --persist-io uring submits its writes and fsyncs as SQEs on the writer io thread's ring,
        // and under this engine that ring does not exist. Rather than half-support it, the network
        // choice implies the persistence one: same kernel interface, one decision. Announced, not
        // silent -- a run whose durability path changed under it must say so.
        if (cfg.persist_io != PersistIoEngine::Normal) {
            cfg.persist_io = PersistIoEngine::Normal;
            std::fprintf(stderr, "--net-io epoll: persist-io forced to normal "
                                 "(the uring persistence engine needs a ring)\n");
        }
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
    if (cfg.load_path && !*cfg.load_path) {
        std::fprintf(stderr, "--load requires a non-empty path\n");
        return 1;
    }
    if (!command_registry_init(cfg.tls_port != 0)) {
        std::fprintf(stderr, "command registry init failed\n");
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
    if (cfg.load_path && !aof_base_plan && aof_plans.empty()) {
        std::string error;
        load_plan = snapshot_read_plan(cfg.load_path, cfg.shards, error);
        if (!load_plan) {
            std::fprintf(stderr, "snapshot load plan failed: %s\n", error.c_str());
            return 1;
        }
        // The router consumes the keyed hash, so its key material is part of the persisted format.
        // Restore it before Server::init builds shard ownership and before any loaded key is hashed.
        g_hash_kind = static_cast<HashKind>(load_plan->hash_kind);
        g_hash_seed = load_plan->hash_seed;
        g_sip_k0 = load_plan->sip_k0;
        g_sip_k1 = load_plan->sip_k1;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);      // send() errors arrive as -EPIPE on the CQE instead

    if (!good_size_matches_allocator()) {
        std::fprintf(stderr, "good_size() disagrees with the allocator's size classes\n");
        return 1;
    }
    Server srv;
    const AofReplayPlan* active_aof_plan = aof_plans.empty() ? nullptr : aof_plans.back().get();
    if (!srv.init(cfg, active_aof_plan)) { std::fprintf(stderr, "server init failed\n"); return 1; }
    g_srv = &srv;
    command_bind_server(&srv);
    {
        std::string acl_error;
        if (!acl_initialize(srv, cfg, acl_error)) {
            std::fprintf(stderr, "%s\n", acl_error.c_str());
            return 1;
        }
    }

    // The bind probe moved AFTER the boot load: no listener may exist until every owner has
    // decoded its shard sections (see the post-load probe below).
    int unix_listener = -1;
    if (cfg.unixsocket && *cfg.unixsocket) {
        struct stat st{};
        if (::lstat(cfg.unixsocket, &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                std::fprintf(stderr, "refusing to replace non-socket unix path '%s'\n", cfg.unixsocket);
                return 1;
            }
            sockaddr_un sa{};
            sa.sun_family = AF_UNIX;
            if (std::strlen(cfg.unixsocket) >= sizeof(sa.sun_path)) {
                std::fprintf(stderr, "unixsocket path is too long\n");
                return 1;
            }
            std::memcpy(sa.sun_path, cfg.unixsocket, std::strlen(cfg.unixsocket) + 1);
            const int probe_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe_fd < 0) { std::perror("socket unixsocket probe"); return 1; }
            if (::connect(probe_fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) {
                ::close(probe_fd);
                std::fprintf(stderr, "unixsocket path '%s' is already accepting connections\n",
                             cfg.unixsocket);
                return 1;
            }
            const int connect_error = errno;
            ::close(probe_fd);
            if (connect_error != ECONNREFUSED && connect_error != ENOENT) {
                errno = connect_error;
                std::perror("connect unixsocket probe");
                return 1;
            }
            if (::unlink(cfg.unixsocket) != 0) { std::perror("unlink unixsocket"); return 1; }
        } else if (errno != ENOENT) {
            std::perror("stat unixsocket"); return 1;
        }
        unix_listener = IoLoop::make_unix_listener(cfg.unixsocket, srv.cfg().tcp_backlog);
        if (unix_listener < 0) { std::perror("bind unixsocket"); return 1; }
    }

    srv.topo().dump(stdout);
    const char* mname = "2s (io sends)";
    std::printf("tomokv-cpp: %u threads (%zu io + %zu ex), %u shard(s), %s,"
                " %s, alloc=%s\n", srv.nthreads(),
                srv.placement().ifid_threads().size(), srv.placement().ex_threads().size(),
                cfg.shards, mname,
                cfg.net_io == NetIoEngine::Epoll ? "epoll" : "io_uring", alloc_backend());
    for (const ThreadPlacement& p : srv.placement().threads()) {
        const char* role = p.role == Role::Ifid ? "ifid" : p.role == Role::Ex ? "ex" : "wb";
        std::printf("  thread t%u: role=%s cpu=%d L3=%u shards=%zu send=", p.id, role, p.cpu,
                    p.domain, srv.thread(p.id).shards().size());
        std::printf("self\n");
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
    for (uint32_t i = 0; i < nthreads; i++) g_threads.push_back(&srv.thread(i));

    auto pin_for = [&](uint32_t tid) {
        if (cfg.pin_threads) pin_to(srv.placement().cpu_of_thread(tid));
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
            bool ok = exs[tid].init(&srv, &self);
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
            exs[tid].run();
        });

    // Main performed every read(2); the real owning executor threads now deserialize their own
    // shard sections in parallel.  No listener exists until all owners report success.
    {
        std::unique_lock<std::mutex> lock(load_mu);
        load_cv.wait(lock, [&] {
            return loaders_done == static_cast<uint32_t>(srv.placement().ex_threads().size());
        });
    }
    if (!load_ok) {
        for (uint32_t i = 0; i < nthreads; i++) srv.thread(i).stop_flag().store(true);
        for (auto& thread : pool) thread.join();
        std::fprintf(stderr, "persistence load failed: %s\n", load_error.c_str());
        return 1;
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

    const uint32_t unix_owner = srv.placement().ifid_threads().front();
    for (uint32_t tid : srv.placement().ifid_threads())
        pool.emplace_back([&, tid] {
            pin_for(tid);
            ThreadCtx& self = srv.thread(tid);
            self.latch_placement(srv.topo());
            const int unix_fd = tid == unix_owner ? unix_listener : -1;
            if (!ios[tid].init(&srv, &self, cfg.bind_addr, cfg.port, unix_fd,
                               tls_context.get())) return;
            ios[tid].run();
        });

    if (cfg.port) std::printf("listening on %s:%u\n", cfg.bind_addr, cfg.port);
    if (cfg.tls_port) std::printf("listening with TLS on %s:%u\n", cfg.bind_addr, cfg.tls_port);
    if (unix_listener >= 0) std::printf("listening on unix:%s\n", cfg.unixsocket);
    std::fflush(stdout);

    for (auto& t : pool) t.join();
    if (cfg.unixsocket && *cfg.unixsocket) ::unlink(cfg.unixsocket);

    // All owners and readers are quiescent. Release pending-entry references before IoLoop destruction,
    // then return their deferred ScatterState arenas to the correct IO-owned pools. Server normally
    // outlives those pools, so leaving this to FlatStore destructors would leak the retained arenas.
    for (uint32_t sid = 0; sid < srv.nshards(); sid++)
        srv.shard(static_cast<int32_t>(sid)).store().atomic_shutdown_release_records();
    for (IoLoop& io : ios) io.reap_atomic_deferred();

    // One line of accounting on the way out. Cheap, and the absence of it is how a run ends with no
    // evidence of what it did.
    uint64_t ops = 0, disp = 0;
    for (uint32_t i = 0; i < srv.nthreads(); i++) {
        const LoopSignals& s = srv.thread(i).sig();
        (srv.thread(i).role() == Role::Ifid ? disp : ops) += s.ops;
    }
    uint64_t acc = 0, aerr = 0, arearm = 0, starved = 0, ndrop = 0;
    for (uint32_t i = 0; i < srv.nthreads(); i++) {
        const LoopSignals& s = srv.thread(i).sig();
        acc += s.accepts; aerr += s.accept_err; arearm += s.accept_rearm; starved += s.sqe_starved;
        ndrop += s.notify_drop;
    }
    // Per-thread breakdown. The aggregate hides the thing you actually need: whether a stage is
    // saturated, starved, or spending its life in the kernel waiting to be told there is work.
    std::printf("\n%-6s %-4s %12s %10s %9s %9s %9s %9s %8s\n",
                "thread","role","ops","iters","busy_ms","idle_ms","cpu_ms","wake_tx","wake_rx");
    for (uint32_t i = 0; i < srv.nthreads(); i++) {
        const LoopSignals& s = srv.thread(i).sig();
        const Role r = srv.thread(i).role();
        std::printf("t%-5u %-4s %12llu %10llu %9.1f %9.1f %9.1f %9llu %8llu\n", i,
                    r == Role::Ifid ? "io" : "ex",
                    (unsigned long long)s.ops, (unsigned long long)s.iterations,
                    s.busy_ns / 1e6, s.idle_ns / 1e6, s.cpu_ns / 1e6,
                    (unsigned long long)s.wakes_sent, (unsigned long long)s.wakes_recv);
    }
    // WHERE DID THE REPLIES GO. dispatched==executed only proves the STORE finished its work; it
    // says nothing about whether the answer reached the socket. These three levels localise a stall
    // to one hop: retired < executed means replies are stranded in the ROB (the sender was never
    // told). retired == executed with bytes_sent short means they are staged but unsent (the pump
    // was never re-triggered). Both looked identical from outside before this existed.
    WbEngine::Stats w{};
    auto addw = [&](const WbEngine::Stats& x) {
        w.sends_submitted += x.sends_submitted; w.sends_completed += x.sends_completed;
        w.short_writes    += x.short_writes;    w.send_errors     += x.send_errors;
        w.peer_aborts     += x.peer_aborts;
        w.bytes_sent      += x.bytes_sent;      w.retired         += x.retired;
        w.direct          += x.direct;
        w.zc_sends        += x.zc_sends;        w.zc_bytes        += x.zc_bytes;
        w.zc_releases     += x.zc_releases;
        w.serves          += x.serves;          w.serves_empty    += x.serves_empty;
    };
    for (uint32_t i = 0; i < srv.nthreads(); i++) {
        addw(ios[i].engine().stats()); addw(exs[i].engine().stats());
    }
    uint64_t tls_accepts = 0, tls_started = 0, tls_completed = 0, tls_failed = 0,
             tls_freed = 0, tls_want_read = 0, tls_want_write = 0,
             tls_cipher_in = 0, tls_plain_in = 0, tls_cipher_out = 0,
             tls_plain_out = 0, tls_zc_suppressed = 0, tls_ktls_active = 0,
             tls_ktls_fallback = 0;
    for (uint32_t i = 0; i < srv.nthreads(); i++) {
        const LoopSignals& s = srv.thread(i).sig();
        tls_accepts += s.tls_accepts;
        tls_started += s.tls_handshakes_started;
        tls_completed += s.tls_handshakes_completed;
        tls_failed += s.tls_handshakes_failed;
        tls_freed += s.tls_connections_freed;
        tls_want_read += s.tls_want_read;
        tls_want_write += s.tls_want_write;
        tls_cipher_in += s.tls_ciphertext_input_bytes;
        tls_plain_in += s.tls_plaintext_input_bytes;
        tls_cipher_out += s.tls_ciphertext_output_bytes;
        tls_plain_out += s.tls_plaintext_output_bytes;
        tls_zc_suppressed += s.tls_zc_suppressed;
        tls_ktls_active += s.tls_ktls_active;
        tls_ktls_fallback += s.tls_ktls_fallback;
    }
    // And the smoking gun: connections still holding work at shutdown, by WHICH kind.
    uint64_t stuck_rob = 0, stuck_wr = 0, live = 0;
    uint64_t st_done = 0, st_issued = 0, st_free = 0, st_flag = 0;
    for (uint32_t i = 0; i < srv.nthreads(); i++)
        for (Client* c : srv.thread(i).clients()) {
            if (!c) continue;
            live++;
            if (!c->rob().quiesced()) {
                stuck_rob++;
                // THE DEDUP FLAG ON A STRANDED CLIENT. retire_queued is the whole notification
                // protocol: a worker claims the client by CASing it false->true and then posts it to
                // the sender, and the sender clears it before serving. So on a client whose replies
                // are Done and unretired there are exactly two stories, and this bit tells them apart:
                //   true  -> someone claimed it and the post never took effect (claim leaked)
                //   false -> nobody was holding a claim, so the notification was simply never made
                if (c->retire_queued().load(std::memory_order_acquire)) st_flag++;
#ifdef TOMO_WEDGE_FORENSICS
                std::printf("  stranded conn: claims=%u defers=%u serves=%u inflight=%u flag=%d\n",
                            c->n_claims.load(std::memory_order_relaxed),
                            c->n_defers.load(std::memory_order_relaxed),
                            c->n_serves.load(std::memory_order_relaxed),
                            c->rob().in_flight(),
                            (int)c->retire_queued().load(std::memory_order_acquire));
#endif
                // WHICH KIND OF STRANDED. The counts above prove ops were dispatched and never
                // retired; they cannot say why. The state of each un-retired slot does:
                //   Done   -> it executed and the sender was never told  (a lost-notification bug)
                //   Issued -> it never executed at all                   (a lost-dispatch bug)
                // Those need opposite fixes, so guessing between them is how you fix the wrong one.
                for (uint64_t i = c->rob().flush_id(), d = c->rob().dispatch_id(); i != d; i++) {
                    switch (c->rob().at(i).state.load(std::memory_order_acquire)) {
                        case OpState::Done:   st_done++;   break;
                        case OpState::Issued: st_issued++; break;
                        default:              st_free++;   break;
                    }
                }
            }
            if (!c->nothing_to_write()) stuck_wr++;        }
    std::printf("wb: retired=%llu direct=%llu sends=%llu/%llu short=%llu err=%llu"
                " peer_aborts=%llu bytes=%llu"
                " zc_sends=%llu zc_bytes=%llu zc_releases=%llu serves=%llu empty=%llu\n",
                (unsigned long long)w.retired, (unsigned long long)w.direct, (unsigned long long)w.sends_completed,
                (unsigned long long)w.sends_submitted, (unsigned long long)w.short_writes,
                (unsigned long long)w.send_errors, (unsigned long long)w.peer_aborts,
                (unsigned long long)w.bytes_sent,
                (unsigned long long)w.zc_sends, (unsigned long long)w.zc_bytes,
                (unsigned long long)w.zc_releases,
                (unsigned long long)w.serves, (unsigned long long)w.serves_empty);
    std::printf("tls: accepts=%llu handshakes=%llu/%llu failed=%llu freed=%llu"
                " want_read=%llu want_write=%llu cipher_in=%llu plain_in=%llu"
                " cipher_out=%llu plain_out=%llu zc_suppressed=%llu"
                " ktls_active=%llu ktls_fallback=%llu\n",
                (unsigned long long)tls_accepts, (unsigned long long)tls_completed,
                (unsigned long long)tls_started, (unsigned long long)tls_failed,
                (unsigned long long)tls_freed, (unsigned long long)tls_want_read,
                (unsigned long long)tls_want_write, (unsigned long long)tls_cipher_in,
                (unsigned long long)tls_plain_in, (unsigned long long)tls_cipher_out,
                (unsigned long long)tls_plain_out, (unsigned long long)tls_zc_suppressed,
                (unsigned long long)tls_ktls_active, (unsigned long long)tls_ktls_fallback);
    std::printf("stuck: live_conns=%llu rob_not_quiesced=%llu unsent_bytes_pending=%llu"
                " | slots done=%llu issued=%llu free=%llu flag_set=%llu\n",
                (unsigned long long)live, (unsigned long long)stuck_rob, (unsigned long long)stuck_wr,
                (unsigned long long)st_done, (unsigned long long)st_issued, (unsigned long long)st_free,
                (unsigned long long)st_flag);
    if (cfg.net_io == NetIoEngine::Epoll) {
        uint64_t epoll_events = 0, epoll_recvs = 0;
        for (uint32_t i = 0; i < srv.nthreads(); i++) {
            epoll_events += srv.thread(i).sig().epoll_events;
            epoll_recvs += srv.thread(i).sig().epoll_recvs;
        }
        std::printf("epoll: events=%llu recvs=%llu\n",
                    (unsigned long long)epoll_events, (unsigned long long)epoll_recvs);
    }
    std::printf("shutdown: dispatched=%llu executed=%llu accepts=%llu accept_err=%llu "
                "rearm=%llu sqe_starved=%llu notify_drop=%llu\n",
                static_cast<unsigned long long>(disp), static_cast<unsigned long long>(ops),
                static_cast<unsigned long long>(acc), static_cast<unsigned long long>(aerr),
                static_cast<unsigned long long>(arearm), static_cast<unsigned long long>(starved),
                static_cast<unsigned long long>(ndrop));
    acl_shutdown();
    return 0;
}
