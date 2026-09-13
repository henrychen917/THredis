// exqueue.h — the IO -> EX dispatch hop. One SPSC ring per (io thread, worker) pair.
//
// SPSC BY CONSTRUCTION. Each ring has exactly one producer (a given IO thread) and one consumer
// (a given worker). With N io threads and M workers there are N*M rings. That is more memory than a
// single MPSC inbox per worker, and it is worth it: an MPSC queue needs an atomic RMW per push from
// every producer, whereas SPSC needs only a release store. The fork measured the handoff cost as
// instruction volume rather than stalls, so removing the RMW is the direct lever.
//
// TWO CACHE LINES, NOT ONE. head and tail sit on separate lines. If they share, the producer's
// store to tail invalidates the line the consumer is reading head from on every single push — the
// classic false-sharing own-goal that makes a "lock-free" queue slower than a mutex.
//
// INDEX CACHING (DPDK-style). The producer keeps its own stale copy of head and only re-reads the
// real one when its cached copy says the ring is full. The consumer does the same with tail. In the
// common case neither thread touches the other's line at all.
#pragma once
#include <atomic>
#include <cstdint>
#include <new>

namespace tomo {

inline constexpr size_t kCacheLine = 64;

template <typename T, uint32_t Capacity>
class ExQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static constexpr uint32_t kMask = Capacity - 1;

public:
    // Producer side. Returns false when full; the caller must NOT drop the op on the floor —
    // ignoring a full queue is exactly the dropped-dispatch bug that lost replies and wedged the
    // connection in the fork. Back off and retry, or apply backpressure.
    bool push(T v) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        const uint32_t next = t + 1;
        if (next - head_cached_ > Capacity) {                  // cached says full: re-read
            head_cached_ = head_.load(std::memory_order_acquire);
            if (next - head_cached_ > Capacity) return false;   // genuinely full
        }
        slots_[t & kMask] = v;
        tail_.store(next, std::memory_order_release);           // publishes the slot write
        return true;
    }

    // Producer-side bundle publication. Capacity is checked against one refreshed consumer
    // frontier and every slot is initialized before the single release-store of tail. This is the
    // same SPSC proof as push(), but a scatter group that touches several shards on one executor
    // pays one publication instead of one per shard task.
    bool push_batch(const T* values, uint32_t count) {
        if (!count) return true;
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        const uint32_t next = t + count;
        if (next - head_cached_ > Capacity) {
            head_cached_ = head_.load(std::memory_order_acquire);
            if (next - head_cached_ > Capacity) return false;
        }
        for (uint32_t i = 0; i < count; i++) slots_[(t + i) & kMask] = values[i];
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Producer-only group reservation check. Unlike depth(), this acquire-refreshes the consumer
    // frontier and is safe for the all-or-nothing scatter preflight. The producer performs no
    // intervening unrelated pushes between this check and its group enqueue.
    uint32_t producer_free_slots() const {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        return Capacity - (tail - head);
    }

    // Consumer side.
    bool pop(T& out) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cached_) {                                // cached says empty: re-read
            tail_cached_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cached_) return false;
        }
        out = slots_[h & kMask];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Called by the consumer AFTER the popped op has actually executed. This is not bookkeeping
    // pedantry: `head` advances at pop time, so head == tail means "nothing left to take", NOT
    // "nothing in flight" — the worker may still be executing what it just popped. Any teardown,
    // reshard, or role conversion that tests head == tail will race against an op that is mid-
    // execution. The fork learned this the hard way and carries the same separate frontier.
    void retire() { retired_.store(retired_.load(std::memory_order_relaxed) + 1,
                                   std::memory_order_release); }

    // THE quiescence predicate. Use this one, never depth() == 0.
    bool quiesced() const {
        return retired_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    // Approximate — for stats and the flip controller's pressure signal, never for control flow.
    uint32_t depth() const {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }

private:
    alignas(kCacheLine) std::atomic<uint32_t> head_{0};
    // Written by the consumer only, and deliberately on the consumer's line beside head_: the
    // producer never reads it on the hot path, only a teardown/reshard path testing quiesced().
    std::atomic<uint32_t> retired_{0};
    uint32_t tail_cached_ = 0;          // consumer-private, shares the consumer's line

    alignas(kCacheLine) std::atomic<uint32_t> tail_{0};
    uint32_t head_cached_ = 0;          // producer-private, shares the producer's line

    alignas(kCacheLine) T slots_[Capacity];
};

}  // namespace tomo
