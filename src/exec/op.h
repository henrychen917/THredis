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
#include <new>
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
// allocation-free in a ROB chunk. These bytes live after the chunk's contiguous Op headers:
// parsing, routing and coded/direct replies need the buffer metadata but not its unused payload.
inline constexpr size_t kInlineReply = 96;

template <size_t Count> struct OpChunk;

// Same byte-sink contract as SmallBuf, with its inline storage supplied by the ROB chunk. Keep
// data/length/capacity in the shared header: reset and Sink's empty/capacity tests must not chase
// a cold body. Only payload accesses follow data_, exactly as they did in SmallBuf. The fourth
// pointer is the stable home used at shrink/destruction, never a cross-thread publication.
//
// Standalone Lua/MULTI Ops have no chunk. They allocate on their first byte reply and retain that
// allocation until shrink/destruction; constructing/resetting a direct or coded Op allocates
// nothing. Every chunk binds its homes before publishing any slot and never relocates them.
class OpReply {
public:
    OpReply() = default;
    ~OpReply() { if (data_ != home_) std::free(data_); }
    OpReply(const OpReply&) = delete;
    OpReply& operator=(const OpReply&) = delete;

    char* data() { return data_; }
    const char* data() const { return data_; }
    size_t size() const { return len_; }
    bool empty() const { return len_ == 0; }
    size_t cap() const { return cap_; }
    void clear() { len_ = 0; }
    void advance(size_t n) { len_ += n; }
    void commit_raw(size_t n) { len_ += n; }
    void shrink_to_inline() {
        if (data_ != home_) { std::free(data_); data_ = home_; }
        cap_ = home_ == empty_ ? 0 : kInlineReply;
        len_ = 0;
    }
    char* reserve(size_t n) {
        if (len_ + n > cap_) grow(len_ + n);
        return data_ + len_;
    }
    void append(const char* s, size_t n) {
        char* p = reserve(n);
        std::memcpy(p, s, n);
        advance(n);
    }
    void append(std::string_view s) { append(s.data(), s.size()); }
    void append(Slice s) { append(s.p, s.n); }
    template <size_t N> void append(const char (&lit)[N]) {
        static_assert(N >= 1);
        if constexpr (N - 1 <= 16) {
            char* p = reserve(N - 1);
            __builtin_memcpy(p, lit, N - 1);
            advance(N - 1);
        } else {
            append(static_cast<const char*>(lit), N - 1);
        }
    }
    void push_back(char ch) { *reserve(1) = ch; advance(1); }

private:
    template <size_t Count> friend struct OpChunk;
    void bind_inline(char* home) {
        data_ = home_ = home;
        cap_ = kInlineReply;
    }
    void grow(size_t need) {
        size_t ncap = cap_ ? cap_ * 2 : kInlineReply;
        while (ncap < need) ncap *= 2;
        char* next = static_cast<char*>(std::malloc(ncap));
        if (!next) throw std::bad_alloc();
        std::memcpy(next, data_, len_);
        if (data_ != home_) std::free(data_);
        data_ = next;
        cap_ = ncap;
    }
    // A valid empty data() also preserves callers that build a string from (data(), 0).
    inline static char empty_[1] = {};
    char* data_ = empty_;
    size_t len_ = 0;
    size_t cap_ = 0;
    char* home_ = empty_;
};
static_assert(sizeof(OpReply) == 32);

// REPLY CODES -- the executor's side of the owner's split: "the executor writes only bytes it
// alone knows; everything else it returns as a result, and the connection's owner formats."
//
// A read's reply is bytes the executor is holding (the value), so it writes them. A WRITE's reply
// is either predetermined (+OK for every SET/MSET) or a NUMBER the executor computed (the count
// from DEL, the new value from INCR) -- neither is a byte string the executor is uniquely
// positioned to produce. Carrying a code plus an integer instead of five formatted bytes removes
// the executor's store AND the owner's copy-back, and in split mode it removes a cross-core
// transfer per write: the reply line was written by the executor and read by the io thread on
// every single write, and now it is written and read by the owner alone.
//
// Codes are materialised by format_reply_code() in ../net/resp.h, at retire, by the thread that
// owns the connection. The bytes on the wire are the same bytes, in the same order.
enum class ReplyCode : uint8_t {
    None      = 0,
    Ok        = 1,   // "+OK\r\n"
    Nil       = 2,   // "$-1\r\n"      RESP2 null bulk
    Pong      = 3,   // "+PONG\r\n"
    NullArray = 4,   // "*-1\r\n"      RESP2 null array
    EmptyStr  = 5,   // "$0\r\n\r\n"
    NullResp3 = 6,   // "_\r\n"        RESP3 null
    True      = 7,   // "#t\r\n"
    False     = 8,   // "#f\r\n"
    Int       = 9,   // ":<reply_ival_>\r\n"
};

// Longest coded reply: ":-2147483648\r\n" is 14. Reserving one constant keeps the owner's
// materialise to a single capacity test with no per-code arithmetic.
inline constexpr uint32_t kReplyCodeMax = 16;

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
    // `Codes` MUST match the ROB's arming (Rob::acquire<Codes>). When it is false this op can
    // never carry a code -- Sink::code() declines on reply_code_ok_, which acquire<false> also
    // leaves alone -- so reply_code_ and reply_ival_ keep the zero they were constructed with and
    // re-zeroing them is pure per-op work on the io thread's parse path, in the mode that gets no
    // benefit from it. Defaults to true so the heap child Ops in multi.inc, which call reset()
    // directly and are never acquired, keep the unconditional clear.
    template <bool Codes = true>
    void reset(uint8_t route_flags = 0) {
        argc_ = 0;
        spec  = nullptr;
        shard = -1;
        read_cut_lo = 0;
        route_flags_ = route_flags;
        if constexpr (Codes) { reply_code_ = 0; reply_ival_ = 0; }
        reply.clear();
        direct = nullptr;
        direct_cap = direct_len = 0;
        zc_ptr = nullptr;
        zc_len = 0;
        zc_shard = -1;
        state.store(OpState::Free, std::memory_order_relaxed);
    }

    // Bits 6 and 7 are shared with Client-only state. Only the armed coarse parser masks those
    // captured connection bits before explicitly classifying the slot; reset() above remains the
    // literal baseline path for every ordinary ROB acquisition.
    void reset_read_local(uint8_t route_flags = 0) {
        reset<true>(static_cast<uint8_t>(
            route_flags & static_cast<uint8_t>(~(kReadLocal | kReadLocalPreciseWrite))));
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
    // Only the low 32 bits are kept; the owner widens the value against the live sequence. The reconstruction is exact
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
    // pending lookup without reading a remote Client cache line. The bit shares the routing byte.
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
    // Fused read-local bookkeeping reuses bit 6 only after the enabled parser masks the captured
    // connection flag above.
    void mark_read_local() { route_flags_ |= kReadLocal; }
    bool read_local() const { return route_flags_ & kReadLocal; }
    // A precise write promises that it cannot mutate outside its declared point/keyset. If an
    // evicting maxmemory policy becomes live after IO made that classification, the owner uses
    // this immutable stamp to suspend eviction for this operation. Bit 7 is Client's blocked
    // flag; reset_read_local() masks the captured connection value before armed code reuses it.
    void mark_read_local_precise_write() { route_flags_ |= kReadLocalPreciseWrite; }
    bool read_local_precise_write() const { return route_flags_ & kReadLocalPreciseWrite; }
    uint8_t route_flags_ = 0;

    // THE CODED REPLY. rbuf_off ends at 28; the following pointer aligns to 32, leaving these bytes.
    // Non-zero means "this op's whole reply is this code"; the owner formats it at retire.
    uint8_t reply_code_ = 0;

    // WHO MAY CARRY A CODE. Only an Op the ROB handed out, because only those retire through
    // WbEngine::serve, which is the one place that knows how to turn a code back into bytes.
    //
    // The tree has Ops that never go near that path and whose reply bytes are read back by other
    // code: MULTI builds a heap child Op per queued command and splices its reply into the public
    // op's buffer (multi.inc make_child_op / set_state_reply), and redis.call() runs into a
    // stack-local Op whose bytes Lua parses back into a Lua value (scripting.cc). Those are
    // reset() but never acquired, so they default to unarmed and keep the byte path exactly as
    // before -- the split is structural rather than a list of sites to remember. reset() must NOT
    // touch this: a ROB slot is armed once and is a ROB slot forever.
    uint8_t reply_code_ok_ = 0;

private:
    // Parse and execute both inspect argc/heap even for two inline arguments. Keeping that
    // metadata beside routing avoids fetching the tail solely to discover there is no heap.
    Slice*   argv_heap_ = nullptr;
    uint32_t argv_cap_  = 0;
    uint32_t argc_      = 0;
public:
    OpReply reply;                         // metadata here; bytes in the chunk's reply body

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

    // The integer that goes with ReplyCode::Int -- a value the executor computed, not a format.
    // `state` leaves seven bytes before the aligned argv array; the integer uses four of them.
    // A count or a counter outside +/-2^31 simply keeps the
    // byte path, which emits the identical digits.
    int32_t reply_ival_ = 0;

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
        // Runtime-length append: values, members, error texts -- every reply a handler builds from
        // bytes whose length it did not know at compile time. reserve() returns either the
        // connection's direct region or op.reply's storage, and neither can alias a handler's
        // source bytes, so the no-overlap contract holds. The fixed replies take the literal
        // overload below instead; reverting THIS line to the plain library call once the literals
        // had moved was measured anyway and is much worse -- SET overwrite falls from -55
        // instructions to -4 -- so the inline copy earns its place here too.
        void append(const char* s, size_t n) {
            char* p = reserve(n);
            if (__builtin_expect(n > kInlineByteCopyMax, false)) std::memcpy(p, s, n);
            else bytes_copy(p, s, n);
            advance(n);
        }
        void append(std::string_view s) { append(s.data(), s.size()); }
        // RESP LITERALS. "+OK\r\n" and "$-1\r\n" are the reply of every SET and every GET miss,
        // and their length is a compile-time constant -- but this append is out of line with 225
        // callers, so the constant never reached the copy and each reply paid a call to get here
        // and a second one to memcpy five bytes. Taken as an array the length survives, and a
        // reply that fits in one machine word becomes one store at the call site with no call at
        // all. Bounded at 16 bytes so the 68-byte WRONGTYPE text and its like stay out of line;
        // the semantics are exactly the string_view overload's, NUL excluded.
        template <size_t N>
        __attribute__((always_inline)) void append(const char (&lit)[N]) {
            static_assert(N >= 1, "append() takes a string literal");
            if constexpr (N - 1 <= 16) {
                char* p = reserve(N - 1);
                __builtin_memcpy(p, lit, N - 1);
                advance(N - 1);
            } else {
                append(static_cast<const char*>(lit), N - 1);
            }
        }
        void push_back(char ch) { char* p = reserve(1); *p = ch; advance(1); }

        // CODED REPLY. Records "the reply is this" instead of writing its bytes. Returns false
        // when this sink is not empty, and then the caller formats bytes exactly as before -- that
        // is what keeps composition safe: EXEC writes its array header first, so every element
        // reply inside it sees a non-empty sink and takes the byte path, and the coded form can
        // only ever stand for a WHOLE reply.
        //
        // Setting a code DISARMS the direct region. A code is materialised at the fill buffer's
        // frontier at retire, and direct bytes live at that same offset; disarming means any
        // append that follows spills to op.reply, which retire emits AFTER the coded bytes. So
        // "code, then more bytes" and "bytes only" both keep RESP order, and the direct region
        // loses nothing -- the owner is writing into that very buffer either way.
        __attribute__((always_inline))
        bool code(ReplyCode c, int32_t v = 0) {
            // THE ARMING TEST CARRIES NO STATIC HINT, deliberately. Folded into the chain below
            // under __builtin_expect(..., false) it told the compiler "expect code() to succeed",
            // which is right in fused and wrong on EVERY call in split -- a mispredict per reply
            // for a decision that is 100% biased for the life of the process, and therefore one
            // the hardware predictor gets right for free in both modes once it is its own branch.
            // Costs no instructions either way; it is the branch that was being paid for.
            if (!op_.reply_code_ok_) return false;
            if (__builtin_expect(op_.reply_code_ != 0 || op_.direct_len != 0 ||
                                 !op_.reply.empty(), false))
                return false;
            op_.reply_code_ = static_cast<uint8_t>(c);
            op_.reply_ival_ = v;
            op_.direct = nullptr;
            return true;
        }
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
    bool replied() const { return !reply.empty() || direct_len != 0 || reply_code_ != 0; }

    // THE reply reset. Every caller that discards a half-written reply to put an error in its
    // place must drop the code too, or the discarded reply survives as five bytes the handler
    // no longer believes it wrote. One method so a future reset site cannot forget the field.
    void clear_reply() {
        reply.clear();
        direct_len = 0;
        reply_code_ = 0;
        reply_ival_ = 0;
    }

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
    static constexpr uint8_t kReadLocal = 1u << 6;
    static constexpr uint8_t kReadLocalPreciseWrite = 1u << 7;
    Slice    argv_inline_[kInlineArgv];
};

// One allocation still owns eight consecutive slots: per-slot allocations previously lost SET
// locality. Header-only stages traverse 248-byte strides instead of stepping over 96 unused reply
// bytes. Executors write body bytes only for nondirect byte replies; IO reads them after Done.
// This is stage storage, not a new owner or handoff. Destruction occurs only after ROB quiescence.
template <size_t Count> struct OpChunk {
    Op ops[Count];
    char reply_bytes[Count][kInlineReply];
    OpChunk() {
        for (size_t i = 0; i < Count; i++) ops[i].reply.bind_inline(reply_bytes[i]);
    }
};

// F11 re-lock: 336 -> 248 header bytes, plus 96 body bytes per materialized slot. The stable home
// pointer costs 8 bytes/slot in total; one 8-slot allocation is 2752 bytes (before allocator
// rounding), versus the former 2688-byte array plus its destructor cookie. S4 must price the
// whole allocation, not claim the smaller header as an RSS reduction.
static_assert(sizeof(Op) == 248, "Op stage header changed: re-price header and body together");
static_assert(sizeof(OpChunk<8>) == 2752, "Op chunk footprint changed");

}  // namespace tomo
