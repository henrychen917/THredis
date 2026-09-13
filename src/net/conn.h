// conn.h — Client: one connection, one struct, one owner.
//
// UNIFIED (owner order, 2026-08-24, after the pure-2s ruling). The ConnIn/ConnOut split — with an
// alignas(64) firewall between the halves — existed so the parsing thread (ifid) and a remote
// sending thread (wb/exwb) could not false-share. In pure 2s ONE io thread owns both halves for
// the connection's whole life, so the split bought padding and a pointer hop for a hazard that no
// longer exists. The layout rule inverts: pack the io thread's recv+send scalars TIGHT (they are
// touched together every pass), and give the only genuinely cross-thread fields — the two atomics
// the EXECUTOR signals through — their own line at the tail.
//
// What ex touches, and nothing else: ROB slots (Op state/reply/direct region — Rob manages its own
// cross-thread layout), the bytes of rbuf via argv Slices (heap data, not this struct), the direct-
// reply region inside buf_[] (data bytes, published by the op's Done), and the two atomics at the
// tail (retire_queued_, wb_slot_). Every scalar above them is single-writer io state.
//
// ============================================================================================
// THE READ BUFFER MUST NEVER MOVE LIVE BYTES. This is the subtle one.
//
// argv Slices point INTO the read buffer — that is what makes parsing zero-copy. Compaction, the
// obvious way to reclaim space, memmoves the live region to offset 0. Doing that while ops are in
// flight silently invalidates every Slice a worker is about to read, on another thread. It would
// produce corrupted keys and wrong replies, intermittently, under pipelining only — close to the
// worst bug shape there is.
//
// So the buffer is APPEND-ONLY while anything is in flight, and is reset only at a quiescence
// point, where by definition no Slice into it survives. Growth is bounded by refusing to read more
// when the buffer is large and not draining; the ROB window already caps in-flight ops, so this
// converges rather than deadlocks.
//
// The alternative — copying argv into each Op — costs a memcpy per argument on the hot path, which
// is exactly the allocation/copy volume the design is trying to avoid.
// ============================================================================================
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <limits>
#include <utility>
#include <sys/socket.h>
#include <sys/uio.h>
#include "rob.h"
#include "../base/slice.h"

#ifdef TOMO_WEDGE_FORENSICS
#define TOMO_FORENSIC(x) do { x; } while (0)
#else
#define TOMO_FORENSIC(x) do { } while (0)
#endif

namespace tomo {

class Client;
struct MultiSession;
void multi_session_destroy(MultiSession* session);

inline constexpr uint32_t kRobWindow    = 64;          // max in-flight ops per connection
inline constexpr size_t   kRbufInitial  = 16 * 1024;
inline constexpr size_t   kRbufSoftCap  = 1 * 1024 * 1024;  // stop BUFFERING BACKLOG past this
// The parser accepts redis-compatible bulks (512MB). A single command must therefore be allowed to
// exceed the soft cap, or a 2MB SET stalls its connection forever: the parser reports Incomplete,
// read_space refuses to grow, and neither side can ever make progress -- a silent wedge with no
// error, found by the perfected-checkpoint audit. The soft cap bounds BACKLOG (many buffered
// commands); one oversized in-flight command may grow to the protocol bound. Memory tracks bytes
// actually received, and reset_rbuf_at_quiescence sheds the growth after the command completes.
inline constexpr size_t   kRbufFrameSlack = 64 * 1024;
inline constexpr size_t   kRbufHardCap  = 512ull * 1024 * 1024 + kRbufFrameSlack;
// Item 4: 512B inline, heap beyond. Two 16KB inline buffers made every connection carry 32KB of
// worst-case staging whether it ever pipelined or not; SmallBuf grows on demand and clear() keeps
// the allocation, so a busy connection pays ONE grow to its working size and idles at 1KB + that.
inline constexpr size_t   kWbufInline   = 512;
inline constexpr size_t   kWbufShed     = 64 * 1024;   // reply staging above this is a burst; shed it

// Item 6: connection-lived execution-side state -- the third lifetime. Session-mutating commands
// are ConnLocal and run on the io thread, single-threaded per connection; handlers never see it,
// they see the snapshot the parser stamps into each op.
struct Session {
    uint32_t db_index = 0;
};

// WHO IS HOLDING THE PARSE BARRIER. Six independent mechanisms park a connection's parse pass, and
// they used to share ONE bool -- so any one of them could clear a barrier another one still needed.
// No reachable interleaving overlapped two owners (see NOTES-BARRIER.md section 2: a blocking op is
// provably alone in its ROB, and every other owner ends the parse pass on the spot), which is
// exactly why the bool survived: the hazard is one relaxed guard away, not present. Owner bits make
// the release symmetric with the acquire -- whoever set it is the one whose release can drop it --
// and cost nothing: the byte was already there, and "is any owner holding" is still one byte test.
//
// Adding an owner? Add a bit here and acquire it at the site that parks the connection. Do NOT
// reuse another owner's bit to save one; that is the shared flag this enum exists to retire.
enum class BarrierOwner : uint8_t {
    Scatter  = 1u << 0,  // two-hop store / all-shards fan-out; io_loop.h nonblocking_dispatch
    Blocking = 1u << 1,  // BLPOP-family, and the move scatter blocking_resume_move converts it into
    Wait     = 1u << 2,  // deferred WAIT parked on its own deadline
    Exec     = 1u << 3,  // EXEC fan-out; multi.inc
    PubSub   = 1u << 4,  // (P|S)(UN)SUBSCRIBE transition awaiting its channel homes
    Climon   = 1u << 5,  // CLIENT UNBLOCK (remote owner), CLIENT LIST / CLIENT KILL fan-out
    // 1u << 6 is the one spare production bit. Client is footprint-locked, so a SEVENTH owner
    // takes it; an eighth needs a real layout decision, not a wider field.
    //
    // Test-only, and deliberately NOT released by the quiescence backstop -- it exists to hold the
    // barrier PAST ROB quiescence, which is the state no production sequence can currently produce
    // and the one the owner-scoped release must survive. Armed only by DEBUG BARRIER-HOLD.
    Debug    = 1u << 7,
};

enum class SegmentKind : uint8_t { Buf = 0, Borrow = 1, Static = 2 };

struct ReplySegment {
    SegmentKind kind  = SegmentKind::Static;
    const char* ptr   = nullptr;
    uint32_t    len   = 0;
    int32_t     shard = -1;
};

// Metadata stays inline for the common [header, value, CRLF] case. BUF payloads own independent
// blocks because queue growth and continued retirement must never move bytes named by an in-flight
// sendmsg. BORROW and STATIC payloads are non-owning under their respective lifetime protocols.
template <uint32_t Inline>
class SegmentQueue {
public:
    SegmentQueue() = default;
    ~SegmentQueue() { clear_without_releases(); if (segs_ != inline_) std::free(segs_); }
    SegmentQueue(const SegmentQueue&) = delete;
    SegmentQueue& operator=(const SegmentQueue&) = delete;

    bool empty() const { return size_ == 0; }
    uint32_t size() const { return size_; }
    uint64_t byte_size() const {
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < size_; i++) {
            const ReplySegment& segment = segs_[head_ + i];
            bytes += segment.len - (i == 0 ? offset_ : 0);
        }
        return bytes;
    }

    void append_buf(const char* ptr, size_t len) {
        while (len) {
            const size_t take = std::min(len, static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
            char* copy = static_cast<char*>(std::malloc(take));
            std::memcpy(copy, ptr, take);
            push(ReplySegment{SegmentKind::Buf, copy, static_cast<uint32_t>(take), -1});
            ptr += take;
            len -= take;
        }
    }

    void append_buf(const char* a, size_t an, const char* b, size_t bn) {
        const size_t total = an + bn;
        if (!total) return;
        char* copy = static_cast<char*>(std::malloc(total));
        if (an) std::memcpy(copy, a, an);
        if (bn) std::memcpy(copy + an, b, bn);
        push(ReplySegment{SegmentKind::Buf, copy, static_cast<uint32_t>(total), -1});
    }

    void append_borrow(const char* ptr, uint32_t len, int32_t shard) {
        push(ReplySegment{SegmentKind::Borrow, ptr, len, shard});
    }
    void append_static(const char* ptr, uint32_t len) {
        push(ReplySegment{SegmentKind::Static, ptr, len, -1});
    }

    uint32_t build_iov(iovec* out, uint32_t cap, uint32_t byte_cap,
                       bool& has_borrow, uint32_t& bytes) const {
        uint32_t n = 0;
        bytes = 0;
        has_borrow = false;
        for (uint32_t i = 0; i < size_ && n < cap && bytes < byte_cap; i++) {
            const ReplySegment& s = segs_[head_ + i];
            const uint32_t off = i == 0 ? offset_ : 0;
            if (off >= s.len) continue;
            const uint32_t len = std::min(s.len - off, byte_cap - bytes);
            out[n].iov_base = const_cast<char*>(s.ptr + off);
            out[n].iov_len = len;
            has_borrow |= s.kind == SegmentKind::Borrow;
            bytes += len;
            n++;
        }
        return n;
    }

    template <typename ReleaseFn>
    uint64_t consume(uint32_t bytes, ReleaseFn&& release) {
        uint64_t borrowed = 0;
        while (bytes && size_) {
            ReplySegment& s = segs_[head_];
            const uint32_t avail = s.len - offset_;
            const uint32_t take = std::min(bytes, avail);
            if (s.kind == SegmentKind::Borrow) borrowed += take;
            offset_ += take;
            bytes -= take;
            if (offset_ != s.len) break;
            if (s.kind == SegmentKind::Borrow) release(s.shard, s.ptr);
            pop_front();
        }
        return borrowed;
    }

    template <typename ReleaseFn>
    void release_all(ReleaseFn&& release) {
        for (uint32_t i = 0; i < size_; i++) {
            ReplySegment& s = segs_[head_ + i];
            if (s.kind == SegmentKind::Borrow) release(s.shard, s.ptr);
            else if (s.kind == SegmentKind::Buf) std::free(const_cast<char*>(s.ptr));
        }
        head_ = size_ = offset_ = 0;
    }

private:
    void push(const ReplySegment& s) {
        if (head_ + size_ == cap_) make_tail_room();
        segs_[head_ + size_++] = s;
    }

    void make_tail_room() {
        if (head_) {
            std::memmove(segs_, segs_ + head_, size_ * sizeof(ReplySegment));
            head_ = 0;
            return;
        }
        const uint32_t ncap = cap_ * 2;
        auto* next = static_cast<ReplySegment*>(std::malloc(ncap * sizeof(ReplySegment)));
        std::memcpy(next, segs_, size_ * sizeof(ReplySegment));
        if (segs_ != inline_) std::free(segs_);
        segs_ = next;
        cap_ = ncap;
    }

    void pop_front() {
        ReplySegment& s = segs_[head_];
        if (s.kind == SegmentKind::Buf) std::free(const_cast<char*>(s.ptr));
        head_++;
        size_--;
        offset_ = 0;
        if (!size_) head_ = 0;
    }

    void clear_without_releases() {
        for (uint32_t i = 0; i < size_; i++) {
            ReplySegment& s = segs_[head_ + i];
            if (s.kind == SegmentKind::Buf) std::free(const_cast<char*>(s.ptr));
        }
        head_ = size_ = offset_ = 0;
    }

public:
    uint64_t pending_bytes() const {
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < size_; i++) bytes += segs_[head_ + i].len;
        return bytes - offset_;
    }

private:
    ReplySegment  inline_[Inline];
    ReplySegment* segs_ = inline_;
    uint32_t      head_ = 0;       // segment index at the wire frontier
    uint32_t      size_ = 0;
    uint32_t      cap_ = Inline;
    uint32_t      offset_ = 0;     // byte offset within head_; only the head may be partial
};

class Client {
public:
    explicit Client(int fd) : fd_(fd) {
        rbuf_ = static_cast<char*>(std::malloc(kRbufInitial));
        rcap_ = kRbufInitial;
    }
    ~Client() { multi_session_destroy(multi_session_); std::free(rbuf_); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    int  fd() const { return fd_; }

    // ---- read side -----------------------------------------------------------------------------
    char*    rbuf()      { return rbuf_; }
    uint32_t rlen() const { return rlen_; }
    uint32_t rpos() const { return rpos_; }
    size_t rcap() const { return rcap_; }
    void     advance_parse(uint32_t n) { rpos_ += n; }

    // Space to recv() into. Returns nullptr to mean "do not read right now".
    //
    // GROWING IS ONLY LEGAL WHEN NOTHING IS IN FLIGHT, and that is the whole subtlety. realloc MOVES
    // the buffer, and every in-flight op's argv Slices point into it — so a grow while ops are
    // outstanding leaves workers reading freed memory. It presents as a key that hashed correctly at
    // parse time but reads as zeros in the handler, which looks like a store bug and is not one.
    // (Guarding only the memmove in reset_rbuf_at_quiescence is NOT enough; realloc moves it too.)
    //
    // So when we cannot grow, we return the free tail if it is big enough to be worth a recv, and
    // otherwise nothing at all — the ROB drains, quiescence arrives, the buffer resets, and reading
    // resumes. That is backpressure, not a stall.
    static constexpr size_t kMinRecv = 2048;

    char* read_space(size_t want, size_t& out_avail, bool may_grow,
                     uint64_t proto_max_bulk_len = 512ull * 1024 * 1024) {
        size_t avail = rcap_ - rlen_;
        // Past the soft cap, growth continues ONLY while the entire buffer is one incomplete
        // command (rpos_ == 0 after the quiescence reset: nothing parsed, nothing in flight --
        // which is also what makes may_grow true). Backlog never grows past the soft cap.
        const size_t hard_cap = static_cast<size_t>(proto_max_bulk_len) + kRbufFrameSlack;
        const size_t cap = (rpos_ == 0) ? hard_cap : kRbufSoftCap;
        if (avail < want && may_grow && rcap_ < cap) {
            size_t ncap = rcap_ * 2;
            while (ncap < rlen_ + want && ncap < cap) ncap *= 2;
            if (ncap > cap) ncap = cap;
            char* n = static_cast<char*>(std::realloc(rbuf_, ncap));
            if (n) { rbuf_ = n; rcap_ = ncap; avail = rcap_ - rlen_; }
        }
        if (avail < kMinRecv) { out_avail = 0; return nullptr; }
        out_avail = avail;
        return rbuf_ + rlen_;
    }
    void commit_read(size_t n) { rlen_ += static_cast<uint32_t>(n); }
    uint32_t last_interaction_s() const { return last_interaction_s_; }
    void set_last_interaction_s(uint32_t value) { last_interaction_s_ = value; }

    // Exactly one recv may be outstanding, and while it is, the kernel holds a RAW POINTER into
    // this buffer. Nothing may move or reallocate it until that recv completes — see
    // reset_rbuf_at_quiescence().
    bool recv_armed() const { return recv_armed_; }
    void set_recv_armed(bool v) { recv_armed_ = v; }

    // Safe ONLY when the ROB is quiescent AND no recv is in flight. Both conditions matter and they
    // are different: quiescence means no Slice into this buffer is still referenced by an op, while
    // !recv_armed means the KERNEL is not holding a pointer into it. Violating the second corrupts
    // the heap — the recv lands at a stale offset, or into freed memory if the realloc below runs.
    // Preserves any partially received command at the tail, which has no Slices pointing at it yet.
    void reset_rbuf_at_quiescence() {
        const uint32_t rest = rlen_ - rpos_;
        if (rest && rpos_) std::memmove(rbuf_, rbuf_ + rpos_, rest);
        rlen_ = rest;
        rpos_ = 0;
        // Also shed burst-grown WRITE buffers here. The guard is size()==0 itself: a buffer with
        // a send in flight (or staged bytes) always has size > 0 and is skipped, so shrinking can
        // never truncate bytes the kernel or the sender still references -- the same invariant
        // that makes the rbuf shed below safe, enforced per buffer instead of by caller contract.
        for (auto& b : buf_)
            if (b.size() == 0 && b.cap() > kWbufShed) b.shrink_to_inline();
        // Shed an oversized command's growth -- but ONLY once the buffer is EMPTY. Shrinking with
        // rest > 0 truncates the allocation underneath rlen_, and the next recv lands past the end
        // of a 16KB block. That exact bug shipped for ~20 minutes: the hard-cap growth fix armed
        // this previously-unreachable branch, and a 2MB SET died with EFAULT mid-stream.
        if (rlen_ == 0 && rcap_ > kRbufSoftCap) {
            char* n = static_cast<char*>(std::realloc(rbuf_, kRbufInitial));
            if (n) { rbuf_ = n; rcap_ = kRbufInitial; }
        }
    }

    // ---- the ROB -------------------------------------------------------------------------------
    Rob<kRobWindow>& rob() { return rob_; }
    const Rob<kRobWindow>& rob() const { return rob_; }

    // ---- write side: DOUBLE BUFFERED, and that is a correctness requirement ------------------
    //
    // A send in flight hands the kernel a raw pointer into the write buffer. Appending a reply to
    // that same buffer can grow it, and growing means malloc + memcpy + FREE OF THE OLD BLOCK — the
    // block the kernel is still reading from. It presents as occasional corrupted or missing replies
    // under pipelining only, because it needs a reply to land while a send is outstanding. Exactly
    // the same shape as the recv-buffer realloc bug, on the other side of the connection.
    //
    // So: replies always append to the FILL buffer, sends always read the SEND buffer, and the two
    // swap only at a point where no send is outstanding. Neither buffer is ever touched by both
    // sides at once, which makes the bug impossible rather than unlikely.
    SmallBuf<kWbufInline>& fill_buf() { return buf_[fill_]; }
    SmallBuf<kWbufInline>& send_buf() { return buf_[fill_ ^ 1]; }
    void append_fill(const char* ptr, size_t len) {
        fill_buf().append(ptr, len);
        if (obuf_tracking_) obuf_bytes_ += len;
    }
    void commit_fill(size_t len) {
        fill_buf().commit_raw(len);
        if (obuf_tracking_) obuf_bytes_ += len;
    }

    bool     has_pending_fill() const { return buf_[fill_].size() > 0; }
    uint32_t wsent() const { return wsent_; }
    void     commit_write(uint32_t n) {
        wsent_ += n;
        if (obuf_tracking_) {
            if (n > obuf_bytes_) std::abort();
            obuf_bytes_ -= n;
        }
    }
    bool     write_drained() const { return wsent_ >= buf_[fill_ ^ 1].size(); }

    // Promote the fill buffer to be the send buffer. Only legal when no send is outstanding, which
    // is the caller's invariant (send_inflight()).
    void swap_buffers() {
        buf_[fill_ ^ 1].clear();      // old send buffer is fully written; recycle it as the next fill
        fill_ ^= 1;
        wsent_ = 0;
    }
    bool nothing_to_write() const {
        return buf_[fill_].size() == 0 && write_drained() && segments_.empty();
    }
    uint64_t buffered_output_bytes() const {
        const size_t send_size = buf_[fill_ ^ 1].size();
        const uint64_t send_remaining = send_size > wsent_ ? send_size - wsent_ : 0;
        return buf_[fill_].size() + send_remaining + segments_.byte_size();
    }
    uint32_t output_list_length() const { return segments_.size(); }

    // Seal ordinary staged bytes before the first borrowed reply. Clearing the fill buffer after
    // copying leaves the existing send buffer untouched, so an older send already in flight stays
    // ahead of every new segment.
    void seal_fill_segment() {
        SmallBuf<kWbufInline>& b = fill_buf();
        if (!b.empty()) { segments_.append_buf(b.data(), b.size()); b.clear(); }
    }
    void append_buf_segment(const char* ptr, size_t len) {
        segments_.append_buf(ptr, len);
        if (obuf_tracking_) obuf_bytes_ += len;
    }
    void append_buf_segment(const char* a, size_t an, const char* b, size_t bn) {
        segments_.append_buf(a, an, b, bn);
        if (obuf_tracking_) obuf_bytes_ += an + bn;
    }
    void append_borrow_segment(const char* ptr, uint32_t len, int32_t shard) {
        segments_.append_borrow(ptr, len, shard);
        if (obuf_tracking_) obuf_bytes_ += len;
    }
    void append_static_segment(const char* ptr, uint32_t len) {
        segments_.append_static(ptr, len);
        if (obuf_tracking_) obuf_bytes_ += len;
    }
    bool has_pending_segments() const { return !segments_.empty(); }

    // ---- OUT-OF-BAND FRAMES ---------------------------------------------------------------
    // Pub/sub deliveries, tracking invalidations and MONITOR feed lines are WHOLE frames produced
    // by something other than this connection's op stream. Two rules govern where they may land,
    // and both were learned the hard way:
    //
    // 1. NEVER INSIDE ANOTHER REPLY'S BYTE RANGE. The predecessor of this routine parked the frame
    //    on the newest live op -- `rob.at(rob.dispatch_id()-1).reply.append(frame)`. `op.reply` is
    //    the frame's TAIL for a copying reply, which is what that trick was designed against, but
    //    it is the frame's HEAD for a BORROWING one: serve emits [direct+reply][borrow][CRLF], and
    //    a borrowed GET puts only `$<len>\r\n` in the sink. So a push landed between a bulk header
    //    and its body and desynchronised the connection by exactly the push length -- silently, on
    //    plaintext and TLS alike. It also raced a worker appending to that same SmallBuf, where
    //    grow() frees the block the other side is writing into.
    //
    // 2. NEVER OVER A LIVE DIRECT-REPLY REGION. `Op::direct` is a raw pointer into fill_buf()'s
    //    SPARE CAPACITY, handed out only when the ROB is idle and the fill buffer is empty. An
    //    append_fill while it is live overwrites the bytes a worker is formatting, can grow (free)
    //    the block underneath it, and leaves retire's commit_fill publishing the wrong offset.
    //
    // Both are satisfied by one test at the point the send engine releases a frame: while younger
    // ops are still in flight, route through the SEGMENT queue, which owns its own block and never
    // touches fill_buf. seal_fill_segment() first, because pump drains the whole segment queue
    // strictly BEFORE it promotes the fill buffer -- so older staged bytes must move into the
    // queue's tail or the frame would jump ahead of them. A quiesced connection with an empty queue
    // (the idle subscriber, which is the hot pub/sub case) keeps the plain fill append unchanged.
    //
    // The op stream and this channel then interleave only at frame boundaries: retire stages each
    // op's bytes in one uninterrupted run, and a frame appended here sits entirely before the next
    // one. WbEngine::defer_oob parks a newly produced frame until every earlier-issued reply has
    // been staged; a parked blocking command is the deliberate latency-bounded exception.
    // Returns TRUE when this append had to take the segment channel. The callers count only direct
    // production-time appends here; frames parked by WbEngine have their own disjoint counter.
    bool append_oob(const char* a, size_t an, const char* b = nullptr, size_t bn = 0) {
        if (segments_.empty() && rob_.quiesced()) {
            append_fill(a, an);
            if (bn) append_fill(b, bn);
            return false;
        }
        seal_fill_segment();
        append_buf_segment(a, an, b, bn);
        return true;
    }
    uint32_t build_segment_iov(bool& has_borrow, uint32_t& bytes) {
        const uint32_t n = segments_.build_iov(send_iov_, kMaxSendIov, kMaxSendBytes,
                                               has_borrow, bytes);
        std::memset(&send_msg_, 0, sizeof(send_msg_));
        send_msg_.msg_iov = send_iov_;
        send_msg_.msg_iovlen = n;
        return n;
    }
    msghdr* send_msg() { return &send_msg_; }
    template <typename ReleaseFn>
    uint64_t consume_segments(uint32_t bytes, ReleaseFn&& release) {
        const uint64_t borrowed = segments_.consume(bytes, std::forward<ReleaseFn>(release));
        if (obuf_tracking_) {
            if (bytes > obuf_bytes_) std::abort();
            obuf_bytes_ -= bytes;
        }
        return borrowed;
    }
    template <typename ReleaseFn>
    void release_all_segments(ReleaseFn&& release) {
        segments_.release_all(std::forward<ReleaseFn>(release));
        obuf_bytes_ = 0;
    }

    uint64_t obuf_bytes() const { return obuf_bytes_; }
    void start_obuf_tracking() {
        if (obuf_tracking_) return;
        obuf_bytes_ = fill_buf().size() + (send_buf().size() - wsent_) +
                      segments_.pending_bytes();
        obuf_tracking_ = true;
    }
    void stop_obuf_tracking() {
        if (!obuf_tracking_) return;
        obuf_tracking_ = false;
        obuf_bytes_ = 0;
        obuf_soft_since_s_ = 0;
    }
    uint32_t obuf_soft_since_s() const { return obuf_soft_since_s_; }
    void set_obuf_soft_since_s(uint32_t value) { obuf_soft_since_s_ = value; }

    // Exactly ONE send may be outstanding per connection. Two concurrent sends on one socket can
    // complete out of order and interleave bytes, which corrupts the stream in a way that looks
    // like a protocol bug anywhere but here.
    bool send_inflight() const { return send_inflight_; }
    void set_send_inflight(bool v) { send_inflight_ = v; }
    bool segmented_send() const { return segmented_send_; }
    void set_segmented_send(bool v) { segmented_send_ = v; }
    uint32_t send_requested() const { return send_requested_; }
    void set_send_requested(uint32_t n) { send_requested_ = n; }

    // ---- io-thread bookkeeping -----------------------------------------------------------------
    uint64_t id() const { return id_; }
    void set_id(uint64_t v) { id_ = v; }
    uint32_t ifid_thread() const { return ifid_thread_; }
    void set_ifid_thread(uint32_t t) { ifid_thread_ = t; }
    Session& session() { return session_; }
    const Session& session() const { return session_; }

    // Torn down but not yet freed: still legal to READ while outstanding CQEs retire (it sits on
    // the io thread's deferred-free list), but no longer part of any working set.
    bool dead() const { return dead_; }
    void mark_dead() { dead_ = true; }
    bool closing() const { return closing_; }
    void mark_closing() { closing_ = true; }

    // Membership in its io thread's active set, as a FLAG rather than a search; and whether this
    // conn's ready bit fired since io last served it. Plain bools — one thread.
    bool serve_pending() const { return serve_pending_; }
    void set_serve_pending(bool v) { serve_pending_ = v; }
    // An in-flight all-shards scatter (FLUSHALL/FLUSHDB/CONFIG SET fan-out) is a parse barrier:
    // commands behind it on this conn must observe post-scatter state (ConnLocal readers like
    // DBSIZE/INFO execute at parse time, so without the barrier they snapshot early and a
    // pipelined FLUSHALL;DBSIZE answers the PRE-flush count). Set only on successful publish;
    // cleared at ROB quiescence — the scatter is necessarily the newest op, since nothing parses
    // behind it. Same-client read-your-own-writes hazard: a sanctioned stall.
    //
    // The gate the parse loop asks is unchanged and stays one byte test against zero: the owner
    // set is a MASK in the byte the bool already occupied (see BarrierOwner), not a new field.
    bool scatter_barrier() const { return barrier_owners_ != 0; }
    bool barrier_held_by(BarrierOwner who) const {
        return (barrier_owners_ & static_cast<uint8_t>(who)) != 0;
    }
    void barrier_acquire(BarrierOwner who) {
        barrier_owners_ = static_cast<uint8_t>(barrier_owners_ | static_cast<uint8_t>(who));
    }
    // Drops one owner's claim. Returns TRUE when this release was the last one — i.e. when the
    // connection is actually released. A false return is the event the old unconditional clear got
    // wrong; its callers count it, which is what makes a validation run able to fail.
    bool barrier_release(BarrierOwner who) {
        barrier_owners_ = static_cast<uint8_t>(barrier_owners_ & ~static_cast<uint8_t>(who));
        return barrier_owners_ == 0;
    }
    // The backstop, at ROB quiescence: with nothing in flight every PRODUCTION owner has completed
    // by definition, so quiescence releases them all in one store. Only the injected test owner is
    // exempt, and only because holding past quiescence is the whole point of it.
    void barrier_release_quiesced() {
        barrier_owners_ =
            static_cast<uint8_t>(barrier_owners_ & static_cast<uint8_t>(BarrierOwner::Debug));
    }
    // Resource backpressure is deliberately distinct from the semantic scatter barrier. It keeps
    // this connection's unconsumed frame parked only while the global memory valve is full; as
    // soon as any group retires, parsing may resume without waiting for this connection's ROB.
    bool atomic_backpressure() const { return atomic_backpressure_; }
    void set_atomic_backpressure(bool v) { atomic_backpressure_ = v; }
    bool subscriber_mode() const { return subscriber_mode_; }
    void set_subscriber_mode(bool v) { subscriber_mode_ = v; }
    bool blocked() const { return connection_flags_ & kBlocked; }
    void set_blocked(bool value) {
        if (value) connection_flags_ |= kBlocked;
        else connection_flags_ &= static_cast<uint8_t>(~kBlocked);
    }
    bool resp3() const { return connection_flags_ & kResp3; }
    void set_resp3(bool value) {
        if (value) connection_flags_ |= kResp3;
        else connection_flags_ &= static_cast<uint8_t>(~kResp3);
    }
    // CLIENT NO-TOUCH. Bit 4 was free in this byte AND is ignored by Op::route_flags_, so the
    // connection's answer reaches every operation through the store op_route_flags() already
    // makes -- no new field, no new load, no growth of the footprint-locked Client.
    bool no_touch() const { return connection_flags_ & kNoTouch; }
    void set_no_touch(bool value) {
        if (value) connection_flags_ |= kNoTouch;
        else connection_flags_ &= static_cast<uint8_t>(~kNoTouch);
    }
    // Bit 2 deliberately matches Op::route_flags_'s Resp3 assignment. Passing the byte through
    // Op::reset folds protocol capture into the ROB's existing flags store: RESP2 pays one load,
    // no mask and no branch. kBlocked occupies an Op-ignored high bit.
    uint8_t op_route_flags() const { return connection_flags_; }
    static constexpr size_t connection_flags_offset();
    // The owning IO thread captures this into each Op before dispatch. Executors never read Client
    // atomic bookkeeping: doing so pulled this tail cache line across cores for every shard task
    // and regressed the pure-MGET cell even when no atomic group existed.
    bool has_atomic_group_io() const { return atomic_groups_io_ != 0; }
    void atomic_group_started() { ++atomic_groups_io_; }
    void atomic_group_finished() {
        if (!atomic_groups_io_) std::abort();
        --atomic_groups_io_;
    }
    bool in_active() const { return in_active_; }
    void set_in_active(bool v) { in_active_ = v; }

    // MULTI/WATCH state is cold and allocated only on first use.  These fields consume padding in
    // the executor-facing tail; the signed 1984-byte Client footprint remains unchanged.
    MultiSession* multi_session() const { return multi_session_; }
    void set_multi_session(MultiSession* state) { multi_session_ = state; }
    uint64_t watch_generation() const {
        return watch_generation_.load(std::memory_order_acquire);
    }
    uint64_t next_watch_generation() {
        return watch_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    bool watch_dirty() const { return watch_dirty_.load(std::memory_order_acquire); }
    void set_watch_dirty() { watch_dirty_.store(true, std::memory_order_release); }
    void clear_watch_dirty() { watch_dirty_.store(false, std::memory_order_release); }
    bool authenticated() const { return authenticated_; }
    void set_authenticated(bool value) { authenticated_ = value; }
    uint32_t acl_user_idx() const { return acl_user_idx_; }
    void set_acl_user_idx(uint32_t value) { acl_user_idx_ = value; }
    static constexpr size_t acl_user_idx_offset();
    void watch_ref() { watched_refs_.fetch_add(1, std::memory_order_relaxed); }
    void watch_unref() {
        if (watched_refs_.fetch_sub(1, std::memory_order_acq_rel) == 0) std::abort();
    }
    // ---- the ONLY cross-thread fields ----------------------------------------------------------
    // A connection may be freed only when (a) nothing is in flight through its ROB AND (b) no
    // Client* naming it can still surface from a notification channel. (b) is what the ASAN
    // use-after-free proved: a worker posts the client for retirement, io serves it through
    // flush_ready instead, the ROB quiesces, the peer disconnects, close frees the client -- and
    // io then drains its channel into freed memory. The claim flag covers every CLAIMED post: it is
    // set before posting and cleared only when the consumer takes the entry, and after quiescence
    // no new claim can ever be made (no op will complete again), so requiring it false here is
    // race-free.
    bool safe_to_release() {
        return rob_.quiesced() &&
               !recv_armed_ && !send_inflight_ &&        // the KERNEL holds no pointer into us
               !retire_queued_.load(std::memory_order_acquire) &&
               watched_refs_.load(std::memory_order_acquire) == 0;
    }

    // Set by a worker before it tells the owning IO thread this client has ops to retire; cleared by
    // that IO thread when it picks the client up. Without it, a pipelined burst of N completions
    // enqueues the same client N times and the retire channel fills with duplicates.
    std::atomic<bool>& retire_queued() { return retire_queued_; }

    // The io thread's ready-mask slot for this connection. Written by io (assign at accept,
    // kNoWbSlot at close), read by every worker deciding how to signal completion. A stale read
    // falls back to the channel path, which is always correct -- so relaxed is enough.
    static constexpr uint32_t kNoWbSlot = UINT32_MAX;
    uint32_t wb_slot() const { return wb_slot_.load(std::memory_order_relaxed); }
    void set_wb_slot(uint32_t s) { wb_slot_.store(s, std::memory_order_release); }

    // TLS state is owned by the connection's IO thread and lives out-of-line.  The slot consumes
    // the four-byte tail hole re-derived after the limits + ACL merges (offset 1980), preserving the
    // signed Client footprint.  It is deliberately not at the stale audit offset 12: timeout owns
    // that io-hot word now.
    static constexpr uint32_t kNoTlsSlot = UINT32_MAX;
    bool is_tls() const { return tls_slot_ != kNoTlsSlot; }
    uint32_t tls_slot() const { return tls_slot_; }
    void set_tls_slot(uint32_t slot) { tls_slot_ = slot; }
    static constexpr size_t tls_slot_offset();

#ifdef TOMO_WEDGE_FORENSICS
    // FORENSICS for the stranded-reply class: claims (worker won the CAS), defers (lost it),
    // serves (io actually served). serves < claims on a stranded client names the dropped link.
    std::atomic<uint32_t> n_claims{0};
    std::atomic<uint32_t> n_defers{0};
    std::atomic<uint32_t> n_serves{0};
#endif

private:
    // --- io-hot scalars, packed: touched together on every pass ---------------------------------
    int       fd_   = -1;
    uint32_t  rlen_ = 0;          // bytes received
    uint32_t  rpos_ = 0;          // bytes parsed
    uint32_t  last_interaction_s_ = 0; // monotonic whole seconds; occupies the rbuf_ alignment hole
    char*     rbuf_ = nullptr;
    size_t    rcap_ = 0;
    uint32_t  fill_  = 0;         // index of the buffer replies append to
    uint32_t  wsent_ = 0;         // bytes of the SEND buffer already written
    bool      recv_armed_    = false;
    bool      send_inflight_ = false;
    bool      segmented_send_ = false;
    uint32_t  send_requested_ = 0;
    bool      serve_pending_ = false;
    bool      in_active_     = false;
    bool      closing_       = false;
    bool      dead_          = false;
    // BarrierOwner mask, not a bool. Same byte, same offset, same alignment: it occupies the slot
    // the bool held inside the FULL 48..55 bool run (connection_flags_ is static_asserted at 55
    // below), so the io-hot head does not move and sizeof(Client) is unchanged. Widening it past
    // uint8_t would displace connection_flags_ and grow the 64-byte-aligned Client -- a seventh
    // owner takes the one spare bit (1u << 6), it does not grow the field.
    uint8_t   barrier_owners_ = 0;
    bool      atomic_backpressure_ = false;
    bool      subscriber_mode_ = false;  // IO-owned; consumes existing alignment padding
    // The former blocked_ bool is a one-byte flag cell. RESP3 shares it instead of extending the
    // already-full 48..55 bool run and moving id_ (which would grow the 64-byte-aligned Client).
    static constexpr uint8_t kBlocked = 1u << 7;
    static constexpr uint8_t kResp3 = 1u << 2;
    static constexpr uint8_t kNoTouch = 1u << 4;
    uint8_t   connection_flags_ = 0;

    // --- cold io state --------------------------------------------------------------------------
    uint64_t  id_ = 0;
    uint32_t  ifid_thread_ = 0;
    Session   session_;

    // --- the ROB (manages its own cross-thread layout) ------------------------------------------
    Rob<kRobWindow> rob_;

    // --- write buffers (ex writes the direct-reply DATA region inside; header fields io-only) ---
    SmallBuf<kWbufInline> buf_[2];

    // sendmsg reads both the iovec array and msghdr asynchronously, so both live with the Client.
    static constexpr uint32_t kMaxSendIov = 16;
    static constexpr uint32_t kMaxSendBytes = 0x7ffff000u;  // Linux MAX_RW_COUNT; CQE res is int
    SegmentQueue<8> segments_;
    iovec           send_iov_[kMaxSendIov] = {};
    msghdr          send_msg_ = {};

    // --- executor-facing atomics, on their own line ---------------------------------------------
    alignas(64) std::atomic<bool>     retire_queued_{false};
    std::atomic<uint32_t> wb_slot_{kNoWbSlot};
    uint32_t atomic_groups_io_ = 0; // connection-IO-owned; captured into Op before dispatch
    MultiSession* multi_session_ = nullptr;
    std::atomic<uint64_t> watch_generation_{0};
    std::atomic<uint32_t> watched_refs_{0};
    std::atomic<bool> watch_dirty_{false};
    uint64_t obuf_bytes_ = 0;          // fill + unsent send + segment bytes
    uint32_t obuf_soft_since_s_ = 0;   // 0 = not continuously at/over the soft limit
    bool obuf_tracking_ = false;        // enabled once per serve/cron arm, not per reply append
    bool authenticated_ = false;        // requirepass state; shares the documented cold-tail hole
    uint32_t acl_user_idx_ = 0;          // ACL user handle; occupies the final 4-byte aligned hole
    uint32_t tls_slot_ = kNoTlsSlot;     // out-of-line TlsConn handle; occupies tail padding
};

constexpr size_t Client::acl_user_idx_offset() { return offsetof(Client, acl_user_idx_); }
static_assert(Client::acl_user_idx_offset() + sizeof(uint32_t) <= sizeof(Client));
constexpr size_t Client::connection_flags_offset() { return offsetof(Client, connection_flags_); }
static_assert(Client::connection_flags_offset() == 55,
              "connection flags moved: re-run the declaration-order Client mirror probe");
constexpr size_t Client::tls_slot_offset() { return offsetof(Client, tls_slot_); }
static_assert(Client::tls_slot_offset() == 1980,
              "TLS slot moved: re-run the declaration-order Client mirror probe");

// Same footprint law as Op: Client is per-connection resident memory and its io-hot head is
// layout-tuned. Growing it is allowed -- knowingly. 1984 = 1408 + the zero-copy send state
// (segment queue, iovec window, msghdr), which must persist per conn across send CQEs; signed
// against the 64c A/B of the zc merge.
static_assert(sizeof(Client) == 1984, "Client grew: re-check the io-hot line packing and idle RSS");

}  // namespace tomo
