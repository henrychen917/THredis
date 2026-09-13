// op.h — one in-flight command. This is the execution context, and it is the ring slot of the ROB.
//
// GENERALITY BEYOND STRINGS comes from the reply representation, not from a type hierarchy: the
// worker writes RESP bytes straight into the Op's own buffer. The one exception is the explicit
// borrowed-byte descriptor used by large GETs; it describes lifetime and routing, not a value type.
// A command that returns a hash, a range, or a nested array still uses the same byte sink.
//
// LIFETIME, which is the subtle part. argv entries are Slices INTO the connection's read buffer —
// that is what makes parsing zero-copy. So the read buffer must stay pinned until this Op retires.
// The ROB gives that for free: the buffer is pinned until flush_id passes the last Op referencing
// it. Getting this wrong is the same shape as the fork's worker-argv refcount race, where a worker
// touched argv after the owning thread had recycled it.
//
// OWNERSHIP HANDOFF. Exactly one field crosses threads: `state`. The IO thread publishes an Op with
// a release store of Issued; the worker consumes it, fills `reply`, and publishes Done with a
// release store; the IO thread observes Done with an acquire load and only then reads `reply`.
// Everything else is touched by one thread at a time, ordered by that single pair.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "../base/slice.h"

namespace tomo {

struct CommandSpec;
class  Client;
struct ScatterState;
struct BlockingState;

enum class OpState : uint8_t {
    Free   = 0,   // slot is reusable
    Issued = 1,   // IO published it; the owning worker may take it
    Done   = 2,   // worker finished; reply is complete and safe to read
};

// Most commands are 2-3 arguments (GET k / SET k v / DEL k). Spill only for the multi-key forms.
inline constexpr uint32_t kInlineArgv = 8;

// A GET reply for a 64-byte value is ~72 bytes, so 96 inline still keeps the common case
// allocation-free. 96, not 128: the direct-reply fields took 16 bytes and the zero-copy borrow
// descriptor took 16 more; both are paid out of the inline reply so sizeof(Op) stays 336 -- the
// 352B version measured -3.7% at 64c p32, where the server sits on the DRAM wall and ROB
// footprint is the price of everything.
inline constexpr size_t kInlineReply = 96;

class Op {
public:
    static constexpr int32_t kScatterStateMarker = -2;
    static constexpr int32_t kLocalXshardMarker  = -3;
    static constexpr int32_t kBlockingStateMarker = -4;
    static constexpr int32_t kMultiStateMarker   = -5;
    Op() = default;
    ~Op() { std::free(argv_heap_); }
    Op(const Op&) = delete;
    Op& operator=(const Op&) = delete;

    // ---- built by the IO thread while parsing ------------------------------------------------
    void reset(uint8_t route_flags = 0) {
        argc_ = 0;
        spec  = nullptr;
        shard = -1;
        read_cut_lo = 0;
        route_flags_ = route_flags;
        reply.clear();
        direct = nullptr;
        direct_cap = direct_len = 0;
        zc_ptr = nullptr;
        zc_len = 0;
        zc_shard = -1;
        state.store(OpState::Free, std::memory_order_relaxed);
    }

    bool push_arg(Slice s) {
        if (!argv_heap_ && argc_ < kInlineArgv) { argv_inline_[argc_++] = s; return true; }
        const uint32_t need = argc_ + 1;
        if (need > argv_cap_) {
            uint32_t ncap = argv_cap_ ? argv_cap_ * 2 : kInlineArgv * 2;
            while (ncap < need) ncap *= 2;
            auto* n = static_cast<Slice*>(std::realloc(argv_heap_, ncap * sizeof(Slice)));
            if (!n) return false;
            if (!argv_heap_) std::memcpy(n, argv_inline_, kInlineArgv * sizeof(Slice));
            argv_heap_ = n;
            argv_cap_  = ncap;
        }
        argv_heap_[argc_++] = s;
        return true;
    }

    uint32_t argc() const { return argc_; }
    Slice arg(uint32_t i) const { return argv_heap_ ? argv_heap_[i] : argv_inline_[i]; }
    Slice cmd_name() const { return argc_ ? arg(0) : Slice{}; }
    Slice key() const { return argc_ > 1 ? arg(1) : Slice{}; }   // v1: key is always argv[1]

    // Recycles heap growth so one MSET-1000 or one 64MB reply does not permanently inflate every
    // ring slot. Called from Rob::drain at retire, gated so the common case (inline argv, inline
    // reply) pays one predictable branch and frees nothing.
    static constexpr size_t kShrinkKeep = 4096;   // reply heap above this is a burst, not a working size
    void shrink() {
        if (argv_heap_) { std::free(argv_heap_); argv_heap_ = nullptr; argv_cap_ = 0; }
        reply.shrink_to_inline();
    }
    bool oversized() const { return argv_heap_ != nullptr || reply.cap() > kShrinkKeep; }

    // ---- fields --------------------------------------------------------------------------------
    const CommandSpec* spec  = nullptr;
    int32_t            shard = -1;          // resolved by the router before publishing

    // THE READ CUT, PINNED IN PROGRAM ORDER. A multi-key read pins its epoch on IO at prepare and
    // resolves against it later; a plain read used to sample the sequence at EXECUTION and answer
    // with the newest committed world. On one connection that inverts time: `GET a` posted just
    // before `MGET a b` executes after the MGET's pin, so a foreign atomic commit landing in
    // between made the EARLIER reply newer than the LATER one. IO therefore stamps every
    // read-only op with the sequence as of its own prepare, which is the connection's program
    // order, and the owner resolves against that instead of "now". Writes deliberately keep
    // "newest": a write's read is the base of its own update and staleness there is a lost update.
    //
    // Only the low 32 bits are kept -- Op's 336-byte footprint has exactly this 4-byte hole and no
    // more, and the owner widens the value against the live sequence. The reconstruction is exact
    // while fewer than 2^31 commits separate dispatch from execution; the highest commit rate this
    // engine has produced would need ~40 seconds of queueing to reach that, and the widening
    // saturates to a stale-but-safe (older) cut rather than a newer one if it ever did.
    uint32_t           read_cut_lo = 0;
    uint64_t           hash  = 0;           // computed once by IO, reused by the worker

    // Offset into the connection's read buffer where this op's arguments begin. Because the ROB
    // retires strictly in order, the oldest live op's rbuf_off is the low-water mark below which
    // every byte is dead — that is how the read buffer gets compacted without refcounting the
    // Slices that point into it.
    uint32_t rbuf_off = 0;

    // IO knows whether this command was issued behind an unfinished cross-shard atomic group on
    // the same connection. Capture that fact before publish so executors can skip the owner-local
    // pending lookup without reading a remote Client cache line. The bit occupies existing padding
    // before SmallBuf; Op's signed 336-byte footprint is unchanged.
    void mark_atomic_hazard() { route_flags_ |= kAtomicHazard; }
    bool atomic_hazard() const { return route_flags_ & kAtomicHazard; }
    void mark_no_borrow() { route_flags_ |= kNoBorrow; }
    bool no_borrow() const { return route_flags_ & kNoBorrow; }
    void mark_resp3() { route_flags_ |= kResp3; }
    bool resp3() const { return route_flags_ & kResp3; }
    // CLIENT REPLY OFF/SKIP. Marked by the io thread's armed gate, honoured by the cold
    // suppressing serve variant. Bit 3 is free in BOTH this word and Client::connection_flags_,
    // whose byte is copied in wholesale by reset(), so no existing capture changes meaning.
    void mark_reply_skip() { route_flags_ |= kReplySkip; }
    void clear_reply_skip() { route_flags_ &= static_cast<uint8_t>(~kReplySkip); }
    bool reply_skip() const { return route_flags_ & kReplySkip; }
    // CLIENT NO-TOUCH, captured from the connection flag byte by reset(). Read only by ExLoop,
    // and only when maxmemory is enabled.
    bool no_touch() const { return route_flags_ & kNoTouch; }
    // Bit 5 is free in BOTH this word and Client::connection_flags_, whose byte reset() copies in
    // wholesale, so an unstamped op always reads "no cut" and takes the newest world exactly as
    // before.
    void set_read_cut(uint64_t cut) {
        read_cut_lo = static_cast<uint32_t>(cut);
        route_flags_ |= kReadCut;
    }
    bool has_read_cut() const { return route_flags_ & kReadCut; }
    // `now` must be a sequence value observed no earlier than the stamp, which every caller has
    // because commit sequences only move forward and the stamp happened on this connection's IO
    // thread before dispatch.
    uint64_t read_cut(uint64_t now) const {
        uint64_t cut = (now & ~uint64_t{0xFFFFFFFF}) | read_cut_lo;
        if (cut > now) cut -= uint64_t{1} << 32;
        return cut;
    }
    uint8_t route_flags_ = 0;

    SmallBuf<kInlineReply> reply;           // worker writes RESP here (the spill/general sink)

    // DIRECT REPLY (owner's c->buf trick, both postures). When io dispatches an op that is the ROB
    // HEAD of a connection with an EMPTY fill buffer, it points `direct` at that buffer's storage.
    // The worker then formats RESP bytes straight into their final destination and only records the
    // length here; the SENDER publishes the length into the buffer at in-order retire (commit_raw).
    // The bytes cross threads under the same Done release/acquire as everything else; the buffer's
    // SIZE is only ever written by the sender, so no new sharing is introduced. Nothing can move or
    // grow the buffer in the window: there are no prior unretired ops (head) and no staged bytes
    // (empty), so no append and no fill/send swap can occur before our own retire.
    // Replies that do not fit fall back to `reply` mid-op; retire emits direct bytes first, then the
    // spill, preserving RESP order. This is NOT exwb: the send syscall stays with the sender.
    char*    direct     = nullptr;
    uint32_t direct_cap = 0;
    uint32_t direct_len = 0;

    // A borrowed GET value is published with Done exactly like reply bytes. The pointer names
    // FlatStore-owned memory; the shard id is the return address for the eventual io->ex release.
    // No Client or socket state crosses to the executor through this descriptor.
    const char* zc_ptr   = nullptr;
    uint32_t    zc_len   = 0;
    int32_t     zc_shard = -1;

    // The only cross-thread field. Acquire/release on this orders everything else.
    std::atomic<OpState> state{OpState::Free};

    // The handler-facing reply sink: prefers the direct region while the whole reply fits, spills
    // to op.reply otherwise. Same interface as SmallBuf, so the resp.h helpers take either.
    class Sink {
    public:
        explicit Sink(Op& o) : op_(o) {}
        char* reserve(size_t n) {
            if (op_.direct && op_.reply.empty() &&
                op_.direct_len + n <= op_.direct_cap) {
                last_direct_ = true;
                return op_.direct + op_.direct_len;
            }
            last_direct_ = false;
            return op_.reply.reserve(n);
        }
        void advance(size_t n) {
            if (last_direct_) op_.direct_len += static_cast<uint32_t>(n);
            else              op_.reply.advance(n);
        }
        void append(const char* s, size_t n) { char* p = reserve(n); std::memcpy(p, s, n); advance(n); }
        void append(std::string_view s) { append(s.data(), s.size()); }
        void push_back(char ch) { char* p = reserve(1); *p = ch; advance(1); }
    private:
        Op&  op_;
        bool last_direct_ = false;
    };
    Sink sink() { return Sink(*this); }

    // "Has a handler already written a reply?" BOTH regions have to be consulted. A short reply
    // lands in the direct region and leaves `reply` empty, so `reply.empty()` alone reads as
    // "nothing written yet" and a caller that then emits its own fallback error puts TWO replies
    // on the wire, permanently shifting every later reply on that connection. XTRIM's option
    // errors did exactly that on an unpipelined connection.
    bool replied() const { return !reply.empty() || direct_len != 0; }

    bool has_scatter_state() const {
        return zc_ptr != nullptr && zc_shard == kScatterStateMarker;
    }
    ScatterState* scatter_state() const {
        return has_scatter_state()
            ? reinterpret_cast<ScatterState*>(const_cast<char*>(zc_ptr)) : nullptr;
    }
    void attach_scatter_state(ScatterState* state_) {
        zc_ptr = reinterpret_cast<const char*>(state_);
        zc_len = 0;
        zc_shard = kScatterStateMarker;
    }
    void detach_scatter_state() {
        zc_ptr = nullptr;
        zc_len = 0;
        zc_shard = -1;
    }
    void mark_local_xshard() {
        zc_ptr = nullptr;
        zc_len = 0;                 // snapshot write-gate cursor
        zc_shard = kLocalXshardMarker;
    }
    bool local_xshard() const { return zc_shard == kLocalXshardMarker; }
    void attach_multi_state(void* state_) {
        zc_ptr = static_cast<const char*>(state_);
        zc_len = 0;
        zc_shard = kMultiStateMarker;
    }
    bool has_multi_state() const { return zc_ptr != nullptr && zc_shard == kMultiStateMarker; }
    void* multi_state() const {
        return has_multi_state() ? const_cast<char*>(zc_ptr) : nullptr;
    }

    bool has_blocking_state() const {
        return zc_ptr != nullptr && zc_shard == kBlockingStateMarker;
    }
    BlockingState* blocking_state() const {
        return has_blocking_state()
            ? reinterpret_cast<BlockingState*>(const_cast<char*>(zc_ptr)) : nullptr;
    }
    void attach_blocking_state(BlockingState* state_) {
        zc_ptr = reinterpret_cast<const char*>(state_);
        zc_len = 0;
        zc_shard = kBlockingStateMarker;
    }
    void detach_blocking_state() {
        zc_ptr = nullptr;
        zc_len = 0;
        zc_shard = -1;
    }

    // Notifications deliberately consume no negative marker.  Bit 15 on shard-routed command
    // specs makes a non-borrow zc pointer unambiguous; special commands keep their batch inside
    // their already-marked heap state instead.
    bool has_notify_state() const {
        return zc_ptr != nullptr && zc_shard == -1;
    }
    void attach_notify_state(void* state_) {
        zc_ptr = static_cast<const char*>(state_);
        zc_len = 0;
        zc_shard = -1;
    }
    void* notify_state() const {
        return has_notify_state() ? const_cast<char*>(zc_ptr) : nullptr;
    }

private:
    static constexpr uint8_t kAtomicHazard = 1u << 0;
    static constexpr uint8_t kNoBorrow = 1u << 1;
    static constexpr uint8_t kResp3 = 1u << 2;
    static constexpr uint8_t kReplySkip = 1u << 3;
    static constexpr uint8_t kNoTouch = 1u << 4;
    static constexpr uint8_t kReadCut = 1u << 5;
    Slice    argv_inline_[kInlineArgv];
    Slice*   argv_heap_ = nullptr;
    uint32_t argv_cap_  = 0;
    uint32_t argc_      = 0;
};

// THE FOOTPRINT LOCK (owner law, 2026-08-24): +16 bytes on Op measured -3.7% at 64c p32 -- at
// the DRAM wall, ROB footprint is throughput. Growing Op requires paying for it elsewhere in the
// struct (kInlineReply bought the direct-reply fields) or re-earning the size with an A/B.
static_assert(sizeof(Op) == 336, "Op grew: pay for it inside the struct or re-run the 64c A/B");

}  // namespace tomo
