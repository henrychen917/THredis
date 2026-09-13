// kv_block_cache.h — the fused owner's post-grace KvObj block cache.
//
// WHY IT EXISTS. While read-local is armed a published object is immutable: readers hold no lock,
// so a write may never overwrite one in place. Every armed write therefore builds a FRESH object,
// publishes it, and retires the displaced one through QSBR — one mallocx and one sdallocx per
// write that the unarmed in-place path does not pay at all.
//
// WHY REUSE IS SAFE. Once the QSBR grace floor has passed a retired object's stamp, no reader can
// still hold a pointer to it, and by the single-owner law no thread but its shard's owner may
// touch it. That is exactly the licence the code already used to call free() at that point. This
// cache is fed at precisely that call site and drained only by the same owner thread, so:
//   * single-owner writes are preserved — the cache is owner-private, never shared, never locked;
//   * readers are never obstructed — nothing is recycled before the grace floor has released it;
//   * immutable replacement is preserved — a reused block becomes a DIFFERENT object than any
//     reader can be holding, because the block only returns here after every such reader is gone.
// Recycling reuses provably unreachable memory. It does not shorten any object's lifetime.
//
// WHY PER OWNER AND NOT PER SHARD. Measured, not assumed. A per-shard variant of this cache was
// built first and lost 2-4% of armed SET throughput at 8 and 64 shards while winning at 1 shard:
// its head array and byte gauge are two extra cache lines PER SHARD on a path whose allocator
// alternative (jemalloc's tcache) is per-THREAD and always hot. One cache per fused owner restores
// that property — a single head array, hot in L1 for every write regardless of which shard the key
// lands on — and a shard ownership handoff simply changes which owner's cache serves it next,
// without any cache ever becoming cross-thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "../base/alloc.h"
#include "kvobj.h"
#include "read_local_reclaim.h"
#ifdef TOMO_RL_CACHE_DEBUG
#include <cstdio>
#include <unordered_set>
#include <unistd.h>
#include <sys/syscall.h>
#endif

namespace tomo {

// Size-class index for a good_size()-rounded allocation. Injective over good_size outputs, which
// is what lets a class head hold exactly one request size and a take() skip any header decode.
// This is the table AtomicPendingState's value/entry pools have always used; atomic_pool_class()
// forwards here so the two can never drift apart.
inline uint32_t kv_block_class(size_t allocation) {
    if (allocation <= 8) return 0;
    if (allocation <= 128) return static_cast<uint32_t>(allocation / 16);
    const int k = 63 - __builtin_clzll(static_cast<unsigned long long>(allocation - 1));
    const size_t step = size_t{1} << (k - 2);
    const uint32_t quarter = static_cast<uint32_t>(allocation / step);
    return 9u + 4u * static_cast<uint32_t>(k - 7) + (quarter - 5u);
}

struct KvBlockCache {
    // The list node is written OVER the retired object; a block is either a live KvObj or a node,
    // never both. 16 bytes, so any class this cache accepts can hold one.
    struct FreeBlock {
        FreeBlock* next;
        size_t allocation;
    };

    // Classes through ~112 KiB, the same span the shard pools accept. Larger blocks fall straight
    // through to the allocator: they are collection or huge-key objects, not what a SET builds.
    static constexpr uint32_t kClasses = 48;

    // THE BOUND, derived rather than tuned.
    //
    //  * kMaxNodesPerClass — one grace drain's worth. A drain releases at most one retire ring of
    //    objects, so a class that refuses blocks below that number is refusing memory the very
    //    next owner pass will ask for again. Measured: with a 32-node cap the reuse rate collapses
    //    to a few percent and the whole mechanism stops paying (see NOTES-RECYCLE.md).
    //  * kMaxBytes — one grace drain of maximum-inline-value objects. The retire ring ALREADY
    //    permits kReadLocalRetireRingCapacity retired-but-unreclaimed objects per owner to be
    //    resident, and kEmbedThreshold is the largest value an eligible block can carry, so this
    //    cache adds at most the transient the QSBR machinery already tolerates: 768 KiB per fused
    //    owner thread, and nothing per thread that is not one.
    //
    // Callers additionally pass a per-put ceiling (the writing shard's own live object footprint),
    // so an empty or shrinking keyspace shrinks the cache instead of pinning its high-water mark.
    static constexpr uint32_t kMaxNodesPerClass = kReadLocalRetireRingCapacity;
    static constexpr size_t kMaxBytes =
        static_cast<size_t>(kReadLocalRetireRingCapacity) * kEmbedThreshold;

    static bool eligible(size_t allocation) { return allocation >= sizeof(FreeBlock); }

#ifdef TOMO_RL_CACHE_DEBUG
    // DEBUG-BUILD INVARIANTS (not compiled into the shipped binary).
    //
    // The two laws this cache rests on are (1) it is owner-private -- one thread, no lock -- and
    // (2) a block is resident exactly once. Both are violated silently: a cross-thread splice or a
    // double return corrupts the list, and the damage only surfaces at some LATER take() as a wild
    // head. These checks name the violation at the instant it happens, with the block address and
    // both thread ids, instead of leaving a segfault three hundred operations downstream.
    long dbg_owner_tid = 0;
    std::unordered_set<void*> dbg_resident;

    // `fatal` is false for release_all only: the graceful-shutdown path legitimately drains every
    // owner's queue from the main thread AFTER pool.join(), so that one crossing is single-threaded
    // by construction. It is still reported, once, because the same entry point is reachable from
    // FLUSH and from maxmemory pressure, where a crossing would NOT be benign.
    void dbg_check_owner(const char* op, bool fatal = true) {
        const long tid = static_cast<long>(::syscall(SYS_gettid));
        if (!dbg_owner_tid) { dbg_owner_tid = tid; return; }
        if (dbg_owner_tid != tid) {
            std::fprintf(stderr,
                "\nRLCACHE-%s owner: %s on cache %p from tid %ld, owner tid %ld\n",
                fatal ? "VIOLATION" : "CROSSING", op, static_cast<void*>(this), tid,
                dbg_owner_tid);
            std::fflush(stderr);
            if (fatal) std::abort();
        }
    }
    void dbg_enter(void* memory, size_t allocation, uint32_t cls) {
        if (!dbg_resident.insert(memory).second) {
            std::fprintf(stderr,
                "\nRLCACHE-VIOLATION double-put: block %p allocation %zu class %u already "
                "resident (cache %p, class_nodes %u, bytes %zu)\n",
                memory, allocation, cls, static_cast<void*>(this), class_nodes[cls], bytes);
            std::fflush(stderr);
            std::abort();
        }
    }
    void dbg_leave(void* memory, size_t allocation, uint32_t cls) {
        if (dbg_resident.erase(memory) != 1) {
            std::fprintf(stderr,
                "\nRLCACHE-VIOLATION take-of-nonresident: block %p allocation %zu class %u "
                "(cache %p, class_nodes %u, bytes %zu)\n",
                memory, allocation, cls, static_cast<void*>(this), class_nodes[cls], bytes);
            std::fflush(stderr);
            std::abort();
        }
    }
    // The list a class actually holds must match its counter, and every block on it must carry
    // the class's own request size. A clobbered `next` is caught HERE, on the operation after the
    // write that clobbered it, rather than when it is finally dereferenced.
    uint64_t dbg_walk_countdown = 0;
    // The walk is O(list length) and the list can hold thousands of nodes, so running it on every
    // operation slowed the reclaim path enough to close the race this build exists to catch. The
    // O(1) residency checks stay on every operation; the walk samples.
    void dbg_walk(uint32_t cls, size_t allocation, const char* op) {
        if (dbg_walk_countdown--) return;
        dbg_walk_countdown = 1023;
        uint32_t seen = 0;
        for (FreeBlock* b = heads[cls]; b; b = b->next) {
            if (!dbg_resident.count(b)) {
                std::fprintf(stderr,
                    "\nRLCACHE-VIOLATION %s: class %u list holds non-resident block %p at "
                    "depth %u (cache %p)\n", op, cls, static_cast<void*>(b), seen,
                    static_cast<void*>(this));
                std::fflush(stderr);
                std::abort();
            }
            if (b->allocation != allocation) {
                std::fprintf(stderr,
                    "\nRLCACHE-VIOLATION %s: class %u block %p carries allocation %zu, class "
                    "size %zu, depth %u (cache %p)\n", op, cls, static_cast<void*>(b),
                    b->allocation, allocation, seen, static_cast<void*>(this));
                std::fflush(stderr);
                std::abort();
            }
            if (++seen > class_nodes[cls]) {
                std::fprintf(stderr,
                    "\nRLCACHE-VIOLATION %s: class %u list longer than its counter %u "
                    "(cycle or lost update; cache %p)\n", op, cls, class_nodes[cls],
                    static_cast<void*>(this));
                std::fflush(stderr);
                std::abort();
            }
        }
        if (seen != class_nodes[cls]) {
            std::fprintf(stderr,
                "\nRLCACHE-VIOLATION %s: class %u list length %u != counter %u (cache %p)\n",
                op, cls, seen, class_nodes[cls], static_cast<void*>(this));
            std::fflush(stderr);
            std::abort();
        }
    }
#endif

    // `allocation` is already good_size()-rounded by the caller, so the class identifies exactly
    // one request size and the returned block needs no header decode. Returns null on a miss; the
    // caller then allocates, which is the unchanged baseline path.
    void* take(size_t allocation) {
        const uint32_t cls = kv_block_class(allocation);
        if (cls >= kClasses) return nullptr;
#ifdef TOMO_RL_CACHE_DEBUG
        dbg_check_owner("take");
        dbg_walk(cls, allocation, "take-entry");
#endif
        FreeBlock* block = heads[cls];
        if (!block) return nullptr;
        if (block->allocation != allocation || !class_nodes[cls] || bytes < allocation)
            std::abort();
#ifdef TOMO_RL_CACHE_DEBUG
        dbg_leave(block, allocation, cls);
#endif
        heads[cls] = block->next;
        class_nodes[cls]--;
        bytes -= allocation;
        return block;
    }

    // Post-grace return. `ceiling` is the caller's own admission clause (see kMaxBytes above);
    // false means "not cached", and the caller must free the block on its unchanged path.
    bool put(void* memory, size_t allocation, size_t ceiling) {
        if (!eligible(allocation)) return false;
        const uint32_t cls = kv_block_class(allocation);
        if (cls >= kClasses || class_nodes[cls] == kMaxNodesPerClass) return false;
        const size_t limit = ceiling < kMaxBytes ? ceiling : kMaxBytes;
        if (allocation > limit - (bytes < limit ? bytes : limit)) return false;
        // An immediate double-return would splice the list into a self-loop, which take() would
        // only notice much later (when class_nodes hits 0 with a non-null head). The allocator's
        // own double-free detector is what caught this class of bug before the cache existed; say
        // so at the same instant instead. One predicted-false compare, on the reclaim path only.
        if (heads[cls] == memory) std::abort();
#ifdef TOMO_RL_CACHE_DEBUG
        dbg_check_owner("put");
        dbg_walk(cls, allocation, "put-entry");
        dbg_enter(memory, allocation, cls);
#endif
        auto* block = reinterpret_cast<FreeBlock*>(memory);
        block->next = heads[cls];
        block->allocation = allocation;
        heads[cls] = block;
        class_nodes[cls]++;
        bytes += allocation;
        return true;
    }

    // Hand every cached block back to the allocator. Owner-thread only, like everything here.
    void release_all() {
#ifdef TOMO_RL_CACHE_DEBUG
        dbg_check_owner("release_all", false);
#endif
        if (!bytes) return;
        for (uint32_t cls = 0; cls < kClasses; cls++) {
            while (FreeBlock* block = heads[cls]) {
                heads[cls] = block->next;
#ifdef TOMO_RL_CACHE_DEBUG
                dbg_leave(block, block->allocation, cls);
#endif
                free_sized(block, block->allocation);
            }
            class_nodes[cls] = 0;
        }
        bytes = 0;
    }

    FreeBlock* heads[kClasses] = {};
    uint32_t class_nodes[kClasses] = {};
    size_t bytes = 0;
};

}  // namespace tomo
