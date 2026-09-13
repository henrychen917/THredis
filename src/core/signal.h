// signal.h — ONE cross-thread signalling mechanism, used by all three loops, reporting in ONE set
// of units.
//
// WHY THIS FILE EXISTS. The three handoffs started out as three different mechanisms: IO->EX was a
// bare queue push with no wake, EX->IO was a msg_ring poke plus a scan of every active client, and
// the WB handoff was a queue plus a poke. A flip/LB controller reading those would be comparing
// three incomparable things — a depth, a scan cost, and a wake rate — and would have to special-case
// each one. Every balancer defect in the fork came from comparing mismatched quantities.
//
// So: every cross-thread handoff is a Channel, and every loop reports LoopSignals. Same shape, same
// units, whatever the direction.
//
//   IO -> EX    Channel<Task>      dispatch a parsed op to the shard's owner
//   EX -> IO    Channel<Client*>   tell the owner it has completed ops to retire
//   IO/EX -> WB Channel<Client*>   tell the sender it has bytes to write
//
// UNITS, fixed here so nothing has to be converted at the point of comparison:
//   work      operations (uint64 monotonic count)
//   time      NANOSECONDS, and busy is TIME-WEIGHTED, not an event count. A balancer that counts
//             events treats one expensive op the same as one cheap one; the fork's working pressure
//             balancer is time-weighted for exactly this reason.
//   pressure  queue depth in ENTRIES, accumulated with a sample count so a consumer can take a
//             time-average rather than a spot reading. Spot depth is noisy enough to make a
//             controller chase its own tail.
//
// THE 3% RULE. This machinery is always on, so it must cost under 3% or it does not ship. Hence:
// counters are plain non-atomic uint64 written only by their owning thread; the only atomic on the
// hot path is the peer-blocked flag, and it is read, not written.
#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#include "../exec/exqueue.h"
#include "../net/uring.h"

namespace tomo {

inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// Wall clock in milliseconds. Key deadlines are absolute CLOCK_REALTIME milliseconds, so ANYTHING
// that will be compared against a deadline must come from here and never from now_ns() above --
// that one is CLOCK_MONOTONIC, so its "milliseconds" are milliseconds since boot and sit roughly
// five orders of magnitude below any real deadline. A monotonic value used as an expiry cut does
// not skew the answer, it disables expiry outright; see NOTES-EXPWIDE.md defect W3.
inline int64_t now_realtime_ms() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Thread CPU time. Needed because DEFER_TASKRUN defers completion work to the point where the thread
// waits on the ring, so wall-clock "time spent in the work section" understates how busy the thread
// really is. A controller that reads only wall time under DEFER_TASKRUN is measuring the wrong thing
// — that already produced one wrong reading in the fork.
inline uint64_t thread_cpu_ns() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// What every loop reports, regardless of role. A controller reads this and nothing else.
struct LoopSignals {
    // ---- work -------------------------------------------------------------------------------
    uint64_t ops        = 0;    // units this loop completed: parsed+dispatched / executed / sent
    uint64_t iterations = 0;

    // ---- time (ns), time-weighted ------------------------------------------------------------
    uint64_t busy_ns    = 0;    // wall time inside the work section
    uint64_t idle_ns    = 0;    // wall time blocked or spinning with nothing to do
    uint64_t cpu_ns     = 0;    // thread CPU time; the DEFER_TASKRUN-proof busyness reading

    // ---- pressure ----------------------------------------------------------------------------
    uint64_t depth_sum     = 0; // sum of sampled inbound depths, in ENTRIES
    uint64_t depth_samples = 0; // divide to get a time-average rather than a spot reading
    uint64_t full_events   = 0; // outbound push refused: real backpressure, not a guess

    // ---- signalling --------------------------------------------------------------------------
    uint64_t wakes_sent = 0;
    uint64_t wakes_recv = 0;
    uint64_t spins      = 0;

    // ---- accept / submission health ------------------------------------------------------------
    uint64_t accepts     = 0;   // connections taken
    uint64_t accept_err  = 0;   // accept completions with a negative result
    uint64_t accept_rearm= 0;   // multishot dropped and had to be re-armed
    uint64_t sqe_starved = 0;
    uint64_t notify_drop = 0;   // post to the sender was refused; the claim had to be released   // get_sqe returned null even after a submit — the ring is saturated
    uint64_t acl_access_denied_cmd = 0;
    uint64_t acl_access_denied_key = 0;
    uint64_t acl_access_denied_channel = 0;
    uint64_t acl_access_denied_auth = 0;

    // Socket bytes are owned by the connection's IO thread and summed only by INFO. Keeping these
    // plain avoids a shared atomic RMW on every recv/send completion.
    uint64_t net_input_bytes = 0;
    uint64_t net_output_bytes = 0;

    // TLS counters are written only by the owning IO thread. With tls-port=0 the clean loop
    // specialization contains no increments or per-operation TLS tests.
    uint64_t plain_accepts = 0;
    uint64_t tls_accepts = 0;
    uint64_t tls_handshakes_started = 0;
    uint64_t tls_handshakes_completed = 0;
    uint64_t tls_handshakes_failed = 0;
    uint64_t tls_connections_freed = 0;
    uint64_t tls_want_read = 0;
    uint64_t tls_want_write = 0;
    uint64_t tls_ciphertext_input_bytes = 0;
    uint64_t tls_plaintext_input_bytes = 0;
    uint64_t tls_ciphertext_output_bytes = 0;
    uint64_t tls_plaintext_output_bytes = 0;
    uint64_t tls_zc_suppressed = 0;
    uint64_t tls_ktls_active = 0;       // current bidirectionally-offloaded connections
    uint64_t tls_ktls_fallback = 0;     // successful userspace-mode handshakes

    // --net-io epoll only. Both stay zero under io_uring, which is what makes them a usable
    // FIRED-MECHANISM proof: a run claiming to be on the epoll engine and reporting
    // epoll_events=0 was not on the epoll engine.
    uint64_t epoll_events = 0;          // readiness events returned by epoll_wait
    uint64_t epoll_recvs = 0;           // recv syscalls that returned bytes

    // Derived, computed on read so the hot path never divides.
    double avg_depth() const {
        return depth_samples ? static_cast<double>(depth_sum) / static_cast<double>(depth_samples) : 0.0;
    }
};

// Scoped timer: accumulates wall time into a counter. One per work section, so busy/idle add up to
// wall time by construction rather than by remembering to stop the right clock.
class Span {
public:
    explicit Span(uint64_t& sink) : sink_(sink), t0_(now_ns()) {}
    ~Span() { sink_ += now_ns() - t0_; }
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
private:
    uint64_t& sink_;
    uint64_t  t0_;
};

// ---------------------------------------------------------------------------------------------
// NotifyMask — the "where should I look" bitmap. One bit per producer.
//
// WITHOUT IT every consumer polls every producer's channel each iteration: at 128 threads that is
// 128 loads to discover that one of them has work. The mask turns that into one load plus a
// find-first-set per non-empty producer, so the cost tracks the number of ACTIVE producers rather
// than the number of possible ones.
//
// THE RMW TRAP, which is the whole reason this is not just a fetch_or: a blind atomic OR per push is
// a read-modify-write on a line shared with every other producer of that consumer. SPSC exists
// precisely to avoid per-push RMWs, so adding one here would give back what the queue design bought.
// Hence the read-first guard below: once a producer's bit is set it stays set until the consumer
// takes it, so a busy producer pays a shared-line READ and no write at all. Reads scale; writes
// serialise.
//
// THE ORDERING THAT MAKES IT CORRECT. The consumer must TAKE (exchange to zero) before draining,
// never after:
//
//      take bits -> drain those channels          correct
//      drain channels -> clear bits               LOSES a push that landed mid-drain
//
// In the wrong order a producer that pushes between the drain and the clear has its bit wiped, and
// its item sits in the queue until something unrelated happens to notify again. Same shape as the
// queued-flag ordering in the WB path.
// ---------------------------------------------------------------------------------------------
// ReadyMask — per-SENDER "which of my clients has completed work" bits, one bit per wb slot.
//
// THE #19/#20 DESIGN, ported: workers signal reply-readiness by setting an IDEMPOTENT bit in the
// sender's mask instead of posting the Client* through a channel. Three properties fall out, and
// each one retires a bug class this tree has already paid for:
//   - no claim protocol: a bit set twice is one bit, so the CAS-claim dance (and its StoreLoad
//     fence discipline) is not needed on this path;
//   - no pointers in flight: a channel entry can outlive the client (the teardown UAF); a bit
//     cannot -- the sender maps slot -> client through a table IT owns and IT clears;
//   - no stranding: the bit stays set until the sender takes it, and any completion after a take
//     re-sets it, so "both sides walk away" is unrepresentable.
// Same take-then-serve ordering as NotifyMask; same read-first guard so a busy producer pays a
// shared-line read, not an RMW. set() returns true on the empty->flagged transition (that RMW is a
// full fence) -- the caller who gets true owes the park-wake, exactly like the channel protocol.
class ReadyMask {
public:
    static constexpr uint32_t kSlots = 1024;
    static constexpr uint32_t kWords = kSlots / 64;

    bool set(uint32_t slot) {
        const uint64_t bit = 1ull << (slot & 63);
        auto& w = words_[(slot >> 6) % kWords];
        if (w.load(std::memory_order_relaxed) & bit) return false;
        return (w.fetch_or(bit, std::memory_order_seq_cst) & bit) == 0;
    }
    // Load-first: an exchange is a locked RMW that takes the line exclusive even when the word is
    // zero -- and these words are fetch_or'd by workers on OTHER CCDs, so an unconditional 16-word
    // exchange sweep per loop pass bounces contended lines for nothing. The relaxed load costs a
    // shared-line read; a zero word is the common case and now stays shared. A bit set between the
    // load and a skipped exchange is not lost: it is still set, and the next pass takes it.
    uint64_t take(uint32_t word) {
        if (!words_[word].load(std::memory_order_relaxed)) return 0;
        return words_[word].exchange(0, std::memory_order_acquire);
    }
    bool any() const {
        for (uint32_t i = 0; i < kWords; i++)
            if (words_[i].load(std::memory_order_relaxed)) return true;
        return false;
    }

private:
    alignas(64) std::atomic<uint64_t> words_[kWords] = {};
};

class NotifyMask {
public:
    static constexpr uint32_t kBits  = 128;
    static constexpr uint32_t kWords = kBits / 64;

    // Producer side. One relaxed load in the common case; the RMW is paid only on the empty->flagged
    // transition, which under load is rare.
    //
    // RETURNS TRUE only when THIS producer performed the transition, and that answer is what decides
    // who owns the wake — see the Dekker note on Channel::wake(). A producer that finds the bit
    // already set owes no wake: the bit is still set, so the consumer has not taken it yet, so the
    // consumer is still going to take it and drain this channel, and our item is already in the queue
    // by then. The seq_cst on the RMW is load-bearing, not decoration: it is the full fence that keeps
    // the store of the bit from being reordered after the load of blocked_ in wake().
    bool set(uint32_t producer) {
        const uint64_t bit = 1ull << (producer & 63);
        auto& w = words_[(producer >> 6) & (kWords - 1)];
        if (w.load(std::memory_order_relaxed) & bit) return false;   // already flagged: no write
        return (w.fetch_or(bit, std::memory_order_seq_cst) & bit) == 0;
    }

    // Consumer side. Takes and clears one word's worth of flags. Acquire pairs with the producer's
    // release so the queue contents behind the bit are visible.
    uint64_t take(uint32_t word) {
        return words_[word].exchange(0, std::memory_order_acquire);
    }

    bool any() const {
        for (uint32_t i = 0; i < kWords; i++)
            if (words_[i].load(std::memory_order_relaxed)) return true;
        return false;
    }

private:
    std::atomic<uint64_t> words_[kWords] = {};
};

// One directed cross-thread handoff. SPSC queue + a wake that is only paid when the peer is asleep.
template <typename T, uint32_t Cap>
class Channel {
public:
    // ---- producer side --------------------------------------------------------------------------
    // Returns false when full. A false MUST be handled — never dropped. Losing a queued item here
    // loses a reply and wedges the connection waiting for it, which is precisely the bug that
    // shipped in the fork.
    // `peer_ring` is the CONSUMER's ring, passed in rather than bound at construction: a bound
    // pointer would need every thread's ring to exist before any channel is wired, and getting that
    // startup order wrong leaves the pointer null and NO WAKE IS EVER SENT — a consumer that blocks
    // then sleeps forever on a non-empty queue. Passing it makes the dependency impossible to forget.
    // SPLIT IN TWO ON PURPOSE. The consumer's decision to sleep is made by reading the notify mask,
    // so the mask bit must be published BETWEEN the push and the wake decision. When this was one
    // call that set the bit afterwards, the producer read blocked_ before the bit existed and the
    // consumer read the bit before the producer wrote it -- both sides saw "nothing to do" and an
    // isolated request hung forever. Pipelined traffic hid it, because the next request re-poked the
    // consumer; only a lone command exposed it.
    bool push(T v, LoopSignals& sig) {
        if (!q_.push(v)) { sig.full_events++; return false; }
        return true;
    }
    bool push_batch(const T* values, uint32_t count, LoopSignals& sig) {
        if (!q_.push_batch(values, count)) { sig.full_events++; return false; }
        return true;
    }

    // Call ONLY when the caller performed the mask's empty->flagged transition. That RMW is a full
    // fence, which is what makes this load safe: without it, store(blocked_)/load(mask) on the
    // consumer and store(mask)/load(blocked_) here are a Dekker pair, and x86 permits exactly the one
    // reordering (StoreLoad) that lets both sides miss each other.
    //
    // Only pay a syscall if the consumer is actually blocked. Under load it is spinning and will see
    // the item on its next pass, so the common case costs one relaxed atomic read.
    void wake(Ring& my_ring, LoopSignals& sig, Ring* peer_ring) {
        if (peer_ring && blocked_.load(std::memory_order_acquire)) {
            my_ring.msg_to(*peer_ring, ur_tag(UrKind::Wake, nullptr));
            sig.wakes_sent++;
        }
    }

    // ---- consumer side --------------------------------------------------------------------------
    bool recv(T& out) { return q_.pop(out); }

    // Call AFTER the item has actually been processed — see exqueue.h. quiesced() is the predicate
    // every teardown, reshard and role conversion must test; depth() == 0 is not the same thing.
    void retire()          { q_.retire(); }
    bool quiesced() const  { return q_.quiesced(); }
    uint32_t depth() const { return q_.depth(); }
    uint32_t producer_free_slots() const { return q_.producer_free_slots(); }

    // Declare intent to block, so producers know a wake is worth its syscall. Set it, then re-check
    // the queue before actually blocking: without that re-check a producer that pushed just before
    // the flag was set would not have woken us, and we would sleep on a non-empty queue.
    void arm_blocked()   { blocked_.store(true,  std::memory_order_release); }
    void clear_blocked() { blocked_.store(false, std::memory_order_release); }

private:
    ExQueue<T, Cap>   q_;
    std::atomic<bool> blocked_{false};
};

}  // namespace tomo
