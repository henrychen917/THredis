// rob.h — the reorder buffer. One per connection.
//
// THE PROBLEM IT SOLVES. A pipelined client sends GET a, GET b, GET c on one socket. Those keys can
// live on three different workers, which finish in any order. The protocol requires the replies in
// the order sent. So we need out-of-order execution with in-order retirement — a reorder buffer, and
// the name is not an analogy: dispatch_id is the issue pointer, flush_id is the commit pointer, and
// a slot retires only when every older slot has retired.
//
//   dispatch_id  next id to issue     — advanced at parse
//   flush_id     next id to retire    — advanced at retire (both by the connection's io thread)
//   slot(id)     id & mask
//   in_flight    dispatch_id - flush_id
//
//   dispatch_id == flush_id is the universal quiescence fence. Nothing is in flight, so the
//   connection can be closed, migrated, or have its buffers recycled. Every teardown path tests
//   exactly this and nothing else.
//
// ============================================================================================
// WHAT CROSSES THREADS, in pure 2s: the io thread owns BOTH counters (it parses and it retires).
// The executor only ever touches individual Op slots — reached through chunk pointers published
// with release/acquire, contents ordered by the Op's own state handshake (Issued/Done). The two
// counters stay atomics anyway: on x86 the same-thread case degrades to ordinary loads and stores
// (zero cost), and dispatch_id == flush_id is the quiescence fence a future connection MIGRATION
// between io threads must read from the other side — demoting them buys nothing and closes a door.
//
// READ-BUFFER PINNING STILL FALLS OUT. argv Slices point into the connection's read buffer. Because
// retirement is strictly in order, the oldest live op is always slot(flush_id) — so every byte
// before that op's rbuf_off is dead. No refcounting, no generation numbers.
//
// SLOTS ARE CHUNKED, NOT INLINE AND NOT SCATTERED. An ExecContext is ~328 bytes; held by value a
// 64-slot ROB is ~21KB resident per connection whether it ever pipelines or not. The first pointer
// version fixed that with one heap Op per slot — and measured a −3% SET p32 regression, because 64
// scattered allocations lost the sequential locality the drain used to get from the inline array
// for free. So: contexts materialize in CONTIGUOUS CHUNKS of eight, on the first touch of any slot
// in the chunk. A p1 connection holds one chunk (~2.6KB), a 32-deep pipeliner four, and the drain
// walks sequential memory within every chunk. Each context recycles in place forever ("flushed"
// logically at retire — state goes Free — never returned to the allocator until the connection
// dies); steady-state allocator traffic is zero, and jemalloc needs no pool in front of it.
#pragma once
#include <atomic>
#include <cstdint>
#include "../exec/op.h"

namespace tomo {

template <uint32_t Capacity>
class Rob {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static constexpr uint32_t kMask = Capacity - 1;

public:
    // ---- producer side (the parser / io thread) ------------------------------------------------
    uint64_t dispatch_id() const { return dispatch_.load(std::memory_order_relaxed); }
    uint64_t flush_id()    const { return flush_.load(std::memory_order_acquire); }

    uint32_t in_flight() const { return static_cast<uint32_t>(dispatch_id() - flush_id()); }
    bool     full()      const { return in_flight() >= Capacity; }
    bool     quiesced()  const { return dispatch_id() == flush_id(); }

    // Claim the next slot to fill. Null when the window is full — the caller must stop parsing and
    // let the sender drain. That backpressure is what bounds memory under a client that pipelines
    // without reading, and dropping it is how a server OOMs on one misbehaving connection.
    // Materialization happens HERE and only here — the parser is the sole allocator, so workers
    // and the sender only ever dereference slots that a publish made real.
    Op* acquire(uint8_t route_flags = 0) {
        if (full()) return nullptr;
        Op* op = slot(static_cast<uint32_t>(dispatch_id()) & kMask, true);
        op->reset(route_flags);
        return op;
    }

    // Publish the slot claimed by acquire(). RELEASE: everything written into the slot must be
    // visible to the consumer before it can observe the new dispatch_id. Separate from acquire()
    // because the parser may abandon a half-built op without advancing the ROB.
    void publish() { dispatch_.store(dispatch_id() + 1, std::memory_order_release); }

    // Undo the last publish(). Only legal while the op is still un-dispatched, i.e. no worker can
    // have marked it Done -- which is exactly the refused-dispatch path. Safe even if a sender has
    // already observed the higher dispatch_: it can only have seen the slot as not-Done and stopped,
    // because retirement never touches an op that is not Done.
    void unpublish() { dispatch_.store(dispatch_id() - 1, std::memory_order_release); }

    // ---- consumer side (whichever stage sends) -------------------------------------------------
    // Retire every completed op from the head, in order, handing each reply to `sink`. Stops at the
    // first op still running — a later op finishing early must wait, which is the whole point.
    template <typename Sink>
    uint32_t drain(Sink&& sink) {
        const uint64_t d = dispatch_.load(std::memory_order_acquire);
        uint64_t f = flush_.load(std::memory_order_relaxed);
        uint32_t n = 0;
        while (f != d) {
            Op& op = *slot(static_cast<uint32_t>(f) & kMask, false);
            if (op.state.load(std::memory_order_acquire) != OpState::Done) break;
            sink(op);                                   // the acquire above orders the reply bytes
            if (op.oversized()) op.shrink();            // bounded retention: bursts do not pin heap
            op.state.store(OpState::Free, std::memory_order_relaxed);
            f++;
            n++;
        }
        // One release store for the whole batch rather than one per op: the producer only needs to
        // learn the final position, and each store would otherwise bounce the line.
        if (n) flush_.store(f, std::memory_order_release);
        return n;
    }

    // Slot access for the worker side, which addresses ops by id rather than by pointer so a stale
    // pointer can never outlive a recycle.
    Op& at(uint64_t id) { return *slot(static_cast<uint32_t>(id) & kMask, false); }
    const Op& at(uint64_t id) const { return const_cast<Rob*>(this)->at(id); }

    ~Rob() { for (uint32_t i = 0; i < kChunks; i++) delete[] chunks_[i]; }

private:
    static constexpr uint32_t kChunkOps = 8;
    static constexpr uint32_t kChunks   = Capacity / kChunkOps;
    static_assert(Capacity % kChunkOps == 0);

    // may_grow is true only from acquire() — the parser. Everyone else dereferences ground the
    // parser already materialized.
    __attribute__((always_inline)) Op* slot(uint32_t idx, bool may_grow) {
        Op*& ch = chunks_[idx / kChunkOps];
        if (!ch && may_grow) ch = new Op[kChunkOps];
        return &ch[idx % kChunkOps];
    }

    Op* chunks_[kChunks] = {};
    // Separate cache lines: the producer writes dispatch_ while the consumer writes flush_, and
    // sharing a line would make every publish invalidate the consumer's copy and vice versa.
    alignas(64) std::atomic<uint64_t> dispatch_{0};
    alignas(64) std::atomic<uint64_t> flush_{0};
};

}  // namespace tomo
