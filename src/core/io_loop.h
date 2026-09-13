// io_loop.h — the IO stage. Accepts, receives, parses, routes, publishes, retires, and (in Io mode)
// sends.
//
// EVERY CROSS-THREAD SIGNAL HERE IS A Channel, and every measurement is a LoopSignals field, so this
// loop is comparable with the EX and WB loops through one interface. See signal.h.
//
//   out  task_in of the shard's owner        a parsed op to execute
//   in   client_in from workers              "you have completed ops to retire"
//
// WHAT MOVES BETWEEN MODES, AND WHAT DOES NOT. The ROB is ALWAYS drained by the IO thread that owns
// the connection, in every mode. Only the send syscall moves. Letting a second thread retire from
// the ROB would make dispatch_id/flush_id a cross-thread pair for no measured benefit. So this is
// NOT a byte-for-byte reproduction of the fork's ex-wb, which had the executor build and send its
// own contiguous ready prefix without returning to IO — said here so no result from that mode is
// misread as a verdict on that design.
#pragma once
#include <deque>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "server.h"
#include "signal.h"
#include "../net/conn.h"
#include "../net/resp.h"
#include "../net/uring.h"
#include "../net/epoll.h"
#include "../net/wb.h"
#include "../net/tls.h"
#include "../cmd/command.h"
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

// THE FIVE LOOPS. Each mode composes threads from five specialised loop shapes, distinguished by
// what the thread OWNS while serving (pure 2s, owner ruling 2026-08-24):
//
//   io    IoLoop    recv+parse+retire+send; owns the whole client
//   ex    ExLoop    execute+notify; never sends

class IoLoop {
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
              int unix_listen_fd = -1, const TlsContext* tls_context = nullptr) {
        srv_ = srv; self_ = self;
        self_->set_wb_engine(&wb_);
        if (port) {
            listen_fd_ = make_reuseport_listener(addr, port, srv_->cfg().tcp_backlog);
            if (listen_fd_ < 0) return false;
        }
        tls_context_ = tls_context;
        if (tls_context_) {
            tls_listen_fd_ = make_reuseport_listener(addr, srv_->cfg().tls_port,
                                                      srv_->cfg().tcp_backlog, true);
            if (tls_listen_fd_ < 0) return false;
        }
        unix_listen_fd_ = unix_listen_fd;
        epoll_ = srv_->cfg().net_io == NetIoEngine::Epoll;
        if (!ring_.init(4096)) return false;
        self_->set_ring(&ring_);
        if (epoll_ && !init_epoll()) return false;
        if (srv_->aof().configured()) {
            std::string error;
            if (!srv_->aof().bind_writer(*self_, ring_, error)) {
                std::fprintf(stderr, "AOF writer init failed: %s\n", error.c_str());
                return false;
            }
        }
        wb_.bind(&ring_, this, [](void* ctx, int32_t shard, const char* ptr) {
            static_cast<IoLoop*>(ctx)->queue_borrow_release(shard, ptr);
        }, this, [](void* ctx, Client& client, Op& op) {
            auto* loop = static_cast<IoLoop*>(ctx);
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
        }, srv_->client_obuf_armed_ptr(), this, [](void* ctx, Client& client) {
            return static_cast<IoLoop*>(ctx)->client_obuf_check(&client, true);
        }, &cached_now_s_, &self_->sig());
        return true;
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

    // Linux does not provide TCP-style SO_REUSEPORT distribution for filesystem AF_UNIX paths:
    // the pathname is a unique bind key. One listener is therefore armed by one IO thread, which
    // round-robins accepted Client handles through the existing per-producer client channels.
    static int make_unix_listener(const char* path, uint32_t backlog = 511) {
        sockaddr_un sa{};
        if (!path || !*path || std::strlen(path) >= sizeof(sa.sun_path)) return -1;
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sa.sun_family = AF_UNIX;
        std::memcpy(sa.sun_path, path, std::strlen(path) + 1);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 ||
            ::listen(fd, static_cast<int>(backlog)) != 0) {
            ::close(fd); return -1;
        }
        return fd;
    }

    Ring& ring() { return ring_; }

    // ---- epoll engine: registration -----------------------------------------------------------
    // Everything that will ever be waited on is registered ONCE here. Listeners are
    // level-triggered on purpose: a listener that we stop accepting from (maxclients reached, a
    // failed Client allocation) must keep telling us there is a backlog, and an edge we consumed
    // and could not act on would be gone. Connections are edge-triggered for the opposite reason --
    // see net/epoll.h. The doorbell eventfd is level-triggered and drained explicitly.
    bool init_epoll() {
        if (!ep_.init()) return false;
        auto add_listener = [&](int fd, UrKind kind) {
            if (fd < 0) return true;
            if (!set_nonblocking(fd)) return false;
            return ep_.add(fd, EPOLLIN, ur_tag(kind, nullptr));
        };
        if (!add_listener(listen_fd_, UrKind::Accept)) return false;
        if (!add_listener(tls_listen_fd_, UrKind::TlsAccept)) return false;
        if (!add_listener(unix_listen_fd_, UrKind::UnixAccept)) return false;
        // The cross-thread doorbell. Without this in the set, an executor that completes work while
        // this thread is parked in epoll_wait cannot reach it, and the reply waits out the timeout.
        if (ring_.wake_fd() < 0) return false;
        return ep_.add(ring_.wake_fd(), EPOLLIN, ur_tag(UrKind::Wake, nullptr));
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
    void run() {
        const bool has_unix = unix_listen_fd_ >= 0 ||
                              (srv_->cfg().unixsocket && *srv_->cfg().unixsocket);
        if (epoll_) {
            if (tls_context_) {
                if (has_unix) run_loop<true, true, true>();
                else run_loop<false, true, true>();
            } else {
                if (has_unix) run_loop<true, false, true>();
                else run_loop<false, false, true>();
            }
            return;
        }
        if (tls_context_) {
            if (has_unix) run_loop<true, true, false>();
            else run_loop<false, true, false>();
        } else {
            if (has_unix) run_loop<true, false, false>();
            else run_loop<false, false, false>();
        }
    }

private:
    friend bool multi_dispatch_entry(IoLoop&, Client&, Op&, uint32_t);
    friend bool auth_dispatch_entry(IoLoop&, Client&, Op&, uint32_t);
    friend bool acl_dispatch_entry(IoLoop&, Client&, Op&, uint32_t, uint8_t);
    friend bool acl_finish_dispatch_denial(IoLoop&, Client&, Op&, uint32_t,
                                           AclDeniedReason, uint32_t);
    friend void acl_command_entry(IoLoop&, Client&, Op&);
    friend void acl_broadcast_user_change(IoLoop&, uint32_t, const AclPerm*, bool);
    friend void multi_retire_entry(IoLoop&, Client&, Op&);
    friend uint32_t multi_owner_pass_entry(IoLoop&);
    friend uint32_t multi_owner_reap_entry(IoLoop&);
    friend void multi_close_entry(IoLoop&, Client&);
    friend void multi_shutdown_entry(IoLoop&);
    friend void notify_retire_batch_entry(IoLoop&, NotifyBatch*, uint64_t);
#include "pubsub.inc"

    template <bool HasUnix, bool HasTls, bool kEp>
    void run_loop() {
        if constexpr (!kEp) {
            if (listen_fd_ >= 0) arm_accept(UrKind::Accept);
            if constexpr (HasTls) arm_accept(UrKind::TlsAccept);
            if constexpr (HasUnix) if (unix_listen_fd_ >= 0) arm_accept(UrKind::UnixAccept);
        }
        LoopSignals& sig = self_->sig();
        while (!self_->stop_flag().load(std::memory_order_relaxed)) {
            refresh_notify_config();
            // ONE relaxed load per io batch. Per-batch checks are free; this is what buys the
            // per-operation hooks their zero-cost-when-off property.
            if (__builtin_expect(srv_->climon_armed() != climon_armed_cached_, false))
                climon_refresh_armed();
            // A live CLIENT PAUSE is the only lane feature that needs a clock of its own; the
            // deadline is checked once per batch, never per operation.
            if (__builtin_expect(climon_pause_armed(), false)) {
                cached_now_ms_ = now_ns() / 1000000ull;
                cached_now_s_ = static_cast<uint32_t>(cached_now_ms_ / 1000);
                if (cached_now_ms_ >= climon_pause_deadline_ms_) climon_release_pause();
            }
            const bool client_cron_armed = srv_->client_cron_armed();
            const bool save_cron_armed = srv_->save_cron_writer(self_->id());
            if (__builtin_expect(client_cron_armed || save_cron_armed, false)) {
                cached_now_ms_ = now_ns() / 1000000ull;
                cached_now_s_ = static_cast<uint32_t>(cached_now_ms_ / 1000);
                if (client_cron_armed && !client_cron_was_armed_) {
                    for (Client* c : self_->clients()) c->set_last_interaction_s(cached_now_s_);
                    client_cron_beat_ms_ = cached_now_ms_;
                }
            }
            if (!client_cron_armed && __builtin_expect(client_cron_was_armed_, false)) {
                // Turning the last client cron consumer off also retires output accounting once.
                // The disabled write-back specialization then has no per-serve cleanup branch.
                for (Client* c : self_->clients()) c->stop_obuf_tracking();
            }
            client_cron_was_armed_ = client_cron_armed;
            sig.iterations++;
            self_->sample_depth();
            reap_dead();               // free clients dead for a full iteration -- see close_client
            scatter_pool_.reap_deferred();

            uint32_t did = 0;
            {
                Span busy(sig.busy_ns);
                // A dropped accept re-arm means the server stops taking connections entirely, so it
                // is retried every pass until it lands.
                if constexpr (!kEp) {
                    if (accept_pending_) arm_accept(UrKind::Accept);
                    if constexpr (HasTls) if (tls_accept_pending_) arm_accept(UrKind::TlsAccept);
                    if constexpr (HasUnix)
                        if (unix_accept_pending_) arm_accept(UrKind::UnixAccept);
                }
                // In epoll mode this drains the doorbell mailbox instead of a CQ ring; the tag
                // stream, and therefore this switch, is identical. See uring.h.
                did += ring_.for_each_cqe([&](io_uring_cqe* cqe) { on_cqe<HasTls, kEp>(cqe); });
                if constexpr (kEp) did += epoll_pass<HasUnix, HasTls>(0);
                did += scatter_pool_.refresh_snapshot_floor(*srv_, self_->id());
                if constexpr (HasUnix) did += flush_handoffs();
                did += multi_owner_pass_entry(*this);
                if (srv_->aof().writer_is(self_->id()))
                    did += srv_->aof().writer_pass(*self_, ring_);
                if (srv_->snapshot().writer_is(self_->id()))
                    did += srv_->snapshot().writer_pass(*self_, ring_);
                if (__builtin_expect(!deferred_waits_.empty(), false)) {
                    cached_now_ms_ = now_ns() / 1000000ull;
                    cached_now_s_ = static_cast<uint32_t>(cached_now_ms_ / 1000);
                    did += deferred_wait_pass(cached_now_ms_);
                }
                did += flush_borrow_releases();
                did += collect_retire_work<HasUnix, kEp>();
                did += flush_ready<HasTls, kEp>();
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
            sig.cpu_ns = thread_cpu_ns();

            // Flush prepared SQEs before looping. Recv re-arms and cross-ring wakes are
            // PREPARED during the work section but only reach the kernel on submit; taking
            // the busy path without submitting strands them in the SQ forever, and the peer
            // that is waiting on that wake never runs.
            if (did) { ring_.submit_and_reap(); continue; }

            // Nothing to do: declare intent to block, re-check (a producer may have pushed between
            // the last drain and the flag being set), then wait.
            // Mask-independent sweep before parking. The mask is a hint for the hot path; it must
            // not be the only thing that can find queued work, or one lost bit wedges a connection
            // forever. Runs only when this thread has already concluded it has nothing to do.
            if (sweep<HasUnix, HasTls, kEp>()) { ring_.submit_and_reap(); continue; }

            Span idle(sig.idle_ns);
            self_->arm_blocked();
            if constexpr (kEp) {
                // The park. Same 50ms ceiling as the ring wait, and for the same reason: the stop
                // flag is only re-read at the top of the loop, so an unbounded block would make
                // shutdown depend on a connection arriving.
                if (!self_->any_io_inbound()) epoll_pass<HasUnix, HasTls>(50);
            } else {
                if (!self_->any_io_inbound()) ring_.submit_and_wait(1);
                else                       ring_.submit_and_reap();
            }
            self_->clear_blocked();
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

    // ---- submission -----------------------------------------------------------------------------
    void arm_accept(UrKind kind) {
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
        s->user_data = ur_tag(kind, nullptr);
        ring_.note_pending();
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
        if (c->recv_armed() || c->closing()) return;
        if constexpr (kEp) { epoll_recv(c); return; }
        size_t avail = 0;
        // may_grow ONLY at quiescence: realloc moves the buffer that every in-flight argv Slice
        // points into. See Conn::read_space.
        char* dst = c->read_space(
            kRecvChunk, avail, c->rob().quiesced(), proto_max_bulk_len_);
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

    template <bool kEp>
    void arm_tls_recv(Client* c) {
        if (c->recv_armed() || c->closing()) return;
        TlsConn* tls = tls_engine(c);
        if (!tls || !tls->memory_bio()) return;
        // Any engine output is submitted before another socket read. This is the memory-BIO
        // flush-before-read rule that prevents WANT_READ from hiding a required write.
        if (tls->output_pending()) {
            (void)wb_.pump_tls<kEp>(*c, *tls);
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
                on_tls_recv<kEp>(c, static_cast<int>(n));
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
    template <bool HasUnix, bool HasTls>
    uint32_t epoll_pass(int timeout_ms) {
        const int n = ep_.wait(timeout_ms);
        if (n <= 0) return 0;
        self_->sig().epoll_events += static_cast<uint64_t>(n);
        uint32_t work = 0;
        for (int i = 0; i < n; i++) {
            const epoll_event& ev = ep_.event(i);
            switch (ur_kind(ev.data.u64)) {
                case UrKind::Accept: work += epoll_accept<true>(UrKind::Accept); break;
                case UrKind::TlsAccept:
                    if constexpr (HasTls) work += epoll_accept<true>(UrKind::TlsAccept);
                    break;
                case UrKind::UnixAccept:
                    if constexpr (HasUnix) work += epoll_accept<true>(UrKind::UnixAccept);
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
                case UrKind::Recv: {
                    Client* c = ur_ptr<Client>(ev.data.u64);
                    if (!c || c->dead()) break;
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
                    mark_active(c);
                    work++;
                    break;
                }
                default: break;
            }
        }
        return work;
    }

    // ---- completions ----------------------------------------------------------------------------
    template <bool kEp>
    void on_plain_send_cqe(io_uring_cqe* cqe) {
        Client* c = ur_ptr<Client>(cqe->user_data);
        if (c->dead()) {
            // sendmsg's msghdr/iovecs and borrowed payload remain live through this CQE.
            wb_.on_dead_send_complete(*c, cqe->res);
            return;
        }
        if (!wb_.on_send_complete(*c, cqe->res)) close_client(c);
    }

    template <bool kEp>
    void on_tls_send_cqe(io_uring_cqe* cqe) {
        Client* c = ur_ptr<Client>(cqe->user_data);
        TlsConn* tls = tls_engine(c);
        if (!tls) { close_client(c); return; }
        if (c->dead()) {
            wb_.on_dead_tls_send_complete(*c, *tls, cqe->res);
            return;
        }
        if (!wb_.on_tls_send_complete(*c, *tls, cqe->res)) close_client(c);
        else mark_active(c);
    }

    template <bool HasTls, bool kEp>
    void on_cqe(io_uring_cqe* cqe) {
        if constexpr (!HasTls) {
            // Keep the tls-port=0 completion dispatch byte-for-byte shaped like the base switch.
            switch (ur_kind(cqe->user_data)) {
                case UrKind::Accept: on_accept<kEp>(cqe, UrKind::Accept); break;
                case UrKind::UnixAccept: on_accept<kEp>(cqe, UrKind::UnixAccept); break;
                case UrKind::Recv: on_recv<false, kEp>(ur_ptr<Client>(cqe->user_data), cqe->res); break;
                case UrKind::Send: on_plain_send_cqe<kEp>(cqe); break;
                case UrKind::Wake: self_->sig().wakes_recv++; break;
                case UrKind::SnapshotStart: break;
                case UrKind::AofIo:
                    srv_->aof().on_io_complete(*self_, ring_, ur_ptr<void>(cqe->user_data),
                                               cqe->res); break;
                case UrKind::SnapshotIo:
                    srv_->snapshot().on_io_complete(*self_, ring_, ur_ptr<void>(cqe->user_data),
                                                    cqe->res); break;
                case UrKind::Close: break;
                case UrKind::TlsReadPoll: break;
                case UrKind::TlsWritePoll: break;
                default: break;
            }
        } else {
            switch (ur_kind(cqe->user_data)) {
                case UrKind::Accept: on_accept<kEp>(cqe, UrKind::Accept); break;
                case UrKind::TlsAccept: on_accept<kEp>(cqe, UrKind::TlsAccept); break;
                case UrKind::UnixAccept: on_accept<kEp>(cqe, UrKind::UnixAccept); break;
                case UrKind::Recv: on_recv<true, kEp>(ur_ptr<Client>(cqe->user_data), cqe->res); break;
                case UrKind::TlsRecv: on_tls_recv<kEp>(ur_ptr<Client>(cqe->user_data), cqe->res); break;
                case UrKind::Send: on_plain_send_cqe<kEp>(cqe); break;
                case UrKind::TlsSend: on_tls_send_cqe<kEp>(cqe); break;
                case UrKind::TlsReadPoll:
                    on_tls_socket_poll<kEp>(ur_ptr<Client>(cqe->user_data), cqe->res,
                                       TlsOp::WantRead); break;
                case UrKind::TlsWritePoll:
                    on_tls_socket_poll<kEp>(ur_ptr<Client>(cqe->user_data), cqe->res,
                                       TlsOp::WantWrite); break;
                case UrKind::Wake: self_->sig().wakes_recv++; break;
                case UrKind::SnapshotStart: break;
                case UrKind::AofIo:
                    srv_->aof().on_io_complete(*self_, ring_, ur_ptr<void>(cqe->user_data),
                                               cqe->res); break;
                case UrKind::SnapshotIo:
                    srv_->snapshot().on_io_complete(*self_, ring_, ur_ptr<void>(cqe->user_data),
                                                    cqe->res); break;
                case UrKind::Close: break;
            }
        }
    }

    void rearm_accept(io_uring_cqe* cqe, UrKind kind) {
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            self_->sig().accept_rearm++;
            arm_accept(kind);
        }
    }

    template <bool kEp>
    void on_accept(io_uring_cqe* cqe, UrKind kind) {
        if (cqe->res < 0) {
            // Do not swallow this silently: a failing accept with no trace is indistinguishable from
            // a hung server, which is exactly how the 1024-connection failure presented.
            self_->sig().accept_err++;
            arm_accept(kind);
            return;
        }
        admit_fd<kEp>(cqe->res, kind);
        rearm_accept(cqe, kind);
    }

    // Epoll's accept: the listener only told us there is a backlog, so drain it. Level-triggered
    // registration means an unfinished drain is re-reported rather than lost, but draining to
    // EAGAIN here keeps one epoll_wait per burst instead of one per connection.
    template <bool kEp>
    uint32_t epoll_accept(UrKind kind) {
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
            admit_fd<kEp>(fd, kind);
        }
    }

    // Everything an accepted fd goes through before it becomes a served connection. Shared by both
    // engines verbatim: maxclients, protected mode, Client allocation, TLS attachment, unix
    // round-robin handoff. Only the way the fd ARRIVED differs, which is the whole engine boundary.
    template <bool kEp>
    void admit_fd(int fd, UrKind kind) {
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
            if (target == self_->id()) adopt_client<kEp>(c, true);
            else if (!srv_->thread(target).post_client(self_->id(), c, ring_, self_->sig()))
                pending_handoffs_.push_back(c);
        } else {
            c->set_ifid_thread(self_->id());
            adopt_client<kEp>(c, false, tls_socket);
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
                            srv_->cfg().tls_ktls, error)) {
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

    template <bool kEp>
    void adopt_client(Client* c, bool unix_socket, bool tls_socket = false) {
        // ARMED ONCE, HERE, FOR LIFE, IN THE OWNING THREAD'S SET. Both directions, edge triggered;
        // ::close() is the only deregistration. Registration belongs here rather than at accept
        // because an AF_UNIX connection is accepted by one io thread and OWNED by another -- the
        // round-robin handoff below -- and an fd sitting in the accepting thread's epoll set would
        // deliver every one of its events to a thread that must not touch it.
        if constexpr (kEp) {
            if (!set_nonblocking(c->fd()) ||
                !ep_.add(c->fd(), EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET,
                         ur_tag(UrKind::Recv, c))) {
                std::fprintf(stderr, "epoll registration failed for client fd %d\n", c->fd());
                self_->sig().accept_err++;
                self_->clients().push_back(c);
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
        // The ready-mask slot is assigned immediately: WE are the sender, for life.
        c->set_wb_slot(self_->assign_wb_slot(c));
        self_->clients().push_back(c);
        const std::string addr = socket_address(c->fd(), unix_socket, true);
        const std::string laddr = socket_address(c->fd(), unix_socket, false);
        const uint64_t accepted_ms = cached_now_ms_ ? cached_now_ms_ : now_ns() / 1000000ull;
        command_client_connected(c, addr.c_str(), laddr.c_str(), unix_socket, accepted_ms);
        climon_track_client(c);
        if (tls_socket) {
            TlsConn* tls = tls_slot_conn(c);
            if (tls && tls->fd_handshake()) {
                (void)drive_tls<kEp>(c);
                if (!c->closing() && tls->ktls()) arm_recv<kEp>(c);
                else if (!c->closing() && tls->memory_userspace()) arm_tls_recv<kEp>(c);
            } else {
                arm_tls_recv<kEp>(c);
            }
        } else arm_recv<kEp>(c);
        // Reachability, not optimism: if that arm starved for an SQE, nothing else names this
        // conn -- it would sit accepted and silent forever (audit finding). The active set's
        // phase-1 re-arms it until the recv lands; one wasted visit if the arm succeeded.
        mark_active(c);
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

    template <bool HasTls, bool kEp>
    void on_recv(Client* c, int res) {
        c->set_recv_armed(false);       // the kernel has released its pointer
        if (res > 0) self_->sig().net_input_bytes += static_cast<uint64_t>(res);
        // A send error can close the fd while this recv is still owned by io_uring. The Client stays
        // alive until this CQE arrives, but it is a corpse: positive bytes must not resurrect it by
        // parsing and dispatching new Tasks after the teardown quiescence fence.
        if (c->dead()) return;
        if (res <= 0) { close_client(c); return; }
        c->commit_read(static_cast<size_t>(res));
        c->set_last_interaction_s(cached_now_s_);
        if constexpr (HasTls) {
            if (c->is_tls()) parse_and_dispatch<true>(c);
            else parse_and_dispatch<false>(c);
        } else {
            parse_and_dispatch<false>(c);
        }
        // Deliberately NOT re-armed here. flush_ready() re-arms AFTER it may have reset the read
        // buffer; arming first would leave the kernel holding a pointer that the reset then moves.
        mark_active(c);
    }

    template <bool kEp>
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
            (void)wb_.pump_tls<kEp>(*c, *tls);
            if (tls->has_pinned_plain()) {
                arm_tls_socket_poll<kEp>(c, tls->wanted());
                return true;
            }
        }

        if (tls->output_pending() || c->send_inflight()) {
            (void)wb_.pump_tls<kEp>(*c, *tls);
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
            (void)wb_.pump_tls<kEp>(*c, *tls);  // alerts and handshake flights are flushed first
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

        bool decrypted = false;
        while (tls->connected()) {
            size_t avail = 0;
            char* dst = c->read_space(
                kRecvChunk, avail, c->rob().quiesced(), proto_max_bulk_len_);
            if (!dst) break;
            const TlsIoResult result = tls->read_plain(dst, avail);
            if (result.op == TlsOp::Progress) {
                // Only decrypted bytes enter the RESP buffer. Ciphertext counts are committed to
                // the BIO in on_tls_recv and can never reach this cursor.
                c->commit_read(result.bytes);
                self_->sig().tls_plaintext_input_bytes += result.bytes;
                decrypted = true;
                if (tls->output_pending()) { (void)wb_.pump_tls<kEp>(*c, *tls); break; }
                continue;
            }
            if (result.op == TlsOp::WantRead) {
                self_->sig().tls_want_read++;
                if (tls->socket_userspace()) arm_tls_socket_poll<kEp>(c, result.op);
            }
            else if (result.op == TlsOp::WantWrite) {
                self_->sig().tls_want_write++;
                if (tls->socket_userspace()) arm_tls_socket_poll<kEp>(c, result.op);
                else (void)wb_.pump_tls<kEp>(*c, *tls);
            } else if (result.op == TlsOp::GracefulEof) {
                (void)tls->shutdown();
                (void)wb_.pump_tls<kEp>(*c, *tls);
                close_client(c, tls->output_pending() || c->send_inflight());
                return false;
            } else {
                if (!tls->last_error().empty())
                    std::fprintf(stderr, "TLS client %llu: %s\n",
                                 static_cast<unsigned long long>(c->id()),
                                 tls->last_error().c_str());
                (void)wb_.pump_tls<kEp>(*c, *tls);
                close_client(c, tls->output_pending() || c->send_inflight());
                return false;
            }
            break;
        }
        if (decrypted || c->rpos() < c->rlen()) parse_and_dispatch<true>(c);
        return !tls->failed();
    }

    template <bool kEp>
    void on_tls_socket_poll(Client* c, int res, TlsOp wanted) {
        TlsConn* tls = tls_slot_conn(c);
        if (!tls) { close_client(c); return; }
        tls->set_poll_armed(wanted, false);
        c->set_recv_armed(tls->any_poll_armed());
        if (c->dead()) return;
        if (c->closing() || res < 0) { close_client(c); return; }
        (void)drive_tls<kEp>(c);
        mark_active(c);
    }

    template <bool kEp>
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
        (void)drive_tls<kEp>(c);
        mark_active(c);
    }

    // ---- parse -> route -> publish -----------------------------------------------------------------
    template <bool NoBorrow>
    void parse_and_dispatch(Client* c) {
        Client& conn = *c;
        Rob<kRobWindow>& rob = c->rob();
        LoopSignals& sig = self_->sig();
        bool head_candidate = true;   // only the pass's FIRST dispatch can be the direct head
        const uint8_t security_flags = srv_->security_flags();
        const bool auth_required = (security_flags & Server::kSecurityAuth) != 0;
        const bool acl_active = (security_flags & Server::kSecurityAcl) != 0;
        const bool notify_armed = notify_armed_;
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

        for (;;) {
            if (c->scatter_barrier() || c->atomic_backpressure()) break;
            Op* op = rob.acquire(conn.op_route_flags());
            if (!op) break;                    // window full: backpressure; let replies drain first
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
                    conn.rbuf(), conn.rlen(), pos, *op, &err, 10, 16384);
            } else if (__builtin_expect(
                           proto_max_bulk_len_ == 512ull * 1024 * 1024, true)) {
                pr = resp_parse(conn.rbuf(), conn.rlen(), pos, *op, &err);
            } else {
                pr = resp_parse_limited(conn.rbuf(), conn.rlen(), pos, *op, &err,
                                        1024 * 1024, proto_max_bulk_len_);
            }
            security_check |= acl_active;

            if (pr == ParseResult::Incomplete) break;
            if (pr == ParseResult::Error) {
                finish_locally(c, *op, err ? err : "ERR protocol error");
                conn.advance_parse(conn.rlen() - conn.rpos());
                c->mark_closing();
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
                if (__builtin_expect(climon_armed_gate(c, *op), false)) break;
            } else {
                if constexpr (NoBorrow) spec = command_tls_variant(spec);
                op->spec = spec;
            }
            if (__builtin_expect(security_check, false) &&
                acl_dispatch_entry(*this, conn, *op, consumed, security_flags)) continue;
            if (__builtin_expect((spec->flags & CmdFlags::Transaction) != 0, false) ||
                __builtin_expect(conn.multi_session() != nullptr, false)) {
                if (multi_dispatch_entry(*this, conn, *op, consumed)) continue;
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
                    climon_reset_client(c);
                    pubsub_start_reset(c, *op);
                    sig.ops++;
                    mark_active(c);
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
                    pubsub_reply_ping(*op);
                    finish_prebuilt(c, *op);
                    continue;
                }
                if (!subscription_control && !op->cmd_name().eq_icase("quit")) {
                    conn.advance_parse(consumed);
                    self_->note_command(spec->id);
                    pubsub_reply_restricted(*op);
                    finish_prebuilt(c, *op);
                    continue;
                }
            }
subscriber_checks_done:
            if (spec->flags & CmdFlags::PubSub) {
                conn.advance_parse(consumed);
                self_->note_command(spec->id);
                const PubSubStartResult result = pubsub_start_command(c, *op);
                if (result == PubSubStartResult::Async) {
                    sig.ops++;
                    mark_active(c);
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
                // An unsatisfied WAIT has no shard work, but Redis keeps the connection parked
                // until its deadline (zero means forever). Publish an unfinished ROB slot and let
                // this connection's IO owner complete it. MULTI does not enter this branch: its
                // IoLocal child calls cmd_wait at retirement and receives :0 immediately.
                if (__builtin_expect((spec->flags & CmdFlags::DeferredLocal) != 0, false)) {
                    uint64_t timeout_ms = 0;
                    const WaitCommandResult wait = server_tail_prepare_wait(*op, timeout_ms);
                    if (wait == WaitCommandResult::Unsatisfied) {
                        if (!deferred_wait_start(c, rob.dispatch_id(), timeout_ms)) {
                            reply_err(op->sink(), "ERR out of memory");
                        } else {
                            conn.advance_parse(consumed);
                            self_->note_command(spec->id);
                            rob.publish();
                            c->set_blocked(true);
                            // Released by the quiescence backstop, not here: a parked WAIT's own
                            // completion (deferred_wait_pass) fires before its op retires, and
                            // dropping the barrier there would let younger frames parse ahead of
                            // the WAIT reply's staging. Owner bit named so the release is
                            // attributable; the release site is deliberately unchanged.
                            barrier_arm(c, BarrierOwner::Wait);
                            mark_active(c);
                            break;
                        }
                    } else if (wait == WaitCommandResult::Immediate) {
                        reply_int(op->sink(), 0);
                    }
                    // Error already carries its exact validation reply. Immediate already carries
                    // :0. Both retire through the ordinary local completion path below.
                    conn.advance_parse(consumed);
                    self_->note_command(spec->id);
                    op->state.store(OpState::Done, std::memory_order_release);
                    rob.publish();
                    enqueue_serve(c);
                    mark_active(c);
                    continue;
                }
                // RESET clears this lane's connection state (monitor mode, tracking registration,
                // CLIENT REPLY mode) before the ordinary handler writes +RESET. One predicted-
                // false flag test on a word the dispatcher already holds, on an already-cold
                // command class -- no name comparison, and nothing on the ordinary path.
                if (__builtin_expect((spec->flags & CmdFlags::Climon) != 0, false))
                    climon_reset_client(c);
                conn.advance_parse(consumed);
                self_->note_command(spec->id);
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
                    slowlog_record(self_->id(), c->id(), *op, now_ns() - slow_started,
                                   static_cast<int64_t>(wall.tv_sec) * 1000 +
                                       wall.tv_nsec / 1000000,
                                   slowlog_arm_, true);
                }
                snapshot_bind_io(nullptr, nullptr);
                command_set_local_context(nullptr, nullptr);
                op->state.store(OpState::Done, std::memory_order_release);
                rob.publish();
                enqueue_serve(c);
                mark_active(c);
                if (c->closing() || acl_command) break;
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
                        srv_->thread(tid).task_free_slots(self_->id()) < needed[tid]) {
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
                    if (!owner.post_task_quiet(self_->id(), task, sig)) std::abort();
                    if (!touched_[tid]) {
                        touched_[tid] = true;
                        touched_list_[ntouched_++] = tid;
                    }
                }
                self_->note_command(spec->id);
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
                mark_active(c);
                break;
            }

nonblocking_dispatch:
            ScatterDispatch scatter_dispatch;
            const ScatterPrepare scatter_prepared =
                xshard_prepare(*srv_, *op, scatter_pool_, self_->id(), c->id(), scatter_dispatch,
                               false, c);
            if (scatter_prepared == ScatterPrepare::Error) {
                conn.advance_parse(consumed);
                finish_prebuilt(c, *op);
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
                    xshard_destroy(scatter_dispatch.state, scatter_pool_, self_->id());
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
                        if (srv_->thread(tid).task_free_slots(self_->id()) < needed[tid]) {
                            room = false;
                            break;
                        }
                    }
                    // Restore the zero-on-entry invariant before EVERY exit from this arm.
                    for (uint32_t p = 0; p < nparticipants; p++) needed[participants[p]] = 0;
                    if (!room) {
                        xshard_destroy(scatter_dispatch.state, scatter_pool_, self_->id());
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
                        if (!owner.post_task_quiet(self_->id(), task, sig)) std::abort();
                        if (!touched_[tid]) {
                            touched_[tid] = true;
                            touched_list_[ntouched_++] = tid;
                        }
                    }
                    self_->note_command(spec->id);
                    conn.advance_parse(consumed);
                    sig.ops++;
                    head_candidate = false;
                    if (scatter_dispatch.barrier) barrier_arm(c, BarrierOwner::Scatter);
                    mark_active(c);
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
                    if (srv_->thread(tid).task_free_slots(self_->id()) < needed[tid]) {
                        room = false; break;
                    }
                }
                if (!room) {
                    xshard_destroy(scatter_dispatch.state, scatter_pool_, self_->id());
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
                    if (!owner.post_tasks_quiet(
                            self_->id(), posts + begin, end - begin, sig)) std::abort();
                    if (!touched_[tid]) { touched_[tid] = true; touched_list_[ntouched_++] = tid; }
                }
                self_->note_command(spec->id); // one public command, not one count per shard task
                conn.advance_parse(consumed);
                sig.ops++;
                head_candidate = false;
                if (scatter_dispatch.barrier) barrier_arm(c, BarrierOwner::Scatter);
                mark_active(c);
                continue;
            }

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
                op->hash  = FlatStore::hash_key(op->arg(static_cast<uint32_t>(spec->first_key)));
                op->shard = srv_->router().shard_of(op->hash);
            }
            ThreadCtx& worker = srv_->thread(srv_->worker_of_shard(op->shard));

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
            if (!worker.post_task_quiet(self_->id(), t, sig)) {
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
            conn.advance_parse(consumed);
            sig.ops++;
            {
                const uint32_t wkr = static_cast<uint32_t>(srv_->worker_of_shard(op->shard));
                if (!touched_[wkr]) { touched_[wkr] = true; touched_list_[ntouched_++] = wkr; }
            }
            mark_active(c);
        }
        // Item 2: one notify per worker per parse pass, not per op. The pushes above are already
        // visible in the queues; this publishes the "look here" bit and pays the wake decision once.
        // The touched set is a LIST, not a scan: the first version swept all 128 thread slots per
        // pass, which at p1 is 128 loads per op and measured -2.5% -- the batching win eaten by its
        // own bookkeeping.
        for (uint32_t i = 0; i < ntouched_; i++) {
            const uint32_t wkr = touched_list_[i];
            touched_[wkr] = false;
            srv_->thread(wkr).flush_task_notify(self_->id(), ring_, sig);
        }
        ntouched_ = 0;
    }

    // THE ONE DOOR ONTO THE PARSE BARRIER. Every owner parks a connection through here so the
    // overlap -- two owners holding the barrier at once -- is COUNTED rather than assumed absent.
    // NOTES-BARRIER.md section 2 argues from the source that no production sequence produces one
    // today; barrier_owner_overlaps is that argument's live assertion, and a validation run that
    // wants the two-owner geometry gates on it rather than trusting the prose. Cold by
    // construction: the six owners are EXEC, a subscribe, a blocking command, a deferred WAIT, a
    // barriered scatter and a CLIENT fan-out. GET and SET never reach it.
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
        if (c->in_active()) return;          // one load, not a scan of the whole set
        c->set_in_active(true);
        active_.insert(c);
    }

    // ---- inbound: workers telling us a client has completed ops -----------------------------------
    // Inbound from workers: "ops are Done" -- the claimed-post fallback for a conn with no
    // ready-mask slot. Either way the answer is the same: put the client back in the active set.
    template <bool HasUnix, bool HasTls, bool kEp>
    uint32_t sweep() {
        uint32_t work = 0;
        if constexpr (HasUnix) work += flush_handoffs();
        work += flush_borrow_releases() + collect_retire_work<HasUnix, kEp>(true) +
                flush_ready<HasTls, kEp>();
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

    template <bool HasUnix, bool kEp>
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
                    return;
                }
            c->retire_queued().store(false, std::memory_order_release);
            enqueue_serve(c);                    // a posted client is a serve request
            mark_active(c);
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
                if (c && !c->dead()) { enqueue_serve(c); mark_active(c); n++; }
            }
        }
        return n + pubsub_work;
    }

    // ---- retire -> stage bytes -> send or hand off -------------------------------------------------
    // The io thread's own work per active client. In 2-stage it also owns the reply side and calls
    // serve() here; in ex-wb and 3-stage the sender does that on its own thread and io only keeps
    // the READ side moving — reclaim the buffer once nothing points into it, and re-arm.
    template <bool HasTls, bool kEp>
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
            TlsConn* tls = nullptr;
            if constexpr (HasTls) tls = tls_engine(c);
            if (backstop_pass_ && !c->serve_pending()) enqueue_serve(c);

            if constexpr (HasTls) {
                if (tls) {
                    // BIO_nwrite0 pins the input-ring frontier until the recv CQE commits it.
                    // SSL_write and opposite-direction BIO reads are proven safe while pinned,
                    // but SSL_read/SSL_accept consume the same direction and can move that
                    // frontier. Only the recv completion may drive inbound TLS while armed.
                    if (!c->closing() && !c->recv_armed()) (void)drive_tls<kEp>(c);
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
                        if (tls) arm_tls_recv<kEp>(c);
                        else arm_recv<kEp>(c);
                    } else {
                        arm_recv<kEp>(c);
                    }
                    if constexpr (HasTls) if (tls) { (void)drive_tls<kEp>(c); tls = tls_engine(c); }
                    if (wb_.take_send_failure()) epoll_request_close(c);
                }
            }

            // Re-parse the buffered remainder. parse_and_dispatch stops when the ROB window fills
            // and is otherwise only driven by recv completions, so a client that sent a whole
            // pipeline in ONE write would get `window` replies and then hang. Retiring frees slots,
            // which is what makes the rest parseable.
            if (!c->closing() && conn.rpos() < conn.rlen() && !c->scatter_barrier() &&
                !c->atomic_backpressure()) {
                // A CLIENT PAUSE hold deliberately leaves the parsed frame at rpos. Counting that
                // as work would spin the ring at 100% until the deadline instead of parking it,
                // so while a pause is live the pass reports progress only if the cursor moved.
                // With no pause armed the accounting is byte-for-byte the pre-lane behaviour --
                // one predicted-false test per active connection per pass. The dispatch variant
                // stays keyed on c->is_tls() (the kTLS handoff's contract), not the slot pointer.
                if (__builtin_expect(climon_pause_armed(), false)) {
                    const uint32_t rpos_before = conn.rpos();
                    if constexpr (HasTls) {
                        if (c->is_tls()) parse_and_dispatch<true>(c);
                        else parse_and_dispatch<false>(c);
                    } else {
                        parse_and_dispatch<false>(c);
                    }
                    if (conn.rpos() != rpos_before) work++;
                } else {
                    if constexpr (HasTls) {
                        if (c->is_tls()) parse_and_dispatch<true>(c);
                        else parse_and_dispatch<false>(c);
                    } else {
                        parse_and_dispatch<false>(c);
                    }
                    work++;
                }
            }

            if constexpr (!kEp) {
                if constexpr (HasTls) {
                    if (tls && tls->memory_bio()) arm_tls_recv<kEp>(c);
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

            const bool more_input = conn.rpos() < conn.rlen();
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
        while (served < kServeBudget && !pending_serve_.empty()) {
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
                    if (wb_.serve_tls<kEp>(*c, *tls)) work++;
                    if (tls->socket_userspace() && tls->has_pinned_plain())
                        arm_tls_socket_poll<kEp>(c, tls->wanted());
                    if (tls->failed()) close_client(c, tls->output_pending() || c->send_inflight());
                } else if (TlsConn* slot = tls_slot_conn(c); slot && slot->ktls()) {
                    if (wb_.serve_ktls<kEp>(*c)) work++;
                } else if (wb_.serve<kEp>(*c)) {
                    work++;
                }
            } else if (wb_.serve<kEp>(*c)) {
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

    bool deferred_wait_start(Client* client, uint64_t op_id, uint64_t timeout_ms) {
        const uint64_t deadline_ms = timeout_ms
            ? now_ns() / 1000000ull + timeout_ms
            : 0;
        try { deferred_waits_.push_back(DeferredWait{client, op_id, deadline_ms}); }
        catch (const std::bad_alloc&) { return false; }
        srv_->blocking_client_parked();
        return true;
    }

    uint32_t deferred_wait_pass(uint64_t now_ms) {
        uint32_t completed = 0;
        for (size_t i = 0; i < deferred_waits_.size();) {
            const DeferredWait wait = deferred_waits_[i];
            if (!wait.deadline_ms || now_ms < wait.deadline_ms) {
                i++;
                continue;
            }
            Client* client = wait.client;
            Op& op = client->rob().at(wait.op_id);
            reply_int(op.sink(), 0);
            op.state.store(OpState::Done, std::memory_order_release);
            client->set_blocked(false);
            srv_->blocking_client_unparked();
            deferred_waits_[i] = deferred_waits_.back();
            deferred_waits_.pop_back();
            enqueue_serve(client);
            mark_active(client);
            completed++;
        }
        return completed;
    }

    bool deferred_wait_cancel(Client* client) {
        bool cancelled = false;
        for (size_t i = 0; i < deferred_waits_.size();) {
            const DeferredWait wait = deferred_waits_[i];
            if (wait.client != client) {
                i++;
                continue;
            }
            Op& op = client->rob().at(wait.op_id);
            op.state.store(OpState::Done, std::memory_order_release);
            srv_->blocking_client_unparked();
            deferred_waits_[i] = deferred_waits_.back();
            deferred_waits_.pop_back();
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
            if (deferred_wait_cancel(c)) enqueue_serve(c);
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
        auto& v = self_->clients();
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] == c) { v[i] = v.back(); v.pop_back(); break; }
        // THE RETRY IS THE LOAD-BEARING PART of this wait: the client left the active set when it
        // went quiet, so nothing revisits it unless mark_active puts it back -- returning without
        // doing so leaked the entire client (~137KB) per disconnect once. Only a quiesced,
        // claim-free conn may release its slot and die.
        if (!c->safe_to_release()) { mark_active(c); return; }
        climon_untrack_client(c);
        command_client_disconnected(c);
        self_->release_wb_slot(c->wb_slot());
        c->set_wb_slot(Client::kNoWbSlot);
        wb_.teardown(*c);
        ::close(c->fd());
        srv_->client_released();
        // NOT delete. An ex-side claimed post may still sit un-consumed in our inbound channels
        // naming this client. Every such entry was pushed BEFORE this point, and channels are FIFO
        // with their mask bits set -- so ONE full
        // collect_retire_work pass consumes all of them. Park the corpse for one loop iteration and
        // free it at the top of the one after; the drain lambda skips dead clients.
        c->mark_dead();
        dead_next_.push_back(c);
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
            if (c->serve_pending() || c->send_inflight() || c->recv_armed()) {
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
    ThreadCtx* self_ = nullptr;
    static constexpr uint32_t kFlushBackstopEvery = 64;
    // Serves per pass. Sized so a pass's serve work stays comparable to its recv work: ~16 serves
    // x a ~32-op prefix each is one CQ batch worth of replies. The queue, not the pass, absorbs
    // overload.
    static constexpr uint32_t kServeBudget = 16;
    std::deque<Client*> pending_serve_;
    std::deque<BorrowRelease> pending_releases_;
    std::deque<Client*> pending_handoffs_;
    struct DeferredWait {
        Client* client = nullptr;
        uint64_t op_id = 0;
        uint64_t deadline_ms = 0;  // zero is Redis's wait-forever spelling
    };
    std::vector<DeferredWait> deferred_waits_;  // allocates only after an unsatisfied WAIT
    ScatterArenaPool scatter_pool_;          // touched only by this connection-owning IO thread
    uint32_t flush_tick_ = 0;
    bool     backstop_pass_ = false;
    static constexpr uint32_t kClientCronBeatsPerSecond = 10;
    static constexpr uint32_t kClientCronMinVisits = 5;
    uint64_t client_cron_beat_ms_ = 0;
    uint64_t save_cron_beat_ms_ = 0;
    size_t   client_cron_cursor_ = 0;
    uint64_t cached_now_ms_ = 0;
    uint32_t cached_now_s_ = 0;
    bool     client_cron_was_armed_ = false;
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
};

}  // namespace tomo
