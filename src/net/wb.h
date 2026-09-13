// wb.h — write-back: turning completed replies into bytes on a socket.
//
// PURE 2s (owner ruling 2026-08-24): the io thread that owns a connection is its only sender,
// for life. The 3s posture (dedicated wb threads) was measured and deleted -- its one win was
// +2-4% at 64c p32 (deep pipe + ex-scarce, the split tax amortized), and it lost everything
// else: p1 -22..-35%, p32<=32c -8%, overload -8% with double the tail, large values tied at the
// wire wall with worse tail. The depth sweep (p1..p32: -33% -> +4%, crossover ~p16-32) is the
// architecture story: the split's per-op tax (extra kernel transitions, Client/sock line
// migration, one hop) amortizes with batching, and only then can decoupled edges convert saved
// cores into ex. One base is worth more than that corner.
//
// (Executor-issued sends (exwb) existed and were DELETED
// 2026-08-24: #1 in zero measured cells. Send work rides the scarce ex role: nearly free at p1,
// ruinous at p32. The p1 width law explains its old small-size p1 second places, and 2s dominates
// those anyway.)
//
// The width law (2026-08-24): p1 throughput = min(recv lanes, send lanes) x per-lane rate; a 2s io
// thread is both lanes at once, so 2s buys width ~twice as cheaply and owns p1. 3s protects ex from
// send work and wins high-core high-pipe (64c p32) and overload.
//
// ============================================================================================
// THE LOCK BUG THIS FILE EXISTS TO PREVENT
//
// In the fork, lock and unlock each independently re-derived the write-back object from the client:
//
//     tomoWbLockClient(c)   { wc = clientTail(c)->wb; ...lock(wc); }
//     tomoWbUnlockClient(c) { wc = clientTail(c)->wb; unlock(wc); assert(before > 0); }
//
// If that expression yielded a different object between the two calls, unlock released a mutex it
// never held and decremented a counter lock never incremented. It crashed on the 25GbE NIC and was
// NOT reproducible on loopback — the worst possible shape.
//
// WbGuard resolves the target ONCE, in its constructor, and uses that captured pointer in its
// destructor. Re-derivation is impossible, so the entire bug class is gone by construction rather
// than by discipline.
// ============================================================================================
//
// TWO ENGINES, ONE REPLY STRUCTURE (--net-io, 2026-08-28). Under io_uring a send is SUBMITTED and
// its result arrives later as a CQE; under epoll the io thread issues the syscall itself and the
// result is in hand immediately. Everything else -- what gets staged, in what order, which borrow
// is released when, the one-send-per-socket rule -- is identical, and deliberately so: the reply
// path's shape is not the engine's business.
//
// The split is a TEMPLATE PARAMETER, not a field. serve/pump/submit take <bool kEp> and the io loop
// passes the value its own boot-resolved instantiation carries, so the uring build emits exactly the
// instruction sequence it emitted before this lane and the epoll build never tests a mode bit. The
// two cold entry points that cannot be templated (serve_suppressing, reached only from the CLIENT
// REPLY object; the TLS drain inside close_client) dispatch on epoll_ instead -- both are already
// out-of-line and neither is on a hot path.
//
// WHY EPOLL DOES NOT REUSE on_send_complete. That handler ends in "resubmit -> pump", which under
// io_uring means "queue another SQE and return". Called synchronously it would mean "syscall again,
// right now, from inside the accounting for the previous syscall" -- and on EAGAIN, which is the
// normal steady state of a backed-up socket, it would recurse until the stack ran out. The epoll
// pump is therefore an explicit loop with EAGAIN as its exit, and it stops leaving the connection
// staged; the EPOLLOUT edge (armed once at accept, never re-armed) is what resumes it.
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <string>
#include <sys/socket.h>
#include <unordered_map>
#include "conn.h"
#include "epoll.h"
#include "resp.h"       // format_reply_code: the owner's half of the coded reply
#include "tls.h"
#include "uring.h"
#include "../core/signal.h"

namespace tomo {

// THE OWNER'S HALF of the coded reply (see ReplyCode in src/exec/op.h) has TWO shapes here, and
// the split is a performance contract, not a style choice.
//
// The HOT path -- an op retiring with nothing staged ahead of it -- renders straight into the fill
// buffer's frontier through reserve_fill/commit_fill. No temporary.
//
// The COLD paths -- a borrowed value, or a segment queue already open -- take op_materialise_code()
// instead, which turns the code back into bytes inside op.reply and leaves their existing
// append_buf_segment(op.direct, op.direct_len, op.reply.data(), op.reply.size()) lines completely
// untouched. A code implies op.reply is empty and op.direct_len is zero, so the bytes land in the
// right order with no reordering logic at all.
//
// The obvious middle road -- render into a `char scratch[kReplyCodeMax]` and pass it along -- was
// built first and MEASURED WORSE. A local array makes GCC apply -fstack-protector-strong to the
// enclosing function, and the enclosing function here is the per-op retire lambda: it went from
// 139-196 instructions to 237-280 and picked up two to three canary sequences, which EVERY retired
// op paid, GETs included, for a benefit only coded replies receive. That is the whole of the split
// GET regression. No array on this path, ever.

// The coded reply's ONLY appearance in the per-op retire lambda is a call to this. Rendering it
// inline meant inlining reserve_fill -> SmallBuf::reserve -> grow(), i.e. malloc/memcpy/free, into
// the hottest function the io thread runs -- paid in code size and register pressure by every op,
// including the GETs that never carry a code. Base reached its reply through an out-of-line
// SmallBuf::append call too, so this trades like for like.
template <bool TrackOutput>
__attribute__((noinline)) void stage_coded_reply(Client& conn, Op& op) {
    const uint32_t n = format_reply_code(conn.reserve_fill(kReplyCodeMax),
                                         op.reply_code_, op.reply_ival_);
    if constexpr (TrackOutput) conn.commit_fill(n);
    else                       conn.fill_buf().commit_raw(n);
}

// THE LOCK BUG note above is preserved as history: WbGuard died with the multi-sender designs
// (exwb, then 3s). In pure 2s exactly one thread -- the connection's io thread -- ever touches the
// send side, so there is nothing to lock and no object to re-derive. If a future flip ever puts
// two servers on one connection, resurrect the guard from git history, not from memory.

// Per-thread send engine. Every loop that sends owns one and calls the same two methods.
class WbEngine {
    struct DeferredOob {
        uint64_t after = 0;             // exclusive ROB issue frontier preceding these frames
        std::string bytes;
    };
    using DeferredOobQueue = std::deque<DeferredOob>;

public:
    using ReleaseFn = void (*)(void*, int32_t, const char*);
    using RetireFn = void (*)(void*, Client&, Op&);
    using LimitFn = bool (*)(void*, Client&);

    void bind(Ring* ring, void* release_ctx = nullptr, ReleaseFn release_fn = nullptr,
              void* retire_ctx = nullptr, RetireFn retire_fn = nullptr,
              const std::atomic<bool>* limit_armed = nullptr,
              void* limit_ctx = nullptr, LimitFn limit_fn = nullptr,
              const uint32_t* cached_now_s = nullptr, LoopSignals* tls_signals = nullptr) {
        epoll_ = g_ring_epoll_mode;
        ring_ = ring;
        release_ctx_ = release_ctx;
        release_fn_ = release_fn;
        retire_ctx_ = retire_ctx;
        retire_fn_ = retire_fn;
        limit_armed_ = limit_armed;
        limit_ctx_ = limit_ctx;
        limit_fn_ = limit_fn;
        cached_now_s_ = cached_now_s;
        tls_signals_ = tls_signals;
    }

    Ring&  ring()       { return *ring_; }
    // Only cold, non-templated send sites consult this boot-latched fallback. Hot paths carry the
    // pipeline-1 classifier as a template argument.
    void set_cold_send_classification(bool enabled) { classify_cold_sends_ = enabled; }


    // ---- OUT-OF-BAND FRAMES WAITING FOR EARLIER-ISSUED REPLIES ----------------------------------
    //
    // A retire callback calls back into the loop: cross-shard assembly (assemble_mget stages
    // [array header][borrow][...] into the segment queue and leaves the reply's TAIL in the Op),
    // then the notification/tracking hook, which can synthesise a pub/sub delivery, a tracking
    // invalidation or a MONITOR line for the very connection being drained. At that instant the
    // connection's newest reply is only PARTIALLY staged, so Client::append_oob's frame-boundary
    // argument does not hold and appending would splice the frame into the middle of it.
    //
    // The same parking is also required before a drain when this connection's ROB is busy. Putting
    // the frame straight into the segment queue is a safe frame boundary, but that queue precedes
    // replies which have not retired yet: an SSUBSCRIBE delivery can then overtake its own ack.
    // Each parked frame therefore records the current dispatch frontier and flushes only after a
    // drain has staged replies through that frontier. Frames from different connections, and from
    // different frontiers on one connection, cannot share the old single string.
    //
    // A genuinely parked blocking command is the bounded exception. It is the sole ROB entry (the
    // blocking dispatch is a whole-connection barrier), and while it remains Issued it has no reply
    // bytes to preserve, so a push may take the segment route immediately instead of waiting behind
    // a 30-second BLPOP. Once it is Done, its reply is in flight and ordinary deferral applies.
    // Inside this connection's drain deferral is unconditional because reply staging may be partial.
    bool defer_oob(Client& c, const char* a, size_t an,
                   const char* b = nullptr, size_t bn = 0) {
        if (draining_ != &c) {
            Rob<kRobWindow>& rob = c.rob();
            if (rob.quiesced()) return false;
            if (c.blocked() &&
                rob.at(rob.flush_id()).state.load(std::memory_order_acquire) != OpState::Done)
                return false;
        }
        const uint64_t after = c.rob().dispatch_id();
        DeferredOobQueue& queue = oob_defer_[&c];
        if (queue.empty() || queue.back().after != after)
            queue.push_back(DeferredOob{after, std::string{}});
        queue.back().bytes.append(a, an);
        if (bn) queue.back().bytes.append(b, bn);
        return true;
    }

    // THE WHOLE REPLY SIDE, in one call: retire completed ops IN ORDER, stage their bytes, and
    // write. All three belong together and all three belong to the sender -- if the io thread
    // retired and merely handed bytes over, only the send syscall would move between modes, and
    // an "ex-wb" measured that way would not be ex-wb. In pure 2s the sender is the connection's
    // io thread and nobody else (the Wb-thread and EX-mode callers this comment once listed were
    // deleted with those postures; see the head of this file), so no lock exists or is needed and
    // the ROB stays SPSC by construction. Returns true if it did anything, so a caller can tell
    // progress from an empty poll.
    template <bool kEp = false, bool ClassifySend = false, bool Coded = false>
    bool serve(Client& c) {
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, false, kEp, true, ClassifySend, Coded>(c);
        return serve_impl<false, false, kEp, true, ClassifySend, Coded>(c);
    }

    // Micro-pipeline retirement half. It drains exactly the same in-order prefix and stages the
    // same buffers/segments as serve(), but deliberately leaves SQE construction to pump().
    template <bool kEp = false, bool Coded = false>
    bool prepare(Client& c, bool& submit_allowed) {
        submit_allowed = true;
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, false, kEp, false, false, Coded>(c, &submit_allowed);
        return serve_impl<false, false, kEp, false, false, Coded>(c, &submit_allowed);
    }

    // Unified pipeline batches already guard a nullable Client slot before their submit half. Its
    // bound limit callback tombstones that slot on the rare refusal, avoiding a parallel bool on
    // every ordinary reply while leaving the established split-pipeline prepare API untouched.
    template <bool kEp = false, bool Coded = false>
    bool prepare_pipeline(Client& c) {
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, false, kEp, false, false, Coded>(c);
        return serve_impl<false, false, kEp, false, false, Coded>(c);
    }

    // kTLS uses the ordinary plaintext staging and send path. This separate instantiation only
    // enforces/counts the pre-existing TLS no-borrow contract; plaintext clients pay no mode test.
    template <bool kEp = false, bool ClassifySend = false, bool Coded = false>
    bool serve_ktls(Client& c) {
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, true, kEp, true, ClassifySend, Coded>(c);
        return serve_impl<false, true, kEp, true, ClassifySend, Coded>(c);
    }

    template <bool kEp = false, bool Coded = false>
    bool prepare_ktls(Client& c, bool& submit_allowed) {
        submit_allowed = true;
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, true, kEp, false, false, Coded>(c, &submit_allowed);
        return serve_impl<false, true, kEp, false, false, Coded>(c, &submit_allowed);
    }

    template <bool kEp = false, bool Coded = false>
    bool prepare_pipeline_ktls(Client& c) {
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_impl<true, true, kEp, false, false, Coded>(c);
        return serve_impl<false, true, kEp, false, false, Coded>(c);
    }

    // TLS is a separate write-back variant selected by the IO owner. Plain serve()/pump() above
    // remain untouched and are the only instantiated path when tls-port is zero.
    template <bool kEp = false, bool ClassifySend = false, bool Coded = false>
    bool serve_tls(Client& c, TlsConn& tls) {
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_tls_impl<true, kEp, true, ClassifySend, Coded>(c, tls);
        return serve_tls_impl<false, kEp, true, ClassifySend, Coded>(c, tls);
    }

    template <bool kEp = false, bool Coded = false>
    bool prepare_tls(Client& c, TlsConn& tls, bool& submit_allowed) {
        submit_allowed = true;
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_tls_impl<true, kEp, false, false, Coded>(c, tls, &submit_allowed);
        return serve_tls_impl<false, kEp, false, false, Coded>(c, tls, &submit_allowed);
    }

    template <bool kEp = false, bool Coded = false>
    bool prepare_pipeline_tls(Client& c, TlsConn& tls) {
        if (__builtin_expect(limit_armed_ &&
                             limit_armed_->load(std::memory_order_relaxed), false))
            return serve_tls_impl<true, kEp, false, false, Coded>(c, tls);
        return serve_tls_impl<false, kEp, false, false, Coded>(c, tls);
    }

    // THE ENGINE'S ONE ESCALATION CHANNEL. Under io_uring a fatal send error is reported by
    // on_send_complete returning false and the io loop closing the connection right there. A
    // synchronous send has no such return path to ride -- it happens several frames inside
    // serve()/drive_tls() -- so it latches here and the io loop consumes it immediately after each
    // epoll-instantiated serve/pump site. Consuming clears it: a stale bit would close the NEXT
    // connection served, which is exactly the class of bug this must not introduce.
    bool take_send_failure() {
        const bool failed = send_failed_;
        send_failed_ = false;
        return failed;
    }

    // CLIENT REPLY OFF/SKIP (Lane F). Cold: reached only from the climon object when the reply
    // armed bit is set, never from serve(), so the send engine's hot template pair is untouched.
    //
    // PER-OP, and that is the point. Suppression cannot be decided per connection: a pipelined
    // `CLIENT REPLY SKIP; PING; PING` retires all three in ONE drain, and a per-connection
    // decision would swallow both PONGs. The mark is made by the io thread's armed gate before
    // dispatch, so each op carries its own answer here. The per-connection reply mode only
    // SELECTS this variant (climon_reply_suppressed), and the hot serve never reads the mark, so
    // the mode must outlive every marked op: CLIENT REPLY ON and RESET leave OFF through the
    // SkipNow drain state (climon.cc) instead of dropping to ON while marked ops are in flight.
    //
    // Special command state MUST still be surrendered through retire_fn_ even when the bytes are
    // dropped, or scatter/blocking/MULTI/notification state leaks and cross-shard groups never
    // complete. A borrowed value that survives retire_fn_ is returned to its owning shard instead
    // of being sent. Direct-reply bytes live in the fill buffer's spare capacity and are simply
    // never committed.
    __attribute__((noinline, cold))
    bool serve_suppressing(Client& c) {
        return serve_suppressing_impl(c, true);
    }

    __attribute__((noinline, cold))
    bool prepare_suppressing(Client& c, bool& submit_allowed) {
        submit_allowed = true;
        return serve_suppressing_impl(c, false, &submit_allowed);
    }

private:
    __attribute__((noinline, cold))
    bool serve_suppressing_impl(Client& c, bool submit,
                                bool* submit_allowed = nullptr) {
        stats_.serves++;
        Client& conn = c;
        conn.start_obuf_tracking();
        draining_ = &c;
        const uint32_t retired = c.rob().drain([&](Op& op) {
            // A retire hook may stage THIS op's own bytes before the skip is consulted: cross-
            // shard MGET assembly seals the fill buffer, then appends the array header, every
            // borrowed bulk and its CRLF to the segment queue. Record the queue frontier first so
            // a suppressed op can take exactly those back instead of leaking a partial array.
            const uint32_t segments_before = conn.output_list_length();
            const bool fill_was_staged = conn.has_pending_fill();
            if (op.zc_ptr) {
                if (retire_fn_) retire_fn_(retire_ctx_, conn, op);
            }
            if (op.reply_skip()) {
                if (op.zc_ptr && op.zc_shard >= 0) release(op.zc_shard, op.zc_ptr);
                // Everything past the frontier is this op's reply -- except the seal, which moved
                // OLDER fill bytes into the queue and must stay. It exists iff the fill buffer
                // went from staged to empty across the hook; nothing else empties it mid-drain.
                const uint32_t keep = segments_before +
                    ((fill_was_staged && !conn.has_pending_fill()) ? 1u : 0u);
                conn.truncate_segments(keep,
                    [&](int32_t shard, const char* ptr) { release(shard, ptr); });
                return;
            }
            if (op.zc_ptr) {
                conn.seal_fill_segment();
                if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
                conn.append_borrow_segment(op.zc_ptr, op.zc_len, op.zc_shard);
                conn.append_static_segment(kCrlf, sizeof(kCrlf));
                return;
            }
            // A suppressed op ahead of this one may have left uncommitted direct bytes at the
            // fill frontier; this op's direct region was handed out at the same offset only if
            // nothing was staged, so committing here stays correct.
            if (conn.has_pending_segments()) {
                if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
            } else {
                // A coded reply is rendered straight into the fill frontier: no temporary, and a
                // compile-time length instead of the runtime-length memcpy op.reply needed.
                if (op.reply_code_) stage_coded_reply<true>(conn, op);
                else if (op.direct_len) conn.commit_fill(op.direct_len);
                if (!op.reply.empty()) conn.append_fill(op.reply.data(), op.reply.size());
            }
        });
        draining_ = nullptr;
        bool did = retired != 0;
        did |= flush_deferred_oob(conn);
        if (limit_fn_ && limit_fn_(limit_ctx_, c)) {
            if (submit_allowed) *submit_allowed = false;
            stats_.retired += retired;
            return true;
        }
        if (submit && !conn.nothing_to_write()) {
            if (epoll_)
                did |= pump<true>(c);
            else if (classify_cold_sends_)
                did |= pump<false, true>(c);
            else
                did |= pump<false>(c);
        }
        stats_.retired += retired;
        return did;
    }

public:

    // Try to push whatever this client has buffered. Safe to call spuriously: if nothing is pending
    // or a send is already outstanding it does nothing. Returns true if a send was submitted.
    template <bool kEp = false, bool ClassifySend = false>
    bool pump(Client& c) {
      if constexpr (kEp) { return pump_epoll(c); }
      else {
        if (c.send_inflight()) return false;               // preserve one-send-per-socket ordering

        Client& conn = c;
        // A legacy contiguous send may predate the first borrowed reply. It remains the wire head
        // until drained; the segment queue starts strictly after it.
        const size_t legacy_total = conn.send_buf().size();
        const size_t legacy_sent  = conn.wsent();
        if (legacy_sent < legacy_total)
            return submit_legacy<kEp, ClassifySend>(c, legacy_total, legacy_sent);

        if (conn.has_pending_segments()) {
            bool has_borrow = false;
            uint32_t total = 0;
            const uint32_t niov = conn.build_segment_iov(has_borrow, total);
            if (!niov) return false;

            io_uring_sqe* s = ring_->sqe();
            if (!s) return false;
            io_uring_prep_sendmsg(s, conn.fd(), conn.send_msg(), MSG_NOSIGNAL);
            s->user_data = ur_tag(UrKind::Send, &c);
            ring_->note_send_pending<ClassifySend>();

            conn.set_segmented_send(true);
            conn.set_send_requested(total);
            conn.set_send_inflight(true);
            stats_.sends_submitted++;
            if (has_borrow) stats_.zc_sends++;
            return true;
        }

        // Nothing outstanding, so if the send buffer is fully written we may promote the fill
        // buffer. This is the ONLY point at which the two swap, and it is safe precisely because
        // send_inflight is false here.
        if (conn.write_drained() && conn.has_pending_fill()) conn.swap_buffers();

        const size_t total = conn.send_buf().size();
        const size_t sent  = conn.wsent();
        return sent < total ? submit_legacy<kEp, ClassifySend>(c, total, sent) : false;
      }
    }

    // The epoll send loop. Same three sources in the same order as the uring pump -- legacy
    // remainder, then the segment queue (which is where a BORROWed value rides, so zero-copy is
    // unchanged: the borrow is still handed to the kernel by pointer through an iovec and released
    // only for the bytes the kernel reports accepted), then a promoted fill buffer -- but each
    // write is issued here and accounted here.
    //
    // THREE EXITS, and only the first one is "done": nothing left to write; EAGAIN (the socket is
    // full, so stop staged and let the EPOLLOUT edge call us back); or a fatal errno, which latches
    // send_failed_ for the io loop to turn into a close. A short write is not an exit -- it loops,
    // exactly as the uring path resubmits from the (head index, byte offset) frontier.
    bool pump_epoll(Client& c) {
        Client& conn = c;
        bool did = false;
        for (;;) {
            const size_t legacy_total = conn.send_buf().size();
            const size_t legacy_sent  = conn.wsent();
            if (legacy_sent < legacy_total) {
                if (!write_legacy_epoll(c, legacy_total, legacy_sent, did)) return did;
                continue;
            }
            if (conn.has_pending_segments()) {
                bool has_borrow = false;
                uint32_t total = 0;
                const uint32_t niov = conn.build_segment_iov(has_borrow, total);
                if (!niov) return did;
                conn.set_segmented_send(true);
                conn.set_send_requested(total);
                stats_.sends_submitted++;
                if (has_borrow) stats_.zc_sends++;
                const ssize_t n = ::sendmsg(conn.fd(), conn.send_msg(),
                                            MSG_NOSIGNAL | MSG_DONTWAIT);
                if (n <= 0) { note_send_stop(c, n); return did; }
                stats_.zc_bytes += conn.consume_segments(static_cast<uint32_t>(n),
                    [&](int32_t shard, const char* ptr) { release(shard, ptr); });
                stats_.bytes_sent += static_cast<uint64_t>(n);
                if (static_cast<uint32_t>(n) < total) stats_.short_writes++;
                else stats_.sends_completed++;
                if (cached_now_s_) c.set_last_interaction_s(*cached_now_s_);
                did = true;
                continue;
            }
            if (conn.write_drained() && conn.has_pending_fill()) {
                conn.swap_buffers();
                continue;
            }
            return did;
        }
    }

    // Non-templated wrapper for the two cold sites that cannot carry the engine in their type:
    // close_client's TLS alert/close_notify drain, and the CLIENT REPLY suppressed serve.
    bool pump_tls_any(Client& c, TlsConn& tls) {
        if (epoll_) return pump_tls<true>(c, tls);
        return classify_cold_sends_ ? pump_tls<false, true>(c, tls)
                                    : pump_tls<false>(c, tls);
    }

    // Engine independent except for its one ciphertext write, which submit_tls_cipher<kEp> owns.
    // The plaintext -> OpenSSL half is identical in both engines.
    template <bool kEp = false, bool ClassifySend = false>
    bool pump_tls(Client& c, TlsConn& tls) {
        if (c.send_inflight()) return false;

        // Ciphertext already materialized by OpenSSL is always the wire head. Its CQE advances only
        // the external BIO cursor; it never advances Client's plaintext segment/wsent frontiers.
        const char* cipher = nullptr;
        const int cipher_bytes = tls.peek_output(cipher);
        if (cipher_bytes > 0)
            return submit_tls_cipher<kEp, ClassifySend>(
                c, tls, cipher, static_cast<uint32_t>(cipher_bytes));
        if (!tls.connected()) return false;

        Client& conn = c;
        const char* plain = nullptr;
        size_t plain_bytes = 0;
        bool segmented = false;

        const size_t legacy_total = conn.send_buf().size();
        const size_t legacy_sent = conn.wsent();
        if (legacy_sent < legacy_total) {
            plain = conn.send_buf().data() + legacy_sent;
            plain_bytes = legacy_total - legacy_sent;
        } else if (conn.has_pending_segments()) {
            bool has_borrow = false;
            uint32_t total = 0;
            const uint32_t niov = conn.build_segment_iov(has_borrow, total);
            if (!niov) return false;
            // Both producers are gated before this point. Keep this defensive assertion close to
            // encryption: a future producer must not silently resurrect borrowed TLS output.
            if (has_borrow) std::abort();
            plain = static_cast<const char*>(conn.send_msg()->msg_iov[0].iov_base);
            plain_bytes = conn.send_msg()->msg_iov[0].iov_len;
            segmented = true;
        } else {
            if (conn.write_drained() && conn.has_pending_fill()) conn.swap_buffers();
            const size_t total = conn.send_buf().size();
            const size_t sent = conn.wsent();
            if (sent >= total) return false;
            plain = conn.send_buf().data() + sent;
            plain_bytes = total - sent;
        }

        if (tls.has_pinned_plain()) {
            const char* pinned = nullptr;
            size_t pinned_bytes = 0;
            tls.pinned_plain(pinned, pinned_bytes);
            // The retry contract is byte-identical. The client frontier cannot move on WANT_*.
            if (pinned != plain || pinned_bytes > plain_bytes) std::abort();
            plain = pinned;
            plain_bytes = pinned_bytes;
        }

        const TlsIoResult encrypted = tls.write_plain(plain, plain_bytes);
        if (encrypted.op == TlsOp::Progress) {
            const uint32_t plain_accepted = encrypted.bytes;
            if (segmented) {
                (void)conn.consume_segments(plain_accepted,
                    [&](int32_t shard, const char* ptr) { release(shard, ptr); });
            } else {
                conn.commit_write(plain_accepted);
            }
            if (tls_signals_) tls_signals_->tls_plaintext_output_bytes += plain_accepted;
        } else if (encrypted.op == TlsOp::WantWrite) {
            if (tls_signals_) tls_signals_->tls_want_write++;
        } else if (encrypted.op == TlsOp::WantRead) {
            if (tls_signals_) tls_signals_->tls_want_read++;
        }
        if (tls.failed()) {
            if (!tls.last_error().empty())
                std::fprintf(stderr, "TLS client %llu: %s\n",
                             static_cast<unsigned long long>(c.id()),
                             tls.last_error().c_str());
            return false;
        }

        cipher = nullptr;
        const int ready = tls.peek_output(cipher);
        if (ready > 0)
            return submit_tls_cipher<kEp, ClassifySend>(
                c, tls, cipher, static_cast<uint32_t>(ready));
        return encrypted.op == TlsOp::Progress;
    }

    // Completion handler. `res` is the CQE result: bytes written, or negative errno.
    // Returns false when the connection should be torn down.
    template <bool SubmitFollowup = true, bool ClassifySend = false>
    bool on_send_complete(Client& c, int res) {
        bool resubmit = false;
        {
            c.set_send_inflight(false);

            if (res < 0) {
                if (res == -EAGAIN || res == -EINTR) { resubmit = true; }
                else if (res == -ECONNRESET || res == -EPIPE || res == -ECONNABORTED ||
                         c.closing()) {
                    // The peer tore the connection down mid-send (abrupt-disconnect batteries,
                    // CLIENT KILL, kTLS conns on this plain path). No application reply is being
                    // lost that the peer could still read; counting these as data-path send
                    // errors made err=0 a timing lottery. Same carve-out the memory-BIO arm has.
                    stats_.peer_aborts++;
                    return false;
                }
                else { stats_.send_errors++; return false; }
            } else {
                Client& conn = c;
                if (conn.segmented_send()) {
                    stats_.zc_bytes += conn.consume_segments(static_cast<uint32_t>(res),
                        [&](int32_t shard, const char* ptr) { release(shard, ptr); });
                } else {
                    conn.commit_write(static_cast<uint32_t>(res));
                }
                stats_.bytes_sent += static_cast<uint64_t>(res);
                if (tls_signals_) tls_signals_->net_output_bytes += static_cast<uint64_t>(res);
                if (cached_now_s_) c.set_last_interaction_s(*cached_now_s_);
                if (static_cast<uint32_t>(res) < conn.send_requested()) stats_.short_writes++;
                const bool drained = conn.segmented_send()
                    ? !conn.has_pending_segments()
                    : conn.write_drained();
                if (drained) {
                    stats_.sends_completed++;
                    // More replies may have accumulated in the fill buffer while this send was in
                    // flight; resubmit so they go out rather than waiting for an unrelated event.
                    if (conn.has_pending_fill() || conn.has_pending_segments()) resubmit = true;
                } else {
                    // This includes a short write and a full 16-iovec window with more queued
                    // segments. Both resume from the queue's (head index, byte offset) frontier.
                    resubmit = true;
                }
            }
        }
        if constexpr (SubmitFollowup)
            if (resubmit) pump<false, ClassifySend>(c);
        return true;
    }

    template <bool SubmitFollowup = true, bool ClassifySend = false>
    bool on_tls_send_complete(Client& c, TlsConn& tls, int res) {
        c.set_send_inflight(false);
        bool resubmit = false;
        if (res < 0) {
            if (res == -EAGAIN || res == -EINTR) resubmit = true;
            else if (c.closing()) {
                // A rejected handshake or an abrupt peer can make the alert/close_notify flight
                // lose its race with socket teardown.  The connection is already doomed and no
                // application reply is being lost, so drain the memory BIO instead of reporting a
                // false data-path send error or keeping close_client waiting on unsendable bytes.
                const char* pending = nullptr;
                int bytes = 0;
                while ((bytes = tls.peek_output(pending)) > 0)
                    if (!tls.consume_output(static_cast<uint32_t>(bytes))) break;
                return true;
            } else if (res == -ECONNRESET || res == -EPIPE || res == -ECONNABORTED) {
                stats_.peer_aborts++;
                return false;
            } else { stats_.send_errors++; return false; }
        } else if (res == 0) {
            if (c.closing()) {
                const char* pending = nullptr;
                int bytes = 0;
                while ((bytes = tls.peek_output(pending)) > 0)
                    if (!tls.consume_output(static_cast<uint32_t>(bytes))) break;
                return true;
            }
            stats_.send_errors++;
            return false;
        } else {
            const uint32_t cipher_sent = static_cast<uint32_t>(res);
            if (!tls.consume_output(cipher_sent)) {
                stats_.send_errors++;
                return false;
            }
            stats_.bytes_sent += cipher_sent;
            if (tls_signals_) {
                tls_signals_->net_output_bytes += cipher_sent;
                tls_signals_->tls_ciphertext_output_bytes += cipher_sent;
            }
            if (cached_now_s_) c.set_last_interaction_s(*cached_now_s_);
            if (cipher_sent < c.send_requested()) stats_.short_writes++;
            else stats_.sends_completed++;
            resubmit = true;
        }
        if constexpr (SubmitFollowup)
            if (resubmit) pump_tls<false, ClassifySend>(c, tls);
        return !tls.failed();
    }

    // A closed connection cannot send remaining segments. If a sendmsg is still in flight its
    // iovecs remain pinned until the CQE; otherwise every BORROW is returned immediately.
    void teardown(Client& c) {
        oob_defer_.erase(&c);
        if (c.send_inflight()) return;
        c.release_all_segments([&](int32_t shard, const char* ptr) { release(shard, ptr); });
    }

    // Migration must neither splice nor strand a push frame. Combined with Client's empty output
    // segments and no-send predicate, this also proves that no zero-copy value pointer remains in
    // the kernel on behalf of this connection.
    bool migration_ready(const Client& c) const {
        return draining_ != &c && oob_defer_.find(const_cast<Client*>(&c)) == oob_defer_.end();
    }

    // Dead clients still receive their send CQE during the deferred-free window. Consume whatever
    // reached the wire, then release both the completed and unsent BORROW segments exactly once.
    void on_dead_send_complete(Client& c, int res) {
        c.set_send_inflight(false);
        if (res > 0) {
            if (c.segmented_send()) {
                stats_.zc_bytes += c.consume_segments(static_cast<uint32_t>(res),
                    [&](int32_t shard, const char* ptr) { release(shard, ptr); });
            } else {
                c.commit_write(static_cast<uint32_t>(res));
            }
            stats_.bytes_sent += static_cast<uint64_t>(res);
            if (tls_signals_) tls_signals_->net_output_bytes += static_cast<uint64_t>(res);
        }
        teardown(c);
    }

    void on_dead_tls_send_complete(Client& c, TlsConn& tls, int res) {
        c.set_send_inflight(false);
        if (res > 0) {
            const uint32_t cipher_sent = static_cast<uint32_t>(res);
            if (tls.consume_output(cipher_sent)) {
                stats_.bytes_sent += cipher_sent;
                if (tls_signals_) {
                    tls_signals_->net_output_bytes += cipher_sent;
                    tls_signals_->tls_ciphertext_output_bytes += cipher_sent;
                }
            }
        }
        teardown(c);
    }

    struct Stats {
        uint64_t sends_submitted = 0;
        uint64_t sends_completed = 0;
        uint64_t short_writes    = 0;
        uint64_t send_errors     = 0;
        uint64_t peer_aborts     = 0;   // peer closed mid-send; expected under kill/disconnect
        uint64_t serves          = 0;
        uint64_t serves_empty    = 0;   // serve() retired nothing: a wake with no head-ready
        uint64_t bytes_sent      = 0;
        uint64_t retired         = 0;   // ops retired from ROBs by this sender
        uint64_t direct          = 0;   // replies formatted in place by the worker (c->buf trick)
        uint64_t zc_sends        = 0;   // sendmsg submissions whose iovecs include a BORROW
        uint64_t zc_bytes        = 0;   // borrowed value bytes reported complete by the kernel
        uint64_t zc_releases     = 0;   // BORROW segments returned on completion or teardown
    };
    Stats& stats() { return stats_; }
    const Stats& stats() const { return stats_; }
    void note_zc_suppressed_tls() {
        if (tls_signals_) tls_signals_->tls_zc_suppressed++;
    }

private:
    // Empty on every serve of every connection that has no subscription, tracking or monitor:
    // one predicted-true test per serve, as before. The drain has finished staging and published
    // its new flush frontier, so every append below is on a frame boundary.
    bool flush_deferred_oob(Client& conn) {
        if (__builtin_expect(oob_defer_.empty(), true)) return false;
        auto found = oob_defer_.find(&conn);
        if (found == oob_defer_.end()) return false;
        const uint64_t retired_through = conn.rob().flush_id();
        DeferredOobQueue& queue = found->second;
        bool flushed = false;
        while (!queue.empty() && queue.front().after <= retired_through) {
            conn.append_oob(queue.front().bytes.data(), queue.front().bytes.size());
            queue.pop_front();
            flushed = true;
        }
        if (queue.empty()) oob_defer_.erase(found);
        return flushed;
    }

    // `Coded` IS AN ORDINARY TEMPLATE ARGUMENT, and that is the whole point of this shape. The
    // previous version decided it at RUNTIME from a boot-latched bool, which meant serve_impl
    // became a forwarder in front of the real body -- a new function boundary in the io thread's
    // hot path that moved inlining and cost the split cells ~0.9% of real work (measured against a
    // dead-pad placement control: placement alone was -0.11%..-0.30%, the binary was -0.83%..-1.48%).
    //
    // Every caller already knows the mode statically, so nothing had to be threaded:
    //   flush_ready              carries `Fused` as its 3rd template parameter already, and is the
    //                            path BOTH modes take -> passes Fused
    //   wb_serve_natural         split-only: run_loop reaches pipeline_pass under
    //   wb_retire_prepare        constexpr IoPipe = !Fused && Pipeline == 1  -> pass false
    //   genthread_*              fused-only: run_loop reaches run_fused_iofused_loop and
    //   run_fused_streams_loop   run_fused_streams_loop under if constexpr (Fused && ...) -> true
    // With Coded=false every coded block below is deleted by `if constexpr`, so a 2s instantiation
    // is the pre-reply-code function, not a variant of it.
    template <bool TrackOutput, bool TlsNoBorrow, bool kEp, bool Submit, bool ClassifySend,
              bool Coded>
    bool serve_impl(Client& c, bool* submit_allowed = nullptr) {
        TOMO_FORENSIC(c.n_serves.fetch_add(1, std::memory_order_relaxed));
        stats_.serves++;
        Client& conn = c;
        if constexpr (TrackOutput) conn.start_obuf_tracking();
        draining_ = &c;
        const uint32_t retired = c.rob().drain([&](Op& op) {
            if constexpr (TlsNoBorrow) {
                if (op.no_borrow()) note_zc_suppressed_tls();
            }
            // Cross-shard completion publishes descriptors/state, not bytes.  The connection's IO
            // owner turns those into the final ordered reply here, before the generic staging path
            // inspects the (possibly repurposed) zero-copy fields.
            // Plain commands have no sidecar and take exactly the pre-notify zc_ptr branch. Special
            // command state, borrowed values, and armed notification batches all already use this
            // field, so their retirement hook nests behind that existing test.
            if (op.zc_ptr) {
                if (retire_fn_) retire_fn_(retire_ctx_, conn, op);
            }
            if (op.zc_ptr) {
                // Anything already staged is older than this op. Once sealed, every subsequent
                // reply uses segments until the queue drains, so no fill-buffer append can jump a
                // borrowed value that is only partially written.
                conn.seal_fill_segment();
                if constexpr (Coded) if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
                if constexpr (TlsNoBorrow) {
                    conn.append_buf_segment(op.zc_ptr, op.zc_len);
                    release(op.zc_shard, op.zc_ptr);
                    note_zc_suppressed_tls();
                } else {
                    conn.append_borrow_segment(op.zc_ptr, op.zc_len, op.zc_shard);
                }
                conn.append_static_segment(kCrlf, sizeof(kCrlf));
                if (op.direct_len) stats_.direct++;
                return;
            }

            // Direct bytes are already in the fill buffer; publishing the length is the whole
            // "copy". A reply that outgrew the region spilled to op.reply -- emit it AFTER the
            // direct part so the RESP stream stays in order.
            if (conn.has_pending_segments()) {
                if constexpr (Coded) if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
                if (op.direct_len) stats_.direct++;
            } else {
                // Coded reply: render at the fill frontier, one constant-length store by the
                // thread that owns the buffer. The whole block is deleted for Coded=false, and
                // what remains below is the pre-reply-code text, instruction for instruction.
                if constexpr (Coded) {
                    if (op.reply_code_) {
                        stage_coded_reply<TrackOutput>(conn, op);
                        if (!op.reply.empty()) {
                            if constexpr (TrackOutput)
                                conn.append_fill(op.reply.data(), op.reply.size());
                            else conn.fill_buf().append(op.reply.data(), op.reply.size());
                        }
                        return;
                    }
                }
                if (op.direct_len) {
                    if constexpr (TrackOutput) conn.commit_fill(op.direct_len);
                    else conn.fill_buf().commit_raw(op.direct_len);
                    stats_.direct++;
                }
                if (!op.reply.empty()) {
                    if constexpr (TrackOutput) conn.append_fill(op.reply.data(), op.reply.size());
                    else conn.fill_buf().append(op.reply.data(), op.reply.size());
                }
            }
        });
        draining_ = nullptr;
        bool did = retired != 0;
        did |= flush_deferred_oob(conn);
        if constexpr (TrackOutput) {
            if (limit_fn_ && limit_fn_(limit_ctx_, c)) {
                if (submit_allowed) *submit_allowed = false;
                stats_.retired += retired;
                if (!retired) stats_.serves_empty++;
                return true;
            }
        }
        if constexpr (Submit)
            if (!conn.nothing_to_write()) did |= pump<kEp, ClassifySend>(c);
        stats_.retired += retired;
        // A serve that retires nothing: the POLLING paths (flush_ready, the backstop) finding
        // nothing, which is expected and cheap.
        if (!retired) stats_.serves_empty++;
        return did;
    }

    template <bool TrackOutput, bool kEp, bool Submit, bool ClassifySend, bool Coded>
    bool serve_tls_impl(Client& c, TlsConn& tls, bool* submit_allowed = nullptr) {
        TOMO_FORENSIC(c.n_serves.fetch_add(1, std::memory_order_relaxed));
        stats_.serves++;
        Client& conn = c;
        if constexpr (TrackOutput) conn.start_obuf_tracking();
        draining_ = &c;
        const uint32_t retired = c.rob().drain([&](Op& op) {
            if (op.no_borrow()) note_zc_suppressed_tls();
            if (op.zc_ptr && retire_fn_) retire_fn_(retire_ctx_, conn, op);
            if (op.zc_ptr) {
                conn.seal_fill_segment();
                if constexpr (Coded) if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
                conn.append_buf_segment(op.zc_ptr, op.zc_len);
                conn.append_static_segment(kCrlf, sizeof(kCrlf));
                release(op.zc_shard, op.zc_ptr);
                note_zc_suppressed_tls();
                if (op.direct_len) stats_.direct++;
                return;
            }
            if (conn.has_pending_segments()) {
                if constexpr (Coded) if (op.reply_code_) op_materialise_code(op);
                conn.append_buf_segment(op.direct, op.direct_len,
                                        op.reply.data(), op.reply.size());
                if (op.direct_len) stats_.direct++;
            } else {
                // Coded reply: render at the fill frontier, one constant-length store by the
                // thread that owns the buffer. The whole block is deleted for Coded=false, and
                // what remains below is the pre-reply-code text, instruction for instruction.
                if constexpr (Coded) {
                    if (op.reply_code_) {
                        stage_coded_reply<TrackOutput>(conn, op);
                        if (!op.reply.empty()) {
                            if constexpr (TrackOutput)
                                conn.append_fill(op.reply.data(), op.reply.size());
                            else conn.fill_buf().append(op.reply.data(), op.reply.size());
                        }
                        return;
                    }
                }
                if (op.direct_len) {
                    if constexpr (TrackOutput) conn.commit_fill(op.direct_len);
                    else conn.fill_buf().commit_raw(op.direct_len);
                    stats_.direct++;
                }
                if (!op.reply.empty()) {
                    if constexpr (TrackOutput) conn.append_fill(op.reply.data(), op.reply.size());
                    else conn.fill_buf().append(op.reply.data(), op.reply.size());
                }
            }
        });
        draining_ = nullptr;
        bool did = retired != 0;
        did |= flush_deferred_oob(conn);
        if constexpr (TrackOutput) {
            if (limit_fn_ && limit_fn_(limit_ctx_, c)) {
                if (submit_allowed) *submit_allowed = false;
                stats_.retired += retired;
                if (!retired) stats_.serves_empty++;
                return true;
            }
        }
        if constexpr (Submit)
            if (!conn.nothing_to_write() || tls.output_pending())
                did |= pump_tls<kEp, ClassifySend>(c, tls);
        stats_.retired += retired;
        if (!retired) stats_.serves_empty++;
        return did;
    }
    template <bool kEp, bool ClassifySend = false>
    bool submit_legacy(Client& c, size_t total, size_t sent) {
      if constexpr (kEp) { bool did = false; (void)write_legacy_epoll(c, total, sent, did); return did; }
      else {
        const size_t request = std::min<size_t>(total - sent, kMaxSendBytes);
        io_uring_sqe* s = ring_->sqe();
        if (!s) return false;
        io_uring_prep_send(s, c.fd(), c.send_buf().data() + sent, request, MSG_NOSIGNAL);
        s->user_data = ur_tag(UrKind::Send, &c);
        ring_->note_send_pending<ClassifySend>();

        c.set_segmented_send(false);
        c.set_send_requested(static_cast<uint32_t>(request));
        c.set_send_inflight(true);
        stats_.sends_submitted++;
        return true;
      }
    }

    // One legacy-buffer write. `did` is only raised when bytes actually moved; the bool return says
    // "keep going" so the caller's loop can distinguish a short write (retry) from a stop.
    bool write_legacy_epoll(Client& c, size_t total, size_t sent, bool& did) {
        const size_t request = std::min<size_t>(total - sent, kMaxSendBytes);
        c.set_segmented_send(false);
        c.set_send_requested(static_cast<uint32_t>(request));
        stats_.sends_submitted++;
        const ssize_t n = ::send(c.fd(), c.send_buf().data() + sent, request,
                                 MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n <= 0) { note_send_stop(c, n); return false; }
        c.commit_write(static_cast<uint32_t>(n));
        stats_.bytes_sent += static_cast<uint64_t>(n);
        if (static_cast<size_t>(n) < request) stats_.short_writes++;
        else if (c.write_drained()) stats_.sends_completed++;
        if (cached_now_s_) c.set_last_interaction_s(*cached_now_s_);
        did = true;
        return true;
    }

    // Classify a non-positive synchronous send. EAGAIN and EINTR are ordinary flow control and must
    // NOT be counted as data-path errors; the peer-abort family is the same carve-out the CQE path
    // makes, so err=0 does not become a timing lottery under connection churn. Anything else is
    // fatal for this connection and latches send_failed_.
    void note_send_stop(Client& c, ssize_t n) {
        if (n == 0) return;                              // wrote nothing; treat as would-block
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) return;
        if (err == ECONNRESET || err == EPIPE || err == ECONNABORTED || c.closing())
            stats_.peer_aborts++;
        else
            stats_.send_errors++;
        send_failed_ = true;
    }

    template <bool kEp, bool ClassifySend = false>
    bool submit_tls_cipher(Client& c, TlsConn& tls, const char* cipher, uint32_t bytes) {
      if constexpr (kEp) {
        // Drain every record OpenSSL has already produced in one call. Recursing back through
        // pump_tls would re-enter the plaintext half and could hand SSL_write a moved frontier.
        bool did = false;
        const char* pending = cipher;
        uint32_t remaining = bytes;
        while (remaining) {
            c.set_send_requested(remaining);
            stats_.sends_submitted++;
            const ssize_t n = ::send(c.fd(), pending, remaining, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n <= 0) { note_send_stop(c, n); return did; }
            const uint32_t sent = static_cast<uint32_t>(n);
            if (!tls.consume_output(sent)) { send_failed_ = true; return did; }
            stats_.bytes_sent += sent;
            if (tls_signals_) tls_signals_->tls_ciphertext_output_bytes += sent;
            if (cached_now_s_) c.set_last_interaction_s(*cached_now_s_);
            did = true;
            if (sent < remaining) stats_.short_writes++;
            else stats_.sends_completed++;
            // consume_output moved the BIO frontier, so re-peek rather than advancing `pending`
            // ourselves: the next record may live in a different block of the memory BIO.
            pending = nullptr;
            const int ready = tls.peek_output(pending);
            remaining = ready > 0 ? static_cast<uint32_t>(ready) : 0;
        }
        return did;
      } else {
        io_uring_sqe* s = ring_->sqe();
        if (!s) return false;
        io_uring_prep_send(s, c.fd(), cipher, bytes, MSG_NOSIGNAL);
        s->user_data = ur_tag(UrKind::TlsSend, &c);
        ring_->note_send_pending<ClassifySend>();
        c.set_send_requested(bytes);
        c.set_send_inflight(true);
        stats_.sends_submitted++;
        return true;
      }
    }

    void release(int32_t shard, const char* ptr) {
        stats_.zc_releases++;
        if (release_fn_) release_fn_(release_ctx_, shard, ptr);
    }

    inline static constexpr char kCrlf[2] = {'\r', '\n'};
    Ring*  ring_ = nullptr;
    void*  release_ctx_ = nullptr;
    ReleaseFn release_fn_ = nullptr;
    void*  retire_ctx_ = nullptr;
    RetireFn retire_fn_ = nullptr;
    const std::atomic<bool>* limit_armed_ = nullptr;
    void*  limit_ctx_ = nullptr;
    LimitFn limit_fn_ = nullptr;
    const uint32_t* cached_now_s_ = nullptr;
    LoopSignals* tls_signals_ = nullptr;
    // Engine, for the cold non-templated entry points only. The hot send path never reads it.
    bool   epoll_ = false;
    bool   send_failed_ = false;
    bool   classify_cold_sends_ = false;
    // The connection whose retire drain is running right now, or null. defer_oob() uses it to make
    // parking unconditional across the partial-reply staging window.
    const Client* draining_ = nullptr;
    std::unordered_map<Client*, DeferredOobQueue> oob_defer_;
    Stats  stats_;
};

}  // namespace tomo
