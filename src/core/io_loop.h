// io_loop.h — accepts, receives, parses, routes, publishes, retires, and sends for its clients.
// In fused mode the same physical thread also executes its owned shards; split mode hands tasks
// to executor threads. Reply retirement and sending stay with the connection's IO owner.
//
//   out  task_in of the shard's owner        a parsed op to execute
//   in   ready mask / client_in             completed ops to retire and send
#pragma once
#include <array>
#include <deque>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "server.h"
#include "iopipe_pipeline.h"
#include "signal.h"
#include "ex_loop.h"
#include "genthread_pipeline.h"
#include "../net/conn.h"
#include "../net/resp.h"
#include "../net/uring.h"
#include "../net/epoll.h"
#include "../net/wb.h"
#include "../net/tls.h"
#include "../cmd/command.h"
#include "../cmd/debug_sleep.h"
#include "../cmd/slowlog.h"
#include "../cmd/blocking.h"
#include "../cmd/auth.h"
#include "../cmd/acl.h"
#include "../cmd/multi.h"
#include "../cmd/notify.h"
#include "../cmd/server_tail.h"
#include "../cmd/xshard.h"
#include "../snapshot/snapshot.h"
#include "../persist/aof.h"

namespace tomo {

inline constexpr uint32_t kRecvChunk = 16 * 1024;

// IO and EX responsibilities compose on one physical thread in fused mode:
//
//   io    IoLoop    recv+parse+retire+send; owns the whole client
//   ex    ExLoop    execute+notify; never sends

class IoLoop {
    friend struct CoreConcurrencyTest;
public:
    WbEngine& engine() { return wb_; }
    uint32_t reap_atomic_deferred() {
        return scatter_pool_.reap_deferred() + multi_owner_reap_entry(*this);
    }

    // ONE LISTENING SOCKET PER IO THREAD, via SO_REUSEPORT.
    //
    // Sharing a single listen fd across io threads does NOT distribute connections: every thread
    // arms a multishot accept on it and the kernel satisfies them all from one ring. Measured
    // consequence with 6 io threads and 577 connections — t5 took every single one and t0..t4 sat
    // idle for the entire run, so the server was really running on one io thread. It looked like a
    // latency problem (uniform ~3.5 ms at p1) and was actually a distribution problem.
    //
    // With SO_REUSEPORT the kernel hashes each incoming connection to one of the listening sockets,
    // which spreads them across threads without any userspace handoff. Note this is safe WITHIN one
    // process; two SERVER PROCESSES sharing a port is the failure mode that once faked data loss,
    // so a boot must still verify nothing else holds the port.
    bool init(Server* srv, ThreadCtx* self, const char* addr, uint16_t port,
              int unix_listen_fd = -1, const TlsContext* tls_context = nullptr,
              bool dormant = false) {
        srv_ = srv; self_ = self;
        (void)addr;
        configured_port_ = port;
        tls_context_ = tls_context;
        unix_listen_fd_ = unix_listen_fd;
        epoll_ = srv_->cfg().net_io == NetIoEngine::Epoll;
        targeted_ifid_ = srv_->cfg().thread_mode == ThreadMode::Fused &&
                         srv_->cfg().overlap != 0;
        age_sample_rate_cached_ = srv_->effective_age_sample_rate();
        age_signals_armed_ = age_sample_rate_cached_ != 0;
        client_lb_signal_armed_ = srv_->lb_machinery_enabled();
        lb_controller_armed_ = srv_->lb_machinery_enabled();
        if (!ring_.init(4096)) return false;
        if (epoll_ && !init_epoll()) return false;
        wb_.bind(&ring_, this, [](void* ctx, int32_t shard, const char* ptr) {
            static_cast<IoLoop*>(ctx)->queue_borrow_release(shard, ptr);
        }, this, [](void* ctx, Client& client, Op& op) {
            auto* loop = static_cast<IoLoop*>(ctx);
            const bool was_blocked = client.blocked();
            NotifyBatch* notifications = nullptr;
            if (op.has_scatter_state()) {
                notifications = notify_take_batch(op);
                xshard_retire(*loop->srv_, *loop->self_, loop->ring_, client, op,
                    loop->scatter_pool_, loop->self_->id(), loop,
                    [](void* release_ctx, int32_t shard, const char* ptr) {
                        static_cast<IoLoop*>(release_ctx)->queue_borrow_release(shard, ptr);
                    }, [](void* release_ctx) {
                        static_cast<IoLoop*>(release_ctx)->wb_.note_zc_suppressed_tls();
                    });
            } else if (op.has_blocking_state()) {
                notifications = notify_take_batch(op);
                blocking_retire(*loop->srv_, client, op, *loop->self_);
            } else if (op.has_multi_state()) {
                notifications = notify_take_batch(op);
                multi_retire_entry(*loop, client, op);
            } else if (__builtin_expect(op.has_notify_state(), false)) {
                notifications = notify_take_batch(op);
            }
            if (__builtin_expect(notifications != nullptr, false))
                notify_retire_batch_entry(*loop, notifications, client.id());
            if (was_blocked && !client.blocked()) {
                // A blocking wait is timeout-exempt, so its parked duration cannot become the
                // idle age the first cron pass sees after retirement. The socket send refresh is
                // too late: cron runs later in this same IO pass, before its CQE can arrive.
                client.set_last_interaction_s(loop->cached_now_s_);
                // One-shot test synchronizer: make that vulnerable retirement -> cron ordering
                // deterministic. Production never arms it, and the blocking path is already cold.
                if (loop->srv_->debug_blocking_timeout_reap_take())
                    loop->client_cron_beat_ms_ = 0;
            }
        }, srv_->client_obuf_armed_ptr(), this, [](void* ctx, Client& client) {
            return static_cast<IoLoop*>(ctx)->client_obuf_check(&client, true);
        }, &cached_now_s_, &self_->sig());
        wb_.set_cold_send_classification(
            srv_->cfg().thread_mode == ThreadMode::Fused &&
            srv_->cfg().overlap != 0);
        initialized_ = true;
        return dormant || activate();
    }

    ~IoLoop() {
        if (self_) pubsub_shutdown_events();
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (tls_listen_fd_ >= 0) ::close(tls_listen_fd_);
        if (unix_listen_fd_ >= 0) ::close(unix_listen_fd_);
        for (Client* c : pending_handoffs_) {
            ::close(c->fd());
            srv_->client_released();
            delete c;
        }
        multi_shutdown_entry(*this);
    }

    static int make_reuseport_listener(const char* addr, uint16_t port, uint32_t backlog = 511,
                                       bool defer_accept = false) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
        if (defer_accept) setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &one, sizeof(one));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(port);
        if (::inet_pton(AF_INET, addr, &sa.sin_addr) != 1) { ::close(fd); return -1; }
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) { ::close(fd); return -1; }
        if (::listen(fd, static_cast<int>(backlog)) != 0) { ::close(fd); return -1; }
        return fd;
    }

    Ring& ring() { return ring_; }

    // Boot-only listener transfer. Persistence loading and IoLoop allocation/init can therefore
    // complete before the AF_UNIX pathname starts accepting. The caller retains fd ownership on
    // failure and releases it only after this method succeeds.
    bool attach_listener(int fd) {
        if (fd < 0 || !initialized_ || active_role_ || prepared_role_ || unix_listen_fd_ >= 0)
            return false;
        unix_listen_fd_ = fd;
        return true;
    }

    // Epoll can allocate kernel registration state. Reserve the actual destination registration
    // while the source still owns the connection; destination readiness events are owner-checked
    // and ignored until commit. io_uring has no per-fd destination registration to reserve.
    bool prepare_client_registration(Client* client) {
        return !epoll_ || ep_.add(client->fd(), EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
                                  ur_tag(UrKind::Recv, client));
    }
    void cancel_client_registration(Client* client) {
        if (epoll_ && !ep_.del(client->fd())) std::abort();
    }

    bool activate() {
        if (!initialized_) return false;
        if (active_role_) {
            self_->set_ring(&ring_);
            self_->set_wb_engine(&wb_);
            return true;
        }
        if (!prepare_activation()) return false;
        accept_quiescing_ = false;
        accept_cancel_submitted_ = tls_accept_cancel_submitted_ = false;
        self_->set_ring(&ring_);
        self_->set_wb_engine(&wb_);
        iopipe_depth_gate_.reset(self_->sig().ops);
        active_role_ = true;
        return true;
    }

    // Prepare every fallible role-tenure resource while the thread is still an EX owner and no
    // connection or bucket ownership edge has occurred. ExLoop invokes this through ThreadCtx's
    // type-erased role hook on the physical thread that owns this IoLoop.
    bool prepare_activation() {
        if (!initialized_) return false;
        if (prepared_role_) return true;
        auto fail = [&]() {
            if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
            if (tls_listen_fd_ >= 0) { ::close(tls_listen_fd_); tls_listen_fd_ = -1; }
            prepared_role_ = false;
            return false;
        };
        accept_generation_++;
        if (configured_port_ && listen_fd_ < 0) {
            listen_fd_ = make_reuseport_listener(
                srv_->cfg().bind_addr, configured_port_, srv_->cfg().tcp_backlog);
            if (listen_fd_ < 0) return fail();
        }
        if (tls_context_ && tls_listen_fd_ < 0) {
            tls_listen_fd_ = make_reuseport_listener(
                srv_->cfg().bind_addr, srv_->cfg().tls_port, srv_->cfg().tcp_backlog, true);
            if (tls_listen_fd_ < 0) return fail();
        }
        if (epoll_ && !register_epoll_listeners()) return fail();
        if (srv_->aof().configured() && !aof_bound_) {
            std::string error;
            if (!srv_->aof().bind_writer(*self_, ring_, error)) {
                std::fprintf(stderr, "AOF writer init failed: %s\n", error.c_str());
                return fail();
            }
            aof_bound_ = true;
        }
        prepared_role_ = true;
        return true;
    }

    void cancel_prepared_activation() {
        if (active_role_ || !prepared_role_) return;
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        if (tls_listen_fd_ >= 0) { ::close(tls_listen_fd_); tls_listen_fd_ = -1; }
        accept_generation_++;
        prepared_role_ = false;
    }

    void deactivate() {
        if (!active_role_) return;
        if (!self_->clients().empty() || !client_migrations_.empty()) std::abort();
        // Channel homes are members of the live IO set. A thread leaving that set retires its
        // owner-local indexes here; every new/surviving IO rebuilds the next epoch in RoleReady.
        pubsub_clear_home_indexes();
        // The UNIX listener is pinned to its boot IO and that thread is never a conversion
        // candidate. TCP/TLS SO_REUSEPORT listeners are per-role-tenure resources.
        if (unix_listen_fd_ >= 0) std::abort();
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        if (tls_listen_fd_ >= 0) { ::close(tls_listen_fd_); tls_listen_fd_ = -1; }
        accept_generation_++; // makes any late multishot CQE recognisably stale
        accept_cancel_submitted_ = tls_accept_cancel_submitted_ = false;
        active_role_ = false;
        prepared_role_ = false;
    }

    // Called only on this loop's physical thread by the migration control lane. The public split
    // keeps preflight (no state change) distinct from request (source remains owner until an async
    // recv cancellation has acknowledged that the old ring released Client*).
    bool client_transfer_ready(Client* client, uint32_t destination, std::string& error) const {
        if (!client || destination >= srv_->nthreads() || destination == self_->id()) {
            error = "invalid client transfer owner";
            return false;
        }
        if (client->ifid_thread() != self_->id() || client->dead() || client->closing()) {
            error = "connection is not live on the source IO thread";
            return false;
        }
        if (client_pipeline_referenced(client)) {
            error = "connection is held by a generalized-thread pipeline batch";
            return false;
        }
        uint32_t directory_owner = UINT32_MAX;
        if (!command_client_directory_find(client->id(), directory_owner) ||
            directory_owner != self_->id()) {
            error = "connection directory is not owned by the source IO thread";
            return false;
        }
        if (client->is_tls()) {
            error = "TLS connection has owner-local engine state";
            return false;
        }
        if (!client->migration_protocol_idle()) {
            error = "connection ROB, reply, borrow, or owner-local protocol state is busy";
            return false;
        }
        if (!wb_.migration_ready(*client)) {
            error = "connection has a deferred out-of-band frame";
            return false;
        }
        if (!climon_migration_ready(client)) {
            error = "connection has transient CLIENT/MONITOR/TRACKING state";
            return false;
        }
        if (!client_executor_quiesced(client) || !client->migration_protocol_idle()) {
            error = "connection has an unfinished executor completion";
            return false;
        }
        return true;
    }

    bool prepare_client_transfer_capacity(uint32_t incoming) {
        try {
            self_->clients().reserve(self_->clients().size() + incoming);
            active_.v.reserve(active_.v.size() + incoming);
            pubsub_local_.reserve(pubsub_local_.size() + incoming);
            climon_conn_.reserve(climon_conn_.size() + incoming);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return self_->reserve_wb_slots(incoming) && command_client_migration_reserve(incoming);
    }

    bool request_client_transfer(Client* client, uint32_t destination, std::string& error) {
        return epoll_ ? request_client_transfer_impl<true>(client, destination, false, error)
                      : request_client_transfer_impl<false>(client, destination, false, error);
    }

    bool prepare_client_transfer(Client* client, uint32_t destination, std::string& error) {
        return epoll_ ? request_client_transfer_impl<true>(client, destination, true, error)
                      : request_client_transfer_impl<false>(client, destination, true, error);
    }

    bool reserve_client_transfer_state(uint32_t count) {
        try {
            client_migrations_.reserve(client_migrations_.size() + count);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool client_transfers_idle() const { return client_migrations_.empty(); }
    bool client_transfers_prepared() const {
        for (const auto& migration : client_migrations_)
            if (!migration.prepared) return false;
        return true;
    }
    void commit_prepared_client_transfers() {
        if (epoll_) commit_prepared_client_transfers_impl<true>();
        else        commit_prepared_client_transfers_impl<false>();
    }
    void cancel_prepared_client_transfers() {
        if (epoll_) cancel_prepared_client_transfers_impl<true>();
        else        cancel_prepared_client_transfers_impl<false>();
    }
    uint64_t client_transfer_failures() const { return client_transfer_failures_; }

    // ---- epoll engine: registration -----------------------------------------------------------
    // Everything that will ever be waited on is registered ONCE here. Listeners are
    // level-triggered on purpose: a listener that we stop accepting from (maxclients reached, a
    // failed Client allocation) must keep telling us there is a backlog, and an edge we consumed
    // and could not act on would be gone. Connections are edge-triggered for the opposite reason --
    // see net/epoll.h. The doorbell eventfd is level-triggered and drained explicitly.
    bool init_epoll() {
        if (!ep_.init()) return false;
        // The cross-thread doorbell exists for the lifetime of the physical thread. Role-tenure
        // listeners are added by activate() and removed by close() in deactivate().
        if (ring_.wake_fd() < 0) return false;
        if (!ep_.add(ring_.wake_fd(), EPOLLIN, ur_tag(UrKind::Wake, nullptr))) return false;
        return ring_.shutdown_fd() < 0 ||
               ep_.add(ring_.shutdown_fd(), EPOLLIN, ur_tag(UrKind::Shutdown, nullptr));
    }

    bool register_epoll_listeners() {
        auto add_listener = [&](int fd, UrKind kind) {
            if (fd < 0) return true;
            if (!set_nonblocking(fd)) return false;
            return ep_.add(fd, EPOLLIN, ur_tag(kind, nullptr));
        };
        if (!add_listener(listen_fd_, UrKind::Accept)) return false;
        if (!add_listener(tls_listen_fd_, UrKind::TlsAccept)) return false;
        if (!add_listener(unix_listen_fd_, UrKind::UnixAccept)) return false;
        return true;
    }

    // THE ENGINE DECISION, MADE ONCE, HERE. It joins the two shape decisions this loop already
    // resolved by instantiation (has a unix listener / has a TLS listener) rather than adding a
    // third kind of runtime state. Everything below is `if constexpr` on kEp, so the uring build
    // contains no epoll code and no test for it, and the epoll build contains no ring code and no
    // test for it. This is the same compile-or-boot-time-variant pattern the registry uses for
    // keyspace notifications (handler_notify in cmd/command.h: two instantiations of the handler,
    // one pointer chosen off a per-batch flag, zero per-operation branching) applied at the loop
    // level instead of the handler level -- the engine is a property of the whole loop, so the
    // choice belongs at its outermost frame.
    template <uint8_t Pipeline>
    void run_split() {
        const bool has_unix = unix_listen_fd_ >= 0 ||
                              (srv_->cfg().unixsocket && *srv_->cfg().unixsocket);
        if (epoll_) {
            if (tls_context_) {
                if (has_unix) run_loop<true, true, true, false, Pipeline>();
                else run_loop<false, true, true, false, Pipeline>();
            } else {
                if (has_unix) run_loop<true, false, true, false, Pipeline>();
                else run_loop<false, false, true, false, Pipeline>();
            }
            return;
        }
        if (tls_context_) {
            if (has_unix) run_loop<true, true, false, false, Pipeline>();
            else run_loop<false, true, false, false, Pipeline>();
        } else {
            if (has_unix) run_loop<true, false, false, false, Pipeline>();
            else run_loop<false, false, false, false, Pipeline>();
        }
    }

    void run() {
        if (srv_->cfg().overlap == 1) run_split<1>();
        else                                  run_split<0>();
    }

    void bind_fused_executor(FusedExLoop* executor) {
        fused_executor_ = executor;
    }

    template <bool TargetedIfid>
    void fused_executor_completion(Client* client) {
        if (!client || client->dead()) return;
        enqueue_serve(client);
        mark_active_known<TargetedIfid>(client);
    }

    void run_fused();

private:
    void refresh_age_sampling() {
        const uint32_t wanted = srv_->effective_age_sample_rate();
        if (wanted == age_sample_rate_cached_) return;
        age_sample_rate_cached_ = wanted;
        age_signals_armed_ = wanted != 0;
        self_->sig().configure_age_sampling(wanted);
        if (!wanted) {
            rob_head_ages_.clear();
            rob_head_ages_.rehash(0);
        }
    }

    enum class DispatchResult : uint8_t {
        Progress,
        NeedInput,
        Error,
        Closed,
    };

    struct WbPipelineBatch {
        std::array<Client*, kGenthreadPipelineWbBatchConns> clients{};
        uint32_t count = 0;
    };

    struct ClientMigration {
        Client* client = nullptr;
        uint32_t destination = 0;
        bool cancel_submitted = false;
        bool hold_for_commit = false;
        bool prepared = false;
        bool destination_registered = false;
        int source_backup_fd = -1;
        void* catalog = nullptr;
        void* routing = nullptr;
    };

    friend bool multi_dispatch_entry(IoLoop&, Client&, Op&, uint32_t);
    friend bool multi_dispatch_entry_iofused(IoLoop&, Client&, Op&, uint32_t);
    template <bool IofusedPrivateQueue>
    friend bool multi_dispatch_entry_impl(IoLoop&, Client&, Op&, uint32_t);
    friend bool auth_dispatch_entry(IoLoop&, Client&, Op&, uint32_t);
    friend bool acl_dispatch_entry(IoLoop&, Client&, Op&, uint32_t, uint8_t);
    friend bool acl_finish_dispatch_denial(IoLoop&, Client&, Op&, uint32_t,
                                           AclDeniedReason, uint32_t);
    friend void acl_command_entry(IoLoop&, Client&, Op&);
    friend void acl_broadcast_user_change(IoLoop&, uint32_t, const AclPerm*, bool);
    friend void multi_retire_entry(IoLoop&, Client&, Op&);
    friend uint32_t multi_owner_pass_entry(IoLoop&);
    friend uint32_t multi_owner_pass_entry_iofused(IoLoop&);
    template <bool IofusedPrivateQueue>
    friend uint32_t multi_owner_pass_entry_impl(IoLoop&);
    friend uint32_t multi_owner_reap_entry(IoLoop&);
    friend void multi_close_entry(IoLoop&, Client&);
    friend void multi_shutdown_entry(IoLoop&);
    friend void notify_retire_batch_entry(IoLoop&, NotifyBatch*, uint64_t);
#include "pubsub.inc"

    template <bool HasUnix, bool HasTls, bool kEp, bool Fused = false,
              uint8_t Pipeline = 0>
    void run_loop() {
        static_assert(Pipeline <= 2);
        if constexpr (Fused && Pipeline == 1) {
            run_fused_iofused_loop<HasUnix, HasTls, kEp, false>();
            return;
        }
        if constexpr (Fused && Pipeline == 2) {
            // Overlap 2 is the gated three-way extension of iofused.
            run_fused_iofused_loop<HasUnix, HasTls, kEp, true>();
            return;
        }
        constexpr bool IoPipe = !Fused && Pipeline == 1;
        if constexpr (!kEp) {
            if (listen_fd_ >= 0) arm_accept(UrKind::Accept);
            if constexpr (HasTls) arm_accept(UrKind::TlsAccept);
            if constexpr (HasUnix) if (unix_listen_fd_ >= 0) arm_accept(UrKind::UnixAccept);
        }
        LoopSignals& sig = self_->sig();
        while (!self_->stop_flag().load(std::memory_order_relaxed) &&
               self_->role() == Role::Ifid) {
            refresh_notify_config();
            // ONE relaxed load per io batch. Per-batch checks are free; this is what buys the
            // per-operation hooks their zero-cost-when-off property.
            if (__builtin_expect(srv_->climon_armed() != climon_armed_cached_, false))
                climon_refresh_armed();
            const bool pause_armed = climon_pause_armed();
            const bool client_cron_armed = !srv_->flip_dispatch_paused() &&
                                           srv_->client_cron_armed();
            const bool client_lb_signal_armed = client_lb_signal_armed_;
            const bool lb_controller_armed = lb_controller_armed_;
            // Placement's dense role vectors are mutated only under FLIP's global dispatch
            // barrier. Do not consult them from an IO pass while that cold transaction is live.
            const bool save_cron_armed = !srv_->flip_dispatch_paused() &&
                                         srv_->save_cron_writer(self_->id());
            const bool client_cron_newly_armed = client_cron_armed && !client_cron_was_armed_;
            if (!client_cron_armed && __builtin_expect(client_cron_was_armed_, false)) {
                // Turning the last client cron consumer off also retires output accounting once.
                // The disabled write-back specialization then has no per-serve cleanup branch.
                for (Client* c : self_->clients()) c->stop_obuf_tracking();
            }
            client_cron_was_armed_ = client_cron_armed;
            sig.iterations++;
            reap_dead();               // free clients dead for a full iteration -- see close_client
            scatter_pool_.reap_deferred();

            uint32_t did = 0;
            bool submitted = false;
            bool natural_order = false;
            if constexpr (IoPipe)
                natural_order = iopipe_depth_gate_.loop_boundary(sig.ops);
            {
                Span busy(sig.busy_ns);
                // The work-span clock is already sampled once for this pass. Reuse that cut for
                // every monotonic millisecond consumer instead of issuing separate clock_gettime
                // reads for pause, cron and WAIT. A pass is microseconds; their public granularity
                // is milliseconds or seconds.
                bool pass_time_cached = pause_armed || client_cron_armed || save_cron_armed ||
                                        client_lb_signal_armed || lb_controller_armed ||
                                        !deferred_timers_.empty();
                if (__builtin_expect(pass_time_cached, true)) {
                    cached_now_ms_ = busy.start_ns() / 1000000ull;
                    cached_now_s_ = static_cast<uint32_t>(cached_now_ms_ / 1000);
                }
                if (__builtin_expect(pause_armed &&
                                     cached_now_ms_ >= climon_pause_deadline_ms_, false))
                    climon_release_pause();
                if (client_cron_newly_armed) {
                    for (Client* c : self_->clients()) c->set_last_interaction_s(cached_now_s_);
                    client_cron_beat_ms_ = cached_now_ms_;
                }
                if (self_->sample_depth(busy.start_ns() / 1000)) {
                    // CLOCK_THREAD_CPUTIME_ID can require a real syscall. cpu_ns is diagnostic
                    // only (the placement controller deliberately uses busy/idle), so sample it
                    // on the existing 100us signal beat instead of every hot pass.
                    sig.cpu_ns = thread_cpu_ns();
                    refresh_age_sampling();
                    if (age_signals_armed_) sample_rob_head_age(sig.cached_now_us);
                }
                // A dropped accept re-arm means the server stops taking connections entirely, so it
                // is retried every pass until it lands.
                if constexpr (!kEp) {
                    if (accept_pending_) arm_accept(UrKind::Accept);
                    if constexpr (HasTls) if (tls_accept_pending_) arm_accept(UrKind::TlsAccept);
                    if constexpr (HasUnix)
                        if (unix_accept_pending_) arm_accept(UrKind::UnixAccept);
                }
                if constexpr (!IoPipe) {
                    // In epoll mode this drains the doorbell mailbox instead of a CQ ring; the tag
                    // stream, and therefore this switch, is identical. See uring.h.
                    did += ring_.for_each_cqe(
                        [&](io_uring_cqe* cqe) {
                            on_cqe<HasTls, kEp, Fused, Pipeline>(cqe);
                        });
                    if constexpr (kEp)
                        did += epoll_pass<HasUnix, HasTls, Fused, Pipeline>(0);
                }
                did += service_client_migrations<kEp>();
                did += drain_client_transfers<kEp>();
                did += scatter_pool_.refresh_snapshot_floor(*srv_, self_->id());
                if constexpr (HasUnix) did += flush_handoffs();
                did += multi_owner_pass_entry(*this);
                if (srv_->aof().writer_is(self_->id()))
                    did += srv_->aof().writer_pass(*self_, ring_);
                if (srv_->snapshot().writer_is(self_->id()))
                    did += srv_->snapshot().writer_pass(*self_, ring_);
                if (__builtin_expect(!deferred_timers_.empty(), false)) {
                    // CQ processing above may have created the first timer after the prologue.
                    if (!pass_time_cached) {
                        cached_now_ms_ = busy.start_ns() / 1000000ull;
                        cached_now_s_ = static_cast<uint32_t>(cached_now_ms_ / 1000);
                        pass_time_cached = true;
                    }
                    did += deferred_timer_pass(cached_now_ms_);
                }
                did += flush_borrow_releases();
                if constexpr (IoPipe) {
                    if (__builtin_expect(!routing_forward_.empty(), false))
                        client_routing_cleanup_pass();
                    did += pipeline_pass<HasUnix, HasTls, kEp>(
                        false, natural_order, submitted);
                } else if constexpr (Fused) {
                    if (__builtin_expect(!routing_forward_.empty(), false))
                        client_routing_cleanup_pass();
                    did += flush_ready<HasTls, kEp, true, HasUnix>();
                } else {
                    did += collect_retire_work<HasUnix, kEp>();
                    if (__builtin_expect(!routing_forward_.empty(), false))
                        client_routing_cleanup_pass();
                    did += flush_ready<HasTls, kEp>();
                }
                did += flip_control_pass<kEp>();
                if (__builtin_expect(client_lb_signal_armed &&
                                     cached_now_ms_ >= lb_client_signal_beat_ms_, false)) {
                    did += lb_client_signal_pass();
                    lb_client_signal_beat_ms_ = cached_now_ms_ + 1000;
                }
                did += lb_control_pass();
                if (__builtin_expect(lb_controller_armed &&
                                     cached_now_ms_ >= lb_controller_beat_ms_, false)) {
                    lb_controller_beat_ms_ = cached_now_ms_ + srv_->lb_tick_ms();
                    if (srv_->lb_cron_writer(self_->id()) &&
                        srv_->lb_controller_tick(self_->id(), cached_now_ms_))
                        lb_schedule_wake_all();
                    did++;
                }
                did += lb_wake_all_pass();
                if (__builtin_expect(client_cron_armed &&
                                     cached_now_ms_ >= client_cron_beat_ms_, false)) {
                    did += client_cron_pass();
                    client_cron_beat_ms_ = cached_now_ms_ + 100;
                }
                if (__builtin_expect(save_cron_armed &&
                                     cached_now_ms_ >= save_cron_beat_ms_, false)) {
                    did += srv_->save_cron_pass(*self_, ring_);
                    save_cron_beat_ms_ = cached_now_ms_ + 1000;
                }
            }
            // Flush prepared SQEs before looping. Recv re-arms and cross-ring wakes are
            // PREPARED during the work section but only reach the kernel on submit; taking
            // the busy path without submitting strands them in the SQ forever, and the peer
            // that is waiting on that wake never runs.
            if (did) {
                if constexpr (IoPipe) {
                    if (!submitted || ring_.sq_ready()) ring_.submit_and_reap();
                } else {
                    ring_.submit_and_reap();
                }
                continue;
            }

            // Nothing to do: declare intent to block, re-check (a producer may have pushed between
            // the last drain and the flag being set), then wait.
            // Mask-independent sweep before parking. The mask is a hint for the hot path; it must
            // not be the only thing that can find queued work, or one lost bit wedges a connection
            // forever. Runs only when this thread has already concluded it has nothing to do.
            uint32_t sweep_work = 0;
            if constexpr (IoPipe)
                sweep_work = pipeline_sweep<HasUnix, HasTls, kEp>(
                    natural_order, submitted);
            else
                sweep_work = sweep<HasUnix, HasTls, kEp, Fused>();
            if (sweep_work) {
                if constexpr (IoPipe) {
                    if (!submitted || ring_.sq_ready()) ring_.submit_and_reap();
                } else {
                    ring_.submit_and_reap();
                }
                continue;
            }

            Span idle(sig.idle_ns);
            if constexpr (Fused) {
                if (__builtin_expect(srv_->read_local_enabled(), false))
                    self_->publish_read_local_parked(srv_->read_local_epoch());
            }
            self_->arm_blocked();
            if constexpr (kEp) {
                // The park. Same 50ms ceiling as the ring wait, and for the same reason: the stop
                // flag is only re-read at the top of the loop, so an unbounded block would make
                // shutdown depend on a connection arriving.
                if constexpr (Fused) {
                    if (!self_->any_fused_inbound())
                        epoll_pass<HasUnix, HasTls, true, Pipeline>(50);
                } else if (!self_->any_io_inbound()) {
                    epoll_pass<HasUnix, HasTls, false, Pipeline>(50);
                }
            } else {
                if constexpr (Fused) {
                    if (!self_->any_fused_inbound()) ring_.submit_and_wait(1);
                    else                            ring_.submit_and_reap();
                } else {
                    if (!self_->any_io_inbound()) ring_.submit_and_wait(1);
                    else                         ring_.submit_and_reap();
                }
            }
            if constexpr (Fused) {
                // Become active conservatively before sampling the epoch. With the retirement RMW
                // and grace scan in the same seq-cst order, a reclaimer either sees this old tick
                // and waits or completed while we were parked; the epoch sample then acquires that
                // unlink before any later foreign slot probe.
                if (__builtin_expect(srv_->read_local_enabled(), false)) {
                    self_->resume_read_local_tick();
                    self_->publish_read_local_tick(srv_->read_local_epoch());
                }
            }
            self_->clear_blocked();
        }
        if constexpr (Fused) {
            // The read loop is over permanently. Teardown below may take longer than another
            // owner's bounded retire queue can tolerate, but it performs no foreign store probe.
            if (srv_->read_local_enabled())
                self_->publish_read_local_parked(srv_->read_local_epoch());
        }
        // A close requested by the last pass's read/send path has no later flush_ready to drain it,
        // and an undrained entry would show up as a live connection in the shutdown accounting.
        if constexpr (kEp) {
            while (!epoll_closes_.empty()) {
                Client* victim = epoll_closes_.back();
                epoll_closes_.pop_back();
                epoll_close_now(victim);
            }
        }
        if (srv_->aof().writer_is(self_->id()))
            srv_->aof().writer_shutdown(*self_, ring_);
        // The normal loop deliberately keeps a dead Client for two prologues so stale channel
        // entries cannot race its delete. At process shutdown all producers have observed the
        // shared stop flag and this IO owner is quiescent; finish those two deterministic grace
        // steps so TlsConn/SSL/BIO ownership is released before shutdown accounting is printed.
        reap_dead();
        reap_dead();
    }

    // The iofused family is a private boot-time instantiation. Neither arm owns streams' unpublished
    // IFID reservations, A/D executor contexts, residual-age gates, or buffered retirement state.
    // Overlap 1 retains the source WB-prefetch -> targeted IFID -> WB -> coarse EX rotation.
    // Overlap 2 selects the gated whole-batch three-way pass below. Both retain the measured
    // SEND-immediate / four-non-SEND N2 handling.
    template <bool HasUnix, bool HasTls, bool kEp, bool ThreeWay>
    void run_fused_iofused_loop() {
        if constexpr (!kEp) {
            if (listen_fd_ >= 0) arm_accept(UrKind::Accept);
            if constexpr (HasTls) arm_accept(UrKind::TlsAccept);
            if constexpr (HasUnix)
                if (unix_listen_fd_ >= 0) arm_accept(UrKind::UnixAccept);
        }
        LoopSignals& sig = self_->sig();
        WbPipelineBatch wb_batch;
        uint32_t non_send_rotations = 0;
        bool three_way_gate_open = false;

        while (!self_->stop_flag().load(std::memory_order_relaxed) &&
               self_->role() == Role::Ifid) {
            refresh_notify_config();
            if (__builtin_expect(srv_->climon_armed() != climon_armed_cached_, false))
                climon_refresh_armed();
            const bool pause_armed = climon_pause_armed();
            const bool client_cron_armed = !srv_->flip_dispatch_paused() &&
                                           srv_->client_cron_armed();
            const bool client_lb_signal_armed = client_lb_signal_armed_;
            const bool lb_controller_armed = lb_controller_armed_;
            const bool save_cron_armed = !srv_->flip_dispatch_paused() &&
                                         srv_->save_cron_writer(self_->id());
            const bool client_cron_newly_armed =
                client_cron_armed && !client_cron_was_armed_;
            if (!client_cron_armed && __builtin_expect(client_cron_was_armed_, false))
                for (Client* c : self_->clients()) c->stop_obuf_tracking();
            client_cron_was_armed_ = client_cron_armed;
            sig.iterations++;
            reap_dead();
            scatter_pool_.reap_deferred();

            uint32_t did = 0;
            {
                Span busy(sig.busy_ns);
                bool pass_time_cached = pause_armed || client_cron_armed || save_cron_armed ||
                                        client_lb_signal_armed || lb_controller_armed ||
                                        !deferred_timers_.empty();
                if (__builtin_expect(pass_time_cached, true)) {
                    cached_now_ms_ = busy.start_ns() / 1000000ull;
                    cached_now_s_ = static_cast<uint32_t>(cached_now_ms_ / 1000);
                }
                if (__builtin_expect(pause_armed &&
                                     cached_now_ms_ >= climon_pause_deadline_ms_, false))
                    climon_release_pause();
                if (client_cron_newly_armed) {
                    for (Client* c : self_->clients())
                        c->set_last_interaction_s(cached_now_s_);
                    client_cron_beat_ms_ = cached_now_ms_;
                }
                if (self_->sample_depth(busy.start_ns() / 1000)) {
                    sig.cpu_ns = thread_cpu_ns();
                    refresh_age_sampling();
                    if (age_signals_armed_) sample_rob_head_age(sig.cached_now_us);
                }
                if constexpr (!kEp) {
                    if (accept_pending_) arm_accept(UrKind::Accept);
                    if constexpr (HasTls)
                        if (tls_accept_pending_) arm_accept(UrKind::TlsAccept);
                    if constexpr (HasUnix)
                        if (unix_accept_pending_) arm_accept(UrKind::UnixAccept);
                }
                did += service_client_migrations<kEp>();
                did += drain_client_transfers<kEp>();
                did += scatter_pool_.refresh_snapshot_floor(*srv_, self_->id());
                if constexpr (HasUnix) did += flush_handoffs();
                did += multi_owner_pass_entry_iofused(*this);
                if (srv_->aof().writer_is(self_->id()))
                    did += srv_->aof().writer_pass(*self_, ring_);
                if (srv_->snapshot().writer_is(self_->id()))
                    did += srv_->snapshot().writer_pass(*self_, ring_);
                if (__builtin_expect(!deferred_timers_.empty(), false)) {
                    if (!pass_time_cached) {
                        cached_now_ms_ = busy.start_ns() / 1000000ull;
                        cached_now_s_ = static_cast<uint32_t>(cached_now_ms_ / 1000);
                        pass_time_cached = true;
                    }
                    did += deferred_timer_pass(cached_now_ms_);
                }
                did += flush_borrow_releases();
                if (__builtin_expect(!routing_forward_.empty(), false))
                    client_routing_cleanup_pass();

                // N0 is the rotation's only completion harvest. Receive parsing and SEND follow-up
                // remain in the explicit IFID/WB body below.
                did += ring_.for_each_cqe([&](io_uring_cqe* cqe) {
                    on_cqe<HasTls, kEp, true, 1>(cqe);
                });
                if constexpr (kEp)
                    did += epoll_pass<HasUnix, HasTls, true, 1>(0);
                if constexpr (ThreeWay)
                    did += genthread_three_way_pass<HasUnix, HasTls, kEp>(
                        wb_batch, three_way_gate_open);
                else
                    did += genthread_iofused_pass<HasUnix, HasTls, kEp>(wb_batch);

                did += flip_control_pass<kEp>();
                if (__builtin_expect(client_lb_signal_armed &&
                                     cached_now_ms_ >= lb_client_signal_beat_ms_, false)) {
                    did += lb_client_signal_pass();
                    lb_client_signal_beat_ms_ = cached_now_ms_ + 1000;
                }
                did += lb_control_pass();
                if (__builtin_expect(lb_controller_armed &&
                                     cached_now_ms_ >= lb_controller_beat_ms_, false)) {
                    lb_controller_beat_ms_ = cached_now_ms_ + srv_->lb_tick_ms();
                    if (srv_->lb_cron_writer(self_->id()) &&
                        srv_->lb_controller_tick(self_->id(), cached_now_ms_))
                        lb_schedule_wake_all();
                    did++;
                }
                did += lb_wake_all_pass();
                if (__builtin_expect(client_cron_armed &&
                                     cached_now_ms_ >= client_cron_beat_ms_, false)) {
                    did += client_cron_pass();
                    client_cron_beat_ms_ = cached_now_ms_ + 100;
                }
                if (__builtin_expect(save_cron_armed &&
                                     cached_now_ms_ >= save_cron_beat_ms_, false)) {
                    did += srv_->save_cron_pass(*self_, ring_);
                    save_cron_beat_ms_ = cached_now_ms_ + 1000;
                }
            }

            if (ring_.take_sq_full_submit()) non_send_rotations = 0;
            if (ring_.send_pending()) {
                ring_.submit_and_reap<true>();
                non_send_rotations = 0;
                continue;
            }
            if (did) {
                if (++non_send_rotations >= kGenthreadIoFusedCoalesceRotations) {
                    ring_.submit_and_reap<true>();
                    non_send_rotations = 0;
                }
                continue;
            }

            if constexpr (ThreeWay) three_way_gate_open = false;
            const uint32_t sweep_work =
                genthread_iofused_sweep<HasUnix, HasTls, kEp>();
            if (sweep_work) {
                if (ring_.take_sq_full_submit()) non_send_rotations = 0;
                if (ring_.send_pending() ||
                    ++non_send_rotations >= kGenthreadIoFusedCoalesceRotations) {
                    ring_.submit_and_reap<true>();
                    non_send_rotations = 0;
                }
                continue;
            }

            Span idle(sig.idle_ns);
            self_->arm_blocked();
            if constexpr (kEp) {
                if (!self_->any_fused_inbound())
                    epoll_pass<HasUnix, HasTls, true, 1>(50);
            } else {
                if (!self_->any_fused_inbound()) ring_.submit_and_wait<true>(1);
                else                            ring_.submit_and_reap<true>();
            }
            non_send_rotations = 0;
            self_->clear_blocked();
        }

        if constexpr (kEp) {
            while (!epoll_closes_.empty()) {
                Client* victim = epoll_closes_.back();
                epoll_closes_.pop_back();
                epoll_close_now(victim);
            }
        }
        clear_ifid_queue();
        if (srv_->aof().writer_is(self_->id()))
            srv_->aof().writer_shutdown(*self_, ring_);
        reap_dead();
        reap_dead();
    }

    // ---- submission -----------------------------------------------------------------------------
    void arm_accept(UrKind kind) {
        if (self_->role() != Role::Ifid || accept_quiescing_) return;
        const bool unix_socket = kind == UrKind::UnixAccept;
        const bool tls_socket = kind == UrKind::TlsAccept;
        io_uring_sqe* s = ring_.sqe();
        // sqe() can still return null when the submission queue is saturated. Writing through it
        // corrupts memory, and losing the accept re-arm silently stops the server taking
        // connections at all — so this is checked, counted, and retried on the next pass.
        if (!s) {
            self_->sig().sqe_starved++;
            if (unix_socket) unix_accept_pending_ = true;
            else if (tls_socket) tls_accept_pending_ = true;
            else accept_pending_ = true;
            return;
        }
        const int listener = unix_socket ? unix_listen_fd_ :
                             tls_socket ? tls_listen_fd_ : listen_fd_;
        io_uring_prep_multishot_accept(s, listener, nullptr, nullptr, 0);
        s->user_data = ur_tag(
            kind, reinterpret_cast<void*>(static_cast<uintptr_t>(accept_generation_)));
        ring_.note_pending();
        accept_armed_ref(kind) = true;
        if (unix_socket) unix_accept_pending_ = false;
        else if (tls_socket) tls_accept_pending_ = false;
        else accept_pending_ = false;
    }

    // ONE recv in flight per connection. While it is armed the kernel holds a raw pointer into the
    // read buffer, so nothing may move or realloc that buffer until the completion arrives.
    //
    // EPOLL READS THE SOCKET HERE INSTEAD. There is no submission to make, so the same call site
    // that "arms" a uring recv performs the recv itself, and recv_armed_ changes meaning to
    // "we reached EAGAIN, an edge is owed" (see net/epoll.h). Its two consumers keep working
    // unchanged: `stuck` in flush_ready keeps a connection in the active set while it is false, so
    // a read that stopped for lack of buffer space is retried; and safe_to_release refuses to free
    // a connection while it is true.
    template <bool kEp>
    void arm_recv(Client* c) {
        if (c->recv_armed() || c->closing() || find_client_migration(c)) return;
        if constexpr (kEp) { epoll_recv(c); return; }
        size_t avail = 0;
        // may_grow ONLY at quiescence: realloc moves the buffer that every in-flight argv Slice
        // points into. See Conn::read_space.
        const bool may_grow = c->rob().quiesced();
        char* dst = c->read_space(
            kRecvChunk, avail, may_grow, proto_max_bulk_len_);
        if (!dst) return;                      // no usable space yet: let the ROB drain first
        io_uring_sqe* s = ring_.sqe();
        if (!s) { self_->sig().sqe_starved++; return; }   // retried from flush_ready next pass
        io_uring_prep_recv(s, c->fd(), dst, avail, 0);
        s->user_data = ur_tag(UrKind::Recv, c);
        ring_.note_pending();
        c->set_recv_armed(true);
    }

    // Read until the socket is drained, the buffer will not take more, or the connection ends.
    // Draining fully is the edge-triggered obligation; stopping early WITHOUT setting recv_armed_
    // is how we remember that no further edge is coming and that we owe ourselves a retry.
    // Bytes are committed but NOT parsed here: this runs inside flush_ready's walk over the active
    // set, and parse_and_dispatch can close connections (CLIENT KILL) whose removal from that set
    // would invalidate the walk's iterator. The pass's existing re-parse step consumes them.
    void epoll_recv(Client* c) {
        for (;;) {
            size_t avail = 0;
            char* dst = c->read_space(kRecvChunk, avail, c->rob().quiesced());
            if (!dst) return;              // no usable space: stay un-armed so a later pass retries
            const ssize_t n = ::recv(c->fd(), dst, avail, MSG_DONTWAIT);
            if (n > 0) {
                self_->sig().epoll_recvs++;
                c->commit_read(static_cast<size_t>(n));
                c->set_last_interaction_s(cached_now_s_);
                if (static_cast<size_t>(n) < avail) { c->set_recv_armed(true); return; }
                continue;                  // filled the offer: there may be more behind it
            }
            if (n == 0) { epoll_request_close(c); return; }   // orderly peer close
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { c->set_recv_armed(true); return; }
            epoll_request_close(c);
            return;
        }
    }

    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    void arm_tls_recv(Client* c) {
        if (c->recv_armed() || c->closing()) return;
        TlsConn* tls = tls_engine(c);
        if (!tls || !tls->memory_bio()) return;
        // Any engine output is submitted before another socket read. This is the memory-BIO
        // flush-before-read rule that prevents WANT_READ from hiding a required write.
        if (tls->output_pending()) {
            (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
            if (tls->output_pending()) return;
        }
        // Ciphertext or decrypted plaintext already inside the engine must be drained before a
        // new zero-copy BIO reservation is pinned. Otherwise a ROB-full pipeline can arm an empty
        // socket recv while its next complete command is already waiting in OpenSSL, deadlocking
        // a request/response client until it happens to send unrelated bytes.
        if (tls->input_pending()) return;
        char* dst = nullptr;
        const int avail = tls->reserve_input(dst, kRecvChunk);
        if (avail <= 0) return;
        if constexpr (kEp) {
            const ssize_t n = ::recv(c->fd(), dst, static_cast<size_t>(avail), MSG_DONTWAIT);
            if (n > 0) {
                self_->sig().epoll_recvs++;
                on_tls_recv<kEp, Fused, Pipeline>(c, static_cast<int>(n));
                return;
            }
            tls->abandon_input();
            if (n == 0) { epoll_request_close(c); return; }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                c->set_recv_armed(true);
                return;
            }
            epoll_request_close(c);
            return;
        } else {
            io_uring_sqe* s = ring_.sqe();
            if (!s) {
                tls->abandon_input();
                self_->sig().sqe_starved++;
                return;
            }
            io_uring_prep_recv(s, c->fd(), dst, static_cast<unsigned>(avail), 0);
            s->user_data = ur_tag(UrKind::TlsRecv, c);
            ring_.note_pending();
            c->set_recv_armed(true);
        }
    }

    // Under epoll the readiness interest for a connection is armed once at accept and covers BOTH
    // directions permanently, so there is nothing to submit: recording the want on the TlsConn is
    // the whole operation, and the edge that satisfies it is already on its way. recv_armed_ is
    // still set for the same reason as the uring path -- it is what keeps the connection parked
    // instead of spinning drive_tls until the peer moves.
    template <bool kEp>
    void arm_tls_socket_poll(Client* c, TlsOp wanted) {
        if (c->closing()) return;
        TlsConn* tls = tls_engine(c);
        if (!tls || tls->poll_armed(wanted)) return;
        if constexpr (kEp) {
            tls->set_poll_armed(wanted, true);
            c->set_recv_armed(true);
            return;
        } else {
            io_uring_sqe* s = ring_.sqe();
            if (!s) { self_->sig().sqe_starved++; return; }
            const unsigned mask = (wanted == TlsOp::WantWrite ? POLLOUT : POLLIN) |
                                  POLLERR | POLLHUP | POLLRDHUP;
            io_uring_prep_poll_add(s, c->fd(), mask);
            s->user_data = ur_tag(wanted == TlsOp::WantWrite
                                      ? UrKind::TlsWritePoll : UrKind::TlsReadPoll, c);
            ring_.note_pending();
            // Reuse the existing kernel-reference fence: a poll CQE names Client just like recv.
            tls->set_poll_armed(wanted, true);
            c->set_recv_armed(true);
        }
    }

    // ---- epoll engine: the readiness pass -------------------------------------------------------
    //
    // What this does NOT do is as important as what it does: for a connection it records readiness
    // and puts the connection back in the active set, and nothing else. The actual recv, parse,
    // retire and send all happen in flush_ready, one frame up, exactly as they do under io_uring.
    // Two reasons. First, parse_and_dispatch can close OTHER connections (CLIENT KILL) and running
    // it from here, mid-event-array, would mean a Client is freed while a later epoll_event in the
    // same batch still names it. Second, keeping every state transition in one place is what makes
    // "epoll changes only how the thread waits" true rather than aspirational.
    template <bool HasUnix, bool HasTls, bool Fused = false, uint8_t Pipeline = 0>
    uint32_t epoll_pass(int timeout_ms) {
        const int n = ep_.wait(timeout_ms);
        if (n <= 0) return 0;
        self_->sig().epoll_events += static_cast<uint64_t>(n);
        uint32_t work = 0;
        for (int i = 0; i < n; i++) {
            const epoll_event& ev = ep_.event(i);
            switch (ur_kind(ev.data.u64)) {
                case UrKind::Accept:
                    work += epoll_accept<true, Fused, Pipeline>(UrKind::Accept); break;
                case UrKind::TlsAccept:
                    if constexpr (HasTls)
                        work += epoll_accept<true, Fused, Pipeline>(UrKind::TlsAccept);
                    break;
                case UrKind::UnixAccept:
                    if constexpr (HasUnix)
                        work += epoll_accept<true, Fused, Pipeline>(UrKind::UnixAccept);
                    break;
                case UrKind::Wake:
                    // The doorbell. Draining it here rather than at the park keeps the level-
                    // triggered registration from re-reporting the same wake every pass.
                    //
                    // work++ IS LOAD-BEARING, and it is not accounting. The bell and its payload
                    // are separate: a peer pushes its tag into the mailbox and THEN writes the
                    // eventfd, and the mailbox is drained by ring_.for_each_cqe() one step earlier
                    // in this pass. A peer that lands in the window between those two steps leaves
                    // us holding a rung bell with its payload still queued -- and if this pass then
                    // reported no work, the loop would park and the payload would wait out the
                    // whole 50 ms ceiling. Counting the bell as work sends the loop round again,
                    // where for_each_cqe picks the tag up immediately. Exactly the shape of the
                    // ~3.9 ms-per-operation reading DEFER_TASKRUN once produced on the other engine.
                    ring_.drain_wake_fd();
                    self_->sig().wakes_recv++;
                    work++;
                    break;
                case UrKind::Shutdown:
                    // Sticky and shared: never drain it, or this loop could steal the terminal
                    // edge from another ring/epoll set. The signal handler published stop first.
                    work++;
                    break;
                case UrKind::Recv: {
                    Client* c = ur_ptr<Client>(ev.data.u64);
                    // During FLIP preflight the fd may already be registered here while source
                    // ownership is still live. Do not touch even a flag until the owner edge.
                    if (!c || c->ifid_thread() != self_->id() || c->dead() ||
                        c->wb_slot() == Client::kWbMigrationInstalling ||
                        find_client_migration(c)) break;
                    // EPOLLERR/EPOLLHUP are folded into the read side on purpose: the recv that
                    // follows returns 0 or the real errno, and close_client is then reached through
                    // the one path that already knows how to tear a connection down.
                    if (ev.events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP))
                        c->set_recv_armed(false);
                    if (ev.events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) enqueue_serve(c);
                    // A TLS connection parked on WANT_READ/WANT_WRITE recorded that want on its
                    // TlsConn (arm_tls_socket_poll has nothing to submit under this engine, since
                    // both directions are already armed). This edge is the answer to whichever want
                    // is outstanding, so retire BOTH and let drive_tls re-record what it still
                    // needs -- the same converge-by-retry the poll CQE gives the uring engine. A
                    // want left recorded would make arm_tls_socket_poll a no-op forever and park
                    // the handshake.
                    if constexpr (HasTls) {
                        if (TlsConn* tls = tls_slot_conn(c)) {
                            tls->set_poll_armed(TlsOp::WantRead, false);
                            tls->set_poll_armed(TlsOp::WantWrite, false);
                            c->set_recv_armed(false);
                        }
                    }
                    mark_active_known<Fused && Pipeline != 0>(c);
                    work++;
                    break;
                }
                default: break;
            }
        }
        return work;
    }

    // ---- completions ----------------------------------------------------------------------------
    template <bool kEp, bool ImmediateProgress = true, bool ClassifySend = false>
    void on_plain_send_cqe(io_uring_cqe* cqe) {
        Client* c = ur_ptr<Client>(cqe->user_data);
        if (c->dead()) {
            // sendmsg's msghdr/iovecs and borrowed payload remain live through this CQE.
            wb_.on_dead_send_complete(*c, cqe->res);
            return;
        }
        if (!wb_.on_send_complete<ImmediateProgress, ClassifySend>(*c, cqe->res))
            close_client(c);
        else if constexpr (!ImmediateProgress) {
            if (!c->nothing_to_write()) enqueue_serve(c);
            enqueue_ifid(c);
        }
    }

    template <bool kEp, bool ImmediateProgress = true, bool ClassifySend = false>
    void on_tls_send_cqe(io_uring_cqe* cqe) {
        Client* c = ur_ptr<Client>(cqe->user_data);
        TlsConn* tls = tls_engine(c);
        if (!tls) { close_client(c); return; }
        if (c->dead()) {
            wb_.on_dead_tls_send_complete(*c, *tls, cqe->res);
            return;
        }
        if (!wb_.on_tls_send_complete<ImmediateProgress, ClassifySend>(
                *c, *tls, cqe->res)) close_client(c);
        else {
            if constexpr (!ImmediateProgress)
                if (!c->nothing_to_write() || tls->output_pending()) enqueue_serve(c);
            mark_active_known<!ImmediateProgress>(c);
        }
    }

    template <bool HasTls, bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    void on_cqe(io_uring_cqe* cqe) {
        constexpr bool ImmediateSendProgress = !(Fused && Pipeline != 0);
        if constexpr (!HasTls) {
            // Keep the tls-port=0 completion dispatch byte-for-byte shaped like the base switch.
            switch (ur_kind(cqe->user_data)) {
                case UrKind::Accept:
                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::Accept); break;
                case UrKind::UnixAccept:
                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::UnixAccept); break;
                case UrKind::Recv:
                    on_recv<false, kEp, Fused, Pipeline>(
                        ur_ptr<Client>(cqe->user_data), cqe->res);
                    break;
                case UrKind::Send:
                    on_plain_send_cqe<kEp, ImmediateSendProgress,
                                      Fused && Pipeline == 1>(cqe); break;
                case UrKind::Wake: self_->sig().wakes_recv++; break;
                case UrKind::Shutdown: break;
                case UrKind::SnapshotStart:
                    if constexpr (Fused)
                        fused_executor_->fused_snapshot_start(
                            ur_ptr<SnapshotManager>(cqe->user_data));
                    break;
                case UrKind::AofIo:
                    srv_->aof().on_io_complete(*self_, ring_, ur_ptr<void>(cqe->user_data),
                                               cqe->res); break;
                case UrKind::SnapshotIo:
                    srv_->snapshot().on_io_complete(*self_, ring_, ur_ptr<void>(cqe->user_data),
                                                    cqe->res); break;
                case UrKind::Close: break;
                case UrKind::MigrateCancel: break;
                case UrKind::TlsReadPoll: break;
                case UrKind::TlsWritePoll: break;
                default: break;
            }
        } else {
            switch (ur_kind(cqe->user_data)) {
                case UrKind::Accept:
                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::Accept); break;
                case UrKind::TlsAccept:
                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::TlsAccept); break;
                case UrKind::UnixAccept:
                    on_accept<kEp, Fused, Pipeline>(cqe, UrKind::UnixAccept); break;
                case UrKind::Recv:
                    on_recv<true, kEp, Fused, Pipeline>(
                        ur_ptr<Client>(cqe->user_data), cqe->res);
                    break;
                case UrKind::TlsRecv:
                    on_tls_recv<kEp, Fused, Pipeline>(
                        ur_ptr<Client>(cqe->user_data), cqe->res);
                    break;
                case UrKind::Send:
                    on_plain_send_cqe<kEp, ImmediateSendProgress,
                                      Fused && Pipeline == 1>(cqe); break;
                case UrKind::TlsSend:
                    on_tls_send_cqe<kEp, ImmediateSendProgress,
                                    Fused && Pipeline == 1>(cqe); break;
                case UrKind::TlsReadPoll:
                    on_tls_socket_poll<kEp, Fused, Pipeline>(
                        ur_ptr<Client>(cqe->user_data), cqe->res, TlsOp::WantRead); break;
                case UrKind::TlsWritePoll:
                    on_tls_socket_poll<kEp, Fused, Pipeline>(
                        ur_ptr<Client>(cqe->user_data), cqe->res, TlsOp::WantWrite); break;
                case UrKind::Wake: self_->sig().wakes_recv++; break;
                case UrKind::Shutdown: break;
                case UrKind::SnapshotStart:
                    if constexpr (Fused)
                        fused_executor_->fused_snapshot_start(
                            ur_ptr<SnapshotManager>(cqe->user_data));
                    break;
                case UrKind::AofIo:
                    srv_->aof().on_io_complete(*self_, ring_, ur_ptr<void>(cqe->user_data),
                                               cqe->res); break;
                case UrKind::SnapshotIo:
                    srv_->snapshot().on_io_complete(*self_, ring_, ur_ptr<void>(cqe->user_data),
                                                    cqe->res); break;
                case UrKind::Close: break;
                case UrKind::MigrateCancel: break;
            }
        }
    }

    void rearm_accept(io_uring_cqe* cqe, UrKind kind) {
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            accept_armed_ref(kind) = false;
            self_->sig().accept_rearm++;
            arm_accept(kind);
        }
    }

    bool& accept_armed_ref(UrKind kind) {
        if (kind == UrKind::UnixAccept) return unix_accept_armed_;
        if (kind == UrKind::TlsAccept) return tls_accept_armed_;
        return accept_armed_;
    }

    bool accepts_quiesced() const {
        return epoll_ || (!accept_armed_ && !tls_accept_armed_ && !unix_accept_armed_);
    }

    void quiesce_accepts_for_conversion() {
        // UNIX owns a unique pathname and its boot owner is never selected for conversion.
        if (unix_listen_fd_ >= 0) std::abort();
        if (!accept_quiescing_) {
            accept_quiescing_ = true;
            accept_cancel_generation_ = accept_generation_;
            accept_generation_++; // every completion from the old tenure is now recognisably stale
            accept_pending_ = tls_accept_pending_ = false;
        }
        if (epoll_) return; // registration stays armed and is immediately reusable on rollback
        auto cancel = [&](UrKind kind, bool armed, bool& submitted) {
            if (!armed || submitted) return;
            io_uring_sqe* sqe = ring_.sqe();
            if (!sqe) { self_->sig().sqe_starved++; return; }
            io_uring_prep_cancel64(
                sqe, ur_tag(kind, reinterpret_cast<void*>(
                                      static_cast<uintptr_t>(accept_cancel_generation_))), 0);
            sqe->user_data = ur_tag(UrKind::MigrateCancel, nullptr);
            ring_.note_pending();
            submitted = true;
        };
        cancel(UrKind::Accept, accept_armed_, accept_cancel_submitted_);
        cancel(UrKind::TlsAccept, tls_accept_armed_, tls_accept_cancel_submitted_);
    }

    bool restore_accepts_after_rollback() {
        if (accept_quiescing_) {
            if (!accepts_quiesced()) return false;
            accept_quiescing_ = false;
            accept_cancel_submitted_ = tls_accept_cancel_submitted_ = false;
        }
        if (epoll_) return true; // tenure registrations were deliberately left installed
        if (listen_fd_ >= 0 && !accept_armed_) arm_accept(UrKind::Accept);
        if (tls_listen_fd_ >= 0 && !tls_accept_armed_) arm_accept(UrKind::TlsAccept);
        // Do not publish the old shape as restored while SQ starvation still leaves one of its
        // listeners unarmed. arm_accept records a pending retry, so the next pass makes progress.
        return (listen_fd_ < 0 || accept_armed_) &&
               (tls_listen_fd_ < 0 || tls_accept_armed_);
    }

    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    void on_accept(io_uring_cqe* cqe, UrKind kind) {
        const uint64_t generation = reinterpret_cast<uintptr_t>(ur_ptr<void>(cqe->user_data));
        if (generation != accept_generation_ || self_->role() != Role::Ifid) {
            if (cqe->res >= 0) ::close(cqe->res);
            if (!(cqe->flags & IORING_CQE_F_MORE)) accept_armed_ref(kind) = false;
            return;
        }
        if (cqe->res < 0) {
            // Do not swallow this silently: a failing accept with no trace is indistinguishable from
            // a hung server, which is exactly how the 1024-connection failure presented.
            self_->sig().accept_err++;
            rearm_accept(cqe, kind);
            return;
        }
        if (srv_->flip_dispatch_paused()) {
            ::close(cqe->res);
            rearm_accept(cqe, kind);
            return;
        }
        admit_fd<kEp, Fused, Pipeline>(cqe->res, kind);
        rearm_accept(cqe, kind);
    }

    // Epoll's accept: the listener only told us there is a backlog, so drain it. Level-triggered
    // registration means an unfinished drain is re-reported rather than lost, but draining to
    // EAGAIN here keeps one epoll_wait per burst instead of one per connection.
    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    uint32_t epoll_accept(UrKind kind) {
        if (srv_->flip_dispatch_paused()) return 0;
        const int listener = kind == UrKind::UnixAccept ? unix_listen_fd_ :
                             kind == UrKind::TlsAccept ? tls_listen_fd_ : listen_fd_;
        if (listener < 0) return 0;
        uint32_t taken = 0;
        for (;;) {
            const int fd = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) self_->sig().accept_err++;
                return taken;
            }
            taken++;
            admit_fd<kEp, Fused, Pipeline>(fd, kind);
        }
    }

    // Everything an accepted fd goes through before it becomes a served connection. Shared by both
    // engines verbatim: maxclients, protected mode, Client allocation, TLS attachment, unix
    // round-robin handoff. Only the way the fd ARRIVED differs, which is the whole engine boundary.
    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    void admit_fd(int fd, UrKind kind) {
        if (srv_->flip_dispatch_paused()) { ::close(fd); return; }
        const bool unix_socket = kind == UrKind::UnixAccept;
        const bool tls_socket = kind == UrKind::TlsAccept;
        self_->sig().accepts++;
        if (tls_socket) self_->sig().tls_accepts++;
        else self_->sig().plain_accepts++;
        if (srv_->live_clients() >= srv_->maxclients()) {
            static constexpr char kErr[] = "-ERR max number of clients reached\r\n";
            if (!tls_socket) {
                const ssize_t sent = ::send(fd, kErr, sizeof(kErr) - 1,
                                            MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent > 0) self_->sig().net_output_bytes += static_cast<uint64_t>(sent);
            }
            ::close(fd);
            srv_->note_rejected_conn();
            return;
        }
        if (__builtin_expect(srv_->protected_mode() && !srv_->requirepass_enabled() &&
                             !peer_is_local(fd, unix_socket), false)) {
            static constexpr char kDenied[] =
                "-DENIED Redis is running in protected mode because protected mode is enabled and no password is set for the default user. In this mode connections are only accepted from the loopback interface. If you want to connect from external computers to Redis you may adopt one of the following solutions: 1) Just disable protected mode sending the command 'CONFIG SET protected-mode no' from the loopback interface by connecting to Redis from the same host the server is running, however MAKE SURE Redis is not publicly accessible from internet if you do so. Use CONFIG REWRITE to make this change permanent. 2) Alternatively you can just disable the protected mode by editing the Redis configuration file, and setting the protected mode option to 'no', and then restarting the server. 3) If you started the server manually just for testing, restart it with the '--protected-mode no' option. 4) Set up an authentication password for the default user. NOTE: You only need to do one of the above things in order for the server to start accepting connections from the outside.\r\n";
            if (!tls_socket) {
                const ssize_t sent = ::send(fd, kDenied, sizeof(kDenied) - 1,
                                            MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent > 0) self_->sig().net_output_bytes += static_cast<uint64_t>(sent);
            }
            ::close(fd);
            srv_->note_rejected_connection();
            return;
        }
        auto* c = new (std::nothrow) Client(fd);
        if (!c) {
            ::close(fd);
            self_->sig().accept_err++;
            return;
        }
        // NOTHING READ-LOCAL IS ALLOCATED HERE ANY MORE (DESIGN-RINGDIET.md). The RYOW write-ring
        // sidecar used to be built at accept for every connection on an armed boot, so a pure-read
        // or idle connection carried 1216 bytes (a 1280-byte jemalloc class) it could never use.
        // It is now built on the first write of a connection a local read has ARMED, from that
        // connection's own IO thread; an allocation failure there falls back to the unarmed
        // contract instead of rejecting a connection that is already established.
        if (tls_socket && !attach_tls(c)) {
            delete c;
            ::close(fd);
            self_->sig().accept_err++;
            return;
        }
        srv_->client_accepted();
        c->set_authenticated(!srv_->requirepass_enabled());
        c->set_acl_user_idx(kAclDefaultUser);
        c->set_id(srv_->next_client_id().fetch_add(1, std::memory_order_relaxed));
        if (unix_socket) {
            const auto& ios = srv_->placement().ifid_threads();
            const uint32_t target = ios[unix_rr_++ % ios.size()];
            c->set_ifid_thread(target);
            if (target == self_->id()) adopt_client<kEp, Fused, Pipeline>(c, true);
            else if (!srv_->thread(target).post_client(self_->id(), c, ring_, self_->sig()))
                pending_handoffs_.push_back(c);
        } else {
            c->set_ifid_thread(self_->id());
            adopt_client<kEp, Fused, Pipeline>(c, false, tls_socket);
        }
    }

    std::string socket_address(int fd, bool unix_socket, bool remote) const {
        if (unix_socket) return std::string(srv_->cfg().unixsocket ? srv_->cfg().unixsocket : "unix") + ":0";
        sockaddr_in address{};
        socklen_t len = sizeof(address);
        char ip[INET_ADDRSTRLEN] = "unknown";
        const int result = remote
            ? ::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &len)
            : ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &len);
        if (result == 0) ::inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
        char out[INET_ADDRSTRLEN + 16];
        std::snprintf(out, sizeof(out), "%s:%u", ip, ntohs(address.sin_port));
        return out;
    }

    bool peer_is_local(int fd, bool unix_socket) const {
        if (unix_socket) return true;
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len) != 0) return false;
        return (ntohl(peer.sin_addr.s_addr) & 0xff000000u) == 0x7f000000u;
    }

    TlsConn* tls_slot_conn(const Client* c) {
        if (!c || !c->is_tls() || c->tls_slot() >= tls_slots_.size()) return nullptr;
        return tls_slots_[c->tls_slot()].get();
    }

    TlsConn* tls_engine(const Client* c) {
        TlsConn* tls = tls_slot_conn(c);
        return tls && !tls->ktls() ? tls : nullptr;
    }

    bool attach_tls(Client* c) {
        try {
            auto conn = std::make_unique<TlsConn>();
            std::string error;
            if (!conn->init(*tls_context_, srv_->cfg().tls_auth_clients, c->fd(),
                            true, error)) {
                std::fprintf(stderr, "TLS connection init failed: %s\n", error.c_str());
                return false;
            }
            uint32_t slot;
            if (!tls_free_slots_.empty()) {
                slot = tls_free_slots_.back();
                tls_free_slots_.pop_back();
                tls_slots_[slot] = std::move(conn);
            } else {
                slot = static_cast<uint32_t>(tls_slots_.size());
                tls_slots_.push_back(std::move(conn));
            }
            c->set_tls_slot(slot);
            self_->sig().tls_handshakes_started++;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    void release_tls(Client* c) {
        if (!c->is_tls()) return;
        const uint32_t slot = c->tls_slot();
        if (slot >= tls_slots_.size() || !tls_slots_[slot]) std::abort();
        if (tls_slots_[slot]->was_ktls()) {
            if (!self_->sig().tls_ktls_active) std::abort();
            self_->sig().tls_ktls_active--;
        }
        tls_slots_[slot].reset();
        tls_free_slots_.push_back(slot);
        c->set_tls_slot(Client::kNoTlsSlot);
        self_->sig().tls_connections_freed++;
    }

    ClientMigration* find_client_migration(const Client* client) {
        for (ClientMigration& migration : client_migrations_)
            if (migration.client == client) return &migration;
        return nullptr;
    }
    const ClientMigration* find_client_migration(const Client* client) const {
        for (const ClientMigration& migration : client_migrations_)
            if (migration.client == client) return &migration;
        return nullptr;
    }

    void erase_client_migration(const Client* client) {
        for (size_t i = 0; i < client_migrations_.size(); i++) {
            if (client_migrations_[i].client != client) continue;
            client_migrations_[i] = client_migrations_.back();
            client_migrations_.pop_back();
            return;
        }
        std::abort();
    }

    template <bool kEp>
    void cancel_client_transfer(Client* client) {
        ClientMigration* migration = find_client_migration(client);
        if (!migration) std::abort();
        if constexpr (kEp) {
            if (migration->destination_registered) {
                srv_->thread(migration->destination).cancel_client_registration(client);
                migration->destination_registered = false;
            }
            if (migration->source_backup_fd < 0) std::abort();
            const int original = client->replace_fd(migration->source_backup_fd);
            migration->source_backup_fd = -1;
            ::close(original); // destination registration was removed above
        }
        if (migration->catalog) {
            if (!command_client_migration_install(migration->catalog)) std::abort();
            migration->catalog = nullptr;
        }
        if (migration->routing) {
            client_routing_discard(migration->routing);
            migration->routing = nullptr;
        }
        const uint64_t client_id = client->id();
        erase_client_migration(client);
        srv_->lb_client_move_cancelled(client_id);
        client_transfer_failures_++;
        mark_active(client);
        arm_recv<kEp>(client);
    }

    template <bool kEp>
    bool request_client_transfer_impl(Client* client, uint32_t destination,
                                      bool hold_for_commit, std::string& error) {
        if (!client_transfer_ready(client, destination, error)) return false;
        if (srv_->thread(destination).role() != Role::Ifid &&
            !(srv_->flip_stage() != FlipStage::Idle &&
              srv_->flip_final_role(destination) == Role::Ifid)) {
            error = "destination thread is not a current or prepared IO owner";
            return false;
        }
        if (find_client_migration(client)) {
            error = "connection transfer is already active";
            return false;
        }
        if (!srv_->thread(destination).client_transfer_free_slots(self_->id())) {
            error = "destination connection-transfer inbox is full";
            return false;
        }
        try {
            client_migrations_.push_back(
                ClientMigration{client, destination, false, hold_for_commit,
                                false, false, -1, nullptr, nullptr});
        } catch (const std::bad_alloc&) {
            error = "could not allocate connection-transfer state";
            return false;
        }
        try {
            // Node storage is prepared separately below; this reserves enough buckets for every
            // migration already admitted on this source, so commit insertion cannot rehash.
            routing_forward_.reserve(routing_forward_.size() + client_migrations_.size());
        } catch (const std::bad_alloc&) {
            client_migrations_.pop_back();
            error = "could not allocate connection-routing forwarding state";
            return false;
        }

        if (client->in_active()) {
            client->set_in_active(false);
            active_.erase(client);
        }
        discard_ifid(client);
        if constexpr (kEp) {
            // Reserve BOTH possible outcomes before removing the original source interest. The
            // duplicate is already registered on source: rollback adopts that fd without ADD.
            ClientMigration* migration = find_client_migration(client);
            const int backup = ::fcntl(client->fd(), F_DUPFD_CLOEXEC, 0);
            if (backup < 0 ||
                !ep_.add(backup, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
                         ur_tag(UrKind::Recv, client))) {
                if (backup >= 0) ::close(backup);
                erase_client_migration(client);
                mark_active(client);
                error = "could not reserve source epoll rollback registration";
                return false;
            }
            migration->source_backup_fd = backup;
            if (!srv_->thread(destination).prepare_client_registration(client)) {
                if (!ep_.del(backup)) std::abort();
                ::close(backup);
                erase_client_migration(client);
                mark_active(client);
                error = "could not reserve destination epoll registration";
                return false;
            }
            migration->destination_registered = true;
            if (!ep_.del(client->fd())) {
                srv_->thread(destination).cancel_client_registration(client);
                if (!ep_.del(backup)) std::abort();
                ::close(backup);
                erase_client_migration(client);
                mark_active(client);
                error = "could not detach original source epoll registration";
                return false;
            }
            client->set_recv_armed(false); // epoll held no buffer pointer; the old edge is discarded
            return finish_client_transfer<kEp>(client);
        }
        return service_client_migrations<kEp>() != 0 || find_client_migration(client) != nullptr;
    }

    template <bool kEp>
    bool finish_client_transfer(Client* client) {
        ClientMigration* migration = find_client_migration(client);
        if (!migration || client->recv_armed()) return false;
        if (migration->prepared) return true;
        const uint32_t destination = migration->destination;
        ThreadCtx& target = srv_->thread(destination);
        if (!target.client_transfer_free_slots(self_->id())) return false;

        std::string error;
        if (!client_transfer_ready(client, destination, error)) {
            cancel_client_transfer<kEp>(client);
            return false;
        }
        if (!client_routing_prepare(*migration, error)) {
            cancel_client_transfer<kEp>(client);
            return false;
        }
        migration->catalog = command_client_migration_extract(client);
        if (!migration->catalog) {
            cancel_client_transfer<kEp>(client);
            return false;
        }
        if (migration->hold_for_commit) {
            migration->prepared = true;
            return true;
        }

        return commit_client_transfer<kEp>(client);
    }

    template <bool kEp>
    bool commit_client_transfer(Client* client) {
        ClientMigration* migration = find_client_migration(client);
        if (!migration || !migration->catalog || client->recv_armed()) return false;
        const uint32_t destination = migration->destination;
        const bool flip_transfer = migration->hold_for_commit;
        ThreadCtx& target = srv_->thread(destination);
        if (!target.client_transfer_free_slots(self_->id())) std::abort();
        if constexpr (kEp) {
            if (!migration->destination_registered || migration->source_backup_fd < 0)
                std::abort();
            ::close(migration->source_backup_fd); // removes the reserved source registration
            migration->source_backup_fd = -1;
        }
        void* const catalog = migration->catalog;
        void* const routing = migration->routing;
        std::vector<PubSubEvent*> rebind_events;
        client_routing_commit_extract(*migration, rebind_events);

        // Everything below was reserved or validated above. The owner store is deliberately last
        // among source touches. A failed transport push after the capacity check is an invariant
        // failure: attempting rollback would make ownership ambiguous.
        const uint64_t client_id = client->id();
        self_->release_wb_slot(client->wb_slot());
        // Destination epoll readiness may arrive as soon as the owner store publishes. This
        // sentinel keeps those events inert until the pre-reserved client/catalog/WB install.
        client->set_wb_slot(Client::kWbMigrationInstalling);
        if (!self_->remove_client(client)) std::abort();

        const ClientTransfer transfer{client, catalog, routing, self_->id()};
        // Last source-side touch before the owner store, so the moved connection's cold arm
        // counters land on the thread that will actually be doing the arming from here on.
        client->rob().set_read_local_arm_stats(target.read_local_arm_stats_or_null());
        client_work_fences_.erase(client);
        client->set_ifid_thread(destination); // THE single connection ownership edge
        command_client_directory_move(client_id, destination);
        if (!target.post_client_transfer(self_->id(), transfer, ring_, self_->sig())) std::abort();
        // These ModifyRequests were allocated while source still owned the connection. Posting
        // after the owner edge lets every channel home resolve the live destination. The receiver
        // drains its transfer inbox before pub/sub events, so a newly routed delivery cannot beat
        // installation of the moved local catalog.
        for (PubSubEvent* event : rebind_events) {
            srv_->pubsub_event_created();
            pubsub_post(event->target_io, event);
        }
        if (flip_transfer) srv_->flip_note_client_transferred(client_id, destination);
        erase_client_migration(client);       // pointer comparison only; source never dereferences
        srv_->lb_client_move_committed(client_id, self_->id(), destination);
        return true;
    }

    template <bool kEp>
    void commit_prepared_client_transfers_impl() {
        while (!client_migrations_.empty()) {
            ClientMigration& migration = client_migrations_.back();
            if (!migration.prepared || !commit_client_transfer<kEp>(migration.client)) std::abort();
        }
    }

    template <bool kEp>
    void cancel_prepared_client_transfers_impl() {
        while (!client_migrations_.empty())
            cancel_client_transfer<kEp>(client_migrations_.back().client);
    }

    template <bool kEp>
    uint32_t service_client_migrations() {
        uint32_t work = 0;
        // finish_client_transfer erases by swap, so an index advances only when its entry remains.
        for (size_t i = 0; i < client_migrations_.size();) {
            ClientMigration& migration = client_migrations_[i];
            Client* client = migration.client;
            if (!client->recv_armed()) {
                const size_t before = client_migrations_.size();
                (void)finish_client_transfer<kEp>(client);
                if (client_migrations_.size() != before) { work++; continue; }
                i++;
                continue;
            }
            if constexpr (!kEp) {
                if (!migration.cancel_submitted) {
                    io_uring_sqe* sqe = ring_.sqe();
                    if (!sqe) { self_->sig().sqe_starved++; i++; continue; }
                    io_uring_prep_cancel64(sqe, ur_tag(UrKind::Recv, client), 0);
                    sqe->user_data = ur_tag(UrKind::MigrateCancel, client);
                    ring_.note_pending();
                    migration.cancel_submitted = true;
                    work++;
                }
            }
            i++;
        }
        return work;
    }

    template <bool kEp>
    uint32_t drain_client_transfers(bool unmasked = false) {
        auto take = [&](const ClientTransfer& transfer) {
            Client* client = transfer.client;
            if (!client || client->ifid_thread() != self_->id()) std::abort();
            if (!command_client_migration_install(transfer.catalog)) std::abort();
            if (!client_routing_install(transfer.routing, client, transfer.source)) std::abort();
            try {
                self_->add_client(client);
            } catch (...) {
                std::abort();
            }
            client->set_wb_slot(self_->assign_wb_slot(client));
            // Epoll's actual registration was installed during reversible preflight. io_uring
            // needs no registration; arm_recv below retries harmless SQ starvation.
            mark_active(client);
            arm_recv<kEp>(client);
        };
        return unmasked ? self_->drain_client_transfers_unmasked(take)
                        : self_->drain_client_transfers(take);
    }

    bool flip_io_drained() const {
        if (!pending_serve_.empty() || !pending_ifid_.empty() || !pending_releases_.empty() ||
            !pending_handoffs_.empty() || !deferred_timers_.empty() ||
            !client_migrations_.empty() || !epoll_closes_.empty() ||
            !dead_next_.empty() || !dead_ready_.empty() ||
            !multi_deferred_.empty() || !pending_multi_cleanups_.empty() ||
            !pubsub_notification_chains_.empty() || !io_pipelines_quiesced() ||
            !self_->io_inbound_quiesced() ||
            srv_->pubsub_inflight() != 0 || srv_->pubsub_pending() != 0)
            return false;
        for (Client* client : self_->clients()) {
            const bool coordinator_op = self_->id() == srv_->flip_coordinator() &&
                client == flip_client_ && flip_epoch_local_ == srv_->flip_epoch();
            if (coordinator_op) {
                if (client->rob().in_flight() != 1 || client->send_inflight() ||
                    client->serve_pending() || !client->nothing_to_write() ||
                    !wb_.migration_ready(*client)) return false;
            } else if (!client->flip_drain_idle() || !wb_.migration_ready(*client)) {
                return false;
            }
        }
        return true;
    }

    bool flip_candidate_clients_ready(std::string& error) const {
        const uint32_t planned = srv_->flip_source_clients(self_->id());
        for (uint32_t ordinal = 0; ordinal < planned; ordinal++) {
            Client* client = srv_->flip_client_at(self_->id(), ordinal);
            if (!client || client == flip_client_ || client->ifid_thread() != self_->id()) {
                error = "the weighted FLIP connection set changed after planning";
                return false;
            }
            std::string refused;
            if (!client_transfer_ready(
                    client, srv_->flip_client_destination(self_->id(), ordinal), refused)) {
                error = refused.empty() ? "a weighted FLIP connection is not transferable" : refused;
                return false;
            }
        }
        return true;
    }

    void flip_wake_all() {
        for (uint32_t tid = 0; tid < srv_->nthreads(); tid++) {
            if (tid == self_->id()) continue;
            Ring* target = srv_->thread(tid).ring();
            if (!target) continue;
            if (ring_.msg_to(*target, ur_tag(UrKind::Wake, nullptr))) self_->sig().wakes_sent++;
            else self_->sig().sqe_starved++;
        }
    }

    void lb_schedule_wake_all() { lb_wake_cursor_ = 0; }

    uint32_t lb_wake_all_pass() {
        if (lb_wake_cursor_ == UINT32_MAX) return 0;
        while (lb_wake_cursor_ < srv_->nthreads()) {
            const uint32_t tid = lb_wake_cursor_++;
            if (tid == self_->id()) continue;
            Ring* target = srv_->thread(tid).ring();
            if (!target) continue;
            if (ring_.msg_to(*target, ur_tag(UrKind::Wake, nullptr))) {
                self_->sig().wakes_sent++;
                continue;
            }
            self_->sig().sqe_starved++;
            lb_wake_cursor_--; // retry this exact target after the pending SQEs are submitted
            return 1;
        }
        lb_wake_cursor_ = UINT32_MAX;
        return 1;
    }

    void flip_publish_stage(FlipStage stage) {
        srv_->flip_set_stage(stage);
        flip_wake_all();
    }

    void finish_flip_command(const char* error = nullptr) {
        // An automatic FLIP is issued by the main monitor and deliberately owns no client ROB
        // slot. Manual FLIP retains the exact asynchronous completion path below.
        if (!flip_client_) return;
        if (flip_epoch_local_ != srv_->flip_epoch()) std::abort();
        Op& op = flip_client_->rob().at(flip_op_id_);
        if (error) reply_err(op.sink(), error);
        else       reply_ok(op.sink());
        op.state.store(OpState::Done, std::memory_order_release);
        enqueue_serve(flip_client_);
        mark_active(flip_client_);
        flip_client_ = nullptr;
        flip_op_id_ = 0;
    }

    void flip_enter_rollback(const std::string& error) {
        srv_->flip_note_failure(error);
        if (srv_->flip_stage() != FlipStage::Rollback)
            flip_publish_stage(FlipStage::Rollback);
    }

    uint32_t lb_control_pass() {
        if (!lb_controller_armed_) return 0;
        const LbStage stage = srv_->lb_stage();
        if (stage != LbStage::ClientDrain) lb_client_wake_pending_ = false;
        if (stage == LbStage::Idle || stage == LbStage::ClientMoving) return 0;
        if (stage == LbStage::IoDrain) {
            // This control tail is outside dispatch: all owner samples taken by this IO
            // have either been posted (including quiet batches) or abandoned for reparse.
            if (!srv_->lb_acked(self_->id())) {
                srv_->lb_ack(self_->id());
                lb_schedule_wake_all();
            }
            if (self_->id() == srv_->lb_coordinator()) {
                if (srv_->lb_begin_ex_drain()) {
                    lb_schedule_wake_all();
                    return 1;
                }
                if (srv_->lb_timed_out()) {
                    srv_->lb_stage_timed_out();
                    lb_schedule_wake_all();
                    return 1;
                }
            }
            return 0;
        }
        if (stage == LbStage::ExDrain) {
            if (self_->id() == srv_->lb_coordinator()) {
                if (srv_->lb_all_ex_acked()) {
                    (void)srv_->lb_commit_shard_plan(cached_now_ms_);
                    lb_schedule_wake_all();
                    return 1;
                } else if (srv_->lb_timed_out()) {
                    srv_->lb_stage_timed_out();
                    lb_schedule_wake_all();
                    return 1;
                }
            }
            return 0;
        }

        const LbClientMove move = srv_->lb_client_move();
        auto wake_source = [&]() {
            Ring* source = srv_->thread(move.source).ring();
            if (source && ring_.msg_to(*source, ur_tag(UrKind::Wake, nullptr))) {
                self_->sig().wakes_sent++;
                lb_client_wake_pending_ = false;
            } else {
                self_->sig().sqe_starved++;
            }
            return 1u;
        };
        if (lb_client_wake_pending_) return wake_source();
        uint32_t work = 0;
        if (move.destination == self_->id() && !srv_->lb_acked(self_->id())) {
            if (!prepare_client_transfer_capacity(1)) {
                srv_->lb_refuse_client_request();
                lb_schedule_wake_all();
                return 1;
            }
            srv_->lb_ack(self_->id());
            lb_client_wake_pending_ = true;
            work += wake_source();
        }
        if (move.source == self_->id() && srv_->lb_stage() == LbStage::ClientDrain &&
            srv_->lb_acked(move.destination)) {
            Client* selected = nullptr;
            for (Client* client : self_->clients())
                if (client->id() == move.id) { selected = client; break; }
            if (!selected || selected->ifid_thread() != self_->id()) {
                srv_->lb_refuse_client_request();
                lb_schedule_wake_all();
                return 1;
            }
            std::string error;
            if (client_transfer_ready(selected, move.destination, error)) {
                if (!srv_->lb_client_move_started(move.id, cached_now_ms_)) return 1;
                const bool started = request_client_transfer(selected, move.destination, error);
                if (!started) srv_->lb_client_move_cancelled(move.id);
                lb_schedule_wake_all();
                return 1;
            }
            if (selected->is_tls() || selected->multi_session() != nullptr ||
                selected->blocked()) {
                srv_->lb_refuse_client_request();
                lb_schedule_wake_all();
                return 1;
            }
        }
        if (self_->id() == srv_->lb_coordinator() && srv_->lb_timed_out()) {
            srv_->lb_stage_timed_out();
            lb_schedule_wake_all();
            return 1;
        }
        return work;
    }

    void flip_commit_roles_and_evacuate_shards() {
        // The exact final owner of every physical shard was chosen and reserved before CLIENT
        // COMMIT. A future EX may receive into its dormant shard vector while dispatch is paused;
        // this avoids a staging owner and makes every changed shard cross exactly one owner edge.
        for (uint32_t sid = 0; sid < srv_->nshards(); sid++) {
            const uint32_t source = srv_->worker_of_shard(static_cast<int32_t>(sid));
            const uint32_t destination = srv_->flip_shard_destination(sid);
            if (source == destination) continue;
            if (!srv_->transfer_shard_quiesced(static_cast<int32_t>(sid), source, destination))
                std::abort();
            srv_->flip_note_bucket_transferred();
        }

        for (uint32_t tid = 0; tid < srv_->nthreads(); tid++) {
            const Role target = srv_->flip_candidate_target(tid);
            if (target == Role::Idle) continue;
            if (srv_->thread(tid).role() == target) continue; // peer changed with its SMT unit
            srv_->flip_change_role(tid, target); // direct old->new store; no Idle ownership hole
            if (Ring* peer = srv_->thread(tid).ring())
                (void)ring_.msg_to(*peer, ur_tag(UrKind::Wake, nullptr));
        }
        flip_publish_stage(FlipStage::RoleReady);
    }

    template <bool kEp>
    uint32_t flip_control_pass() {
        const FlipStage stage = srv_->flip_stage();
        if (stage == FlipStage::Idle) return 0;

        if (stage == FlipStage::IoDrain && !srv_->flip_acked(self_->id(), stage) &&
            flip_io_drained()) {
            srv_->flip_ack(self_->id(), stage);
        } else if (stage == FlipStage::IoPrepare &&
                   !srv_->flip_acked(self_->id(), stage)) {
            if (!prepare_client_transfer_capacity(srv_->flip_incoming_clients(self_->id())))
                srv_->flip_note_failure("ERR FLIP could not reserve destination connection state");
            srv_->flip_ack(self_->id(), stage);
        } else if (stage == FlipStage::ClientPrepare &&
                   !srv_->flip_acked(self_->id(), stage)) {
            const bool converting_to_ex =
                srv_->flip_candidate_target(self_->id()) == Role::Ex;
            if (converting_to_ex) quiesce_accepts_for_conversion();
            const uint32_t planned = srv_->flip_source_clients(self_->id());
            if (planned &&
                flip_prepare_epoch_ != srv_->flip_epoch()) {
                std::string ready_error;
                if (!flip_candidate_clients_ready(ready_error)) {
                    // A saturated pipelined connection is momentarily busy at ANY instant, so a
                    // one-shot readiness snapshot let a single in-flight reply veto the whole
                    // flip -- under 512-conn p16 load FLIP was refused essentially always, while
                    // 8-conn tests sailed through. Dispatch is already held for this stage, so
                    // busy connections drain within milliseconds: leave the epoch unclaimed and
                    // retry every loop pass, and only convert not-ready into failure when the
                    // flip's own 5s deadline expires (a genuinely stuck connection still fails).
                    if (srv_->flip_timed_out()) {
                        flip_prepare_epoch_ = srv_->flip_epoch();
                        srv_->flip_note_failure(
                            "ERR FLIP cannot quiesce a connection: " + ready_error);
                    }
                } else {
                    flip_prepare_epoch_ = srv_->flip_epoch();
                    if (!reserve_client_transfer_state(planned)) {
                        srv_->flip_note_failure(
                            "ERR FLIP could not reserve source connection state");
                    } else {
                        for (uint32_t ordinal = 0; ordinal < planned; ordinal++) {
                            Client* client = srv_->flip_client_at(self_->id(), ordinal);
                            if (!client) std::abort();
                            std::string error;
                            if (!prepare_client_transfer(
                                    client, srv_->flip_client_destination(self_->id(), ordinal),
                                    error)) {
                                srv_->flip_note_failure(
                                    "ERR FLIP could not detach a connection: " + error);
                                break;
                            }
                        }
                    }
                }
            }
            const bool accepts_ready = !converting_to_ex || accepts_quiesced();
            // While the quiesce retry above has not yet claimed the epoch, no prepare has run and
            // client_transfers_prepared() is vacuously true over an empty migration list -- the
            // mismatch test below would misread "still draining" as "set changed" and kill the
            // flip on its first not-ready iteration. Judge outcomes only after the prepare
            // actually ran for THIS flip epoch.
            const bool prepared_this_epoch =
                planned == 0 || flip_prepare_epoch_ == srv_->flip_epoch();
            if (prepared_this_epoch && client_transfers_prepared() &&
                client_migrations_.size() == planned && accepts_ready)
                srv_->flip_ack(self_->id(), stage);
            else if (prepared_this_epoch && client_transfers_prepared() &&
                     client_migrations_.size() != planned)
                srv_->flip_note_failure(
                    "ERR FLIP source connection set changed during reversible preflight");
        } else if (stage == FlipStage::ClientCommit &&
                   !srv_->flip_acked(self_->id(), stage)) {
            if (srv_->flip_source_clients(self_->id()))
                commit_prepared_client_transfers_impl<kEp>();
            srv_->flip_ack(self_->id(), stage);
        } else if (stage == FlipStage::ClientInstall &&
                   !srv_->flip_acked(self_->id(), stage)) {
            const bool converting_to_ex =
                srv_->flip_candidate_target(self_->id()) == Role::Ex;
            // EX is frozen now, so this second full IO drain is the transit barrier for notify,
            // pub/sub, tracking, and client-scatter messages which were posted just after a peer's
            // earlier IoDrain acknowledgement. The global event count prevents every consumer
            // from acknowledging while a payload is still between queues. Only a converting IO
            // closes its listener; surviving IO rings deliberately retain their armed accepts.
            if (flip_io_drained() && self_->client_transfers_quiesced() &&
                (!converting_to_ex || accepts_quiesced()))
                srv_->flip_ack(self_->id(), stage);
        } else if (stage == FlipStage::RoleReady &&
                   !srv_->flip_acked(self_->id(), stage)) {
            if (flip_pubsub_rehome_epoch_ != srv_->flip_epoch()) {
                flip_pubsub_rehome_epoch_ = srv_->flip_epoch();
                pubsub_rehome_local(flip_pubsub_rehome_epoch_);
            }
            if (self_->client_transfers_quiesced() &&
                self_->client_count() == srv_->flip_client_quota(self_->id()) &&
                srv_->pubsub_inflight() == 0)
                srv_->flip_ack(self_->id(), stage);
        } else if (stage == FlipStage::Rollback &&
                   !srv_->flip_acked(self_->id(), stage)) {
            const bool converting_to_ex =
                srv_->flip_candidate_target(self_->id()) == Role::Ex;
            if (converting_to_ex && accept_quiescing_ && !accepts_quiesced())
                quiesce_accepts_for_conversion();
            if (client_transfers_prepared() &&
                (!converting_to_ex || !accept_quiescing_ || accepts_quiesced())) {
                cancel_prepared_client_transfers_impl<kEp>();
                if (converting_to_ex && !restore_accepts_after_rollback()) return 1;
                srv_->flip_ack(self_->id(), stage);
            }
        }

        if (self_->id() != srv_->flip_coordinator()) return 1;

        std::string failure;
        if (stage < FlipStage::ClientCommit && stage != FlipStage::Rollback &&
            srv_->flip_timed_out())
            srv_->flip_note_failure("ERR FLIP timed out before the ownership commit");
        const bool failed = srv_->flip_failed(failure);
        if (failed && stage < FlipStage::ClientCommit && stage != FlipStage::Rollback) {
            flip_enter_rollback(failure);
            return 1;
        }

        switch (stage) {
            case FlipStage::IoDrain:
                if (srv_->flip_all_role_acked(Role::Ifid, stage)) {
                    if (!srv_->flip_build_client_plan(flip_client_, failure))
                        flip_enter_rollback(failure);
                    else
                        flip_publish_stage(FlipStage::IoPrepare);
                }
                break;
            case FlipStage::IoPrepare: {
                bool ex_io_prepared = true;
                for (uint32_t tid = 0; tid < srv_->nthreads(); tid++)
                    if (srv_->flip_candidate_target(tid) == Role::Ifid &&
                        !srv_->flip_acked(tid, stage)) ex_io_prepared = false;
                if (srv_->flip_all_role_acked(Role::Ifid, stage) && ex_io_prepared)
                    flip_publish_stage(FlipStage::ExDrain);
                break;
            }
            case FlipStage::ExDrain:
                if (srv_->flip_all_role_acked(Role::Ex, stage)) {
                    if (!srv_->flip_reserve_shard_plan(failure))
                        flip_enter_rollback(failure);
                    else
                        flip_publish_stage(FlipStage::ClientPrepare);
                }
                break;
            case FlipStage::ClientPrepare:
                if (srv_->flip_all_role_acked(Role::Ifid, stage))
                    flip_publish_stage(FlipStage::ClientCommit);
                break;
            case FlipStage::ClientCommit:
                if (srv_->flip_all_role_acked(Role::Ifid, stage))
                    flip_publish_stage(FlipStage::ClientInstall);
                break;
            case FlipStage::ClientInstall:
                if (srv_->flip_all_role_acked(Role::Ifid, stage))
                    flip_commit_roles_and_evacuate_shards();
                break;
            case FlipStage::RoleReady: {
                bool ready = true;
                for (uint32_t tid = 0; tid < srv_->nthreads(); tid++) {
                    const Role target = srv_->flip_candidate_target(tid);
                    if (target != Role::Idle && srv_->thread(tid).ready_role() != target)
                        ready = false;
                }
                if (ready) {
                    if (srv_->flip_all_role_acked(Role::Ifid, stage)) {
                        flip_publish_stage(FlipStage::ShardCommit);
                        flip_publish_stage(FlipStage::ExInstall);
                    }
                }
                break;
            }
            case FlipStage::ExInstall:
                if (srv_->flip_all_role_acked(Role::Ex, stage)) {
                    srv_->flip_complete_active();
                    finish_flip_command();
                }
                break;
            case FlipStage::Rollback:
                if (srv_->flip_all_role_acked(Role::Ifid, stage) &&
                    srv_->flip_all_role_acked(Role::Ex, stage)) {
                    srv_->flip_refuse_active();
                    finish_flip_command(failure.empty() ? "ERR FLIP refused" : failure.c_str());
                }
                break;
            default: break;
        }
        flip_wake_all();
        return 1;
    }

    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    void adopt_client(Client* c, bool unix_socket, bool tls_socket = false) {
        // ARMED ONCE FOR THIS OWNERSHIP TENURE. Both directions are edge triggered. Normal teardown
        // still lets ::close() deregister; migration alone pre-registers destination + rollback
        // interests and removes the old tenure's original registration around the owner edge.
        // Registration belongs here rather than at accept because an AF_UNIX connection can be
        // accepted by one IO thread and owned by another.
        if constexpr (kEp) {
            if (!set_nonblocking(c->fd()) ||
                !ep_.add(c->fd(), EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
                         ur_tag(UrKind::Recv, c))) {
                std::fprintf(stderr, "epoll registration failed for client fd %d\n", c->fd());
                self_->sig().accept_err++;
                self_->add_client(c);
                c->set_wb_slot(self_->assign_wb_slot(c));
                close_client(c);
                return;
            }
        }
        if (!unix_socket) {
            int one = 1;
            setsockopt(c->fd(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            const uint32_t interval = srv_->tcp_keepalive();
            if (interval) {
                setsockopt(c->fd(), SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
                const int idle = static_cast<int>(interval);
                const int intvl = std::max(1, idle / 3);
                const int count = 3;
                setsockopt(c->fd(), IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                setsockopt(c->fd(), IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                setsockopt(c->fd(), IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
            }
        }
        c->set_last_interaction_s(cached_now_s_ ? cached_now_s_
                                                : static_cast<uint32_t>(now_ns() / 1000000000ull));
        c->set_ifid_thread(self_->id());
        // The armed lane's cold counters belong to whichever thread owns the connection; this is
        // the one place ownership is taken (accept, AF_UNIX handoff, and migration rollback all
        // arrive here), and the migration edge re-points it for the destination.
        c->rob().set_read_local_arm_stats(self_->read_local_arm_stats_or_null());
        // The ready-mask slot is assigned immediately: WE are the sender, for life.
        c->set_wb_slot(self_->assign_wb_slot(c));
        self_->add_client(c);
        const std::string addr = socket_address(c->fd(), unix_socket, true);
        const std::string laddr = socket_address(c->fd(), unix_socket, false);
        const uint64_t accepted_ms = cached_now_ms_ ? cached_now_ms_ : now_ns() / 1000000ull;
        command_client_connected(c, addr.c_str(), laddr.c_str(), unix_socket, accepted_ms);
        climon_track_client(c);
        if (tls_socket) {
            TlsConn* tls = tls_slot_conn(c);
            if (tls && tls->fd_handshake()) {
                (void)drive_tls<kEp, Fused, Pipeline>(c);
                if (!c->closing() && tls->ktls()) arm_recv<kEp>(c);
                else if (!c->closing() && tls->memory_userspace())
                    arm_tls_recv<kEp, Fused, Pipeline>(c);
            } else {
                arm_tls_recv<kEp, Fused, Pipeline>(c);
            }
        } else arm_recv<kEp>(c);
        // Reachability, not optimism: if that arm starved for an SQE, nothing else names this
        // conn -- it would sit accepted and silent forever (audit finding). The active set's
        // phase-1 re-arms it until the recv lands; one wasted visit if the arm succeeded.
        mark_active_known<Fused && Pipeline != 0>(c);
    }

    uint32_t flush_handoffs() {
        uint32_t sent = 0;
        while (!pending_handoffs_.empty()) {
            Client* c = pending_handoffs_.front();
            ThreadCtx& target = srv_->thread(c->ifid_thread());
            if (!target.post_client(self_->id(), c, ring_, self_->sig())) break;
            pending_handoffs_.pop_front();
            sent++;
        }
        return sent;
    }

    template <bool HasTls, bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    void on_recv(Client* c, int res) {
        c->set_recv_armed(false);       // the kernel has released its pointer
        if (res > 0) self_->sig().net_input_bytes += static_cast<uint64_t>(res);
        if (find_client_migration(c)) {
            // The original Recv CQE, not the cancel CQE, is the old-ring pointer fence. Preserve
            // bytes which won the race with cancellation but do not parse them on the losing owner.
            if (res > 0) {
                c->commit_read(static_cast<size_t>(res));
                (void)finish_client_transfer<kEp>(c);
            } else if (res == -ECANCELED) {
                (void)finish_client_transfer<kEp>(c);
            } else {
                cancel_client_transfer<kEp>(c);
                close_client(c);
            }
            return;
        }
        // A send error can close the fd while this recv is still owned by io_uring. The Client stays
        // alive until this CQE arrives, but it is a corpse: positive bytes must not resurrect it by
        // parsing and dispatching new Tasks after the teardown quiescence fence.
        if (c->dead()) return;
        if (res <= 0) { close_client(c); return; }
        c->commit_read(static_cast<size_t>(res));
        c->set_last_interaction_s(cached_now_s_);
        if constexpr (Pipeline == 0) {
            if constexpr (HasTls) {
                if (c->is_tls())
                    parse_and_dispatch<true, Fused ? kGenthreadIfidBatchOps : 0>(c);
                else
                    parse_and_dispatch<false, Fused ? kGenthreadIfidBatchOps : 0>(c);
            } else {
                parse_and_dispatch<false, Fused ? kGenthreadIfidBatchOps : 0>(c);
            }
        }
        // Deliberately NOT re-armed here. flush_ready() re-arms AFTER it may have reset the read
        // buffer; arming first would leave the kernel holding a pointer that the reset then moves.
        mark_active_known<Fused && Pipeline != 0>(c);
    }

    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    bool drive_tls(Client* c) {
        TlsConn* tls = tls_slot_conn(c);
        if (!tls) return false;
        if (tls->ktls()) return true;

        if (tls->handshaking() && tls->fd_handshake()) {
            const TlsOp result = tls->handshake();
            if (result == TlsOp::WantRead || result == TlsOp::WantWrite) {
                if (result == TlsOp::WantRead) self_->sig().tls_want_read++;
                else self_->sig().tls_want_write++;
                arm_tls_socket_poll<kEp>(c, result);
                return true;
            }
            if (result == TlsOp::Progress) {
                self_->sig().tls_handshakes_completed++;
                if (tls->ktls()) self_->sig().tls_ktls_active++;
                else self_->sig().tls_ktls_fallback++;
            } else {
                self_->sig().tls_handshakes_failed++;
                if (!tls->last_error().empty())
                    std::fprintf(stderr, "TLS client %llu: %s\n",
                                 static_cast<unsigned long long>(c->id()),
                                 tls->last_error().c_str());
                close_client(c);
                return false;
            }
            if (tls->ktls()) return true;
        }

        if (tls->socket_userspace() && tls->has_pinned_plain()) {
            (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
            if (tls->has_pinned_plain()) {
                arm_tls_socket_poll<kEp>(c, tls->wanted());
                return true;
            }
        }

        if (tls->output_pending() || c->send_inflight()) {
            (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
            return !tls->failed();
        }

        if (tls->handshaking()) {
            const TlsOp result = tls->handshake();
            if (result == TlsOp::WantRead) self_->sig().tls_want_read++;
            else if (result == TlsOp::WantWrite) self_->sig().tls_want_write++;
            else if (result == TlsOp::Progress) {
                self_->sig().tls_handshakes_completed++;
                self_->sig().tls_ktls_fallback++;
            }
            (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(
                *c, *tls);  // alerts and handshake flights are flushed first
            if (result == TlsOp::Error || result == TlsOp::GracefulEof) {
                self_->sig().tls_handshakes_failed++;
                if (!tls->last_error().empty())
                    std::fprintf(stderr, "TLS client %llu: %s\n",
                                 static_cast<unsigned long long>(c->id()),
                                 tls->last_error().c_str());
                close_client(c, tls->output_pending() || c->send_inflight());
                return false;
            }
            if (!tls->connected() || tls->output_pending() || c->send_inflight()) return true;
        }

        [[maybe_unused]] bool decrypted = false;
        while (tls->connected()) {
            size_t avail = 0;
            bool may_grow = c->rob().quiesced();
            char* dst = c->read_space(
                kRecvChunk, avail, may_grow, proto_max_bulk_len_);
            if (!dst) break;
            const TlsIoResult result = tls->read_plain(dst, avail);
            if (result.op == TlsOp::Progress) {
                // Only decrypted bytes enter the RESP buffer. Ciphertext counts are committed to
                // the BIO in on_tls_recv and can never reach this cursor.
                c->commit_read(result.bytes);
                self_->sig().tls_plaintext_input_bytes += result.bytes;
                decrypted = true;
                if (tls->output_pending()) {
                    (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
                    break;
                }
                continue;
            }
            if (result.op == TlsOp::WantRead) {
                self_->sig().tls_want_read++;
                if (tls->socket_userspace()) arm_tls_socket_poll<kEp>(c, result.op);
            }
            else if (result.op == TlsOp::WantWrite) {
                self_->sig().tls_want_write++;
                if (tls->socket_userspace()) arm_tls_socket_poll<kEp>(c, result.op);
                else (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
            } else if (result.op == TlsOp::GracefulEof) {
                (void)tls->shutdown();
                (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
                close_client(c, tls->output_pending() || c->send_inflight());
                return false;
            } else {
                if (!tls->last_error().empty())
                    std::fprintf(stderr, "TLS client %llu: %s\n",
                                 static_cast<unsigned long long>(c->id()),
                                 tls->last_error().c_str());
                (void)wb_.pump_tls<kEp, Fused && Pipeline == 1>(*c, *tls);
                close_client(c, tls->output_pending() || c->send_inflight());
                return false;
            }
            break;
        }
        if constexpr (Pipeline == 0) {
            if (decrypted || c->rpos() < c->rlen())
                parse_and_dispatch<true, Fused ? kGenthreadIfidBatchOps : 0>(c);
        }
        return !tls->failed();
    }

    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    void on_tls_socket_poll(Client* c, int res, TlsOp wanted) {
        TlsConn* tls = tls_slot_conn(c);
        if (!tls) { close_client(c); return; }
        tls->set_poll_armed(wanted, false);
        c->set_recv_armed(tls->any_poll_armed());
        if (c->dead()) return;
        if (c->closing() || res < 0) { close_client(c); return; }
        (void)drive_tls<kEp, Fused, Pipeline>(c);
        mark_active_known<Fused && Pipeline != 0>(c);
    }

    template <bool kEp, bool Fused = false, uint8_t Pipeline = 0>
    void on_tls_recv(Client* c, int res) {
        c->set_recv_armed(false);
        if (res > 0) self_->sig().net_input_bytes += static_cast<uint64_t>(res);
        TlsConn* tls = tls_engine(c);
        if (!tls) { close_client(c); return; }
        if (c->dead()) { tls->abandon_input(); return; }
        if (res <= 0) {
            tls->abandon_input();
            close_client(c);
            return;
        }
        if (!tls->commit_input(static_cast<size_t>(res))) {
            std::fprintf(stderr, "TLS client %llu: ciphertext BIO commit rejected %d bytes\n",
                         static_cast<unsigned long long>(c->id()), res);
            close_client(c);
            return;
        }
        self_->sig().tls_ciphertext_input_bytes += static_cast<uint64_t>(res);
        c->set_last_interaction_s(cached_now_s_);
        (void)drive_tls<kEp, Fused, Pipeline>(c);
        mark_active_known<Fused && Pipeline != 0>(c);
    }

    static bool read_local_mget(const Op& op) {
        return op.spec && command_is_read_local_mget(*op.spec);
    }

    static void read_local_clear_reply(Op& op) {
        op.clear_reply();       // bytes AND the reply code
        op.zc_ptr = nullptr;
        op.zc_len = 0;
        op.zc_shard = -1;
    }

    // ATTRIBUTION FOR A DEMOTED READ, on the demote path only. Arm-on-demand has two distinct
    // reasons to hold a read back and they must not be summed into one counter: an explicit key
    // conflict with a live write descriptor (InflightWrite, the steady state), and the ARMING
    // TRANSIENT -- writes this connection published before any local read armed it, which no
    // descriptor describes and which are therefore fenced wholesale until they retire. The second
    // is per-connection and one-shot; a bench arm whose ArmTransient count keeps growing has an
    // arming bug, which is exactly what makes reporting it separately worth a counter.
    static ReadLocalFallbackReason read_local_write_fallback_reason(
            const Rob<kRobWindow>& rob) {
        return rob.read_local_arm_fence_live()
            ? ReadLocalFallbackReason::ArmTransient
            : ReadLocalFallbackReason::InflightWrite;
    }

    // A demotion is planned before the stateful MONITOR/tracking gate. A complete plan is not
    // published until ACL and FLIP admit the current frame; per-producer reservations make that
    // later commit infallible. If the existing scatter snapshot window accepts only a prefix, that
    // prefix contains exclusively older, already-admitted reads and is committed before the
    // current frame crosses any stateful hook; the unconsumed frame is then safe to reparse.
    // MGET remains one ROB operation, but its cold fallback expands here through the unchanged
    // scatter planner into one owner Task per touched shard.
    class ReadLocalDemotionPlan {
    private:
        enum class ReadKind : uint8_t { Ordinary, Scatter, Error };

        struct Storage {
            uint64_t ids[kRobWindow];
            ScatterState* scatter[kRobWindow];
            uint16_t scatter_tasks[kRobWindow];
            ReadKind kinds[kRobWindow];
            ReadLocalFallbackReason reasons[kRobWindow];
            uint32_t owners[kMaxThreads];
            uint32_t remaining[kMaxThreads];
        };

    public:
        ReadLocalDemotionPlan() = default;
        ~ReadLocalDemotionPlan() { cancel(); }
        ReadLocalDemotionPlan(const ReadLocalDemotionPlan&) = delete;
        ReadLocalDemotionPlan& operator=(const ReadLocalDemotionPlan&) = delete;

        bool prepare(IoLoop& loop, Client* client, uint64_t hash,
                     bool require_hash_match, int32_t reserve_shard = -1,
                     ReadLocalFallbackReason reason =
                         ReadLocalFallbackReason::ContextOwnerKey,
                     bool reserve_current_without_reads = false,
                     const uint64_t* fallback_ids = nullptr,
                     const ReadLocalFallbackReason* fallback_reasons = nullptr,
                     uint32_t fallback_count = 0,
                     const Op* intersect_command = nullptr,
                     bool intersect_filter_miss = false) {
            if (loop_ || !client) std::abort();
            if (reason == ReadLocalFallbackReason::None) std::abort();
            if ((fallback_ids == nullptr) != (fallback_reasons == nullptr) ||
                (fallback_count != 0) != (fallback_ids != nullptr) ||
                (fallback_count && intersect_command) ||
                (intersect_filter_miss && !intersect_command)) std::abort();
            Rob<kRobWindow>& rob = client->rob();
            const bool reserve_current =
                reserve_current_without_reads && reserve_shard >= 0;
            // A caller that already walked a complete precise keyset may pass its authoritative
            // pending-filter miss. Consume it before reloading the same filter; hit/unknown remains
            // false and takes the unchanged exact-selection path below.
            if (!reserve_current && !fallback_count && intersect_filter_miss) return true;
            // Current intersect callers also passed a known hit when they reach the exact path.
            // Unknown remains safe without this shortcut: collecting an empty set returns below.
            if (!intersect_command && !rob.has_pending_read_local() && !reserve_current)
                return true;
            // Superset pre-check (ReadLocalPendingFilter, rob.h): the filter holds every key hash
            // any still-pending local read of this connection can touch, so a miss PROVES that no
            // pending read shares the point hash / any declared key of the exact command. The
            // ordinary disjoint write returns here without allocating or walking anything; a hit
            // runs the unchanged exact plan below. EX seeds are selected by id, and a reserved
            // current op needs the plan regardless of reads, so neither consults the filter.
            if (!reserve_current && !fallback_count && !intersect_command &&
                require_hash_match && !rob.read_local_pending_may_touch(hash)) return true;
            // Select on the stack; Storage is only paid for once a read is actually demoted or
            // the current op must be reserved.
            uint64_t ids[kRobWindow];
            ReadLocalFallbackReason reasons[kRobWindow];
            bool selected[kRobWindow] = {};
            const uint32_t pending =
                rob.collect_pending_read_local(0, false, ids, kRobWindow);
            for (uint32_t i = 0; i < pending; i++) reasons[i] = reason;
            bool selective = false;
            bool any_selected = false;
            // The key-walking modes visit every pending read, so they also recompute the exact
            // pending-key filter and hand it back: false hits never accumulate.
            ReadLocalPendingFilter exact;
            bool exact_known = false;
            if (pending && fallback_count) {
                selective = true;
                for (uint32_t seed = 0; seed < fallback_count; seed++) {
                    if (fallback_reasons[seed] == ReadLocalFallbackReason::None) continue;
                    bool found = false;
                    for (uint32_t i = 0; i < pending; i++) {
                        if (ids[i] != fallback_ids[seed]) continue;
                        selected[i] = true;
                        any_selected = true;
                        reasons[i] = fallback_reasons[seed];
                        found = true;
                        break;
                    }
                    if (!found) std::abort();
                }
            } else if (pending && intersect_command) {
                selective = true;
                exact_known = true;
                for (uint32_t i = 0; i < pending; i++) {
                    selected[i] = read_local_commands_overlap_precise_keyset_collect(
                        rob.at(ids[i]), *intersect_command, exact);
                    any_selected |= selected[i];
                }
            } else if (pending && require_hash_match) {
                selective = true;
                exact_known = true;
                for (uint32_t i = 0; i < pending; i++) {
                    selected[i] = read_local_command_touches_hash_collect(
                        rob.at(ids[i]), hash, exact);
                    any_selected |= selected[i];
                }
            }
            if (exact_known) rob.reset_read_local_pending_filter(exact);
            if (selective && any_selected) {
                // MGET makes key overlap a graph rather than a single hash. Close every selective
                // parser or EX seed transitively: if a selected MGET touches an otherwise unrelated
                // key, its older/later local read of that key must join the same owner wave too.
                // With nothing selected the closure is the identity, so it is skipped.
                bool changed;
                do {
                    changed = false;
                    for (uint32_t i = 0; i < pending; i++) {
                        if (selected[i]) continue;
                        const Op& candidate = rob.at(ids[i]);
                        for (uint32_t prior = 0; prior < pending; prior++) {
                            if (!selected[prior] ||
                                !read_local_commands_overlap(
                                    candidate, rob.at(ids[prior]))) continue;
                            selected[i] = true;
                            reasons[i] = ReadLocalFallbackReason::ContextOwnerKey;
                            changed = true;
                            break;
                        }
                    }
                } while (changed);
            }
            uint32_t count = pending;
            if (selective) {
                uint32_t out = 0;
                for (uint32_t i = 0; i < pending; i++) {
                    if (!selected[i]) continue;
                    ids[out] = ids[i];
                    reasons[out] = reasons[i];
                    out++;
                }
                count = out;
            }
            if (!count && !reserve_current) return true;
            storage_.reset(new (std::nothrow) Storage);
            if (!storage_) return false;
            count_ = count;
            for (uint32_t i = 0; i < count_; i++) {
                storage_->ids[i] = ids[i];
                storage_->reasons[i] = reasons[i];
            }

            loop_ = &loop;
            client_ = client;
            for (uint32_t i = 0; i < count_; i++) {
                storage_->kinds[i] = ReadKind::Ordinary;
                storage_->scatter[i] = nullptr;
                storage_->scatter_tasks[i] = 0;
            }
            for (uint32_t i = 0; i < count_; i++) {
                Op& op = rob.at(storage_->ids[i]);
                if (read_local_mget(op)) {
                    read_local_clear_reply(op);
                    ScatterDispatch dispatch;
                    const ScatterPrepare prepared = xshard_prepare(
                        *loop.srv_, op, loop.scatter_pool_, loop.self_->id(),
                        client->id(), dispatch, false, client);
                    if (prepared == ScatterPrepare::Backpressure) {
                        // A local run can contain more cross-shard reads than the existing
                        // snapshot window admits concurrently. Publish the already prepared ROB
                        // prefix as one ordered wave and leave this op plus the exact selected
                        // remainder tagged in the local lane. The next EX pass demotes only entries
                        // that overlap the unfinished owner wave; parser callers reparse their
                        // unconsumed current frame after the same prefix commit.
                        if (i) {
                            const uint32_t selected_count = count_;
                            partial_begin_ = i;
                            partial_count_ = selected_count - i;
                            count_ = i;
                            partial_ = true;
                            break;
                        }
                        cancel();
                        return false;
                    }
                    if (prepared == ScatterPrepare::Error) {
                        storage_->kinds[i] = ReadKind::Error;
                        continue;
                    }
                    if (prepared == ScatterPrepare::Ready) {
                        storage_->kinds[i] = ReadKind::Scatter;
                        storage_->scatter[i] = dispatch.state;
                        storage_->scatter_tasks[i] = dispatch.nshards;
                        for (uint32_t task = 0; task < dispatch.nshards; task++) {
                            const int32_t shard = xshard_dispatch_shard(dispatch, task);
                            if (shard < 0) std::abort();
                            add_owner(loop.srv_->worker_of_shard(shard));
                        }
                        continue;
                    }
                }
                if (op.shard < 0) std::abort();
                add_owner(loop.srv_->worker_of_shard(op.shard));
            }
            if (!partial_ && reserve_shard >= 0) {
                reserved_current_worker_ = static_cast<int32_t>(
                    loop.srv_->worker_of_shard(reserve_shard));
                add_owner(static_cast<uint32_t>(reserved_current_worker_));
            }

            uint32_t reserved = 0;
            for (; reserved < nowners_; reserved++) {
                if (!loop.srv_->thread(storage_->owners[reserved]).reserve_task_slots(
                        loop.self_->id(), storage_->remaining[reserved]))
                    break;
            }
            if (reserved != nowners_) {
                for (uint32_t i = 0; i < reserved; i++)
                    loop.srv_->thread(storage_->owners[i]).cancel_task_reservation(
                        loop.self_->id(), storage_->remaining[i]);
                discard_prepared_reads();
                clear();
                return false;
            }
            reservations_live_ = true;
            return true;
        }

        bool active() const { return loop_ != nullptr; }
        bool current_reserved() const { return reserved_current_worker_ >= 0; }
        bool partial() const { return partial_; }
        uint32_t read_count() const { return count_; }

        void commit_reads() {
            if (!loop_) return;
            Rob<kRobWindow>& rob = client_->rob();
            bool completed_locally = false;
            for (uint32_t i = 0; i < count_; i++) {
                Op& op = rob.at(storage_->ids[i]);
                if (storage_->kinds[i] == ReadKind::Error) {
                    rob.complete_pending_read_local(storage_->ids[i]);
                    op.state.store(OpState::Done, std::memory_order_release);
                    completed_locally = true;
                    continue;
                }
                if (storage_->kinds[i] == ReadKind::Scatter) {
                    ScatterState* state = storage_->scatter[i];
                    if (!state || !storage_->scatter_tasks[i]) std::abort();
                    ScatterDispatch dispatch;
                    dispatch.state = state;
                    dispatch.nshards = storage_->scatter_tasks[i];
                    op.attach_scatter_state(state);
                    storage_->scatter[i] = nullptr;  // the Op/IO retirement path owns it now
                    loop_->self_->note_command(op.spec->id);
                    for (uint32_t task = 0; task < dispatch.nshards; task++) {
                        const int32_t shard = xshard_dispatch_shard(dispatch, task);
                        const uint32_t worker = loop_->srv_->worker_of_shard(shard);
                        loop_->srv_->thread(worker).post_task_reserved_quiet(
                            loop_->self_->id(),
                            Task{client_, storage_->ids[i], shard, state},
                            loop_->self_->sig());
                        consume(worker);
                        loop_->touch_worker(worker);
                    }
                } else {
                    const uint32_t worker = loop_->srv_->worker_of_shard(op.shard);
                    loop_->srv_->thread(worker).post_task_reserved_quiet(
                        loop_->self_->id(),
                        Task{client_, storage_->ids[i], -1, nullptr},
                        loop_->self_->sig());
                    consume(worker);
                    loop_->touch_worker(worker);
                }
                rob.publish_pending_read_local_to_owner(storage_->ids[i]);
            }
            // A partial plan's suffix is still local. Publish its exact fallback tags only after
            // the prepared prefix is irrevocable; cancel/backpressure before commit must leave the
            // lane untouched so a later EX pass can probe it normally.
            for (uint32_t i = 0; i < partial_count_; i++) {
                const uint32_t pending = partial_begin_ + i;
                loop_->fused_executor_->preserve_local_read_fallback(
                    client_, storage_->ids[pending], storage_->reasons[pending]);
            }
            if (count_) {
                ReadLocalStats& stats = loop_->self_->read_local_stats();
                for (uint32_t i = 0; i < count_; i++) {
                    loop_->fused_executor_->note_local_read_demoted(
                        rob.at(storage_->ids[i]));
                    stats.note_fallback(
                        storage_->reasons[i], read_local_mget(rob.at(storage_->ids[i])));
                }
            }
            if (completed_locally)
                loop_->fused_executor_completion<false>(client_);
            count_ = 0;  // every prepared scatter/marker is now owned by its published Op
            if (reserved_current_worker_ < 0) {
                for (uint32_t i = 0; i < nowners_; i++)
                    if (storage_->remaining[i]) std::abort();
                clear();
            }
        }

        void post_current(const Task& task, uint32_t worker) {
            if (!loop_ || reserved_current_worker_ != static_cast<int32_t>(worker))
                std::abort();
            loop_->srv_->thread(worker).post_task_reserved_quiet(
                loop_->self_->id(), task, loop_->self_->sig());
            consume(worker);
            loop_->touch_worker(worker);
            for (uint32_t i = 0; i < nowners_; i++)
                if (storage_->remaining[i]) std::abort();
            clear();
        }

    private:
        void add_owner(uint32_t worker) {
            uint32_t at = 0;
            while (at != nowners_ && storage_->owners[at] != worker) at++;
            if (at == nowners_) {
                if (nowners_ == kMaxThreads) std::abort();
                storage_->owners[nowners_] = worker;
                storage_->remaining[nowners_] = 0;
                nowners_++;
            }
            storage_->remaining[at]++;
        }

        void consume(uint32_t worker) {
            uint32_t at = 0;
            while (at != nowners_ && storage_->owners[at] != worker) at++;
            if (at == nowners_ || !storage_->remaining[at]) std::abort();
            storage_->remaining[at]--;
        }

        void discard_prepared_reads() {
            if (!loop_ || !client_) return;
            Rob<kRobWindow>& rob = client_->rob();
            for (uint32_t i = 0; i < count_; i++) {
                Op& op = rob.at(storage_->ids[i]);
                if (storage_->scatter[i]) {
                    xshard_abandon_unpublished(storage_->scatter[i], loop_->scatter_pool_,
                                               loop_->self_->id());
                    storage_->scatter[i] = nullptr;
                }
                if (read_local_mget(op)) read_local_clear_reply(op);
            }
        }

        void cancel() {
            if (!loop_) return;
            if (reservations_live_)
                for (uint32_t i = 0; i < nowners_; i++)
                    if (storage_->remaining[i])
                        loop_->srv_->thread(storage_->owners[i]).cancel_task_reservation(
                            loop_->self_->id(), storage_->remaining[i]);
            discard_prepared_reads();
            clear();
        }

        void clear() {
            loop_ = nullptr;
            client_ = nullptr;
            count_ = nowners_ = 0;
            partial_begin_ = partial_count_ = 0;
            reserved_current_worker_ = -1;
            reservations_live_ = false;
            partial_ = false;
            storage_.reset();
        }

        IoLoop* loop_ = nullptr;
        Client* client_ = nullptr;
        std::unique_ptr<Storage> storage_;
        uint32_t count_ = 0;
        uint32_t nowners_ = 0;
        uint32_t partial_begin_ = 0;
        uint32_t partial_count_ = 0;
        int32_t reserved_current_worker_ = -1;
        bool reservations_live_ = false;
        bool partial_ = false;
    };

public:
    bool fused_demote_local_read_batch(Client* client, const uint64_t* probed,
                                       const ReadLocalFallbackReason* fallbacks,
                                       uint32_t probed_count, uint32_t& demoted) {
        ReadLocalDemotionPlan plan;
        if (!plan.prepare(
                *this, client, 0, false, -1,
                ReadLocalFallbackReason::ContextOwnerKey, false,
                probed, fallbacks, probed_count)) return false;
        demoted = plan.read_count();
        if (!demoted) std::abort();
        plan.commit_reads();
        (void)flush_ifid_posts();
        return true;
    }

private:

    struct EmptyReadLocalDemotionPlan {};

    void touch_worker(uint32_t worker) {
        if (touched_[worker]) return;
        touched_[worker] = true;
        touched_list_[ntouched_++] = worker;
    }

    // ---- parse -> route -> publish -----------------------------------------------------------------
    template <bool NoBorrow, uint32_t BatchOps = 0, bool IoPipe = false,
              bool TargetedIfid = false,
              bool SuppressOrdinaryActiveMark = false,
              bool IofusedPrivateQueue = false>
    DispatchResult parse_and_dispatch(Client* c) {
        static constexpr bool Fused =
            BatchOps == kGenthreadIfidBatchOps &&
            !IoPipe && !TargetedIfid &&
            !SuppressOrdinaryActiveMark && !IofusedPrivateQueue;
        [[maybe_unused]] const bool read_local_enabled =
            Fused && __builtin_expect(srv_->read_local_enabled(), false);
        Client& conn = *c;
        Rob<kRobWindow>& rob = c->rob();
        LoopSignals& sig = self_->sig();
        const uint32_t pass_rpos = conn.rpos();
        const char* const pass_rbuf = conn.rbuf();
        const uint32_t pass_rlen = conn.rlen();
        const uint32_t self_id = self_->id();
        auto task_free_slots = [&](ThreadCtx& owner) {
            if constexpr (IofusedPrivateQueue) {
                return owner.iofused_task_free_slots(self_id);
            } else {
                return owner.task_free_slots(self_id);
            }
        };
        auto post_task_quiet = [&](ThreadCtx& owner, const Task& task) {
            if constexpr (IofusedPrivateQueue) {
                return owner.post_iofused_task_quiet(self_id, task, sig);
            } else {
                return owner.post_task_quiet(self_id, task, sig);
            }
        };
        auto post_tasks_quiet = [&](ThreadCtx& owner, const Task* tasks, uint32_t count) {
            if constexpr (IofusedPrivateQueue) {
                return owner.post_iofused_tasks_quiet(self_id, tasks, count, sig);
            } else {
                return owner.post_tasks_quiet(self_id, tasks, count, sig);
            }
        };
        DispatchResult result = DispatchResult::Progress;
        bool head_candidate = true;   // only the pass's FIRST dispatch can be the direct head
        const uint8_t security_flags = srv_->security_flags();
        const bool auth_required = (security_flags & Server::kSecurityAuth) != 0;
        const bool acl_active = (security_flags & Server::kSecurityAcl) != 0;
        const bool notify_armed = notify_armed_;
        const uint64_t pass_max_bulk_len = proto_max_bulk_len_;
        const bool default_bulk_limit = pass_max_bulk_len == 512ull * 1024 * 1024;
        // IoDrain waits for this whole parse/post pass before opening ExDrain. A task whose
        // owner was sampled here therefore reaches that owner before it can acknowledge.
        const bool lb_pause_this_pass = lb_controller_armed_ &&
            srv_->lb_should_pause(self_id, c->id());
        if (__builtin_expect(lb_pause_this_pass, false)) {
            flip_fingerprint_finish_pass();
            return result;
        }
        // ONE epoch for the whole parse pass, not one per op. Monotonicity needs the stamps to be
        // non-decreasing along the connection, not distinct: every op this pass parses may share
        // the pass's cut, and the next pass's cut is >= this one because the sequence only moves
        // forward. The freshness floor survives the fold -- the pass starts after the bytes it
        // parses arrived, so no read is ever older than its own arrival, which is what keeps the
        // disjoint-window case (a writer's reply fully precedes the reader's send) correct.
        // With --atomic 0 the tracking word is never written by anyone, so this is one L1 hit on a
        // shared-clean line, the sequence load never happens, and the per-op test is never taken.
        const bool atomic_tracking = srv_->atomic_tracking_active();
        const uint64_t pass_read_cut = atomic_tracking ? srv_->atomic_snapshot() : 0;
        uint64_t batch_start_ops = 0;
        [[maybe_unused]] bool read_local_batch = false;
        // LANE ADMISSION (P128.md): how many lane slots (pending local reads) this connection may
        // hold during this pass. UINT32_MAX unless the thread's local-read lane is under pressure,
        // in which case it is the lane divided among the active connections
        // (ExLoop::read_local_lane_quota). The pressure byte is tested FIRST and on its own,
        // because active_.size() would otherwise be evaluated eagerly on every pass and active_
        // lives on an IoLoop cache line this pass never otherwise touches -- see
        // read_local_lane_under_pressure(). Steady state: one byte test per pass on a line the pass
        // already owns; the per-op compare below is a popcount against a register.
        [[maybe_unused]] uint32_t read_local_quota = UINT32_MAX;
        if constexpr (Fused)
            if (read_local_enabled &&
                __builtin_expect(fused_executor_->read_local_lane_under_pressure(), false))
                read_local_quota = fused_executor_->read_local_lane_quota(active_.size());
        if constexpr (BatchOps != 0) batch_start_ops = sig.ops;
        [[maybe_unused]] uint64_t batch_dispatch_start = 0;
        if constexpr (IoPipe) batch_dispatch_start = rob.dispatch_id();
        for (;;) {
            if constexpr (IoPipe)
                if (rob.dispatch_id() - batch_dispatch_start >=
                    kIoPipeIfidBatchOpsPerClient) break;
            if constexpr (BatchOps != 0)
                if (sig.ops - batch_start_ops >= BatchOps) break;
            // The coordinator's own connection already holds the unfinished FLIP head. Do not
            // parse behind it. Other connections may still parse the FLIP report/control command
            // below so live-vs-target remains observable while the dispatch barrier is active.
            if (__builtin_expect(srv_->flip_dispatch_paused() && c == flip_client_, false)) break;
            if (c->scatter_barrier() || c->parse_backpressure()) break;
            if constexpr (Fused)
                if (read_local_enabled && rob.local_mget_fence_pending()) break;
            Op* op;
            if constexpr (Fused) {
                op = read_local_enabled
                    ? rob.acquire_read_local(conn.op_route_flags())
                    : rob.acquire<true>(conn.op_route_flags());   // coded replies: fused only
            } else {
                op = rob.acquire<false>(conn.op_route_flags());   // 2s keeps the byte path
            }
            if (!op) break;                    // window full: backpressure; let replies drain first
            using DemotionPlan = std::conditional_t<
                Fused, ReadLocalDemotionPlan, EmptyReadLocalDemotionPlan>;
            [[maybe_unused]] DemotionPlan read_local_demotion;
            [[maybe_unused]] bool read_local_owner_conflict = false;
            [[maybe_unused]] ReadLocalFallbackReason read_local_owner_conflict_reason =
                ReadLocalFallbackReason::None;
            [[maybe_unused]] ReadLocalFallbackReason read_local_fallback_reason =
                ReadLocalFallbackReason::None;
            [[maybe_unused]] bool extend_read_local_batch = false;
            [[maybe_unused]] bool read_local_mget_candidate = false;
            [[maybe_unused]] bool read_local_write_hazard = false;
            [[maybe_unused]] bool read_local_eligible_decided = false;
            [[maybe_unused]] bool read_local_eligible = false;
            [[maybe_unused]] bool read_local_commit_at_ordinary = false;
            [[maybe_unused]] bool read_local_commit_before_lowering = false;
            [[maybe_unused]] bool read_local_point_prehashed = false;
            // Keys a local read hands to Rob::mark_current_read_local: every MGET key hashed by
            // the eligibility walk below (no second hashing pass). A point read reports exactly
            // one hash and reaches mark_current_read_local_hash() without building a summary.
            [[maybe_unused]] ReadLocalPendingFilter read_local_pending_keys{};
            // Owner-task demand this read would need if demoted: GET one, MGET its touched-shard
            // bound. Computed once here; the lane room gate and the lane append both consume it.
            [[maybe_unused]] uint32_t read_local_lane_demand = 1;
            uint32_t pos = conn.rpos();
            const char* err = nullptr;
            op->rbuf_off = pos;
            // The authenticated/common path takes the constant-folded parser; only pre-AUTH
            // connections (predicted false) pay real limit arguments. See resp.h for the
            // +83 instr/op lesson behind this split.
            bool security_check = auth_required && !conn.authenticated();
            ParseResult pr;
            if (__builtin_expect(security_check, false)) {
                pr = resp_parse_limited(
                    pass_rbuf, pass_rlen, pos, *op, &err, 10, 16384);
            } else if (__builtin_expect(default_bulk_limit, true)) {
                pr = resp_parse(pass_rbuf, pass_rlen, pos, *op, &err);
            } else {
                pr = resp_parse_limited(pass_rbuf, pass_rlen, pos, *op, &err,
                                        1024 * 1024, pass_max_bulk_len);
            }
            security_check |= acl_active;

            if (pr == ParseResult::Incomplete) {
                // No complete frame was consumed in this pass. The buffered tail is deliberately
                // left in place; only a new recv/readability completion may make it actionable.
                if (conn.rpos() == pass_rpos) result = DispatchResult::NeedInput;
                break;
            }
            if (pr == ParseResult::Error) {
                finish_locally(c, *op, err ? err : "ERR protocol error");
                conn.advance_parse(pass_rlen - conn.rpos());
                c->mark_closing();
                result = DispatchResult::Error;
                break;
            }
            // The parse cursor is deliberately NOT advanced here. It advances only once this op is
            // certain to be answered — see the dispatch-refusal path below for why.
            const uint32_t consumed = pos - conn.rpos();

            const CommandSpec* spec = command_lookup(op->cmd_name());
            if (!spec) {
                conn.advance_parse(consumed);
                // Redis names the command and echoes the first arguments; client libraries and
                // humans both read this line to tell a typo from an unsupported command. The
                // shape is exact: each argument is quoted and followed by one space, the argument
                // list stops once it reaches 128 bytes, and the argument that crosses that line
                // is truncated to the remaining budget (so the trailing space is always there,
                // even with no arguments at all).
                char message[512];
                const Slice name = op->cmd_name();
                int used = std::snprintf(message, sizeof(message),
                                         "ERR unknown command '%.*s', with args beginning with: ",
                                         static_cast<int>(name.n), name.p);
                if (used < 0 || static_cast<size_t>(used) >= sizeof(message))
                    used = static_cast<int>(sizeof(message)) - 1;
                const int args_begin = used;
                for (uint32_t i = 1; i < op->argc(); i++) {
                    const int args_len = used - args_begin;
                    if (args_len >= 128) break;
                    const int budget = 128 - args_len;
                    const int room = static_cast<int>(sizeof(message)) - used;
                    const int wrote = std::snprintf(
                        message + used, static_cast<size_t>(room), "'%.*s' ",
                        static_cast<int>(std::min<uint32_t>(op->arg(i).n,
                                                            static_cast<uint32_t>(budget))),
                        op->arg(i).p);
                    if (wrote < 0 || wrote >= room) { used = static_cast<int>(sizeof(message)) - 1; break; }
                    used += wrote;
                }
                finish_locally(c, *op, message); continue;
            }
            if (!command_arity_ok(*spec, op->argc())) {
                // Routed containers and SLOWLOG keep a broad container bound in the registry so
                // malformed requests are rejected before ACL and MULTI. On this already-taken
                // cold error path, recover Redis's more specific subcommand name/grammar.
                const bool container_reply = command_reply_container_outer_arity(*op, *spec);
                conn.advance_parse(consumed);
                if (container_reply) {
                    finish_prebuilt(c, *op);
                    continue;
                }
                char message[128];
                char command[64];
                const size_t name_len = std::min(std::strlen(spec->name), sizeof(command) - 1);
                for (size_t i = 0; i < name_len; i++) {
                    const char ch = spec->name[i];
                    command[i] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
                }
                command[name_len] = '\0';
                std::snprintf(message, sizeof(message),
                              "ERR wrong number of arguments for '%s' command", command);
                finish_locally(c, *op, message); continue;
            }
            if ((spec->flags & CmdFlags::SubcmdRoute) &&
                command_reply_container_subcommand_arity(*op, *spec)) {
                // A known child has its own generated arity and Redis checks that before ACL and
                // MULTI. Unknown children deliberately continue: on an unauthenticated connection
                // XGROUP x is parent-arity-valid and must reach NOAUTH.
                conn.advance_parse(consumed);
                finish_prebuilt(c, *op);
                continue;
            }
            if constexpr (Fused) {
                // A consecutive GET can reuse the first member's stable connection gates. Any
                // other frame ends the run but keeps parsing; later writes demote only conflicting
                // unresolved reads instead of turning the run into a connection-wide hold.
                if (read_local_enabled && read_local_batch) {
                    extend_read_local_batch =
                        (spec->flags & CmdFlags::ReadLocalEligible) &&
                        !command_is_read_local_mget(*spec) &&
                        conn.multi_session() == nullptr &&
                        rob.pending_read_local_count() < read_local_quota &&
                        fused_executor_->local_read_lane_has_room();
                    if (!extend_read_local_batch) read_local_batch = false;
                }
            }
            // THE SOLE DISABLED-STATE FEATURE DECISION on an ordinary operation. The executor
            // receives a spec whose handler pointer is already the clean or armed specialization;
            // no notification mask load reaches its execute path.
            //
            // Lane F rides this ONE branch rather than adding its own. notify_armed_ is the union
            // of "keyspace notifications configured" and "some CLIENT/MONITOR/TRACKING feature is
            // armed" (see climon_refresh_armed), so with everything off the emitted code is
            // byte-for-byte the pre-lane sequence: one predicted-not-taken test, then the tls
            // variant select and the spec store. The armed side pays a cold out-of-line call.
            if (__builtin_expect(notify_armed, false)) {
                spec = command_notify_variant(spec);
                if constexpr (NoBorrow) spec = command_tls_variant(spec);
                op->spec = spec;
            } else {
                if constexpr (NoBorrow) spec = command_tls_variant(spec);
                op->spec = spec;
            }
            if constexpr (Fused) {
                if (read_local_enabled) {
                    constexpr uint32_t kWriteHazards =
                        CmdFlags::Write | CmdFlags::SnapshotWrite |
                        CmdFlags::Transaction | CmdFlags::ScriptRoute;
                    constexpr uint32_t kNonPointRoutes =
                        CmdFlags::AllShards | CmdFlags::RandomShard |
                        CmdFlags::CursorShard | CmdFlags::ConfigRoute |
                        CmdFlags::MultiShard | CmdFlags::ScriptRoute |
                        CmdFlags::Blocking | CmdFlags::Transaction |
                        CmdFlags::StreamRoute | CmdFlags::SubcmdRoute;
                    constexpr uint32_t kReplaySensitiveClimon =
                        Server::kClimonMonitor | Server::kClimonTracking |
                        Server::kClimonReply;
                    const bool write_hazard = (spec->flags & kWriteHazards) != 0;
                    read_local_write_hazard = write_hazard;
                    const bool point_route =
                        (spec->flags & kNonPointRoutes) == 0 &&
                        spec->first_key > 0 && spec->last_key == spec->first_key &&
                        spec->key_step == 1;
                    if (point_route) {
                        op->hash = FlatStore::hash_key(
                            op->arg(static_cast<uint32_t>(spec->first_key)));
                        op->shard = srv_->router().shard_of(op->hash);
                        read_local_point_prehashed = true;
                    }
                    auto classify_owner_conflict = [&](auto&& overlaps) {
                        bool broad = false;
                        const bool conflict = rob.read_local_owner_conflicts_before(
                            rob.dispatch_id(), [&](const Op& owner) {
                                if (!overlaps(owner)) return false;
                                broad |= !read_local_owner_command_is_precise(owner);
                                return true;
                            });
                        return !conflict ? ReadLocalFallbackReason::None
                            : broad ? ReadLocalFallbackReason::ContextRoute
                                    : ReadLocalFallbackReason::ContextOwnerKey;
                    };
                    auto owner_conflict_for_hash = [&](uint64_t hash) {
                        return classify_owner_conflict([&](const Op& owner) {
                            return read_local_owner_command_touches_hash(owner, hash);
                        });
                    };
                    auto owner_conflict_for_command = [&]() {
                        return classify_owner_conflict([&](const Op& owner) {
                            return read_local_commands_overlap(*op, owner);
                        });
                    };

                    if (write_hazard) {
                        // Stage every historical v1 hazard before the stateful climon gate. A
                        // syntactic one-key owner route or blind MSET keyset can be refined now
                        // that arity is proved. Other multi-key writes remain conservative.
                        rob.mark_current_write();
                        const bool ordinary_point_write =
                            point_route &&
                            (spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)) != 0 &&
                            !(spec->flags & CmdFlags::ConnLocal);
                        if (ordinary_point_write) {
                            if (climon_armed_cached_ & kReplaySensitiveClimon) {
                                read_local_owner_conflict_reason =
                                    owner_conflict_for_hash(op->hash);
                                read_local_owner_conflict = read_local_owner_conflict_reason !=
                                    ReadLocalFallbackReason::None;
                            }
                            const bool reserve_owner_fenced_current =
                                read_local_owner_conflict &&
                                (climon_armed_cached_ & kReplaySensitiveClimon) != 0;
                            // An evicting maxmemory policy makes the write conservative, but it is
                            // still an ordinary one-owner route. Reserve that current append along
                            // with the demoted reads so the post-climon sequence remains infallible.
                            //
                            // TWO DIFFERENT QUESTIONS, ONE ANSWER USED FOR BOTH UNTIL NOW.
                            // "Which keys does this write touch" is answered by the eviction
                            // policy alone: with eviction off, an ordinary point write touches
                            // op->hash and nothing else, whatever the ROB's RYOW ring can hold.
                            // "Can the ring record this hash for LATER reads to be checked
                            // against" is a capacity question, and refine_current_write_hash
                            // answers no once sixteen writes are already in flight. Conjoining
                            // them made a full ring turn every further write into a demote-EVERY-
                            // pending-read wave: at 59% writes and pipeline 32 the ring overflows
                            // permanently and every single read on the connection is lowered to
                            // the owner queue (measured: 164000 of 164000 reads, INFO
                            // read_local_fallback_inflight_write). The selection below now asks
                            // only the keys question, which is the owner's rule verbatim -- a read
                            // is held back on explicit key conflict and on nothing else. Nothing
                            // is weakened: require_hash_match=true is already what an unfilled
                            // ring passes here on every ordinary point write.
                            const bool point_write_exact =
                                fused_executor_->read_local_point_writes_precise();
                            // Still gated: with an evicting policy the write may touch keys it
                            // never names, so it must stay a conservative ring generation and no
                            // later read may be cleared against its hash alone.
                            if (point_write_exact) {
                                (void)rob.refine_current_write_hash(op->hash);
                                op->mark_read_local_precise_write();
                            }
                            // DEMOTION-PLAN GATE (DESIGN-DEMOTEGATE.md). prepare() returns true
                            // without building a plan -- loop_ stays null, so active() is false
                            // and commit_reads() is its `if (!loop_) return;` no-op -- unless the
                            // current append must be reserved or a still-pending local read may
                            // overlap this write. Both predicates are single inline loads that
                            // prepare() itself performs first; deciding them here skips the
                            // 12-argument out-of-line call and its abort prologue on the common
                            // disjoint / no-pending-read point write. This is the exact negation
                            // of prepare()'s two true-early-outs for this call site (no intersect
                            // command, no fallback seeds, no filter miss): behaviour identical,
                            // measured -108 instr/op and -23 cyc/op on the armed SET at 1T.
                            const bool needs_demotion_plan =
                                (reserve_owner_fenced_current && op->shard >= 0) ||
                                (rob.has_pending_read_local() &&
                                 (!point_write_exact ||
                                  rob.read_local_pending_may_touch(op->hash)));
                            if (needs_demotion_plan &&
                                !read_local_demotion.prepare(
                                    *this, c, op->hash, point_write_exact, op->shard,
                                    ReadLocalFallbackReason::InflightWrite,
                                    reserve_owner_fenced_current))
                                break;
                            read_local_commit_at_ordinary = true;
                        } else if (read_local_command_is_precise_mset(*op) &&
                                   fused_executor_->read_local_point_writes_precise()) {
                            const uint32_t key_count = (op->argc() - 1) / 2;
                            bool intersect_filter_miss = false;
                            uint64_t filter = 0;
                            if (key_count <= ReadLocalRobState::kMaxPreciseKeysetKeys) {
                                const bool pending_reads = rob.has_pending_read_local();
                                bool pending_filter_hit = false;
                                for (uint32_t arg = 1; arg < op->argc(); arg += 2) {
                                    const uint64_t hash = FlatStore::hash_key(op->arg(arg));
                                    filter |= ReadLocalRobState::keyset_filter(hash);
                                    if (pending_reads && !pending_filter_hit)
                                        pending_filter_hit =
                                            rob.read_local_pending_may_touch(hash);
                                }
                                intersect_filter_miss =
                                    !pending_reads || !pending_filter_hit;
                            }
                            const bool keyset_precise =
                                rob.refine_current_write_keyset(filter, key_count);
                            if (keyset_precise) op->mark_read_local_precise_write();
                            if (!read_local_demotion.prepare(
                                    *this, c, 0, false, -1,
                                    ReadLocalFallbackReason::InflightWrite, false,
                                    nullptr, nullptr, 0,
                                    keyset_precise ? op : nullptr,
                                    keyset_precise && intersect_filter_miss))
                                break;
                            read_local_commit_before_lowering = true;
                        } else if (!read_local_demotion.prepare(
                                       *this, c, 0, false, -1,
                                       ReadLocalFallbackReason::InflightWrite)) {
                            break;
                        } else {
                            read_local_commit_before_lowering = true;
                        }
                    } else if ((spec->flags & CmdFlags::ReadLocalEligible) != 0) {
                        const bool mget = command_is_read_local_mget(*spec);
                        read_local_mget_candidate = mget;
                        // MGET owns a one-command latest-read boundary in both outcomes: local
                        // success publishes its freshly copied vector, while demotion resolves the
                        // whole command at this pass's pinned cut. Do not let an older local GET
                        // from the same pass complete at a newer world and then follow it with an
                        // MGET fallback at the older cut. Leave this frame unconsumed; after the
                        // existing local lane resolves, reparsing samples a fresh pass cut.
                        if (mget && rob.has_pending_read_local()) break;
                        if (!mget && !point_route) std::abort();
                        if (mget) {
                            op->hash = FlatStore::hash_key(op->arg(1));
                            op->shard = srv_->router().shard_of(op->hash);
                            read_local_point_prehashed = true;
                            read_local_lane_demand =
                                std::min<uint32_t>(op->argc() - 1, srv_->nshards());
                        }
                        read_local_owner_conflict_reason = owner_conflict_for_command();
                        read_local_owner_conflict = read_local_owner_conflict_reason !=
                            ReadLocalFallbackReason::None;
                        bool write_conflict = false;
                        bool mget_atomic_pending = false;
                        if (mget) {
                            // Sample only the pending-key filter here. An open group outlives the
                            // parse-to-execute gap, so that sample predicts the executor's answer;
                            // the table-mutation bit is one ~200 ns exchange bracket that has
                            // closed long before the executor reads, and sampling it on every
                            // touched shard rejected about 11% of MGETs for nothing the executor's
                            // own probe/validate would not have caught (mget_fallback_seq_churn).
                            for (uint32_t arg = 1; arg < op->argc(); arg++) {
                                const uint64_t hash = arg == 1
                                    ? op->hash : FlatStore::hash_key(op->arg(arg));
                                const int32_t shard = arg == 1
                                    ? op->shard : srv_->router().shard_of(hash);
                                read_local_pending_keys.add(hash);
                                write_conflict |= rob.read_local_write_conflicts(
                                    hash, read_local_command_touches_hash);
                                mget_atomic_pending |=
                                    srv_->shard(shard).store().foreign_read_key_unsafe(hash);
                            }
                        } else {
                            write_conflict = rob.read_local_write_conflicts(
                                op->hash, read_local_command_touches_hash);
                        }
                        read_local_eligible = extend_read_local_batch &&
                            !write_conflict && !read_local_owner_conflict;
                        if (extend_read_local_batch &&
                            (write_conflict || read_local_owner_conflict)) {
                            read_local_fallback_reason = write_conflict
                                ? read_local_write_fallback_reason(rob)
                                : read_local_owner_conflict_reason;
                            read_local_batch = false;
                        }
                        if (!extend_read_local_batch) {
                            read_local_eligible = true;
                            if (multi_session_watch_size(conn) != 0) {
                                read_local_fallback_reason =
                                    ReadLocalFallbackReason::Watch;
                                read_local_eligible = false;
                            } else if (c->blocked() || c->subscriber_mode()) {
                                read_local_fallback_reason =
                                    ReadLocalFallbackReason::ContextConnectionState;
                                read_local_eligible = false;
                            } else if (op->has_scatter_state() ||
                                       (spec->flags &
                                        (CmdFlags::ScriptRoute | CmdFlags::AllShards)) ||
                                       (!mget && (spec->flags & CmdFlags::MultiShard))) {
                                read_local_fallback_reason =
                                    ReadLocalFallbackReason::ContextRoute;
                                read_local_eligible = false;
                            } else if (write_conflict) {
                                read_local_fallback_reason =
                                    read_local_write_fallback_reason(rob);
                                read_local_eligible = false;
                            } else if (read_local_owner_conflict) {
                                read_local_fallback_reason = read_local_owner_conflict_reason;
                                read_local_eligible = false;
                            } else if (mget &&
                                       fused_executor_->read_local_keymiss_notify_armed()) {
                                // A notify-aware owner lookup can emit keymiss. Local MGET serves
                                // stable misses as nil, so keep the whole command on the unchanged
                                // owner/scatter path while key-miss notifications are configured.
                                read_local_fallback_reason =
                                    ReadLocalFallbackReason::ContextKeymissNotify;
                                read_local_eligible = false;
                            } else {
                                bool atomic_pending = mget_atomic_pending;
                                bool seq_churn = false;
                                if (!mget) {
                                    const uint64_t state = srv_->shard(op->shard)
                                                               .store()
                                                               .read_local_state_acquire();
                                    atomic_pending =
                                        srv_->shard(op->shard)
                                            .store()
                                            .foreign_read_key_unsafe(state, op->hash);
                                    seq_churn = !FlatStore::read_local_state_eligible(state);
                                }
                                if (atomic_pending) {
                                    read_local_fallback_reason =
                                        ReadLocalFallbackReason::AtomicPending;
                                    read_local_eligible = false;
                                } else if (seq_churn) {
                                    read_local_fallback_reason =
                                        ReadLocalFallbackReason::SeqChurn;
                                    read_local_eligible = false;
                                } else if (rob.pending_read_local_count() >= read_local_quota) {
                                    // LANE ADMISSION, fair share (P128.md): the lane is under
                                    // pressure and this connection already holds its share of
                                    // it. DEFER -- see the lane-full arm below for the shape.
                                    // Deliberately does NOT re-arm the pressure window: a signal
                                    // the actuator itself produces cannot police the actuator
                                    // (measured: at the lane boundary the window fed by its own
                                    // quota deferrals stayed armed forever and deferred 0.8% of
                                    // reads where the lane itself refused 0.00008%). Only a real
                                    // lane-full event arms it, so the lane proves pressure again
                                    // every kReadLocalLanePressureRotations rotations.
                                    self_->read_local_stats().defer_quota++;
                                    break;
                                } else if (!fused_executor_->local_read_lane_has_room(
                                               read_local_lane_demand)) {
                                    // LANE ADMISSION, lane full (P128.md): DEFER, never demote.
                                    // Nothing about this frame has been published -- the ROB
                                    // slot is acquired but not published, no lane entry, no
                                    // pending bit, no owner slot -- so leaving the bytes at rpos
                                    // and ending the pass is the same shape as the MGET
                                    // admission fence above. The pass reports Progress, so
                                    // more_input keeps the connection in active_ and the next
                                    // flush_ready re-parses it after this thread's own EX pass
                                    // has drained the lane. (This arm exists only in the coarse
                                    // Fused parser, whose IFID is that walk; pending_ifid_ is
                                    // never consumed there, so it must not be touched here.)
                                    // The former alternative, demoting the read to its shard
                                    // owner as an ordinary task, was the seed of the p128 and
                                    // 2048-connection pure-read collapse: 7/8 of those tasks
                                    // were cross-thread, every pending owner task suppresses the
                                    // receiving thread's lane tail-drain, its lane keeps a
                                    // residue and fills sooner, and the demotions feed back. A
                                    // read whose data is right here now waits one rotation
                                    // instead. The deferral arms the pressure window, so the
                                    // next rotation divides the lane fairly (quota arm above)
                                    // instead of refusing whichever connections it parses last.
                                    self_->read_local_stats().defer_lane_full++;
                                    fused_executor_->note_local_read_lane_full();
                                    break;
                                }
                            }
                        }
                        read_local_eligible_decided = true;
                        if (!read_local_eligible) {
                            if (read_local_fallback_reason ==
                                ReadLocalFallbackReason::None) std::abort();
                            // Keep the owner-routed read visible only to younger overlapping keys.
                            // Disjoint reads may execute locally; ROB retirement orders replies.
                            rob.extend_current_read_local_owner();
                            if (mget) {
                                // The MGET admission fence above reparses instead of reaching here
                                // with any older pending local read, so the intersect set is empty.
                                if (!read_local_demotion.prepare(
                                        *this, c, 0, false, -1,
                                        ReadLocalFallbackReason::ContextOwnerKey,
                                        false, nullptr, nullptr, 0, op, true))
                                    break;
                                read_local_commit_before_lowering = true;
                            } else {
                                const bool reserve_owner_fenced_current =
                                    read_local_owner_conflict &&
                                    (climon_armed_cached_ & kReplaySensitiveClimon) != 0;
                                if (!read_local_demotion.prepare(
                                        *this, c, op->hash, true, op->shard,
                                        ReadLocalFallbackReason::ContextOwnerKey,
                                        reserve_owner_fenced_current))
                                    break;
                                read_local_commit_at_ordinary = true;
                            }
                        }
                    } else if (!(spec->flags & CmdFlags::ConnLocal)) {
                        read_local_owner_conflict_reason = point_route
                            ? owner_conflict_for_hash(op->hash)
                            : classify_owner_conflict([](const Op&) { return true; });
                        read_local_owner_conflict = read_local_owner_conflict_reason !=
                            ReadLocalFallbackReason::None;
                        if (read_local_owner_conflict)
                            rob.extend_current_read_local_owner();
                        const bool reserve_owner_fenced_current =
                            read_local_owner_conflict && point_route &&
                            (climon_armed_cached_ & kReplaySensitiveClimon) != 0;
                        if (!read_local_demotion.prepare(
                                *this, c, point_route ? op->hash : 0,
                                point_route, point_route ? op->shard : -1,
                                point_route ? ReadLocalFallbackReason::ContextOwnerKey
                                            : ReadLocalFallbackReason::ContextRoute,
                                reserve_owner_fenced_current))
                            break;
                        if (point_route) read_local_commit_at_ordinary = true;
                        else read_local_commit_before_lowering = true;
                    }
                }
            }
            if constexpr (Fused) {
                if (read_local_enabled && read_local_demotion.active() &&
                    read_local_demotion.partial()) {
                    if (__builtin_expect(srv_->flip_dispatch_paused(), false) &&
                        !(spec->flags & CmdFlags::FlipAsync)) {
                        c->set_flip_backpressure(true);
                        break;
                    }
                    // Only older, already-admitted reads are published here. The current frame is
                    // still unconsumed, the FLIP fence above admitted publication, and it has not
                    // crossed MONITOR/tracking or ACL, so it can be reparsed after the bounded wave
                    // without replaying stateful hooks.
                    read_local_demotion.commit_reads();
                    break;
                }
            }
            if (__builtin_expect(notify_armed, false) &&
                __builtin_expect(climon_armed_gate(c, *op), false)) break;
            if (__builtin_expect(security_check, false) &&
                acl_dispatch_entry(*this, conn, *op, consumed, security_flags)) continue;
            if (__builtin_expect(srv_->flip_dispatch_paused(), false) &&
                !(spec->flags & CmdFlags::FlipAsync)) {
                // No ordinary request may create IO-local fanout or executor work after the first
                // drain acknowledgement. Leave the frame unconsumed and unpublished: TCP framing
                // keeps younger frames behind it without a ROB barrier, while FlipAsync commands
                // at the head of other connections remain reachable throughout the pause.
                c->set_flip_backpressure(true);
                break;
            }
            if constexpr (Fused) {
                // This must follow the per-frame FLIP gate above: demotion itself publishes owner
                // Tasks. Non-point routes may touch any key, so move every unresolved local read
                // before any of their lowering paths can publish.
                if (read_local_enabled && read_local_commit_before_lowering) {
                    if (read_local_demotion.active() && !read_local_write_hazard) {
                        rob.extend_current_read_local_owner();
                    }
                    read_local_demotion.commit_reads();
                }
            }
            if (__builtin_expect((spec->flags & CmdFlags::Transaction) != 0, false) ||
                __builtin_expect(conn.multi_session() != nullptr, false)) {
                if constexpr (Fused) {
                    if (read_local_enabled &&
                        __builtin_expect((spec->flags & CmdFlags::ReadLocalEligible) &&
                                         multi_session_active(conn), false))
                        self_->read_local_stats().note_fallback(
                            ReadLocalFallbackReason::Multi,
                            command_is_read_local_mget(*spec));
                }
                if constexpr (IofusedPrivateQueue) {
                    if (multi_dispatch_entry_iofused(*this, conn, *op, consumed)) continue;
                } else {
                    if (multi_dispatch_entry(*this, conn, *op, consumed)) continue;
                }
            }
            const bool config_scatter = (spec->flags & CmdFlags::ConfigRoute) &&
                                        command_config_routes_all_shards(*op);

            // RESP2 enters subscribed mode after its first subscription acknowledgement. Only the
            // Redis subscriber command set is legal until the last subscription is removed.
            const bool subscriber_mode = __builtin_expect(c->subscriber_mode(), false);
            if (subscriber_mode) {
                if (op->cmd_name().eq_icase("reset")) {
                    conn.advance_parse(consumed);
                    self_->note_command(spec->id);
                    flip_fingerprint_note(*spec, *op);
                    climon_reset_client(c, *op);
                    pubsub_start_reset(c, *op);
                    sig.ops++;
                    mark_active_known<TargetedIfid>(c);
                    break;
                }
                if (op->resp3()) goto subscriber_checks_done;
                const bool subscription_control =
                    op->cmd_name().eq_icase("subscribe") ||
                    op->cmd_name().eq_icase("unsubscribe") ||
                    op->cmd_name().eq_icase("psubscribe") ||
                    op->cmd_name().eq_icase("punsubscribe") ||
                    op->cmd_name().eq_icase("ssubscribe") ||
                    op->cmd_name().eq_icase("sunsubscribe");
                if (op->cmd_name().eq_icase("ping")) {
                    conn.advance_parse(consumed);
                    self_->note_command(spec->id);
                    flip_fingerprint_note(*spec, *op);
                    pubsub_reply_ping(*op);
                    finish_prebuilt(c, *op);
                    continue;
                }
                if (!subscription_control && !op->cmd_name().eq_icase("quit")) {
                    conn.advance_parse(consumed);
                    self_->note_command(spec->id);
                    flip_fingerprint_note(*spec, *op);
                    pubsub_reply_restricted(*op);
                    finish_prebuilt(c, *op);
                    continue;
                }
            }
subscriber_checks_done:
            if (spec->flags & CmdFlags::PubSub) {
                conn.advance_parse(consumed);
                self_->note_command(spec->id);
                flip_fingerprint_note(*spec, *op);
                const PubSubStartResult result = pubsub_start_command(c, *op);
                if (result == PubSubStartResult::Async) {
                    sig.ops++;
                    mark_active_known<TargetedIfid>(c);
                    if (__builtin_expect(c->scatter_barrier(), false)) break;
                    continue;
                }
                finish_prebuilt(c, *op);
                // A CLIENT subcommand may have just armed or disarmed this lane, invalidating
                // the pass-local armed cache above. Ending the pass is the cheapest correct
                // answer: the next one re-reads it. One predicted-false test on a branch GET and
                // SET never enter.
                if (__builtin_expect(climon_armed_dirty_, false)) {
                    climon_armed_dirty_ = false;
                    break;
                }
                continue;
            }

            // Connection-local commands never reach a worker — the cheapest class, and the one most
            // easily wasted by routing it anyway.
            if ((spec->flags & CmdFlags::ConnLocal) ||
                ((spec->flags & CmdFlags::ConfigRoute) && !config_scatter)) {
                // SCRIPT/FUNCTION read and mutate state that in-flight EVAL/EVALSHA/FCALL
                // activations produce or consume, so they observe same-connection program order
                // through the ROB-head barrier the blocking lowering already uses. Everything
                // else keeps the parse-time answer. The barrier break runs FIRST so a barred op
                // retries from scratch before any lane hook fires.
                if (__builtin_expect((spec->flags & CmdFlags::OrderedLocal) != 0, false) &&
                    rob.in_flight() != 0) break;
                if (__builtin_expect((spec->flags & CmdFlags::FlipAsync) != 0, false) &&
                    op->argc() == 3) {
                    auto parse_count = [](Slice input, uint32_t& value) {
                        if (!input.n) return false;
                        uint64_t parsed = 0;
                        for (uint32_t i = 0; i < input.n; i++) {
                            if (input.p[i] < '0' || input.p[i] > '9') return false;
                            parsed = parsed * 10 + static_cast<uint32_t>(input.p[i] - '0');
                            if (parsed > UINT32_MAX) return false;
                        }
                        value = static_cast<uint32_t>(parsed);
                        return true;
                    };
                    uint32_t target_io = 0, target_ex = 0;
                    std::string error;
                    bool started = parse_count(op->arg(1), target_io) &&
                                   parse_count(op->arg(2), target_ex);
                    if (!started) {
                        error = "ERR FLIP io and ex must be unsigned integers";
                        srv_->flip_note_refused();
                    } else
                        started = srv_->flip_begin(target_io, target_ex, self_id, error);
                    conn.advance_parse(consumed);
                    self_->note_command(spec->id);
                    flip_fingerprint_note(*spec, *op);
                    if (!started || srv_->flip_stage() == FlipStage::Idle) {
                        if (started) reply_ok(op->sink());
                        else reply_err(op->sink(), error.c_str());
                        op->state.store(OpState::Done, std::memory_order_release);
                        rob.publish();
                        enqueue_serve(c);
                        mark_active_known<TargetedIfid>(c);
                        continue;
                    }
                    flip_client_ = c;
                    flip_op_id_ = rob.dispatch_id();
                    flip_epoch_local_ = srv_->flip_epoch();
                    rob.publish();              // sole unfinished op on the coordinator connection
                    mark_active_known<TargetedIfid>(c);
                    break;
                }
                // DEBUG SLEEP parks only this connection. The unfinished ROB slot preserves
                // pipeline order while this IO thread continues serving unrelated clients (and,
                // in 1s, continues owning shards). All other DEBUG forms fall through unchanged.
                if (__builtin_expect((spec->flags & CmdFlags::DebugSleep) != 0, false)) {
                    uint64_t delay_ms = 0;
                    const uint64_t slow_started =
                        __builtin_expect(slowlog_armed_, false) ? now_ns() : 0;
                    DebugSleepResult sleep =
                        debug_sleep_prepare(*srv_, *c, *op, delay_ms);
                    if (sleep != DebugSleepResult::NotSleep) {
                        if (sleep == DebugSleepResult::Deferred &&
                            deferred_timer_start(c, rob.dispatch_id(),
                                                 DeferredTimerKind::DebugSleepOk, delay_ms,
                                                 slow_started, slowlog_arm_)) {
                            conn.advance_parse(consumed);
                            self_->note_command(spec->id);
                            flip_fingerprint_note(*spec, *op);
                            rob.publish();
                            c->set_blocked(true);
                            // As with WAIT, retirement releases the barrier only after the timer's
                            // reply has been staged and the ROB becomes quiescent.
                            barrier_arm(c, BarrierOwner::Sleep);
                            mark_active_known<TargetedIfid>(c);
                            break;
                        }
                        if (sleep == DebugSleepResult::Deferred)
                            reply_err(op->sink(), "ERR out of memory");
                        if (__builtin_expect(slowlog_armed_, false)) {
                            timespec wall{};
                            ::clock_gettime(CLOCK_REALTIME, &wall);
                            slowlog_record(self_id, c->id(), *op, now_ns() - slow_started,
                                           static_cast<int64_t>(wall.tv_sec) * 1000 +
                                               wall.tv_nsec / 1000000,
                                           slowlog_arm_, true);
                        }
                        conn.advance_parse(consumed);
                        self_->note_command(spec->id);
                        flip_fingerprint_note(*spec, *op);
                        op->state.store(OpState::Done, std::memory_order_release);
                        rob.publish();
                        enqueue_serve(c);
                        mark_active_known<TargetedIfid>(c);
                        continue;
                    }
                }
                // An unsatisfied WAIT has no shard work, but Redis keeps the connection parked
                // until its deadline (zero means forever). Publish an unfinished ROB slot and let
                // this connection's IO owner complete it. MULTI does not enter this branch: its
                // IoLocal child calls cmd_wait at retirement and receives :0 immediately.
                if (__builtin_expect((spec->flags & CmdFlags::DeferredLocal) != 0, false)) {
                    uint64_t timeout_ms = 0;
                    const WaitCommandResult wait = server_tail_prepare_wait(*op, timeout_ms);
                    if (wait == WaitCommandResult::Unsatisfied) {
                        if (!deferred_timer_start(c, rob.dispatch_id(),
                                                  DeferredTimerKind::WaitZero, timeout_ms, 0,
                                                  SlowlogArm{})) {
                            reply_err(op->sink(), "ERR out of memory");
                        } else {
                            conn.advance_parse(consumed);
                            self_->note_command(spec->id);
                            flip_fingerprint_note(*spec, *op);
                            rob.publish();
                            c->set_blocked(true);
                            // Released by the quiescence backstop, not here: a parked WAIT's own
                            // completion (deferred_timer_pass) fires before its op retires, and
                            // dropping the barrier there would let younger frames parse ahead of
                            // the WAIT reply's staging. Owner bit named so the release is
                            // attributable; the release site is deliberately unchanged.
                            barrier_arm(c, BarrierOwner::Wait);
                            mark_active_known<TargetedIfid>(c);
                            break;
                        }
                    } else if (wait == WaitCommandResult::Immediate) {
                        reply_int(op->sink(), 0);
                    }
                    // Error already carries its exact validation reply. Immediate already carries
                    // :0. Both retire through the ordinary local completion path below.
                    conn.advance_parse(consumed);
                    self_->note_command(spec->id);
                    flip_fingerprint_note(*spec, *op);
                    op->state.store(OpState::Done, std::memory_order_release);
                    rob.publish();
                    enqueue_serve(c);
                    mark_active_known<TargetedIfid>(c);
                    continue;
                }
                // RESET clears this lane's connection state (monitor mode, tracking registration,
                // CLIENT REPLY mode) before the ordinary handler writes +RESET. One predicted-
                // false flag test on a word the dispatcher already holds, on an already-cold
                // command class -- no name comparison, and nothing on the ordinary path.
                if (__builtin_expect((spec->flags & CmdFlags::Climon) != 0, false))
                    climon_reset_client(c, *op);
                conn.advance_parse(consumed);
                self_->note_command(spec->id);
                flip_fingerprint_note(*spec, *op);
                command_set_local_context(c, self_);
                snapshot_bind_io(self_, &ring_);
                const bool acl_command = __builtin_expect(op->cmd_name().eq_icase("acl"), false);
                const uint64_t slow_started =
                    __builtin_expect(slowlog_armed_, false) ? now_ns() : 0;
                if (acl_command)
                    acl_command_entry(*this, conn, *op);
                else
                    spec->handler(srv_->shard(0), *op);
                if (__builtin_expect(slowlog_armed_, false)) {
                    timespec wall{};
                    ::clock_gettime(CLOCK_REALTIME, &wall);
                    slowlog_record(self_id, c->id(), *op, now_ns() - slow_started,
                                   static_cast<int64_t>(wall.tv_sec) * 1000 +
                                       wall.tv_nsec / 1000000,
                                   slowlog_arm_, true);
                }
                snapshot_bind_io(nullptr, nullptr);
                command_set_local_context(nullptr, nullptr);
                op->state.store(OpState::Done, std::memory_order_release);
                rob.publish();
                enqueue_serve(c);
                mark_active_known<TargetedIfid>(c);
                if (c->closing()) { result = DispatchResult::Closed; break; }
                if (acl_command) break;
                if (__builtin_expect(climon_armed_dirty_, false)) {
                    climon_armed_dirty_ = false;
                    break;
                }
                continue;
            }

            // This command only needs an owner-local same-connection pending lookup when an older
            // cross-shard atomic group was already in flight. Set the immutable bit before the
            // current group increments the count, so a group never treats itself as a predecessor.
            if (c->has_atomic_group_io()) op->mark_atomic_hazard();
            // PIN THE READ CUT IN PROGRAM ORDER. Same-connection ops are prepared here, in the
            // order the client sent them, so a cut taken no later than here is monotone along the
            // connection for free -- which is exactly the property "a later reply may not be older
            // than an earlier one" needs and that sampling at EXECUTION cannot give. Writes are
            // excluded on purpose (see Op::read_cut_lo). Off, this is one predicted-not-taken
            // test; on, it is one store into a line reset() already dirtied.
            // Blocking commands are excluded as well, and not for cost: a parked one is
            // re-prepared by blocking_resume_move() long after its arrival, so a cut pinned at
            // first dispatch would make the resumed read answer from before the write that woke
            // it. They need no cut anyway -- a blocking command is a whole-connection barrier
            // (it waits to be ROB head and stops the parse pass), so nothing younger on this
            // connection is even prepared until it has finished.
            if (__builtin_expect(atomic_tracking, false) &&
                !(spec->flags & (CmdFlags::Write | CmdFlags::SnapshotWrite |
                                 CmdFlags::Blocking)))
                op->set_read_cut(pass_read_cut);

            if (spec->flags & CmdFlags::Blocking) {
                // XREAD is registered as blocking so the ordinary GET/SET branch remains
                // byte-for-byte the established hot gate. Its immediate form skips this lowering.
                if ((spec->flags & CmdFlags::StreamRoute) && !blocking_wants_dispatch(*op))
                    goto nonblocking_dispatch;
                // A blocking command is a connection barrier, including against older frames.
                // Waiting to issue it until the ROB head preserves same-connection program order
                // without teaching the owner registry about younger operations.
                if (rob.in_flight() != 0) break;
                BlockingDispatch dispatch;
                const BlockingPrepare prepared = blocking_prepare(
                    *srv_, *c, *op, rob.dispatch_id(), dispatch);
                if (prepared == BlockingPrepare::Error) {
                    conn.advance_parse(consumed);
                    finish_prebuilt(c, *op);
                    continue;
                }
                uint32_t needed[kMaxThreads] = {};
                for (uint32_t i = 0; i < dispatch.nshards; i++) {
                    const int32_t sid = blocking_dispatch_shard(dispatch, i);
                    needed[srv_->worker_of_shard(sid)]++;
                }
                bool room = true;
                for (uint32_t tid = 0; tid < srv_->nthreads(); tid++) {
                    if (needed[tid] &&
                        task_free_slots(srv_->thread(tid)) < needed[tid]) {
                        room = false;
                        break;
                    }
                }
                if (!room) {
                    blocking_destroy_unpublished(dispatch.state);
                    break;
                }
                const uint64_t op_id = rob.dispatch_id();
                op->attach_blocking_state(dispatch.state);
                blocking_start(dispatch.state, dispatch.nshards);
                rob.publish();
                for (uint32_t i = 0; i < dispatch.nshards; i++) {
                    const int32_t sid = blocking_dispatch_shard(dispatch, i);
                    const uint32_t tid = srv_->worker_of_shard(sid);
                    ThreadCtx& owner = srv_->thread(tid);
                    const Task task{c, op_id, sid,
                                    reinterpret_cast<ScatterState*>(dispatch.state)};
                    if (!post_task_quiet(owner, task)) std::abort();
                    if (!touched_[tid]) {
                        touched_[tid] = true;
                        touched_list_[ntouched_++] = tid;
                    }
                }
                self_->note_command(spec->id);
                flip_fingerprint_note(*spec, *op);
                conn.advance_parse(consumed);
                sig.ops++;
                c->set_blocked(true);
                // The Blocking owner spans the WHOLE parked lifetime, including the move scatter
                // blocking_resume_move() converts this op into: that conversion reuses the ROB
                // slot and inherits this claim rather than taking a second one, so exactly one
                // acquire is matched by exactly one release in blocking_retire() OR
                // blocking_scatter_retire(), whichever of the two exits runs.
                barrier_arm(c, BarrierOwner::Blocking);
                // TEST HOOK (DEBUG BARRIER-HOLD): pin a SECOND owner on this connection so the
                // blocking release has something to fail to drop. The geometry it manufactures is
                // unreachable in production -- which is exactly why it has to be injected.
                //
                // Armed THROUGH barrier_arm on purpose, so this is the positive control for
                // barrier_owner_overlaps as well: with the latch on, every blocking dispatch adds
                // exactly one overlap, which is what proves the counter can count. The production
                // assertion (overlaps == 0) is then read from a phase where the latch is off, and
                // means something, instead of being a number nothing was ever able to move.
                if (__builtin_expect(srv_->debug_barrier_hold_armed(), false))
                    barrier_arm(c, BarrierOwner::Debug);
                mark_active_known<TargetedIfid>(c);
                break;
            }

nonblocking_dispatch:
            // Ordinary single-key commands never enter the scatter engine. Keep xshard_prepare's
            // own classification guard for its other callers, but avoid paying the cross-TU call
            // just to discover that GET/SET have none of the three scatter-routing flags.
            constexpr uint32_t kScatterRouteFlags =
                CmdFlags::AllShards | CmdFlags::MultiShard | CmdFlags::ConfigRoute;
            if constexpr (Fused)
                if (read_local_enabled && read_local_mget_candidate && read_local_eligible)
                    goto ordinary_dispatch;
            if (!(spec->flags & kScatterRouteFlags)) goto ordinary_dispatch;
            {
            ScatterDispatch scatter_dispatch;
            const ScatterPrepare scatter_prepared =
                xshard_prepare(*srv_, *op, scatter_pool_, self_id, c->id(), scatter_dispatch,
                               false, c);
            if (scatter_prepared == ScatterPrepare::Error) {
                conn.advance_parse(consumed);
                finish_prebuilt(c, *op);
                if constexpr (Fused)
                    if (read_local_enabled && read_local_mget_candidate &&
                        read_local_fallback_reason != ReadLocalFallbackReason::None)
                        self_->read_local_stats().note_fallback(
                            read_local_fallback_reason, true);
                continue;
            }
            if (scatter_prepared == ScatterPrepare::Backpressure) {
                // Leave this frame unconsumed and retry it when a window slot retires. TCP framing
                // naturally keeps younger frames behind it; no ROB-quiescence barrier is needed.
                c->set_atomic_backpressure(true);
                break;
            }
            if (scatter_prepared == ScatterPrepare::Ready) {
                // SAME-CONNECTION PROGRAM ORDER ACROSS A SECOND WAVE OF TASKS.
                //
                // A barriered scatter is exactly the set of commands that publish a SECOND wave of
                // owner tasks from an EX thread rather than from here: a two-hop store's phase-2
                // destination install (publish_phase2), an LMPOP/ZMPOP retry, the cross-owner
                // script apply wave. Those tasks enter the destination owner through the EX
                // producer's inbox channel, while this connection's ordinary ops entered through
                // THIS io thread's channel -- and ThreadCtx::drain_tasks visits channels in
                // producer-id order, so nothing orders the two. A phase-2 install could therefore
                // execute BEFORE an older op of the same connection that was still sitting in our
                // channel, and that older op then answered from after a store it precedes.
                // Measured: `ZDIFFSTORE d 2 a b` answering :1 with the very next `ZCARD d`
                // answering :2, and a `DEL` of a destination landing after a younger store.
                //
                // The barrier the dispatch sets below already keeps YOUNGER ops out; this keeps
                // older ones from still being in. Same lowering the blocking path uses: wait for
                // the ROB head, leave the frame unconsumed, and re-parse once the connection
                // quiesces. Read-only and single-wave scatters (MGET, MSET/DEL groups, a direct
                // RENAME) are not barriered and do not pay it -- they post every task from here.
                if (scatter_dispatch.barrier && rob.in_flight() != 0) {
                    xshard_abandon_unpublished(scatter_dispatch.state, scatter_pool_, self_id);
                    break;
                }
                // Read-only/plain scatters keep the compact V3 dispatch arm. Constructing route and
                // bundle arrays for MGET added work without helping its already-cheap individual
                // queue stores. Atomic writes take the bundled arm below, where fan-out dominates.
                if (!scatter_dispatch.atomic_write) {
                    // The demand array is a zero-on-entry member and the participant list is
                    // uninitialised stack, so neither the 512-byte zero-init nor the walk over
                    // every configured thread happens here. A 2-key cross-shard read touches two
                    // owners whether the server runs 4 threads or 128.
                    uint32_t* const needed = dispatch_needed_;
                    uint32_t participants[kMaxThreads];
                    uint32_t nparticipants = 0;
                    for (uint32_t i = 0; i < scatter_dispatch.nshards; i++) {
                        const int32_t sid = xshard_dispatch_shard(scatter_dispatch, i);
                        const uint32_t tid = srv_->worker_of_shard(sid);
                        if (needed[tid]++ == 0) participants[nparticipants++] = tid;
                    }
                    bool room = true;
                    for (uint32_t p = 0; p < nparticipants; p++) {
                        const uint32_t tid = participants[p];
                        if (task_free_slots(srv_->thread(tid)) < needed[tid]) {
                            room = false;
                            break;
                        }
                    }
                    // Restore the zero-on-entry invariant before EVERY exit from this arm.
                    for (uint32_t p = 0; p < nparticipants; p++) needed[participants[p]] = 0;
                    if (!room) {
                        xshard_abandon_unpublished(
                            scatter_dispatch.state, scatter_pool_, self_id);
                        break;
                    }
                    const uint64_t op_id = rob.dispatch_id();
                    op->attach_scatter_state(scatter_dispatch.state);
                    rob.publish();
                    for (uint32_t i = 0; i < scatter_dispatch.nshards; i++) {
                        const int32_t sid = xshard_dispatch_shard(scatter_dispatch, i);
                        const uint32_t tid = srv_->worker_of_shard(sid);
                        ThreadCtx& owner = srv_->thread(tid);
                        const Task task{c, op_id, sid, scatter_dispatch.state};
                        if (!post_task_quiet(owner, task)) std::abort();
                        if (!touched_[tid]) {
                            touched_[tid] = true;
                            touched_list_[ntouched_++] = tid;
                        }
                    }
                    self_->note_command(spec->id);
                    flip_fingerprint_note(*spec, *op);
                    conn.advance_parse(consumed);
                    sig.ops++;
                    if constexpr (Fused)
                        if (read_local_enabled && read_local_mget_candidate &&
                            read_local_fallback_reason != ReadLocalFallbackReason::None)
                            self_->read_local_stats().note_fallback(
                                read_local_fallback_reason, true);
                    head_candidate = false;
                    if (scatter_dispatch.barrier) barrier_arm(c, BarrierOwner::Scatter);
                    mark_active_known<TargetedIfid>(c);
                    continue;
                }

                uint32_t needed[kMaxThreads] = {};
                uint32_t participants[kMaxThreads];
                uint16_t routed_owner[256];
                int32_t routed_shard[256];
                uint32_t nparticipants = 0;
                for (uint32_t i = 0; i < scatter_dispatch.nshards; i++) {
                    const int32_t sid = xshard_dispatch_shard(scatter_dispatch, i);
                    const uint32_t tid = srv_->worker_of_shard(sid);
                    routed_shard[i] = sid;
                    routed_owner[i] = static_cast<uint16_t>(tid);
                    if (needed[tid]++ == 0) participants[nparticipants++] = tid;
                }
                bool room = true;
                for (uint32_t p = 0; p < nparticipants; p++) {
                    const uint32_t tid = participants[p];
                    if (task_free_slots(srv_->thread(tid)) < needed[tid]) {
                        room = false; break;
                    }
                }
                if (!room) {
                    xshard_abandon_unpublished(scatter_dispatch.state, scatter_pool_, self_id);
                    break;
                }
                const uint64_t op_id = rob.dispatch_id();
                op->attach_scatter_state(scatter_dispatch.state);
                c->atomic_group_started();
                rob.publish();
                Task posts[256];
                uint16_t participant_begin[kMaxThreads];
                uint32_t cursor = 0;
                for (uint32_t p = 0; p < nparticipants; p++) {
                    const uint32_t tid = participants[p];
                    participant_begin[p] = static_cast<uint16_t>(cursor);
                    cursor += needed[tid];
                    needed[tid] = participant_begin[p]; // reuse as the fill cursor
                }
                for (uint32_t i = 0; i < scatter_dispatch.nshards; i++) {
                    const uint32_t tid = routed_owner[i];
                    posts[needed[tid]++] = Task{
                        c, op_id, routed_shard[i], scatter_dispatch.state};
                }
                if (cursor != scatter_dispatch.nshards) std::abort();
                for (uint32_t p = 0; p < nparticipants; p++) {
                    const uint32_t tid = participants[p];
                    const uint32_t begin = participant_begin[p];
                    const uint32_t end = p + 1 < nparticipants
                        ? participant_begin[p + 1] : scatter_dispatch.nshards;
                    ThreadCtx& owner = srv_->thread(tid);
                    // Capacity was checked before any push. Publish all of this group's tasks for
                    // one executor with one queue-tail store; the parse-pass notify remains folded.
                    if (!post_tasks_quiet(owner, posts + begin, end - begin)) std::abort();
                    if (!touched_[tid]) { touched_[tid] = true; touched_list_[ntouched_++] = tid; }
                }
                self_->note_command(spec->id); // one public command, not one count per shard task
                flip_fingerprint_note(*spec, *op);
                conn.advance_parse(consumed);
                sig.ops++;
                head_candidate = false;
                if (scatter_dispatch.barrier) barrier_arm(c, BarrierOwner::Scatter);
                mark_active_known<TargetedIfid>(c);
                continue;
            }
            }

ordinary_dispatch:
            // The ordinary key position is registry metadata. Container children and the other
            // special routes refine it in the existing CursorShard hook below.
            if (spec->flags & CmdFlags::CursorShard) {
                if (!command_prepare_scan_route(*srv_, *op)) {
                    conn.advance_parse(consumed);
                    finish_prebuilt(c, *op);
                    continue;
                }
            } else if (spec->flags & CmdFlags::RandomShard) {
                uint64_t random = next_random();
                uint32_t start = static_cast<uint32_t>(random % srv_->nshards());
                uint32_t chosen = start;
                for (uint32_t n = 0; n < srv_->nshards(); n++) {
                    const uint32_t candidate = (start + n) % srv_->nshards();
                    if (srv_->shard(static_cast<int32_t>(candidate)).published_size()) {
                        chosen = candidate; break;
                    }
                }
                op->hash = random;
                op->shard = static_cast<int32_t>(chosen);
            } else {
                if (!read_local_point_prehashed) {
                    op->hash = FlatStore::hash_key(
                        op->arg(static_cast<uint32_t>(spec->first_key)));
                    op->shard = srv_->router().shard_of(op->hash);
                }
            }

            if constexpr (Fused) {
                if (read_local_enabled && read_local_commit_at_ordinary) {
                    // prepare() reserved the selected reads and this operation before any stateful
                    // parser hook. Publish the reads first; the retained current credit makes the
                    // ordinary owner append below infallible and preserves SPSC FIFO.
                    if (read_local_demotion.active() && !read_local_write_hazard) {
                        rob.extend_current_read_local_owner();
                    }
                    read_local_demotion.commit_reads();
                }
                if (read_local_enabled &&
                    __builtin_expect(spec->flags & CmdFlags::ReadLocalEligible, false)) {
                    if (!read_local_eligible_decided) std::abort();
                    if (read_local_eligible) {
                        if (head_candidate) {
                            head_candidate = false;
                            if (rob.in_flight() == 0 && c->nothing_to_write()) {
                                SmallBuf<kWbufInline>& fb = c->fill_buf();
                                op->direct = fb.data();
                                op->direct_cap = static_cast<uint32_t>(fb.cap());
                            }
                        }
                        const uint64_t op_id = rob.dispatch_id();
                        op->mark_read_local();
                        // Every hash read_local_command_touches_hash(*op, .) can match must enter
                        // the pending filter here (GET: op->hash; MGET: the keys hashed above).
                        // A point read reports exactly one, so it sets that word directly instead
                        // of merging a summary whose other three words it had just zeroed.
                        if (read_local_mget_candidate)
                            rob.mark_current_read_local(op_id, read_local_pending_keys);
                        else
                            rob.mark_current_read_local_hash(op_id, op->hash);
                        if (read_local_mget_candidate)
                            rob.arm_current_local_mget_fence();
                        rob.publish();
                        // Room was proved by local_read_lane_has_room(read_local_lane_demand)
                        // above for this very op (run extension at the top of the frame, run head
                        // in the gate chain); nothing between there and here consumes lane room.
                        fused_executor_->enqueue_local_read(c, op_id, read_local_lane_demand);
                        conn.advance_parse(consumed);
                        sig.ops++;
                        flip_fingerprint_note(*spec, *op);
                        mark_active_known<TargetedIfid>(c);
                        read_local_batch = !read_local_mget_candidate;
                        // Fill at most the existing fused IFID quantum; intervening ordinary frames
                        // simply end this run. MGET holds a one-command cut fence until local
                        // validation or irrevocable owner demotion, so stop this parse pass now.
                        if (read_local_mget_candidate) break;
                        continue;
                    }
                }
            }

            const uint32_t worker_id = srv_->worker_of_shard(op->shard);
            ThreadCtx& worker = srv_->thread(worker_id);

            // PUBLISH BEFORE DISPATCH. The old order posted the task first and published after, which
            // left a window of two instructions in which a worker could receive the task, execute it,
            // mark it Done and notify the sender -- all while dispatch_ still excluded the op. The
            // sender then woke, drained a ROB that did not yet contain the op, retired nothing, and
            // went back to sleep having spent its one notification. Nothing ever notified again, so
            // the reply sat Done in the ROB forever.
            //
            // It cost 3 lost replies in 87 million and wedged the connection permanently. Invisible in
            // 2-stage, where io is the sender and re-drains its own active set unprompted; it was
            // FATAL in the deleted remote-sender modes, and the ordering is kept because it is
            // simply correct: publish, then tell.
            // DIRECT-REPLY eligibility (owner's c->buf trick): this op is the ROB head and the
            // fill buffer is empty, so its bytes can be formatted in place by the worker. True for
            // every op at depth 1 and for the head of each fresh batch at depth. Evaluated ONLY for
            // the first dispatch of the pass: later ops cannot be head, and the in_flight() read
            // touches flush_, a line the sender writes -- checked per op it became a per-op
            // cross-thread load and cost -2..-4% at p32 for a candidate that can never qualify.
            if (head_candidate) {
                head_candidate = false;
                if (rob.in_flight() == 0 && c->nothing_to_write()) {
                    SmallBuf<kWbufInline>& fb = c->fill_buf();
                    op->direct     = fb.data();
                    op->direct_cap = static_cast<uint32_t>(fb.cap());
                }
            }
            Task t{c, rob.dispatch_id(), -1, nullptr};
            rob.publish();
            bool posted = false;
            if constexpr (Fused) {
                if (read_local_enabled && read_local_demotion.current_reserved()) {
                    read_local_demotion.post_current(t, worker_id);
                    posted = true;
                }
            }
            if (!posted && !post_task_quiet(worker, t)) {
                rob.unpublish();          // a refused push must leave NO trace -- including in the ROB
                // A REFUSED PUSH MUST LEAVE NO TRACE. Advancing the parse cursor before this point
                // consumed the command's bytes while publishing no op, so the client waited forever
                // for a reply that would never be produced and the connection wedged. This is not an
                // edge case: with enough io threads feeding few workers the inbox fills routinely,
                // and it hung a benchmark within seconds at io6/ex2. Leaving the cursor untouched
                // means the command is simply re-parsed on a later pass, once retiring has freed
                // inbox space.
                break;
            }
            if constexpr (Fused) {
                // Count only after the owner task is irrevocably queued. A refused SPSC push
                // unpublishes and reparses this frame; charging before it would double-count and
                // could report a fallback for a retry that later takes the local lane.
                if (read_local_enabled &&
                    read_local_fallback_reason != ReadLocalFallbackReason::None)
                    self_->read_local_stats().note_fallback(
                        read_local_fallback_reason, read_local_mget_candidate);
            }
            conn.advance_parse(consumed);
            sig.ops++;
            flip_fingerprint_note(*spec, *op);
            touch_worker(worker_id);
            // Unified pipeline 1 entered with a live active client and its batch tail decides once
            // whether input/backpressure requires another IFID visit. Repeating the same active
            // and queue-dedupe checks for every op was pure per-op work; the other schedules retain
            // their existing mark here.
            if constexpr (!SuppressOrdinaryActiveMark)
                mark_active_known<TargetedIfid>(c);
        }
        if constexpr (!IoPipe) {
            // Item 2: one notify per worker per parse pass, not per op. The pushes above are already
            // visible in the queues; this publishes the "look here" bit and pays the wake decision
            // once. The pipelined schedule deliberately folds the same set across its whole IFID
            // batch and publishes it at IFID.POST.
            for (uint32_t i = 0; i < ntouched_; i++) {
                const uint32_t wkr = touched_list_[i];
                touched_[wkr] = false;
                srv_->thread(wkr).flush_task_notify(self_id, ring_, sig);
            }
            ntouched_ = 0;
        }
        flip_fingerprint_finish_pass();
        return result;
    }

    uint32_t flush_ifid_posts() {
        LoopSignals& sig = self_->sig();
        const uint32_t self_id = self_->id();
        const uint32_t posted = ntouched_;
        for (uint32_t i = 0; i < ntouched_; i++) {
            const uint32_t wkr = touched_list_[i];
            touched_[wkr] = false;
            srv_->thread(wkr).flush_task_notify(self_id, ring_, sig);
        }
        ntouched_ = 0;
        return posted;
    }

    // The per-operation gate of the SAMPLED fingerprint (DESIGN-flipfp.md): one load of the
    // writer's own word -- the line the old enabled() test already read -- and one predicted-false
    // branch. The body below (the argv walk and its five counter stores, 63-86 instr/op when it ran
    // on every frame) runs only inside a sampled parse pass. Dark writer (--flip-auto 0, every 1s
    // boot): the word is never 0 and nothing else runs.
    void flip_fingerprint_note(const CommandSpec& spec, const Op& op) {
        if (__builtin_expect(self_->flip_fingerprint().pass_sampled(), false))
            flip_fingerprint_note_sampled(spec, op);
    }

    // Pass end: the unsampled pass counts down; the sampled pass publishes and draws its gap. The
    // draw is taken here, once per sampled pass, never eagerly per pass.
    void flip_fingerprint_finish_pass() {
        FlipFingerprintWriter& writer = self_->flip_fingerprint();
        if (__builtin_expect(!writer.enabled(), true)) return;
        if (writer.finish_parse_pass()) writer.arm(next_random());
    }

    __attribute__((noinline))
    void flip_fingerprint_note_sampled(const CommandSpec& spec, const Op& op) {
        FlipFingerprintWriter& writer = self_->flip_fingerprint();
        uint32_t keys = 0;
        uint32_t first = 0, last = 0, step = 1;
        if (spec.first_key > 0 && static_cast<uint32_t>(spec.first_key) < op.argc()) {
            first = static_cast<uint32_t>(spec.first_key);
            last = spec.last_key < 0 ? op.argc() - 1
                                     : std::min<uint32_t>(spec.last_key, op.argc() - 1);
            step = spec.key_step > 0 ? static_cast<uint32_t>(spec.key_step) : 1;
            if (last >= first) keys = (last - first) / step + 1;
        }
        uint64_t value_bytes = 0;
        for (uint32_t arg = 1; arg < op.argc(); arg++) {
            const bool key = keys && arg >= first && arg <= last && (arg - first) % step == 0;
            if (!key) value_bytes += op.arg(arg).n;
        }
        FlipFingerprintClass command_class = FlipFingerprintClass::Other;
        if (spec.flags & CmdFlags::Blocking) {
            command_class = FlipFingerprintClass::Blocking;
        } else if (keys > 1 && srv_->cfg().atomic &&
                   (spec.flags & CmdFlags::MultiShard)) {
            command_class = FlipFingerprintClass::AtomicGrouped;
        } else if (keys > 1) {
            command_class = spec.flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)
                ? FlipFingerprintClass::MultiWrite : FlipFingerprintClass::MultiRead;
        } else if (spec.flags & (CmdFlags::Write | CmdFlags::SnapshotWrite)) {
            command_class = FlipFingerprintClass::Write;
        } else if (spec.flags & CmdFlags::Readonly) {
            command_class = FlipFingerprintClass::Read;
        }
        writer.note_command(command_class, keys, value_bytes);
    }

    // THE ONE DOOR ONTO THE PARSE BARRIER. Every owner parks a connection through here so the
    // overlap -- two owners holding the barrier at once -- is COUNTED rather than assumed absent.
    // NOTES-BARRIER.md section 2 argues from the source that no production sequence produces one
    // today; barrier_owner_overlaps is that argument's live assertion, and a validation run that
    // wants the two-owner geometry gates on it rather than trusting the prose. Cold by
    // construction: the seven owners are EXEC, a subscribe, a blocking command, a deferred WAIT,
    // a deferred DEBUG SLEEP, a barriered scatter and a CLIENT fan-out. GET and SET never reach it.
    void barrier_arm(Client* c, BarrierOwner who) {
        if (__builtin_expect(c->scatter_barrier(), false)) srv_->note_barrier_overlap();
        c->barrier_acquire(who);
    }

    void finish_locally(Client* c, Op& op, const char* err) {
        reply_err(op.reply, err);
        finish_prebuilt(c, op);
    }

    void finish_prebuilt(Client* c, Op& op) {
        op.state.store(OpState::Done, std::memory_order_release);
        c->rob().publish();
        enqueue_serve(c);
        mark_active(c);
    }

    uint64_t next_random() {
        random_state_ ^= random_state_ << 13;
        random_state_ ^= random_state_ >> 7;
        random_state_ ^= random_state_ << 17;
        return random_state_;
    }

    // Queue a teardown instead of performing it. Used only by the epoll engine's synchronous
    // read/send paths, which run inside flush_ready's walk over the active set. Deliberately does
    // NOT pre-mark the client closing: close_client's first block (deferred-WAIT cancel, blocking
    // cancel, TLS shutdown, ::shutdown of the socket) is guarded by !closing() and would be skipped.
    void epoll_request_close(Client* c) {
        if (c->dead()) return;
        for (Client* queued : epoll_closes_) if (queued == c) return;
        epoll_closes_.push_back(c);
    }

    // Tear down NOW, then discard any send failure the teardown itself produced. That second half
    // is load-bearing: close_client flushes a TLS alert / close_notify through the same engine, and
    // a failure latched there would be picked up by the NEXT connection's take_send_failure() and
    // close a perfectly healthy client. The latch is a one-slot channel, so every close must leave
    // it empty.
    void epoll_close_now(Client* c, bool drain_tls_output = false) {
        close_client(c, drain_tls_output);
        (void)wb_.take_send_failure();
    }

    void mark_active(Client* c) {
        if (c->dead()) return;               // a corpse from the deferred-free list: entry consumed, nothing to do
        if (targeted_ifid_) enqueue_ifid(c);
        if (c->in_active()) return;          // one load, not a scan of the whole set
        c->set_in_active(true);
        active_.insert(c);
    }

    template <bool TargetedIfid>
    void mark_active_known(Client* c) {
        if (c->dead()) return;
        if constexpr (TargetedIfid)
            if (!c->ifid_pending()) {
                c->set_ifid_pending(true);
                pending_ifid_.push_back(c);
            }
        if (c->in_active()) return;
        c->set_in_active(true);
        active_.insert(c);
    }

    void enqueue_ifid(Client* c) {
        if (!c || c->dead() || c->ifid_pending()) return;
        c->set_ifid_pending(true);
        pending_ifid_.push_back(c);
    }

    void discard_ifid(Client* c) {
        if (!c || !c->ifid_pending()) return;
        for (auto it = pending_ifid_.begin(); it != pending_ifid_.end(); ++it) {
            if (*it != c) continue;
            pending_ifid_.erase(it);
            c->set_ifid_pending(false);
            return;
        }
        std::abort();
    }

    void clear_ifid_queue() {
        while (!pending_ifid_.empty()) {
            Client* c = pending_ifid_.front();
            pending_ifid_.pop_front();
            if (c) c->set_ifid_pending(false);
        }
    }

    // ---- inbound: workers telling us a client has completed ops -----------------------------------
    // Inbound from workers: "ops are Done" -- the claimed-post fallback for a conn with no
    // ready-mask slot. Either way the answer is the same: put the client back in the active set.
    template <bool HasUnix, bool HasTls, bool kEp, bool Fused = false>
    uint32_t sweep() {
        uint32_t work = 0;
        if constexpr (HasUnix) work += flush_handoffs();
        if constexpr (Fused) {
            work += service_client_migrations<kEp>() + drain_client_transfers<kEp>(true) +
                    flush_borrow_releases() +
                    flush_ready<HasTls, kEp, true, HasUnix, true>();
        } else {
            work += service_client_migrations<kEp>() + drain_client_transfers<kEp>(true) +
                    flush_borrow_releases() + collect_retire_work<HasUnix, kEp>(true) +
                    flush_ready<HasTls, kEp>();
        }
        if (__builtin_expect(!routing_forward_.empty(), false))
            client_routing_cleanup_pass();
        if (srv_->snapshot().writer_is(self_->id()))
            work += srv_->snapshot().writer_pass(*self_, ring_, true);
        if (srv_->aof().writer_is(self_->id()))
            work += srv_->aof().writer_pass(*self_, ring_, true);
        return work;
    }

    template <bool HasUnix, bool HasTls, bool kEp>
    uint32_t pipeline_sweep(bool natural_order, bool& submitted) {
        uint32_t work = 0;
        if constexpr (HasUnix) work += flush_handoffs();
        work += service_client_migrations<kEp>() + drain_client_transfers<kEp>(true) +
                flush_borrow_releases();
        // The hot rotation visits one cap-bounded IFID batch. Before parking, run enough batches
        // to inspect the whole active set once, retaining this outer pass's selected order and the
        // unmasked completion drain.
        const size_t active_at_start = active_.size();
        const size_t passes = std::max<size_t>(
            1, (active_at_start + kIoPipeIfidBatchClients - 1) /
                   kIoPipeIfidBatchClients);
        for (size_t pass = 0; pass < passes; pass++)
            work += pipeline_pass<HasUnix, HasTls, kEp>(
                true, natural_order, submitted);
        if (__builtin_expect(!routing_forward_.empty(), false))
            client_routing_cleanup_pass();
        if (srv_->snapshot().writer_is(self_->id()))
            work += srv_->snapshot().writer_pass(*self_, ring_, true);
        if (srv_->aof().writer_is(self_->id()))
            work += srv_->aof().writer_pass(*self_, ring_, true);
        return work;
    }

    void queue_borrow_release(int32_t shard, const char* ptr) {
        pending_releases_.push_back(BorrowRelease{shard, ptr});
        flush_borrow_releases();
    }

    uint32_t flush_borrow_releases() {
        uint32_t n = 0;
        while (!pending_releases_.empty()) {
            const BorrowRelease r = pending_releases_.front();
            ThreadCtx& owner = srv_->thread(srv_->worker_of_shard(r.shard));
            if (!owner.post_release(self_->id(), r, ring_, self_->sig())) break;
            pending_releases_.pop_front();
            n++;
        }
        return n;
    }

    template <bool HasUnix, bool kEp, bool TargetedIfid = false>
    uint32_t collect_retire_work(bool unmasked = false) {
        uint32_t pubsub_work = 0;
        auto take = [&](Client* c) {
            if (!c) {
                pubsub_work += pubsub_drain_events();
                return;
            }
            // AF_UNIX accept handoffs use this existing channel without claiming retirement.
            // Executor completions always CAS retire_queued false->true before posting, so the bit
            // distinguishes the two meanings without adding a Client field or a catalog lock here.
            if constexpr (HasUnix)
                if (!c->retire_queued().load(std::memory_order_acquire)) {
                    adopt_client<kEp>(c, true);
                    if constexpr (TargetedIfid) enqueue_ifid(c);
                    return;
                }
            c->retire_queued().store(false, std::memory_order_release);
            enqueue_serve(c);                    // a posted client is a serve request
            mark_active_known<TargetedIfid>(c);
        };
        uint32_t n = unmasked ? self_->drain_clients_unmasked(take) : self_->drain_clients(take);
        // The ready-mask path: workers set one bit per completed-work burst; we map slot -> client,
        // flag it FOR SERVING, and put it back in the active set. The flag is the point: serving
        // every active conn every pass measured 93% EMPTY serves at 8 nodes -- 526M drain-checks
        // that each pulled a remote worker's cache line to learn there was nothing to do. Targeted
        // serving turns the poll into a response.
        for (uint32_t w = 0; w < ReadyMask::kWords; w++) {
            uint64_t bits = self_->ready().take(w);
            while (bits) {
                const uint32_t b = static_cast<uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                Client* c = self_->wb_slot_client(w * 64 + b);
                if (c && !c->dead()) {
                    enqueue_serve(c);
                    mark_active_known<TargetedIfid>(c);
                    n++;
                }
            }
        }
        return n + pubsub_work;
    }

    bool client_pipeline_referenced(const Client* client) const {
        if (client->ifid_pending()) return true;
        if (active_wb_context_)
            for (uint32_t i = 0; i < active_wb_context_->count; i++)
                if (active_wb_context_->clients[i] == client) return true;
        return false;
    }

    bool io_pipelines_quiesced() const {
        return active_wb_context_ == nullptr;
    }

    // Targeted receive-buffer maintenance and the ordinary uncapped parser, shared by both
    // overlapping fused schedules. Reply retirement and sends remain separate WB stages.
    template <bool HasTls, bool kEp>
    uint32_t genthread_ifid_batch() {
        uint32_t work = 0;
        backstop_pass_ = (++flush_tick_ >= kFlushBackstopEvery);
        if (backstop_pass_) flush_tick_ = 0;
        size_t ready_visits = pending_ifid_.size();

        while (ready_visits) {
            Client* c = pending_ifid_.front();
            pending_ifid_.pop_front();
            ready_visits--;
            if (!c) continue;
            c->set_ifid_pending(false);
            if (c->dead() || !c->in_active()) continue;
            Client& conn = *c;
            DispatchResult dispatch_result = DispatchResult::Progress;
            TlsConn* tls = nullptr;
            if constexpr (HasTls) tls = tls_engine(c);
            if (backstop_pass_ && !c->serve_pending()) enqueue_serve(c);

            if constexpr (HasTls) {
                if (tls) {
                    if (!c->closing() && !c->recv_armed())
                        (void)drive_tls<kEp, true, 1>(c);
                    else if (tls->userspace()) {
                        (void)wb_.pump_tls<kEp, true>(*c, *tls);
                        if (tls->socket_userspace() && tls->has_pinned_plain())
                            arm_tls_socket_poll<kEp>(c, tls->wanted());
                    }
                    tls = tls_engine(c);
                    if constexpr (kEp)
                        if (wb_.take_send_failure()) epoll_request_close(c);
                }
            }

            if (c->scatter_barrier()) {
                const bool resumed = c->blocked() && blocking_resume_move_iofused(
                    *srv_, *self_, ring_, *c, scatter_pool_);
                if (resumed) {
                    enqueue_serve(c);
                    work++;
                }
                if (__builtin_expect(c->barrier_held_by(BarrierOwner::Debug), false) &&
                    !srv_->debug_barrier_hold_armed())
                    c->barrier_release(BarrierOwner::Debug);
                if (c->rob().quiesced())
                    c->barrier_release_quiesced();
            }
            if (c->atomic_backpressure() && srv_->atomic_can_admit(self_->id()) &&
                scatter_pool_.can_register_snapshot())
                c->set_atomic_backpressure(false);
            if (c->flip_backpressure() && !srv_->flip_dispatch_paused())
                c->set_flip_backpressure(false);
            if (c->rob().quiesced() &&
                (kEp || !conn.recv_armed()))
                conn.reset_rbuf_at_quiescence();

            if constexpr (kEp) {
                if (c->closing()) c->set_recv_armed(false);
                if (!c->closing()) {
                    if constexpr (HasTls) {
                        if (tls) arm_tls_recv<kEp, true, 1>(c);
                        else arm_recv<kEp>(c);
                    } else {
                        arm_recv<kEp>(c);
                    }
                    if constexpr (HasTls) if (tls) {
                        (void)drive_tls<kEp, true, 1>(c);
                        tls = tls_engine(c);
                    }
                    if (wb_.take_send_failure()) epoll_request_close(c);
                }
            }

            if (!c->closing() && conn.rpos() < conn.rlen() && !c->scatter_barrier() &&
                !c->parse_backpressure()) {
                if (__builtin_expect(climon_pause_armed(), false)) {
                    const uint32_t rpos_before = conn.rpos();
                    if constexpr (HasTls) {
                        if (c->is_tls())
                            dispatch_result = parse_and_dispatch<
                                true, 0, false, true, true, true>(c);
                        else
                            dispatch_result = parse_and_dispatch<
                                false, 0, false, true, true, true>(c);
                    } else {
                        dispatch_result = parse_and_dispatch<
                            false, 0, false, true, true, true>(c);
                    }
                    if (conn.rpos() != rpos_before) work++;
                } else {
                    if constexpr (HasTls) {
                        if (c->is_tls())
                            dispatch_result = parse_and_dispatch<
                                true, 0, false, true, true, true>(c);
                        else
                            dispatch_result = parse_and_dispatch<
                                false, 0, false, true, true, true>(c);
                    } else {
                        dispatch_result = parse_and_dispatch<
                            false, 0, false, true, true, true>(c);
                    }
                    if (__builtin_expect(dispatch_result != DispatchResult::NeedInput, true))
                        work++;
                }
            }

            if constexpr (!kEp) {
                if constexpr (HasTls) {
                    if (tls && tls->memory_bio())
                        arm_tls_recv<kEp, true, 1>(c);
                    else if (!tls)
                        arm_recv<kEp>(c);
                } else {
                    arm_recv<kEp>(c);
                }
            }

            const bool stuck = (conn.rpos() < conn.rlen() && c->rob().full()) ||
                               (!conn.recv_armed() && !c->closing());
            const bool more_input = conn.rpos() < conn.rlen() &&
                                    dispatch_result != DispatchResult::NeedInput;
            const bool tls_output = tls && (tls->output_pending() || c->send_inflight());
            const bool done = c->rob().quiesced() &&
                              !more_input && !stuck && !c->serve_pending() &&
                              c->nothing_to_write() && !tls_output;
            if (c->dead() || !c->in_active()) continue;
            if (done && !c->closing()) {
                c->set_in_active(false);
                active_.erase(c);
            } else if (c->closing() && !tls_output && c->safe_to_release()) {
                if (pubsub_disconnect_ready(c)) {
                    c->set_in_active(false);
                    active_.erase(c);
                    close_client(c);
                } else {
                    enqueue_ifid(c);
                }
            } else {
                const bool retry_without_retire =
                    c->closing() || c->parse_backpressure() || c->scatter_barrier() ||
                    (!c->recv_armed() && !c->closing()) ||
                    (more_input && !c->rob().full());
                if (retry_without_retire) enqueue_ifid(c);
            }
        }

        if constexpr (kEp) {
            while (!epoll_closes_.empty()) {
                Client* victim = epoll_closes_.back();
                epoll_closes_.pop_back();
                epoll_close_now(victim);
            }
        }
        return work;
    }

    // IOFUSED: launch the targeted WB dependency stream, use the ordinary coarse IFID pass as its
    // filler, consume the warmed retirement batch, then run EX. The outer loop owns the sole submit
    // boundary and applies the measured SEND-immediate / four non-SEND rotations rule.
    template <bool HasUnix, bool HasTls, bool kEp>
    uint32_t genthread_iofused_pass(WbPipelineBatch& batch) {
        if (batch.count || active_wb_context_) std::abort();
        active_wb_context_ = &batch;

        uint32_t work = collect_retire_work<HasUnix, kEp, true>();
        if (!pending_serve_.empty()) {
            AofManager& aof = srv_->aof();
            if (!aof_gate_target_) aof_gate_target_ = aof.posted_sequence();
            if (!aof.reply_gate_ready(aof_gate_target_)) {
                aof.register_send_gate_wait(self_->id());
            } else {
                aof_gate_target_ = 0;
                while (batch.count < kGenthreadPipelineWbBatchConns &&
                       !pending_serve_.empty()) {
                    Client* client = pending_serve_.front();
                    pending_serve_.pop_front();
                    client->set_serve_pending(false);
                    if (!client->dead()) {
                        batch.clients[batch.count] = client;
                        batch.count++;
                    }
                }
            }
        } else {
            aof_gate_target_ = 0;
        }

        for (uint32_t i = 0; i < batch.count; i++) {
            Client* client = batch.clients[i];
            if (!client || client->dead()) continue;
            Rob<kRobWindow>& rob = client->rob();
            const uint64_t first = rob.flush_id();
            const uint64_t last = rob.dispatch_id();
            const uint64_t count = std::min<uint64_t>(
                last - first, kGenthreadWbPrefetchOpsPerConn);
            for (uint64_t off = 0; off < count; off++)
                __builtin_prefetch(&rob.at(first + off).state, 0, 3);
        }
        for (uint32_t i = 0; i < batch.count; i++) {
            Client* client = batch.clients[i];
            if (!client || client->dead()) continue;
            Rob<kRobWindow>& rob = client->rob();
            const uint64_t first = rob.flush_id();
            const uint64_t last = rob.dispatch_id();
            const uint64_t count = std::min<uint64_t>(
                last - first, kGenthreadWbPrefetchOpsPerConn);
            for (uint64_t off = 0; off < count; off++) {
                Op& op = rob.at(first + off);
                if (op.state.load(std::memory_order_acquire) != OpState::Done) break;
                if (!op.zc_ptr || op.zc_shard < 0 || !op.zc_len) continue;
                const uint32_t bytes = std::min(
                    op.zc_len, kGenthreadWbBorrowPrefetchBytes);
                for (uint32_t pos = 0; pos < bytes; pos += kGenthreadCacheLineBytes)
                    __builtin_prefetch(op.zc_ptr + pos, 0, 1);
            }
        }

        work += batch.count;
        work += genthread_ifid_batch<HasTls, kEp>();
        if (__builtin_expect(pubsub_pass_pending_, false)) work += pubsub_pass_flush();

        for (uint32_t i = 0; i < batch.count; i++) {
            Client*& submit_client = batch.clients[i];
            Client* client = submit_client;
            if (!client || client->dead()) continue;
            if (__builtin_expect(
                    (climon_armed_cached_ & Server::kClimonReply) != 0, false) &&
                climon_reply_suppressed(client)) {
                bool submit_allowed;
                (void)climon_prepare_suppressed(client, submit_allowed);
                if (!submit_allowed) submit_client = nullptr;
                continue;
            }
            if constexpr (HasTls) {
                if (TlsConn* tls = tls_engine(client))
                    (void)wb_.prepare_pipeline_tls<kEp, true>(*client, *tls);
                else if (TlsConn* slot = tls_slot_conn(client); slot && slot->ktls())
                    (void)wb_.prepare_pipeline_ktls<kEp, true>(*client);
                else
                    (void)wb_.prepare_pipeline<kEp, true>(*client);
            } else {
                (void)wb_.prepare_pipeline<kEp, true>(*client);
            }
        }
        for (uint32_t i = 0; i < batch.count; i++) {
            Client* client = batch.clients[i];
            if (!client || client->dead()) continue;
            bool retry_plain_submit = false;
            if constexpr (HasTls) {
                if (TlsConn* tls = tls_engine(client)) {
                    (void)wb_.pump_tls<kEp, true>(*client, *tls);
                    if (tls->socket_userspace() && tls->has_pinned_plain())
                        arm_tls_socket_poll<kEp>(client, tls->wanted());
                    if (tls->failed())
                        close_client(client,
                                     tls->output_pending() || client->send_inflight());
                } else if (TlsConn* slot = tls_slot_conn(client); slot && slot->ktls()) {
                    const bool sent = wb_.pump<kEp, true>(*client);
                    retry_plain_submit = !kEp && !sent &&
                        !client->send_inflight() && !client->nothing_to_write();
                } else {
                    const bool sent = wb_.pump<kEp, true>(*client);
                    retry_plain_submit = !kEp && !sent &&
                        !client->send_inflight() && !client->nothing_to_write();
                }
            } else {
                const bool sent = wb_.pump<kEp, true>(*client);
                retry_plain_submit = !kEp && !sent &&
                    !client->send_inflight() && !client->nothing_to_write();
            }
            if constexpr (kEp)
                if (wb_.take_send_failure()) epoll_close_now(client);
            if (retry_plain_submit && !client->dead()) {
                self_->sig().sqe_starved++;
                enqueue_serve(client);
            }
            if (!client->dead() && client->in_active()) enqueue_ifid(client);
        }
        batch.count = 0;
        active_wb_context_ = nullptr;

        work += fused_executor_->fused_coarse_pass();
        return work;
    }

    // OVERLAP 2: iofused's ready lists and whole batches, with the first ordinary EX batch split at
    // its existing prefetch seam.  The gate starts closed and samples only work completed by this
    // pass. Closed rotations use the literal coarse IFID -> EX -> WB owner order; an open rotation
    // freezes and warms WB first, then runs IFID -> EX loads -> WB stores -> EX consumption. The WB
    // callback is synchronous, so neither its client batch nor EX's stack task batch crosses an
    // outer boundary.
    template <bool HasUnix, bool HasTls, bool kEp>
    uint32_t genthread_three_way_pass(WbPipelineBatch& batch, bool& gate_open) {
        if (batch.count || active_wb_context_) std::abort();
        LoopSignals& sig = self_->sig();
        uint32_t occupancy = 0;
        auto note_ops_since = [&](uint64_t before) {
            const uint64_t delta = sig.ops >= before ? sig.ops - before : 0;
            occupancy = std::max<uint32_t>(
                occupancy, static_cast<uint32_t>(std::min<uint64_t>(delta, UINT32_MAX)));
        };

        if (!gate_open) {
            uint32_t work = 0;
            uint64_t before = sig.ops;
            work += genthread_ifid_batch<HasTls, kEp>();
            note_ops_since(before);

            before = sig.ops;
            work += fused_executor_->fused_coarse_pass();
            note_ops_since(before);

            work += collect_retire_work<HasUnix, kEp, true>();
            uint32_t wb_occupancy = 0;
            work += genthread_wb_batch<HasTls, kEp, true>(&wb_occupancy);
            occupancy = std::max(occupancy, wb_occupancy);
            gate_open = occupancy >= kGenthreadThreeWayMinBatchOccupancy;
            return work;
        }

        active_wb_context_ = &batch;
        uint32_t work = collect_retire_work<HasUnix, kEp, true>();
        if (!pending_serve_.empty()) {
            AofManager& aof = srv_->aof();
            if (!aof_gate_target_) aof_gate_target_ = aof.posted_sequence();
            if (!aof.reply_gate_ready(aof_gate_target_)) {
                aof.register_send_gate_wait(self_->id());
            } else {
                aof_gate_target_ = 0;
                while (batch.count < kGenthreadPipelineWbBatchConns &&
                       !pending_serve_.empty()) {
                    Client* client = pending_serve_.front();
                    pending_serve_.pop_front();
                    client->set_serve_pending(false);
                    if (!client->dead()) batch.clients[batch.count++] = client;
                }
            }
        } else {
            aof_gate_target_ = 0;
        }

        for (uint32_t i = 0; i < batch.count; i++) {
            Client* client = batch.clients[i];
            if (!client || client->dead()) continue;
            Rob<kRobWindow>& rob = client->rob();
            const uint64_t first = rob.flush_id();
            const uint64_t last = rob.dispatch_id();
            const uint64_t count = std::min<uint64_t>(
                last - first, kGenthreadWbPrefetchOpsPerConn);
            for (uint64_t off = 0; off < count; off++)
                __builtin_prefetch(&rob.at(first + off).state, 0, 3);
        }
        for (uint32_t i = 0; i < batch.count; i++) {
            Client* client = batch.clients[i];
            if (!client || client->dead()) continue;
            Rob<kRobWindow>& rob = client->rob();
            const uint64_t first = rob.flush_id();
            const uint64_t last = rob.dispatch_id();
            const uint64_t count = std::min<uint64_t>(
                last - first, kGenthreadWbPrefetchOpsPerConn);
            for (uint64_t off = 0; off < count; off++) {
                Op& op = rob.at(first + off);
                if (op.state.load(std::memory_order_acquire) != OpState::Done) break;
                if (!op.zc_ptr || op.zc_shard < 0 || !op.zc_len) continue;
                const uint32_t bytes = std::min(
                    op.zc_len, kGenthreadWbBorrowPrefetchBytes);
                for (uint32_t pos = 0; pos < bytes; pos += kGenthreadCacheLineBytes)
                    __builtin_prefetch(op.zc_ptr + pos, 0, 1);
            }
        }

        const uint32_t wb_occupancy = batch.count;
        occupancy = std::max(occupancy, wb_occupancy);
        work += wb_occupancy;
        const uint64_t before_ifid = sig.ops;
        work += genthread_ifid_batch<HasTls, kEp>();
        note_ops_since(before_ifid);
        if (__builtin_expect(pubsub_pass_pending_, false)) work += pubsub_pass_flush();

        bool wb_filled = false;
        auto wb_filler = [&] {
            if (wb_filled) std::abort();
            wb_filled = true;
            for (uint32_t i = 0; i < batch.count; i++) {
                Client*& submit_client = batch.clients[i];
                Client* client = submit_client;
                if (!client || client->dead()) continue;
                if (__builtin_expect(
                        (climon_armed_cached_ & Server::kClimonReply) != 0, false) &&
                    climon_reply_suppressed(client)) {
                    bool submit_allowed;
                    (void)climon_prepare_suppressed(client, submit_allowed);
                    if (!submit_allowed) submit_client = nullptr;
                    continue;
                }
                if constexpr (HasTls) {
                    if (TlsConn* tls = tls_engine(client))
                        (void)wb_.prepare_pipeline_tls<kEp, true>(*client, *tls);
                    else if (TlsConn* slot = tls_slot_conn(client); slot && slot->ktls())
                        (void)wb_.prepare_pipeline_ktls<kEp, true>(*client);
                    else
                        (void)wb_.prepare_pipeline<kEp, true>(*client);
                } else {
                    (void)wb_.prepare_pipeline<kEp, true>(*client);
                }
            }
            for (uint32_t i = 0; i < batch.count; i++) {
                Client* client = batch.clients[i];
                if (!client || client->dead()) continue;
                bool retry_plain_submit = false;
                if constexpr (HasTls) {
                    if (TlsConn* tls = tls_engine(client)) {
                        (void)wb_.pump_tls<kEp, true>(*client, *tls);
                        if (tls->socket_userspace() && tls->has_pinned_plain())
                            arm_tls_socket_poll<kEp>(client, tls->wanted());
                        if (tls->failed())
                            close_client(client,
                                         tls->output_pending() || client->send_inflight());
                    } else if (TlsConn* slot = tls_slot_conn(client); slot && slot->ktls()) {
                        const bool sent = wb_.pump<kEp, true>(*client);
                        retry_plain_submit = !kEp && !sent &&
                            !client->send_inflight() && !client->nothing_to_write();
                    } else {
                        const bool sent = wb_.pump<kEp, true>(*client);
                        retry_plain_submit = !kEp && !sent &&
                            !client->send_inflight() && !client->nothing_to_write();
                    }
                } else {
                    const bool sent = wb_.pump<kEp, true>(*client);
                    retry_plain_submit = !kEp && !sent &&
                        !client->send_inflight() && !client->nothing_to_write();
                }
                if constexpr (kEp)
                    if (wb_.take_send_failure()) epoll_close_now(client);
                if (retry_plain_submit && !client->dead()) {
                    self_->sig().sqe_starved++;
                    enqueue_serve(client);
                }
                if (!client->dead() && client->in_active()) enqueue_ifid(client);
            }
            batch.count = 0;
            active_wb_context_ = nullptr;
        };

        const uint64_t before_ex = sig.ops;
        work += fused_executor_->fused_three_way_pass(wb_filler);
        note_ops_since(before_ex);
        if (!wb_filled || batch.count || active_wb_context_) std::abort();

        gate_open = occupancy >= kGenthreadThreeWayMinBatchOccupancy;
        return work;
    }

    template <bool HasTls, bool kEp, bool ReportOccupancy = false>
    uint32_t genthread_wb_batch(uint32_t* occupancy = nullptr) {
        if constexpr (ReportOccupancy) {
            if (!occupancy) std::abort();
            *occupancy = 0;
        }
        uint32_t work = 0;
        if (__builtin_expect(pubsub_pass_pending_, false)) work += pubsub_pass_flush();
        if (!pending_serve_.empty()) {
            AofManager& aof = srv_->aof();
            if (!aof_gate_target_) aof_gate_target_ = aof.posted_sequence();
            if (!aof.reply_gate_ready(aof_gate_target_)) {
                aof.register_send_gate_wait(self_->id());
                return work;
            }
            aof_gate_target_ = 0;
        } else {
            aof_gate_target_ = 0;
        }
        uint32_t served = 0;
        while (served < kGenthreadPipelineWbBatchConns && !pending_serve_.empty()) {
            Client* c = pending_serve_.front();
            pending_serve_.pop_front();
            c->set_serve_pending(false);
            if (c->dead()) continue;
            served++;
            if (__builtin_expect((climon_armed_cached_ & Server::kClimonReply) != 0, false) &&
                climon_reply_suppressed(c)) {
                work += climon_serve_suppressed(c);
                if constexpr (kEp)
                    if (wb_.take_send_failure()) epoll_close_now(c);
                continue;
            }
            if constexpr (HasTls) {
                if (TlsConn* tls = tls_engine(c)) {
                    if (wb_.serve_tls<kEp, true, true>(*c, *tls)) work++;
                    if (tls->socket_userspace() && tls->has_pinned_plain())
                        arm_tls_socket_poll<kEp>(c, tls->wanted());
                    if (tls->failed())
                        close_client(c, tls->output_pending() || c->send_inflight());
                } else if (TlsConn* slot = tls_slot_conn(c); slot && slot->ktls()) {
                    if (wb_.serve_ktls<kEp, true, true>(*c)) work++;
                } else if (wb_.serve<kEp, true, true>(*c)) {
                    work++;
                }
            } else if (wb_.serve<kEp, true, true>(*c)) {
                work++;
            }
            if constexpr (kEp)
                if (wb_.take_send_failure()) epoll_close_now(c);
        }
        if constexpr (ReportOccupancy) *occupancy = served;
        return work + served;
    }

    // Idle audit shared by both overlapping fused schedules.
    template <bool HasUnix, bool HasTls, bool kEp>
    uint32_t genthread_iofused_sweep() {
        uint32_t work = 0;
        if constexpr (HasUnix) work += flush_handoffs();
        work += service_client_migrations<kEp>() + drain_client_transfers<kEp>(true) +
                flush_borrow_releases();
        work += genthread_ifid_batch<HasTls, kEp>();
        work += fused_executor_->fused_coarse_sweep();
        work += collect_retire_work<HasUnix, kEp, true>(true) +
                genthread_wb_batch<HasTls, kEp>();
        if (__builtin_expect(!routing_forward_.empty(), false))
            client_routing_cleanup_pass();
        if (srv_->snapshot().writer_is(self_->id()))
            work += srv_->snapshot().writer_pass(*self_, ring_, true);
        if (srv_->aof().writer_is(self_->id()))
            work += srv_->aof().writer_pass(*self_, ring_, true);
        return work;
    }

    struct IfidBatch {
        std::array<Client*, kIoPipeIfidBatchClients> clients{};
        uint32_t count = 0;
        void clear() { count = 0; }
    };

    struct WbBatch {
        std::array<Client*, kIoPipeWbBatchClients> clients{};
        std::array<bool, kIoPipeWbBatchClients> submit_allowed{};
        uint32_t count = 0;
        void clear() { count = 0; }
    };

    // Detach one backlog-scaled connection batch from the serve FIFO. Clearing serve_pending at
    // detach preserves the old race contract: a completion that lands while this batch is being
    // prepared can enqueue a fresh future visit.
    uint32_t wb_gather(WbBatch& batch) {
        if (batch.count) std::abort();
        if (pending_serve_.empty()) {
            aof_gate_target_ = 0;
            return 0;
        }

        AofManager& aof = srv_->aof();
        if (!aof_gate_target_) aof_gate_target_ = aof.posted_sequence();
        if (!aof.reply_gate_ready(aof_gate_target_)) {
            aof.register_send_gate_wait(self_->id());
            return 0;
        }
        aof_gate_target_ = 0;

        const size_t visits = std::min<size_t>(pending_serve_.size(),
                                               kIoPipeWbBatchClients);
        for (size_t i = 0; i < visits; i++) {
            Client* client = pending_serve_.front();
            pending_serve_.pop_front();
            client->set_serve_pending(false);
            if (!client->dead()) {
                batch.clients[batch.count] = client;
                batch.submit_allowed[batch.count] = true;
                batch.count++;
            }
        }
        return batch.count;
    }

    // WB.OBSERVE: consume completion hints, then gather the shallow pipeline's WB batch early so
    // its state/payload prefetch can overlap IFID work.
    template <bool HasUnix, bool kEp>
    uint32_t wb_observe(bool unmasked, WbBatch& batch) {
        return collect_retire_work<HasUnix, kEp>(unmasked) + wb_gather(batch);
    }

    // WB.PF pass 1 issues hints for every potentially retireable state line. Pass 2 performs the
    // required acquire and, only for a published plain BORROW, hints the store payload. The hint
    // neither reads nor copies payload bytes and never changes the borrow lifetime.
    void wb_prefetch(WbBatch& batch) {
        for (uint32_t i = 0; i < batch.count; i++) {
            Client* client = batch.clients[i];
            if (client->dead()) continue;
            Rob<kRobWindow>& rob = client->rob();
            const uint64_t first = rob.flush_id();
            const uint64_t last = rob.dispatch_id();
            const uint64_t count = std::min<uint64_t>(last - first,
                                                       kIoPipeWbPrefetchOpsPerClient);
            for (uint64_t off = 0; off < count; off++)
                __builtin_prefetch(&rob.at(first + off).state, 0, 3);
        }
        for (uint32_t i = 0; i < batch.count; i++) {
            Client* client = batch.clients[i];
            if (client->dead()) continue;
            Rob<kRobWindow>& rob = client->rob();
            const uint64_t first = rob.flush_id();
            const uint64_t last = rob.dispatch_id();
            const uint64_t count = std::min<uint64_t>(last - first,
                                                       kIoPipeWbPrefetchOpsPerClient);
            for (uint64_t off = 0; off < count; off++) {
                Op& op = rob.at(first + off);
                if (op.state.load(std::memory_order_acquire) != OpState::Done) break;
                if (!op.zc_ptr || op.zc_shard < 0 || !op.zc_len) continue;
                const uint32_t bytes = std::min(op.zc_len, kIoPipeWbBorrowPrefetchBytes);
                for (uint32_t pos = 0; pos < bytes; pos += kIoPipeCacheLineBytes)
                    __builtin_prefetch(op.zc_ptr + pos, 0, 1);
            }
        }
    }

    // IFID.RX: harvest this thread's wire completions/readiness, then select a bounded slice of the
    // independently maintained active set. In epoll mode the same tagged callback stream comes
    // from the doorbell mailbox before socket readiness is drained.
    template <bool HasUnix, bool HasTls, bool kEp>
    uint32_t ifid_rx(IfidBatch& batch) {
        uint32_t work = ring_.for_each_cqe(
            [&](io_uring_cqe* cqe) { on_cqe<HasTls, kEp, false, 1>(cqe); });
        if constexpr (kEp) work += epoll_pass<HasUnix, HasTls, false, 1>(0);
        if (batch.count) std::abort();
        const size_t available = active_.size();
        if (!available) { ifid_cursor_ = 0; return work; }
        if (ifid_cursor_ >= available) ifid_cursor_ = 0;
        const size_t visits = std::min<size_t>(available, kIoPipeIfidBatchClients);
        for (size_t i = 0; i < visits; i++) {
            Client* client = active_.at(ifid_cursor_);
            if (++ifid_cursor_ == available) ifid_cursor_ = 0;
            if (!client->dead()) batch.clients[batch.count++] = client;
        }
        return work;
    }

    // ---- IFID.PARSE+HASH: read-buffer maintenance, decode/hash/route, quiet publication --------
    template <bool HasTls, bool kEp>
    uint32_t ifid_parse_hash(IfidBatch& batch) {
        uint32_t work = 0;
        backstop_pass_ = (++flush_tick_ >= kIoPipeWbBackstopTurns);
        if (backstop_pass_) flush_tick_ = 0;

        // The read side is one bounded batch before this rotation's retirement/send. Arming first
        // keeps the arrival stream flowing independently of the reply backlog.
        for (uint32_t batch_index = 0; batch_index < batch.count; batch_index++) {
            Client* c = batch.clients[batch_index];
            if (c->dead() || !c->in_active()) continue;
            Client& conn = *c;
            DispatchResult dispatch_result = DispatchResult::Progress;
            TlsConn* tls = nullptr;
            if constexpr (HasTls) tls = tls_engine(c);
            if (backstop_pass_ && !c->serve_pending()) enqueue_serve(c);

            if constexpr (HasTls) {
                if (tls) {
                    // Only the recv completion may drive inbound TLS while its BIO input frontier
                    // is pinned. Pipeline callbacks decrypt but defer parsing to this stage.
                    if (!c->closing() && !c->recv_armed())
                        (void)drive_tls<kEp, false, 1>(c);
                    else if (tls->userspace()) {
                        (void)wb_.pump_tls<kEp>(*c, *tls);
                        if (tls->socket_userspace() && tls->has_pinned_plain())
                            arm_tls_socket_poll<kEp>(c, tls->wanted());
                    }
                    tls = tls_engine(c);
                    if constexpr (kEp)
                        if (wb_.take_send_failure()) epoll_request_close(c);
                }
            }

            if (c->scatter_barrier()) {
                if (c->blocked() &&
                    blocking_resume_move(*srv_, *self_, ring_, *c, scatter_pool_)) {
                    enqueue_serve(c);
                    work++;
                }
                if (__builtin_expect(c->barrier_held_by(BarrierOwner::Debug), false) &&
                    !srv_->debug_barrier_hold_armed())
                    c->barrier_release(BarrierOwner::Debug);
                if (c->rob().quiesced()) c->barrier_release_quiesced();
            }
            if (c->atomic_backpressure() && srv_->atomic_can_admit(self_->id()) &&
                scatter_pool_.can_register_snapshot())
                c->set_atomic_backpressure(false);
            if (c->flip_backpressure() && !srv_->flip_dispatch_paused())
                c->set_flip_backpressure(false);
            if (c->rob().quiesced() && (kEp || !conn.recv_armed()))
                conn.reset_rbuf_at_quiescence();

            if constexpr (kEp) {
                if (c->closing()) c->set_recv_armed(false);
                if (!c->closing()) {
                    if constexpr (HasTls) {
                        if (tls) arm_tls_recv<kEp, false, 1>(c);
                        else arm_recv<kEp>(c);
                    } else {
                        arm_recv<kEp>(c);
                    }
                    if constexpr (HasTls) if (tls) {
                        (void)drive_tls<kEp, false, 1>(c);
                        tls = tls_engine(c);
                    }
                    if (wb_.take_send_failure()) epoll_request_close(c);
                }
            }

            if (!c->closing() && conn.rpos() < conn.rlen() && !c->scatter_barrier() &&
                !c->parse_backpressure()) {
                // The pause accounting and transport choice are the ordinary diet-era parse path;
                // only the 64-entry ROB-publication cap and deferred IFID.POST are schedule-specific.
                if (__builtin_expect(climon_pause_armed(), false)) {
                    const uint32_t rpos_before = conn.rpos();
                    if constexpr (HasTls) {
                        if (c->is_tls())
                            dispatch_result = parse_and_dispatch<true, 0, true>(c);
                        else
                            dispatch_result = parse_and_dispatch<false, 0, true>(c);
                    } else {
                        dispatch_result = parse_and_dispatch<false, 0, true>(c);
                    }
                    if (conn.rpos() != rpos_before) work++;
                } else {
                    if constexpr (HasTls) {
                        if (c->is_tls())
                            dispatch_result = parse_and_dispatch<true, 0, true>(c);
                        else
                            dispatch_result = parse_and_dispatch<false, 0, true>(c);
                    } else {
                        dispatch_result = parse_and_dispatch<false, 0, true>(c);
                    }
                    if (__builtin_expect(dispatch_result != DispatchResult::NeedInput, true))
                        work++;
                }
            }

            if constexpr (!kEp) {
                if constexpr (HasTls) {
                    if (tls && tls->memory_bio()) arm_tls_recv<kEp, false, 1>(c);
                    else if (!tls) arm_recv<kEp>(c);
                } else {
                    arm_recv<kEp>(c);
                }
            }

            const bool stuck = (conn.rpos() < conn.rlen() && c->rob().full()) ||
                               (!conn.recv_armed() && !c->closing());
            const bool more_input = conn.rpos() < conn.rlen() &&
                                    dispatch_result != DispatchResult::NeedInput;
            const bool tls_output = tls && (tls->output_pending() || c->send_inflight());
            const bool done = c->rob().quiesced() && !more_input && !stuck &&
                              !c->serve_pending() && c->nothing_to_write() && !tls_output;
            // Batch entries remain readable through the existing corpse grace; membership is the
            // authoritative check before mutating the active set.
            if (c->dead() || !c->in_active()) continue;
            if (done && !c->closing()) {
                c->set_in_active(false);
                active_.erase(c);
            } else if (c->closing() && !tls_output && c->safe_to_release()) {
                if (pubsub_disconnect_ready(c)) {
                    c->set_in_active(false);
                    active_.erase(c);
                    close_client(c);
                }
            }
        }

        if constexpr (kEp) {
            while (!epoll_closes_.empty()) {
                Client* victim = epoll_closes_.back();
                epoll_closes_.pop_back();
                epoll_close_now(victim);
            }
        }
        return work;
    }

    // IFID.POST: quiet queue tails are already published. Fold their notification/wake edge once
    // per destination, then preserve the existing pub/sub parse-to-send boundary.
    uint32_t ifid_post(IfidBatch&) {
        uint32_t work = flush_ifid_posts();
        if (__builtin_expect(pubsub_pass_pending_, false)) work += pubsub_pass_flush();
        return work;
    }

    // WB.RETIRE+PREP: drain only the in-order Done prefix and construct reply buffers/iovecs. The
    // engine methods are the ordinary serve bodies with their final pump deliberately omitted.
    template <bool HasTls, bool kEp>
    uint32_t wb_retire_prepare(WbBatch& batch) {
        uint32_t work = 0;
        for (uint32_t i = 0; i < batch.count; i++) {
            Client* c = batch.clients[i];
            if (c->dead()) continue;
            if (__builtin_expect((climon_armed_cached_ & Server::kClimonReply) != 0, false) &&
                climon_reply_suppressed(c)) {
                work += climon_prepare_suppressed(c, batch.submit_allowed[i]);
                continue;
            }
            if constexpr (HasTls) {
                if (TlsConn* tls = tls_engine(c)) {
                    if (wb_.prepare_tls<kEp, false>(*c, *tls, batch.submit_allowed[i])) work++;
                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
                } else if (TlsConn* slot = tls_slot_conn(c); slot && slot->ktls()) {
                    if (wb_.prepare_ktls<kEp, false>(*c, batch.submit_allowed[i])) work++;
                } else if (wb_.prepare<kEp, false>(*c, batch.submit_allowed[i])) {
                    work++;
                }
            } else if (wb_.prepare<kEp, false>(*c, batch.submit_allowed[i])) {
                work++;
            }
        }
        return work;
    }

    // WB.SUBMIT+RECLAIM: form the actual sends from the warm prepared batch. Send completion
    // reclamation stays at the next rotation's RX/CQE boundary.
    template <bool HasTls, bool kEp>
    uint32_t wb_submit_reclaim(WbBatch& batch, bool& submitted) {
        uint32_t work = 0;
        for (uint32_t i = 0; i < batch.count; i++) {
            Client* c = batch.clients[i];
            if (c->dead() || !batch.submit_allowed[i]) continue;
            if constexpr (HasTls) {
                if (TlsConn* tls = tls_engine(c)) {
                    if (wb_.pump_tls<kEp>(*c, *tls)) work++;
                    if (tls->socket_userspace() && tls->has_pinned_plain())
                        arm_tls_socket_poll<kEp>(c, tls->wanted());
                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
                } else if (wb_.pump<kEp>(*c)) {
                    work++;
                }
            } else if (wb_.pump<kEp>(*c)) {
                work++;
            }
            if constexpr (kEp)
                if (wb_.take_send_failure()) epoll_close_now(c);
        }
        // The source schedule's early fire point is preserved. The live liburing query replaces
        // its shadow pending counter, so the diet keeps zero work at individual SQE producers.
        if constexpr (!kEp) {
            if (ring_.sq_ready()) {
                ring_.submit_and_reap();
                submitted = true;
            }
        }
        return work;
    }

    // At depth, completed IFID work already provides the latency-hiding window. Retain the plain
    // split loop's combined retire/stage/pump order and skip the separate WB prefetch walks.
    template <bool HasTls, bool kEp>
    uint32_t wb_serve_natural(WbBatch& batch, bool& submitted) {
        uint32_t work = 0;
        for (uint32_t i = 0; i < batch.count; i++) {
            Client* c = batch.clients[i];
            if (c->dead()) continue;
            if (__builtin_expect((climon_armed_cached_ & Server::kClimonReply) != 0, false) &&
                climon_reply_suppressed(c)) {
                work += climon_serve_suppressed(c);
                if constexpr (kEp)
                    if (wb_.take_send_failure()) epoll_close_now(c);
                continue;
            }
            if constexpr (HasTls) {
                if (TlsConn* tls = tls_engine(c)) {
                    if (wb_.serve_tls<kEp, false, false>(*c, *tls)) work++;
                    if (tls->socket_userspace() && tls->has_pinned_plain())
                        arm_tls_socket_poll<kEp>(c, tls->wanted());
                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
                } else if (TlsConn* slot = tls_slot_conn(c); slot && slot->ktls()) {
                    if (wb_.serve_ktls<kEp, false, false>(*c)) work++;
                } else if (wb_.serve<kEp, false, false>(*c)) {
                    work++;
                }
            } else if (wb_.serve<kEp, false, false>(*c)) {
                work++;
            }
            if constexpr (kEp)
                if (wb_.take_send_failure()) epoll_close_now(c);
        }
        if constexpr (!kEp) {
            if (ring_.sq_ready()) {
                ring_.submit_and_reap();
                submitted = true;
            }
        }
        return work;
    }

    template <bool HasUnix, bool HasTls, bool kEp>
    uint32_t pipeline_pass(bool unmasked, bool natural_order, bool& submitted) {
        // One synchronous buffer per stream, exactly as measured. Cross-core queue publications
        // and kernel SQEs own their data after their stage, so no ping/pong lifetime is required.
        IfidBatch& ifid = ifid_batch_;
        WbBatch& wb = wb_batch_;
        if (ifid.count || wb.count) std::abort();
        uint32_t work = 0;
        if (natural_order) {
            work += ifid_rx<HasUnix, HasTls, kEp>(ifid);
            work += collect_retire_work<HasUnix, kEp>(unmasked);
            work += ifid_parse_hash<HasTls, kEp>(ifid);
            work += ifid_post(ifid);
            work += wb_gather(wb);
            work += wb_serve_natural<HasTls, kEp>(wb, submitted);
        } else {
            for (const IoPipeStage stage : kIoPipeSchedule) {
                switch (stage) {
                    case IoPipeStage::WbObserve:
                        work += wb_observe<HasUnix, kEp>(unmasked, wb);
                        break;
                    case IoPipeStage::IfidRx:
                        work += ifid_rx<HasUnix, HasTls, kEp>(ifid);
                        break;
                    case IoPipeStage::WbPrefetch:
                        wb_prefetch(wb);
                        break;
                    case IoPipeStage::IfidParseHash:
                        work += ifid_parse_hash<HasTls, kEp>(ifid);
                        break;
                    case IoPipeStage::WbRetirePrepare:
                        work += wb_retire_prepare<HasTls, kEp>(wb);
                        break;
                    case IoPipeStage::IfidPost:
                        work += ifid_post(ifid);
                        break;
                    case IoPipeStage::WbSubmitReclaim:
                        work += wb_submit_reclaim<HasTls, kEp>(wb, submitted);
                        break;
                }
            }
        }
        ifid.clear();
        wb.clear();
        return work;
    }

    // ---- retire -> stage bytes -> send or hand off -------------------------------------------------
    // The io thread's own work per active client. In 2-stage it also owns the reply side and calls
    // serve() here; in ex-wb and 3-stage the sender does that on its own thread and io only keeps
    // the READ side moving — reclaim the buffer once nothing points into it, and re-arm.
    template <bool HasTls, bool kEp, bool Fused = false, bool HasUnix = false,
              bool SweepPass = false>
    uint32_t flush_ready() {
        uint32_t work = 0;
        backstop_pass_ = (++flush_tick_ >= kFlushBackstopEvery);
        if (backstop_pass_) flush_tick_ = 0;

        // PHASE 1 -- the read side, every active conn, BEFORE any serving. At 2048 conns the old
        // interleaved pass collapsed loop iterations 7.5x: each pass walked ~100 conns doing serve
        // and send work while drained recvs sat un-armed, sockets backed up, and arrivals went
        // bursty (113k park/wake round-trips where 22k belonged). Arming first keeps the arrival
        // stream flowing no matter how deep the reply backlog is -- which is exactly the property
        // that made 3s hold flat (-3.7%) at the conn count where 2s lost 21%.
        for (size_t idx = 0; idx < active_.size();) {
            Client* c = active_.at(idx);
            Client& conn = *c;
            DispatchResult dispatch_result = DispatchResult::Progress;
            TlsConn* tls = nullptr;
            if constexpr (HasTls) tls = tls_engine(c);
            if (backstop_pass_ && !c->serve_pending()) enqueue_serve(c);

            if constexpr (HasTls) {
                if (tls) {
                    // BIO_nwrite0 pins the input-ring frontier until the recv CQE commits it.
                    // SSL_write and opposite-direction BIO reads are proven safe while pinned,
                    // but SSL_read/SSL_accept consume the same direction and can move that
                    // frontier. Only the recv completion may drive inbound TLS while armed.
                    if (!c->closing() && !c->recv_armed()) (void)drive_tls<kEp, Fused>(c);
                    else if (tls->userspace()) {
                        (void)wb_.pump_tls<kEp>(*c, *tls);
                        if (tls->socket_userspace() && tls->has_pinned_plain())
                            arm_tls_socket_poll<kEp>(c, tls->wanted());
                    }
                    // A successful fd handshake can switch transport under drive_tls().
                    tls = tls_engine(c);
                    if constexpr (kEp) if (wb_.take_send_failure()) epoll_request_close(c);
                }
            }

            // Reset only when the ROB is quiescent AND no recv is outstanding — see conn.h. Then
            // re-arm, in that order.
            if (c->scatter_barrier()) {
                if (c->blocked() &&
                    blocking_resume_move(*srv_, *self_, ring_, *c, scatter_pool_)) {
                    enqueue_serve(c);
                    work++;
                }
                // TEST HOOK (DEBUG BARRIER-HOLD) release, BEFORE the quiescence backstop so a
                // cleared latch and the backstop can both land in the same pass. Guarded on the
                // connection's own bit first, so production pays one byte test inside an arm that
                // already only runs when a barrier is set -- the server-wide load never happens.
                if (__builtin_expect(c->barrier_held_by(BarrierOwner::Debug), false) &&
                    !srv_->debug_barrier_hold_armed())
                    c->barrier_release(BarrierOwner::Debug);
                // With nothing in flight every production owner has completed by definition, so
                // this releases all of them at once. It is the backstop for the four owners
                // (WAIT, EXEC, pub/sub, CLIENT fan-out) that have no owner-scoped release site.
                if (c->rob().quiesced()) c->barrier_release_quiesced();
            }
            if (c->atomic_backpressure() && srv_->atomic_can_admit(self_->id()) &&
                scatter_pool_.can_register_snapshot())
                c->set_atomic_backpressure(false);
            // Success, pre-commit rollback, and synchronous validation refusal all end by publishing
            // Idle. The flag travels with a migrated Client, so this runs on whichever IO owns it
            // after the FLIP and retries the still-unconsumed frame in the re-parse below.
            if (c->flip_backpressure() && !srv_->flip_dispatch_paused())
                c->set_flip_backpressure(false);
            // Under epoll the second half of this guard is vacuous and would be actively
            // harmful: recv_armed_ means "an edge is owed", not "the kernel holds a pointer into
            // this buffer" (nothing ever does under this engine), so testing it would make the
            // steady state -- armed and quiet -- the one state in which the append-only read
            // buffer never resets, and a long-lived connection would grow to the soft cap and
            // stall there. Quiescence alone is the real precondition, and it still holds.
            if (c->rob().quiesced() && (kEp || !conn.recv_armed()))
                conn.reset_rbuf_at_quiescence();
            // Fill from the socket BEFORE the re-parse below, so bytes that arrived while the ROB
            // window was full are parsed in the same pass that freed the slots.
            if constexpr (kEp) {
                // A CLOSING CONNECTION OWES NO EDGE. Under io_uring this is automatic: arm_recv
                // refuses to re-arm a closing connection, so recv_armed_ falls to false as soon as
                // the outstanding recv completes and safe_to_release() opens. Under epoll nothing
                // completes -- the flag says "an edge is owed", and for a socket being torn down
                // that is simply false. Leaving it set makes safe_to_release() refuse forever, and
                // the connection is never released.
                //
                // The path that exposed this is CLIENT KILL on SELF, which reaches its victim
                // through mark_closing() rather than close_client() (the reply has to go out
                // first), so nothing else would ever clear the flag: the reply was delivered and
                // the socket then stayed open indefinitely. tests/climon.py's
                // "KILL self close-after-reply" is the regression cover.
                if (c->closing()) c->set_recv_armed(false);
                if (!c->closing()) {
                    if constexpr (HasTls) {
                        if (tls) arm_tls_recv<kEp, Fused>(c);
                        else arm_recv<kEp>(c);
                    } else {
                        arm_recv<kEp>(c);
                    }
                    if constexpr (HasTls) if (tls) {
                        (void)drive_tls<kEp, Fused>(c);
                        tls = tls_engine(c);
                    }
                    if (wb_.take_send_failure()) epoll_request_close(c);
                }
            }

            // Re-parse the buffered remainder. parse_and_dispatch stops when the ROB window fills
            // and is otherwise only driven by recv completions, so a client that sent a whole
            // pipeline in ONE write would get `window` replies and then hang. Retiring frees slots,
            // which is what makes the rest parseable.
            if (!c->closing() && conn.rpos() < conn.rlen() && !c->scatter_barrier() &&
                !c->parse_backpressure()) {
                // A CLIENT PAUSE hold deliberately leaves the parsed frame at rpos. Counting that
                // as work would spin the ring at 100% until the deadline instead of parking it,
                // so while a pause is live the pass reports progress only if the cursor moved.
                // With no pause armed the accounting is byte-for-byte the pre-lane behaviour --
                // one predicted-false test per active connection per pass. The dispatch variant
                // stays keyed on c->is_tls() (the kTLS handoff's contract), not the slot pointer.
                if (__builtin_expect(climon_pause_armed(), false)) {
                    const uint32_t rpos_before = conn.rpos();
                    if constexpr (HasTls) {
                        if (c->is_tls())
                            dispatch_result = parse_and_dispatch<
                                true, Fused ? kGenthreadIfidBatchOps : 0>(c);
                        else
                            dispatch_result = parse_and_dispatch<
                                false, Fused ? kGenthreadIfidBatchOps : 0>(c);
                    } else {
                        dispatch_result = parse_and_dispatch<
                            false, Fused ? kGenthreadIfidBatchOps : 0>(c);
                    }
                    if (conn.rpos() != rpos_before) work++;
                } else {
                    if constexpr (HasTls) {
                        if (c->is_tls())
                            dispatch_result = parse_and_dispatch<
                                true, Fused ? kGenthreadIfidBatchOps : 0>(c);
                        else
                            dispatch_result = parse_and_dispatch<
                                false, Fused ? kGenthreadIfidBatchOps : 0>(c);
                    } else {
                        dispatch_result = parse_and_dispatch<
                            false, Fused ? kGenthreadIfidBatchOps : 0>(c);
                    }
                    if (__builtin_expect(
                            dispatch_result != DispatchResult::NeedInput, true))
                        work++;
                }
            }

            if constexpr (!kEp) {
                if constexpr (HasTls) {
                    if (tls && tls->memory_bio()) arm_tls_recv<kEp, Fused>(c);
                    else if (!tls) arm_recv<kEp>(c);
                } else {
                    arm_recv<kEp>(c);
                }
            }

            // Progress marker: a full window with unparsed bytes (or an unarmed recv) means this
            // conn must stay active so later passes retry once retiring frees slots. We are our own
            // sender, so no poke protocol is needed -- the flush_ready pass IS the retry.
            const bool stuck = (conn.rpos() < conn.rlen() && c->rob().full()) ||
                               (!conn.recv_armed() && !c->closing());

            // NeedInput parks only the read side: the partial bytes stay buffered and a recv is
            // already armed. Unresolved local reads never hold parsing; conflicts reroute them.
            const bool more_input = conn.rpos() < conn.rlen() &&
                                    dispatch_result != DispatchResult::NeedInput;
            const bool tls_output = tls && (tls->output_pending() || c->send_inflight());
            const bool done = c->rob().quiesced() && !more_input && !stuck &&
                              !c->serve_pending() && c->nothing_to_write() && !tls_output;
            // A close reached from inside the body (drive_tls, and under epoll the read/send
            // paths) may already have removed this client from the set and swapped an unvisited
            // one into its slot. Re-check identity before deciding, and do not advance past a slot
            // whose occupant changed -- the loop bound shrinks with every removal, so this
            // terminates.
            if (idx >= active_.size() || active_.at(idx) != c) continue;
            if (done && !c->closing()) { c->set_in_active(false); active_.erase_at(idx); }
            else if (c->closing() && !tls_output && c->safe_to_release()) {
                // Pub/sub teardown is asynchronous. Keep the client in place while home IOs
                // acknowledge removal; erase+reinsert would turn one closing subscriber into a
                // same-pass spin.
                if (!pubsub_disconnect_ready(c)) { idx++; }
                else { c->set_in_active(false); active_.erase_at(idx); close_client(c); }
            } else idx++;
        }
        // The epoll engine's deferred closes. A synchronous recv/send discovers a dead peer several
        // frames inside the walk above; closing it THERE would mutate the set under the walk, so
        // the sites queue instead and the teardown happens here, between phases, where nothing is
        // iterating. Duplicates are harmless -- close_client is idempotent on an already-dead
        // client and simply retries one whose quiescence fence has not opened yet.
        if constexpr (kEp) {
            while (!epoll_closes_.empty()) {
                Client* victim = epoll_closes_.back();
                epoll_closes_.pop_back();
                epoll_close_now(victim);
            }
        }

        // The fused loop rotates whole streams: finish the bounded IFID phase above, execute one
        // batch, collect its local self-lane completions, then enter the existing write-back phase.
        if constexpr (Fused) {
            work += SweepPass ? fused_executor_->fused_baseline_sweep()
                              : fused_executor_->fused_baseline_pass();
            work += collect_retire_work<HasUnix, kEp>(SweepPass);
        }

        // PUB/SUB PASS BOUNDARY -- between parsing and serving, on purpose. Everything this pass
        // parsed is resolved and appended to its subscribers' buffers HERE, so PHASE 2 sends one
        // coalesced write per subscriber instead of one per message. Off/unarmed servers pay one
        // predicted branch on a bool; all the machinery is out-of-line and cold.
        if (__builtin_expect(pubsub_pass_pending_, false)) work += pubsub_pass_flush();

        // PHASE 2 -- serve AT MOST kServeBudget conns from the FIFO. Bounding the pass is the
        // fourth application of the same law (per-pass work scales with what the pass does, not
        // with connection count): the leftovers stay queued, did > 0 keeps the loop from parking,
        // and FIFO order is arrival-order fairness across connections. Under overload the queue is
        // the latency -- which is the correct place for overload to live; throughput stays at peak.
        if (!pending_serve_.empty()) {
            AofManager& aof = srv_->aof();
            if (!aof_gate_target_) aof_gate_target_ = aof.posted_sequence();
            if (!aof.reply_gate_ready(aof_gate_target_)) {
                aof.register_send_gate_wait(self_->id());
                return work;
            }
            aof_gate_target_ = 0;
        } else {
            aof_gate_target_ = 0;
        }
        uint32_t served = 0;
        constexpr uint32_t serve_budget = Fused ? kGenthreadWbBatchConns : kServeBudget;
        while (served < serve_budget && !pending_serve_.empty()) {
            Client* c = pending_serve_.front();
            pending_serve_.pop_front();
            c->set_serve_pending(false);
            // Closing conns MUST still be served -- their ROB has to drain before quiesce can let
            // close_client finish. Only corpses (freed-pending) are skippable.
            if (c->dead()) continue;
            served++;
            // CLIENT REPLY OFF/SKIP. ONE predicted-false test per SERVED CONNECTION -- not per
            // operation: a p32 batch amortises it over 32 replies. The suppressed drain lives in
            // the cold object and discards bytes instead of staging them.
            if (__builtin_expect((climon_armed_cached_ & Server::kClimonReply) != 0, false) &&
                climon_reply_suppressed(c)) {
                work += climon_serve_suppressed(c);
                if constexpr (kEp) if (wb_.take_send_failure()) epoll_close_now(c);
                continue;
            }
            if constexpr (HasTls) {
                if (TlsConn* tls = tls_engine(c)) {
                    if (wb_.serve_tls<kEp, false, Fused>(*c, *tls)) work++;
                    if (tls->socket_userspace() && tls->has_pinned_plain())
                        arm_tls_socket_poll<kEp>(c, tls->wanted());
                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
                } else if (TlsConn* slot = tls_slot_conn(c); slot && slot->ktls()) {
                    if (wb_.serve_ktls<kEp, false, Fused>(*c)) work++;
                } else if (wb_.serve<kEp, false, Fused>(*c)) {
                    work++;
                }
            } else if (wb_.serve<kEp, false, Fused>(*c)) {
                work++;
            }
            // A synchronous send has no CQE to report a fatal errno through, so the engine latches
            // it and the decision to tear the connection down is taken here instead. Consuming it
            // per served connection is deliberate: a bit left set would close the NEXT one.
            if constexpr (kEp) if (wb_.take_send_failure()) epoll_close_now(c);
        }
        work += served;
        return work;
    }

    void enqueue_serve(Client* c) {
        if (c->serve_pending()) return;                 // already queued
        c->set_serve_pending(true);
        pending_serve_.push_back(c);
    }

    // IO owns both ROB frontiers, so this 100us signal beat can maintain head_since without a
    // Client field or a per-operation hook. The active set bounds the walk to connections already
    // carrying work. Entries not seen in this beat are erased, which also makes client teardown and
    // pointer reuse harmless without touching either lifecycle hot path.
    void sample_rob_head_age(uint64_t now_us) {
        LoopSignals& sig = self_->sig();
        if (++rob_age_generation_ == 0) {
            rob_head_ages_.clear();
            rob_age_generation_ = 1;
        }
        uint64_t oldest_us = 0;
        bool observed = false;
        for (size_t i = 0; i < active_.size(); i++) {
            Client* client = active_.at(i);
            if (!client || client->dead()) continue;
            const uint64_t head = client->rob().flush_id();
            if (head == client->rob().dispatch_id()) continue;
            auto [it, inserted] = rob_head_ages_.try_emplace(client);
            RobHeadAge& age = it->second;
            if (inserted || age.head_id != head) {
                age.head_id = head;
                age.head_since_us = now_us;
            }
            age.seen_generation = rob_age_generation_;
            oldest_us = std::max(oldest_us, now_us - age.head_since_us);
            observed = true;
        }
        for (auto it = rob_head_ages_.begin(); it != rob_head_ages_.end();) {
            if (it->second.seen_generation != rob_age_generation_)
                it = rob_head_ages_.erase(it);
            else
                ++it;
        }
        if (observed) sig.observe_oldest_age(oldest_us);
        else          sig.clear_oldest_age();
    }

    enum class DeferredTimerKind : uint8_t { WaitZero, DebugSleepOk };
    struct DeferredTimer {
        Client* client = nullptr;
        uint64_t op_id = 0;
        uint64_t deadline_ms = 0;  // zero is WAIT's wait-forever spelling
        uint64_t slow_started_ns = 0;
        SlowlogArm slowlog_arm{};
        DeferredTimerKind kind = DeferredTimerKind::WaitZero;
    };

    bool deferred_timer_start(Client* client, uint64_t op_id, DeferredTimerKind kind,
                              uint64_t delay_ms, uint64_t slow_started_ns,
                              const SlowlogArm& slowlog_arm) {
        const uint64_t now_ms = now_ns() / 1000000ull;
        const uint64_t deadline_ms = !delay_ms ? 0
            : delay_ms > UINT64_MAX - now_ms ? UINT64_MAX
                                             : now_ms + delay_ms;
        try {
            deferred_timers_.push_back(
                DeferredTimer{client, op_id, deadline_ms, slow_started_ns, slowlog_arm, kind});
        }
        catch (const std::bad_alloc&) { return false; }
        srv_->blocking_client_parked();
        return true;
    }

    uint32_t deferred_timer_pass(uint64_t now_ms) {
        uint32_t completed = 0;
        for (size_t i = 0; i < deferred_timers_.size();) {
            const DeferredTimer timer = deferred_timers_[i];
            if (!timer.deadline_ms || now_ms < timer.deadline_ms) {
                i++;
                continue;
            }
            Client* client = timer.client;
            Op& op = client->rob().at(timer.op_id);
            if (timer.kind == DeferredTimerKind::WaitZero) {
                reply_int(op.sink(), 0);
            } else {
                reply_ok(op.sink());
                if (timer.slowlog_arm.armed()) {
                    timespec wall{};
                    ::clock_gettime(CLOCK_REALTIME, &wall);
                    slowlog_record(self_->id(), client->id(), op,
                                   now_ns() - timer.slow_started_ns,
                                   static_cast<int64_t>(wall.tv_sec) * 1000 +
                                       wall.tv_nsec / 1000000,
                                   timer.slowlog_arm, true);
                }
            }
            op.state.store(OpState::Done, std::memory_order_release);
            client->set_blocked(false);
            client->set_last_interaction_s(cached_now_s_);
            srv_->blocking_client_unparked();
            deferred_timers_[i] = deferred_timers_.back();
            deferred_timers_.pop_back();
            enqueue_serve(client);
            mark_active(client);
            completed++;
        }
        return completed;
    }

    bool deferred_timer_cancel(Client* client) {
        bool cancelled = false;
        for (size_t i = 0; i < deferred_timers_.size();) {
            const DeferredTimer timer = deferred_timers_[i];
            if (timer.client != client) {
                i++;
                continue;
            }
            Op& op = client->rob().at(timer.op_id);
            op.state.store(OpState::Done, std::memory_order_release);
            srv_->blocking_client_unparked();
            deferred_timers_[i] = deferred_timers_.back();
            deferred_timers_.pop_back();
            cancelled = true;
        }
        if (cancelled) client->set_blocked(false);
        return cancelled;
    }

    bool client_obuf_check(Client* c, bool async) {
        if (c->closing() || c->dead()) return false;
        if (!srv_->client_obuf_armed()) {
            c->stop_obuf_tracking();
            return false;
        }
        c->start_obuf_tracking();
        const ClientLimitsConfigSnapshot limits = srv_->client_limits_snapshot();
        const ClientBufferLimit& limit = c->subscriber_mode() ? limits.pubsub : limits.normal;
        const uint64_t used = c->obuf_bytes();
        bool over = limit.hard_bytes && used >= limit.hard_bytes;
        if (!over && limit.soft_bytes && used >= limit.soft_bytes) {
            if (!c->obuf_soft_since_s()) c->set_obuf_soft_since_s(cached_now_s_);
            else if (cached_now_s_ - c->obuf_soft_since_s() > limit.soft_seconds) over = true;
        } else if (!over) {
            c->set_obuf_soft_since_s(0);
        }
        if (!over) return false;

        srv_->note_client_output_buffer_limit_disconnect();
        if (async)
            std::fprintf(stderr,
                "Client id=%llu scheduled to be closed ASAP for overcoming of output buffer limits.\n",
                static_cast<unsigned long long>(c->id()));
        else
            std::fprintf(stderr,
                "Client id=%llu closed for overcoming of output buffer limits.\n",
                static_cast<unsigned long long>(c->id()));
        close_client(c);
        // Unified W1 has already retired/staged this client but must not let W2 submit the bytes
        // which crossed the hard limit. Its batch already has a nullable-client guard, so encode
        // this rare refusal there and keep the ordinary prepare path free of a parallel bool.
        // close_client runs first while the active context still fences the Client lifetime.
        if (active_wb_context_)
            for (uint32_t i = 0; i < active_wb_context_->count; i++)
                if (active_wb_context_->clients[i] == c) {
                    active_wb_context_->clients[i] = nullptr;
                    break;
                }
        return true;
    }

    uint32_t client_cron_pass() {
        auto& clients = self_->clients();
        const size_t initial = clients.size();
        if (!initial) { client_cron_cursor_ = 0; return 0; }
        size_t visits = initial / kClientCronBeatsPerSecond;
        if (visits < kClientCronMinVisits)
            visits = std::min(initial, static_cast<size_t>(kClientCronMinVisits));

        const uint32_t timeout = srv_->timeout();
        uint32_t work = 0;
        for (size_t visited = 0; visited < visits && !clients.empty(); visited++) {
            if (client_cron_cursor_ >= clients.size()) client_cron_cursor_ = 0;
            Client* c = clients[client_cron_cursor_];
            const size_t before = clients.size();
            bool closed = false;
            if (timeout && !c->blocked() && !c->subscriber_mode() && !c->closing() &&
                cached_now_s_ - c->last_interaction_s() > timeout) {
                close_client(c);
                closed = true;
            } else if (client_obuf_check(c, false)) {
                closed = true;
            }
            work++;
            // close_client removes by swap-with-back. Revisit this index so the swapped-in client
            // is not skipped; if teardown is still pending, advance past the unchanged entry.
            if (!closed || clients.size() == before) client_cron_cursor_++;
        }
        return work;
    }

    uint32_t lb_client_signal_pass() {
        lb_client_observations_.clear();
        if (lb_client_observations_.capacity() < self_->clients().size())
            lb_client_observations_.reserve(self_->clients().size());
        for (Client* client : self_->clients()) {
            if (client->dead()) continue;
            lb_client_observations_.push_back({client->id(), client->rob().dispatch_id(),
                                               client->rob().in_flight()});
        }
        srv_->lb_publish_client_observations(self_->id(), lb_client_observations_);
        return static_cast<uint32_t>(lb_client_observations_.size());
    }

    void refresh_notify_config() {
        LiveConfigSnapshot snapshot;
        if (!srv_->live_config_snapshot_if_changed(notify_config_version_, snapshot)) return;
        notify_config_armed_ = snapshot.notify_events != 0;
        save_config_armed_ = snapshot.save_armed;
        notify_armed_ = notify_config_armed_ || save_config_armed_ ||
                        climon_armed_cached_ != 0;
        proto_max_bulk_len_ = snapshot.proto_max_bulk_len;
        // Connection-local commands never reach an executor, so the IO thread owns their timing.
        // Same snapshot, same pass, no extra load.
        slowlog_arm_.slowlog_us = snapshot.slowlog_log_slower_than;
        slowlog_arm_.latency_ms = snapshot.latency_monitor_threshold;
        slowlog_armed_ = slowlog_arm_.armed();
        if (srv_->read_local_enabled()) {
            fused_executor_->set_read_local_point_writes_precise(
                snapshot.maxmemory == 0 || snapshot.policy == MaxmemoryPolicy::NoEviction);
            fused_executor_->set_read_local_keymiss_notify(
                (snapshot.notify_events & NOTIFY_KEY_MISS) != 0);
        }
        notify_config_version_ = snapshot.version;
    }

    void close_client(Client* c, bool drain_tls_output = false) {
        // IDEMPOTENT, and that is load-bearing: an abrupt disconnect can close a conn twice --
        // once when the recv fails and again when the in-flight reply's send CQE comes back
        // failed. The second call found the client already parked on the deferred-free list and
        // parked it AGAIN: a double delete, one reap later. Caught by the torture battery's RST
        // churn under ASAN; latent since the first teardown path was written.
        if (c->dead()) return;
        if (!c->closing()) {
            c->mark_closing();
            if (deferred_timer_cancel(c)) enqueue_serve(c);
            if (c->blocked() && blocking_cancel_client(*srv_, *self_, ring_, *c))
                enqueue_serve(c);
            TlsConn* slot_tls = tls_slot_conn(c);
            if (slot_tls && slot_tls->ktls() && !c->send_inflight() &&
                !slot_tls->shutdown_started()) {
                // recv(EIO) is how kTLS reports peer close_notify. The first SSL_shutdown call
                // emits our close_notify through TLS_TX without entering the application path.
                (void)slot_tls->shutdown();
            }
            TlsConn* tls = tls_engine(c);
            if (tls && tls->memory_userspace() && !tls->shutdown_started()) {
                (void)tls->shutdown();
                (void)wb_.pump_tls_any(*c, *tls);
            }
            // Under epoll recv_armed_ records an owed EDGE, not a kernel-held buffer pointer, and
            // a torn-down socket owes nothing. Leaving it set would make safe_to_release() refuse
            // forever and leak the whole Client -- the same ~137KB-per-disconnect shape the retry
            // note below describes, reached by a different route.
            if (epoll_) { c->set_recv_armed(false); c->set_send_inflight(false); }
            (void)wb_.take_send_failure();
            // Break any in-flight recv/send NOW: safe_to_release refuses to free while the kernel
            // holds a buffer pointer (recv_armed / send_inflight), and those only clear when their
            // CQEs come back -- which a half-open peer might never trigger on its own.
            const bool pending_tls_output = tls && (tls->output_pending() || c->send_inflight());
            if (!(drain_tls_output && pending_tls_output)) ::shutdown(c->fd(), SHUT_RDWR);
        }
        multi_close_entry(*this, *c);
        if (TlsConn* tls = tls_engine(c)) {
            if (drain_tls_output && (tls->output_pending() || c->send_inflight())) {
                mark_active(c);
                return;
            }
        }
        // Release only at the quiescence fence: a worker may still hold a Task that resolves through
        // this ROB. Anything else is a use-after-free under pipelining. (The retryable wait paths,
        // with their mark_active leak guard, are below.)
        if (!pubsub_disconnect_ready(c)) { mark_active(c); return; }
        c->set_in_active(false);
        active_.erase(c);
        (void)self_->remove_client(c);
        // THE RETRY IS THE LOAD-BEARING PART of this wait: the client left the active set when it
        // went quiet, so nothing revisits it unless mark_active puts it back -- returning without
        // doing so leaked the entire client (~137KB) per disconnect once. Only a quiesced,
        // claim-free conn may release its slot and die.
        if (!c->safe_to_release() || !client_executor_quiesced(c) ||
            !c->safe_to_release()) { mark_active(c); return; }
        client_work_fences_.erase(c);
        climon_untrack_client(c);
        command_client_disconnected(c);
        self_->release_wb_slot(c->wb_slot());
        c->set_wb_slot(Client::kNoWbSlot);
        wb_.teardown(*c);
        ::close(c->fd());
        srv_->client_released();
        srv_->lb_forget_client(c->id());
        // NOT delete. An ex-side claimed post may still sit un-consumed in our inbound channels
        // naming this client. Every such entry was pushed BEFORE this point, and channels are FIFO
        // with their mask bits set -- so ONE full
        // collect_retire_work pass consumes all of them. Park the corpse for one loop iteration and
        // free it at the top of the one after; the drain lambda skips dead clients.
        c->mark_dead();
        dead_next_.push_back(c);
    }

    // Called only after the connection is ROB-quiescent and no longer dispatching. The
    // acquire on Done precedes this snapshot, so every executor that could still hold
    // this Client published its odd scope before we sample. Wait for EACH such scope
    // to end, without requiring all executors to be idle at the same instant.
    bool client_executor_quiesced(Client* client) const {
        try {
            auto [it, inserted] = client_work_fences_.try_emplace(client);
            ClientWorkFence& fence = it->second;
            const uint64_t dispatch = client->rob().dispatch_id();
            if (inserted || fence.dispatch != dispatch) {
                fence.dispatch = dispatch;
                for (uint32_t tid = 0; tid < srv_->nthreads(); tid++)
                    fence.epochs[tid] = srv_->client_work_epoch(tid);
            }
            bool ready = true;
            for (uint32_t tid = 0; tid < srv_->nthreads(); tid++) {
                // This IO's synchronous fused stack cannot run concurrently with teardown;
                // its existing pipeline/serve fences cover retained local handles.
                if (tid == self_->id() || !(fence.epochs[tid] & 1)) continue;
                if (srv_->client_work_epoch(tid) == fence.epochs[tid]) ready = false;
                else fence.epochs[tid] = 0;
            }
            return ready;
        } catch (const std::bad_alloc&) {
            return false; // Keep the Client and retry; allocation failure cannot waive lifetime.
        }
    }

    // Free everything that has been dead for a full iteration. Called once per loop pass, BEFORE
    // this pass's drains, so a corpse parked in pass N is freed in pass N+2's prologue -- after the
    // whole of pass N+1 (including its channel drains) ran with the corpse still readable.
    void reap_dead() {
        // Hold a corpse while ANY outside reference can still surface: a pending_serve_ FIFO
        // entry (the budget serves 16/pass, so an entry can outlive two prologues -- audit
        // finding), or a kernel CQE not yet reaped (a send pins msghdr/iovecs/BORROWs; a recv
        // pins the buffer and the Client* in its user_data). Two prologues are the minimum grace;
        // these fences extend it exactly as long as a reference exists. teardown() before delete
        // returns any BORROW the close path could not (idempotent: release_all empties the queue).
        size_t keep = 0;
        for (Client* c : dead_ready_) {
            if (c->serve_pending() || c->send_inflight() || c->recv_armed() ||
                client_pipeline_referenced(c)) {
                dead_ready_[keep++] = c;
                continue;
            }
            wb_.teardown(*c);
            release_tls(c);
            delete c;
        }
        dead_ready_.resize(keep);
        dead_ready_.insert(dead_ready_.end(), dead_next_.begin(), dead_next_.end());
        dead_next_.clear();
    }

    Server*    srv_  = nullptr;
    struct ClientWorkFence {
        uint64_t dispatch = 0;
        uint64_t epochs[kMaxThreads] = {};
    };
    ThreadCtx* self_ = nullptr;
    IfidBatch ifid_batch_{};
    WbBatch wb_batch_{};
    IoPipeDepthGate iopipe_depth_gate_{};
    size_t ifid_cursor_ = 0;
    static constexpr uint32_t kFlushBackstopEvery = 64;
    // Serves per pass. Sized so a pass's serve work stays comparable to its recv work: ~16 serves
    // x a ~32-op prefix each is one CQ batch worth of replies. The queue, not the pass, absorbs
    // overload.
    static constexpr uint32_t kServeBudget = 16;
    std::deque<Client*> pending_serve_;
    std::deque<Client*> pending_ifid_;
    std::deque<BorrowRelease> pending_releases_;
    std::deque<Client*> pending_handoffs_;
    std::vector<ClientMigration> client_migrations_; // source-owned until old-ring fence completes
    uint64_t client_transfer_failures_ = 0;
    Client*   flip_client_ = nullptr;       // coordinator's sole unfinished FLIP ROB slot
    uint64_t  flip_op_id_ = 0;
    uint64_t  flip_epoch_local_ = 0;
    uint64_t  flip_prepare_epoch_ = 0;
    uint64_t  flip_pubsub_rehome_epoch_ = 0;
    // Allocates only for an unsatisfied WAIT or positive DEBUG SLEEP; never on ordinary commands.
    std::vector<DeferredTimer> deferred_timers_;
    ScatterArenaPool scatter_pool_;          // touched only by this connection-owning IO thread
    uint32_t flush_tick_ = 0;
    bool     backstop_pass_ = false;
    static constexpr uint32_t kClientCronBeatsPerSecond = 10;
    static constexpr uint32_t kClientCronMinVisits = 5;
    uint64_t client_cron_beat_ms_ = 0;
    uint64_t lb_client_signal_beat_ms_ = 0;
    uint64_t lb_controller_beat_ms_ = 0;
    uint64_t save_cron_beat_ms_ = 0;
    size_t   client_cron_cursor_ = 0;
    uint64_t cached_now_ms_ = 0;
    uint32_t cached_now_s_ = 0;
    bool     client_cron_was_armed_ = false;
    bool     client_lb_signal_armed_ = false;
    bool     lb_controller_armed_ = false;
    bool     lb_client_wake_pending_ = false;
    bool     age_signals_armed_ = false;
    uint32_t age_sample_rate_cached_ = 0;
    uint32_t lb_wake_cursor_ = UINT32_MAX;
    std::vector<LbClientObservation> lb_client_observations_;
    struct RobHeadAge {
        uint64_t head_id = 0;
        uint64_t head_since_us = 0;
        uint64_t seen_generation = 0;
    };
    // Empty and allocation-free while controller age sampling is disarmed.
    std::unordered_map<Client*, RobHeadAge> rob_head_ages_;
    uint64_t rob_age_generation_ = 0;
    bool touched_[kMaxThreads] = {};      // dedupe flags for the current parse pass
    uint32_t touched_list_[kMaxThreads] = {}; // the workers actually fed, dense
    uint32_t ntouched_ = 0;
    // Per-owner task demand for the plain scatter dispatch. INVARIANT: every entry is zero on
    // entry to and on exit from the dispatch arm, so the arm never zeroes the whole array and
    // never walks it. Cost becomes proportional to the shards this op actually touches instead
    // of to the configured thread count.
    uint32_t dispatch_needed_[kMaxThreads] = {};
    std::vector<Client*> dead_next_;   // corpses parked this iteration
    std::vector<Client*> dead_ready_;  // corpses freed at the next prologue
    int        listen_fd_ = -1;
    int        tls_listen_fd_ = -1;
    int        unix_listen_fd_ = -1;
    uint16_t   configured_port_ = 0;
    uint64_t   accept_generation_ = 0;
    uint64_t   accept_cancel_generation_ = 0;
    bool       initialized_ = false;
    bool       active_role_ = false;
    bool       prepared_role_ = false;
    bool       aof_bound_ = false;
    Ring       ring_;
    // The second engine's state. `epoll_` is boot-latched and read only by run() (to pick the
    // instantiation) and by the two cold non-templated members that cannot carry it in their type
    // (close_client, and adopt_client's failure path). The set itself is never created in uring
    // mode -- its fd stays -1 and nothing else in it is touched.
    bool       epoll_ = false;
    EpollSet   ep_;
    std::vector<Client*> epoll_closes_;   // teardowns deferred out of the active-set walk
    WbEngine   wb_;
    bool       accept_pending_ = false;
    bool       tls_accept_pending_ = false;
    bool       unix_accept_pending_ = false;
    bool       accept_armed_ = false;
    bool       tls_accept_armed_ = false;
    bool       unix_accept_armed_ = false;
    bool       accept_quiescing_ = false;
    bool       accept_cancel_submitted_ = false;
    bool       tls_accept_cancel_submitted_ = false;
    uint64_t   unix_rr_ = 0;
    uint64_t   random_state_ = 0x9e3779b97f4a7c15ULL;
    uint64_t   aof_gate_target_ = 0;

    // Clients with work outstanding. Populated by dispatch and by the retire channel, never by
    // scanning every client: at 10k+ connections that scan dominates the loop.
    // INDEXED, NOT ITERATED, and that is a correctness property rather than a style choice. The
    // phase-1 walk below can reach close_client (a TLS handshake failure, a peer that hung up, a
    // synchronous send that failed), and close_client's retry paths call mark_active, which
    // push_back()s -- reallocating the vector and invalidating any iterator the walk was holding.
    // ASAN caught exactly that as a heap-buffer-overflow in erase() the first time a connection
    // closed under the epoll engine. An index survives reallocation; an iterator does not.
    struct PtrSet {
        std::vector<Client*> v;
        void insert(Client* c) { v.push_back(c); }
        size_t size() const { return v.size(); }
        Client* at(size_t i) { return v[i]; }
        // Swap-with-back rather than vector::erase: order in the active set carries no meaning, and
        // erase() shifts every element after the removed one.
        void erase_at(size_t i) { v[i] = v.back(); v.pop_back(); }
        void erase(Client* c) {
            for (size_t i = 0; i < v.size(); i++)
                if (v[i] == c) { v[i] = v.back(); v.pop_back(); return; }
        }
    } active_;
    std::vector<MultiExecState*> multi_deferred_;
    std::deque<MultiExecState*> pending_multi_cleanups_;
    // Cold live-config cache. Appended so every pre-existing IoLoop field retains its offset.
    uint64_t notify_config_version_ = UINT64_MAX;
    bool notify_armed_ = false;
    bool notify_config_armed_ = false;   // keyspace-notification half of notify_armed_
    bool save_config_armed_ = false;
    uint64_t proto_max_bulk_len_ = 512ull * 1024 * 1024;
    // Slow-log arm for connection-local commands. Appended here rather than earlier so no
    // pre-existing IoLoop member offset moves.
    bool slowlog_armed_ = false;
    SlowlogArm slowlog_arm_{};
    // Notification publications are sequenced through one coordinator IO. Keep this v2-only
    // carriage at the true cold tail so the entire pre-notification IoLoop layout remains fixed.
    std::deque<std::shared_ptr<PubSubNotificationChain>> pubsub_notification_chains_;
    // Allocated only when tls-port is non-zero. Slots are released at the same deferred-free fence
    // as Client because in-flight recv/send CQEs point into the BIO pair owned by TlsConn.
    const TlsContext* tls_context_ = nullptr;
    std::vector<std::unique_ptr<TlsConn>> tls_slots_;
    std::vector<uint32_t> tls_free_slots_;
#include "../cmd/climon.inc"
    struct ClientForwardRoute {
        bool installed = false;
        bool monitor = false;
        bool tracking = false;
    };

    struct ClientRoutingMigration {
        using PubSubNode = std::unordered_map<uint64_t, PubSubLocalConn>::node_type;
        using ClimonNode = std::unordered_map<uint64_t, ClimonConn>::node_type;
        using ForwardNode = std::unordered_map<uint64_t, ClientForwardRoute>::node_type;

        PubSubNode pubsub;
        ClimonNode climon;
        ForwardNode forward;
        std::vector<std::string> tracking_keys;
        // Allocated during reversible preparation, but counted and posted only after commit.
        std::vector<PubSubEvent*> rebind_events;
        PubSubEvent* installed_event = nullptr;
        uint64_t client_id = 0;
        uint32_t source = 0;
        uint32_t destination = 0;
        bool monitor = false;
        bool tracking = false;

        ~ClientRoutingMigration() {
            for (PubSubEvent* event : rebind_events) delete event;
            delete installed_event;
        }
    };
    std::unordered_map<uint64_t, ClientForwardRoute> routing_forward_;
    // Boot-selected fused loop only; appended so split-mode IoLoop offsets stay unchanged.
    FusedExLoop* fused_executor_ = nullptr;
    // Cold safety views into run-loop locals. Streams may retain the IFID view across its one
    // bounded residual rotation; teardown and migration defer while either context owns a Client.
    WbPipelineBatch* active_wb_context_ = nullptr;
    bool targeted_ifid_ = false;
    // Cold teardown/migration state; leave all established hot member offsets intact.
    mutable std::unordered_map<Client*, ClientWorkFence> client_work_fences_;
};

}  // namespace tomo
