// flatstore.h — the keyspace table. Replaces Redis's dict/kvstore (NOT its RDB, which is a
// persistence format we have not built).
//
// ONE TABLE PER SHARD, executed by exactly one worker at a time. In the fork the table was per-NODE
// and shared by several workers, which forced it to be lock-free with QSBR — not because ownership
// was shared (it never was; ownership is per key) but because open addressing lets worker A's probe
// walk through slots holding worker B's keys. One table per shard removes that whole class of
// problem: no atomics, no CAS, no epoch reclamation, no probe interference.
//
// ============================================================================================
// WHAT IS NEVER COPIED, AND WHAT MOVES
//
//   KvObj (key + value bytes)   NEVER copied and NEVER moved, by anything, ever. A key's storage is
//                               allocated once and freed once. Every operation below moves at most
//                               an 8-byte slot word that POINTS at it.
//
//   Shard migration (LB)        moves NOTHING AT ALL. Reassigning a shard rewrites only the EX bits
//                               in Router's bucket entries behind one descriptor commit. No table
//                               is touched, no pointer is rehashed. Pure ownership handoff.
//
//   Table resize                rehashes 8-byte SLOT WORDS into a larger or smaller array, because
//                               an open-addressed slot index is a function of the capacity. This is
//                               pointer movement, not data movement — the objects those pointers
//                               name do not budge.
//
// ============================================================================================
// RESIZE IS INCREMENTAL. NOTHING STOPS THE WORLD.
//
// Rehashing a large table in one pass is a multi-second stall on the write tail — the fork measured
// exactly that and had to move to serve-while-copy to turn a 2.4 s p99.99 into 39 ms. So this works
// the way Redis's dict does: allocate the new table, keep the old one, and migrate a BOUNDED number
// of slots on every subsequent operation. No operation pays more than that bound, so the tail stays
// flat while the table grows underneath the workload.
//
//   t_[0]  the CURRENT table. Every insert goes here. Always present.
//   t_[1]  the OLD table, present only while rehashing. Drains, then is freed.
//
// THE RESURRECTION HAZARD, easy to get wrong and silently wrong. A key can still sit in the old
// table when a new value for it is inserted into the current one. Delete the current copy and the
// old one becomes visible again — the key returns from the dead. So insert evicts any old-table copy
// first, and erase checks both tables. Exactly ONE copy of a key exists at any instant, which is the
// invariant every lookup depends on.
// ============================================================================================
//
// SLOT ENCODING — one 8-byte word, eight slots per cache line:
//
//   [63:49] 15-bit tag   high bits of the hash; rejects a non-matching probe without touching the key
//   [48]    TOMB
//   [47:0]  KvObj*       boot verifies the allocator stays in the low 48-bit VA range; debug packs
//                       assert the same invariant before using the top bits
//
//   word == 0             EMPTY  the calloc state, and the ONLY thing that stops a probe
//   ptr != 0              LIVE
//   word != 0, ptr == 0   DEAD   tombstone; reusable, never stops a probe
//
// THE CLUSTERING TRAP, kept because it cost real time: the router consumes the LOW bits of the same
// hash that indexes this table. Index with `h & mask` and the routing bits are frozen for every key
// a shard owns — only 1/nshards of slots are natural homes, and linear probing degenerates into
// kilo-slot runs. Measured at ~6,600 probes per lookup. slot_start() mixes again before taking the
// index; that alone was worth 2.5-2.7x.
#pragma once
#include <chrono>
#include <thread>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "../base/alloc.h"
#include "../core/atomic_tripwire.h"
#include "eviction.h"
#include "../cmd/notify.h"
#include "kv_block_cache.h"
#include "kvobj.h"
#include "atomic_mvcc.h"
#include "foreign_read_safety.h"
#include "read_local_reclaim.h"
#include "../snapshot/format.h"
#include "../persist/aof.h"

#ifndef TOMO_READ_LOCAL_RECLAIM_PREFETCHW
#define TOMO_READ_LOCAL_RECLAIM_PREFETCHW 0
#endif

namespace tomo {

static_assert(TOMO_READ_LOCAL_RECLAIM_PREFETCHW == 0 ||
              TOMO_READ_LOCAL_RECLAIM_PREFETCHW == 1,
              "TOMO_READ_LOCAL_RECLAIM_PREFETCHW must be 0 (off) or 1 (on)");
inline constexpr bool kReadLocalReclaimPrefetchw =
    TOMO_READ_LOCAL_RECLAIM_PREFETCHW != 0;

// Fault injection for the cold table-allocation paths. It is compiled in whenever NDEBUG is not
// defined -- which is every build the Makefile produces, release included -- and costs one relaxed
// load per table allocation, never anything on the request path. Defining NDEBUG removes this
// surface (and every assert() in the tree) as a build-system decision.
#ifndef NDEBUG
inline std::atomic<uint32_t> g_flatstore_table_alloc_failures{0};

inline void flatstore_debug_fail_table_allocations(uint32_t count) {
    g_flatstore_table_alloc_failures.store(count, std::memory_order_relaxed);
}

inline bool flatstore_debug_consume_table_alloc_failure() {
    uint32_t remaining = g_flatstore_table_alloc_failures.load(std::memory_order_relaxed);
    while (remaining) {
        if (g_flatstore_table_alloc_failures.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_relaxed)) return true;
    }
    return false;
}
#endif

inline void* flatstore_table_calloc(size_t count, size_t width) {
#ifndef NDEBUG
    if (flatstore_debug_consume_table_alloc_failure()) return nullptr;
#endif
    return std::calloc(count, width);
}

struct ScatterState;
uint64_t xshard_atomic_key_hash(const ScatterState* state, uint32_t ordered_index);
Slice xshard_atomic_key_slice(const ScatterState* state, uint32_t ordered_index);

inline uint64_t mix64(uint64_t h);

// Expiring hashes only, not keys or object pointers: object replacement never invalidates this
// index. State is a byte sidecar so all 64-bit hash values remain representable while occupied
// slots themselves stay densely packed. Sampling advances a persistent cursor and examines at most
// its caller's budget, including empty slots; no pass can accidentally turn into a keyspace walk.
//
// GROWTH AND SHRINK ARE BOTH BOUNDED PER OPERATION, for the same reason the main table's are:
//   - Growth moves at most kMigrateSlotsPerOp old slots per operation into a second table, exactly
//     the two-table shape FlatStore::rehash_step() already uses. A single-pass move of a million
//     deadlines stalled its owning executor -- and therefore its whole shard -- for tens of
//     milliseconds inside one ordinary SET.
//   - Reaching zero live deadlines RELEASES the sidecar instead of clearing every state byte. The
//     old clear was O(capacity) on a transition a workload can hit on every key, and capacity was
//     monotone in the all-time-high volatile population, so one historical burst taxed every later
//     live->0 forever. Releasing is O(1) and makes the footprint follow the CURRENT population.
// A table at or below the minimum keeps its buffer and clears the 16 state bytes, so the ordinary
// "a few volatile keys come and go" case stays allocation-free.
class ExpireIndex {
public:
    ExpireIndex() = default;
    ~ExpireIndex() { std::free(hashes_[0]); std::free(sidecars_[0]);
                     std::free(hashes_[1]); std::free(sidecars_[1]); }
    ExpireIndex(const ExpireIndex&) = delete;
    ExpireIndex& operator=(const ExpireIndex&) = delete;

    uint32_t size() const { return live_[0] + live_[1]; }
    // A move in flight is PENDING WORK for the owner, not a quiet background state: half the
    // index is parked in the old table, and an owner that sleeps on a barren sampling pass stops
    // expiring until the next command wakes it.
    bool migrating() const { return cap_[1] != 0; }
    size_t memory_bytes() const {
        return static_cast<size_t>((static_cast<uint64_t>(cap_[0]) + cap_[1]) *
                                   (sizeof(uint64_t) + sizeof(uint8_t) +
                                    (kTtlDeadlineSidecar ? sizeof(int64_t) : 0)));
    }
    void clear() {
        release(0);
        release(1);
        cursor_ = 0;
        migrate_ = 0;
    }

    bool insert(uint64_t hash, int64_t deadline = kNoTtlDeadline) {
        if (rehashing()) {
            migrate(kMigrateSlotsPerOp);
            // Backstop only. At the shipped step rate the new table is at most ~41% loaded when the
            // move ends, so this cannot fire; finishing the move is still cheaper than letting an
            // insert run out of slots.
            if (rehashing() &&
                (static_cast<uint64_t>(live_[1]) + tombs_[1] + 1) * 100 >=
                    static_cast<uint64_t>(cap_[1]) * 70)
                finish_migration();
        }
        if (!cap_[0] && !allocate(0, kMinCap)) return false;
        if (!rehashing() &&
            (static_cast<uint64_t>(live_[0]) + tombs_[0] + 1) * 100 >=
                static_cast<uint64_t>(cap_[0]) * 70) {
            uint64_t wanted = cap_[0];
            if (static_cast<uint64_t>(live_[0]) * 2 >= cap_[0]) {
                if (wanted > std::numeric_limits<size_t>::max() / uint64_t{2}) return false;
                wanted *= uint64_t{2};
            }
            if (!begin_rehash(static_cast<size_t>(wanted))) return false;
            migrate(kMigrateSlotsPerOp);
        }
        // A deadline re-registered while the move is in flight must leave the old table, or live_
        // double-counts it until the migration catches up.
        if (rehashing()) (void)erase_in(0, hash);
        return insert_raw(rehashing() ? 1 : 0, hash, deadline);
    }

    // Erasing from an index with nothing LIVE in it is a PROVEN no-op, not an approximation:
    // erase_in() only ever clears a kLive slot, and it changes nothing else. So this guard is
    // exact, and it is the only skip that is. Do NOT make the erase conditional on "the key has a
    // deadline" instead -- see track_expire() for the two things that would break.
    bool erase(uint64_t hash) {
        if (size() == 0) return false;
        bool removed = erase_in(1, hash);
        if (!removed) removed = erase_in(0, hash);
        if (removed && size() == 0) collapse_empty();
        return removed;
    }

    // Selector-on prototype: owner-side TTL reads can pay one probe here while ordinary keys pay
    // none.  The inline slot remains the authoritative fallback because this hash-only index
    // cannot yet distinguish an exact 64-bit collision or preserve every MVCC version.
    int64_t deadline(uint64_t hash, int64_t fallback) const {
        if constexpr (!kTtlDeadlineSidecar) return fallback;
        int64_t value = fallback;
        if (deadline_in(1, hash, value) || deadline_in(0, hash, value)) return value;
        return fallback;
    }

    template <typename Fn>
    uint32_t sample(uint32_t budget, Fn&& fn) {
        // Sampling is the pass that still runs when writes have stopped, so it is also what
        // guarantees a migration finishes rather than sitting half-done forever.
        if (rehashing()) migrate(kMigrateSlotsPerOp);
        uint32_t checked = 0;
        // Capacities are re-read every iteration: fn() may erase the last live deadline, and that
        // releases the tables underneath this loop.
        while (checked < budget) {
            // Slots below migrate_ have already been moved out; sampling them would spend the
            // budget on a region that cannot hold a deadline, and a barren pass is exactly what
            // makes an idle owner park instead of expiring.
            const size_t old_left = cap_[0] - migrate_;
            const size_t total = old_left + cap_[1];
            if (!total || size() == 0) break;
            if (cursor_ >= total) cursor_ = 0;
            const size_t pos = cursor_++;
            checked++;
            if (pos < old_left) {
                const size_t slot = migrate_ + pos;
                if (states(0)[slot] == kLive) fn(hashes_[0][slot]);
            } else if (states(1)[pos - old_left] == kLive) {
                fn(hashes_[1][pos - old_left]);
            }
        }
        return checked;
    }

    // Best-effort random live hash selection. Both the random probes and sparse-table cursor
    // fallback are bounded; callers count a miss as part of their sampling budget.
    bool random_hash(uint64_t random, uint32_t attempts, uint64_t& out) {
        if (size() == 0) return false;
        for (uint32_t i = 0; i < attempts; i++)
            for (int t = 0; t < 2; t++) {
                if (!cap_[t] || !live_[t]) continue;
                const size_t pos = static_cast<size_t>(mix64(random + i * 2 + t)) & (cap_[t] - 1);
                if (states(t)[pos] == kLive) { out = hashes_[t][pos]; return true; }
            }
        for (uint32_t i = 0; i < attempts; i++) {
            const size_t old_left = cap_[0] - migrate_;
            const size_t total = old_left + cap_[1];
            if (!total) return false;
            if (cursor_ >= total) cursor_ = 0;
            const size_t pos = cursor_++;
            if (pos < old_left) {
                const size_t slot = migrate_ + pos;
                if (states(0)[slot] == kLive) { out = hashes_[0][slot]; return true; }
            } else if (states(1)[pos - old_left] == kLive) {
                out = hashes_[1][pos - old_left];
                return true;
            }
        }
        return false;
    }

private:
    static constexpr uint8_t  kEmpty = 0;
    static constexpr uint8_t  kLive  = 1;
    static constexpr uint8_t  kTomb  = 2;
    static constexpr size_t   kMinCap = 16;
    // Old slots moved per operation. Same reasoning (and same number) as
    // FlatStore::kRehashSlotsPerOp: large enough that the move finishes long before the new table
    // needs one of its own, small enough that no single operation visibly stalls.
    static constexpr uint32_t kMigrateSlotsPerOp = 8;

    bool rehashing() const { return cap_[1] != 0; }

    uint8_t* states(int t) const {
        if (!sidecars_[t]) return nullptr;
        auto* bytes = sidecars_[t];
        if constexpr (kTtlDeadlineSidecar) bytes += cap_[t] * sizeof(int64_t);
        return bytes;
    }

    int64_t* deadlines(int t) const {
        if constexpr (!kTtlDeadlineSidecar) return nullptr;
        return reinterpret_cast<int64_t*>(sidecars_[t]);
    }

    void release(int t) {
        std::free(hashes_[t]);
        std::free(sidecars_[t]);
        hashes_[t] = nullptr;
        sidecars_[t] = nullptr;
        cap_[t] = 0;
        live_[t] = tombs_[t] = 0;
    }

    // calloc, exactly as FlatStore::alloc_table does, and for the same reason: a large table must
    // arrive as demand-zero pages. Writing the zeroes here would put the whole new sidecar back on
    // one operation's critical path -- which is the stall this change exists to remove.
    bool allocate(int t, size_t cap) {
        auto* hashes = static_cast<uint64_t*>(flatstore_table_calloc(cap, sizeof(uint64_t)));
        const size_t sidecar_width = sizeof(uint8_t) +
                                     (kTtlDeadlineSidecar ? sizeof(int64_t) : 0);
        auto* sidecar = static_cast<uint8_t*>(flatstore_table_calloc(cap, sidecar_width));
        if (!hashes || !sidecar) { std::free(hashes); std::free(sidecar); return false; }
        std::free(hashes_[t]);
        std::free(sidecars_[t]);
        hashes_[t] = hashes;
        sidecars_[t] = sidecar;
        cap_[t] = cap;
        live_[t] = tombs_[t] = 0;
        return true;
    }

    __attribute__((noinline, cold))
    void collapse_empty() {
        if (!rehashing() && cap_[0] <= kMinCap) {
            std::memset(states(0), kEmpty, cap_[0]);
            tombs_[0] = 0;
            cursor_ = 0;
            return;
        }
        release(0);
        release(1);
        cursor_ = 0;
        migrate_ = 0;
    }

    bool begin_rehash(size_t cap) {
        if (!allocate(1, cap)) return false;
        migrate_ = 0;
        return true;
    }

    // A moved slot becomes a TOMBSTONE, never kEmpty: the old table is still being probed by
    // erase_in() while the move runs, and kEmpty terminates a probe run. Punching holes made a
    // still-present deadline unfindable, which showed up as INFO `expires` over-reporting after
    // PERSIST.
    void migrate(uint32_t slots) {
        while (slots && migrate_ < cap_[0]) {
            const size_t pos = migrate_++;
            slots--;
            if (states(0)[pos] != kLive) continue;
            const int64_t deadline = kTtlDeadlineSidecar ? deadlines(0)[pos] : kNoTtlDeadline;
            states(0)[pos] = kTomb;
            live_[0]--;
            tombs_[0]++;
            // Cannot fail: the destination was sized for every live entry plus headroom.
            (void)insert_raw(1, hashes_[0][pos], deadline);
        }
        if (migrate_ >= cap_[0]) finish_migration();
    }

    void finish_migration() {
        while (migrate_ < cap_[0]) {
            const size_t pos = migrate_++;
            if (states(0)[pos] != kLive) continue;
            const int64_t deadline = kTtlDeadlineSidecar ? deadlines(0)[pos] : kNoTtlDeadline;
            states(0)[pos] = kTomb;
            live_[0]--;
            tombs_[0]++;
            (void)insert_raw(1, hashes_[0][pos], deadline);
        }
        std::free(hashes_[0]);
        std::free(sidecars_[0]);
        hashes_[0] = hashes_[1];
        sidecars_[0] = sidecars_[1];
        cap_[0] = cap_[1];
        live_[0] = live_[1];
        tombs_[0] = tombs_[1];
        hashes_[1] = nullptr;
        sidecars_[1] = nullptr;
        cap_[1] = 0;
        live_[1] = tombs_[1] = 0;
        migrate_ = 0;
        cursor_ = 0;
    }

    size_t start(int t, uint64_t hash) const {
        return static_cast<size_t>(mix64(hash)) & (cap_[t] - 1);
    }

    bool insert_raw(int t, uint64_t hash, int64_t deadline) {
        const size_t cap = cap_[t];
        if (!cap) return false;
        size_t pos = start(t, hash);
        size_t first_tomb = cap;
        for (size_t probes = 0; probes < cap; probes++) {
            if (states(t)[pos] == kEmpty) {
                if (first_tomb != cap) { pos = first_tomb; tombs_[t]--; }
                hashes_[t][pos] = hash;
                if constexpr (kTtlDeadlineSidecar) deadlines(t)[pos] = deadline;
                states(t)[pos] = kLive;
                live_[t]++;
                return true;
            }
            if (states(t)[pos] == kTomb) {
                if (first_tomb == cap) first_tomb = pos;
            } else if (hashes_[t][pos] == hash) {
                if constexpr (kTtlDeadlineSidecar) deadlines(t)[pos] = deadline;
                return true;
            }
            pos = (pos + 1) & (cap - 1);
        }
        return false;
    }

    bool erase_in(int t, uint64_t hash) {
        const size_t cap = cap_[t];
        if (!cap) return false;
        size_t pos = start(t, hash);
        for (size_t probes = 0; probes < cap; probes++) {
            if (states(t)[pos] == kEmpty) return false;
            if (states(t)[pos] == kLive && hashes_[t][pos] == hash) {
                states(t)[pos] = kTomb;
                live_[t]--;
                tombs_[t]++;
                return true;
            }
            pos = (pos + 1) & (cap - 1);
        }
        return false;
    }

    bool deadline_in(int t, uint64_t hash, int64_t& out) const {
        if constexpr (!kTtlDeadlineSidecar) return false;
        const size_t cap = cap_[t];
        if (!cap) return false;
        size_t pos = start(t, hash);
        for (size_t probes = 0; probes < cap; probes++) {
            if (states(t)[pos] == kEmpty) return false;
            if (states(t)[pos] == kLive && hashes_[t][pos] == hash) {
                out = deadlines(t)[pos];
                return true;
            }
            pos = (pos + 1) & (cap - 1);
        }
        return false;
    }

    uint64_t* hashes_[2] = {nullptr, nullptr};
    uint8_t*  sidecars_[2] = {nullptr, nullptr};
    size_t    cap_[2]    = {0, 0};
    uint32_t  live_[2]   = {0, 0};
    uint32_t  tombs_[2]  = {0, 0};
    size_t    cursor_ = 0;    // sampling cursor over the concatenation of both tables
    size_t    migrate_ = 0;   // next old-table slot to move while a migration is in flight
};
static_assert(sizeof(ExpireIndex) == 80, "ExpireIndex layout drift");
// 64-bit finalizer (murmur3 fmix64). Cheap, and it decorrelates the index bits from the router's.
// ---- hash hardening ---------------------------------------------------------------------------
// Collisions are a CORRECTNESS non-event (find_in compares full key bytes after the tag filter)
// but a deterministic public hash is a DoS surface: craft keys sharing the low 14 bits and one
// thread does all the work; craft keys sharing slot_start and the probe runs the clustering
// pathology on purpose. Two layers, both keyed at boot from the kernel:
//   - the default mix64 path folds a random seed, killing offline precomputation for free;
//   - --hash siphash switches to SipHash-1-2 (redis's choice since 4.0), the principled PRF
//     answer for adversarial deployments, at a measured (small) per-op cost.
// The kind is a boot-time enum read through one perfectly-predicted branch -- not a function
// pointer, which would tax every hash with an indirect call.
enum class HashKind : uint8_t { Mix64Seeded = 0, SipHash12 = 1 };
inline HashKind  g_hash_kind = HashKind::Mix64Seeded;
inline uint64_t  g_hash_seed = 0;          // set once at boot, before any thread runs
inline uint64_t  g_sip_k0 = 0, g_sip_k1 = 0;

inline uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

// SipHash-1-2, reference-faithful, little-endian loads via memcpy.
inline uint64_t siphash12(const char* p, size_t n) {
    uint64_t v0 = 0x736f6d6570736575ULL ^ g_sip_k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ g_sip_k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ g_sip_k0;
    uint64_t v3 = 0x7465646279746573ULL ^ g_sip_k1;
    auto round = [&] {
        v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
        v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
    };
    const size_t end = n & ~size_t(7);
    for (size_t i = 0; i < end; i += 8) {
        uint64_t m; std::memcpy(&m, p + i, 8);
        v3 ^= m; round(); v0 ^= m;
    }
    uint64_t b = static_cast<uint64_t>(n) << 56;
    for (size_t i = end; i < n; i++) b |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * (i - end));
    v3 ^= b; round(); v0 ^= b;
    v2 ^= 0xff; round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

inline uint64_t mix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

// ---- reverse-binary SCAN cursors ---------------------------------------------------------------
// THE guarantee every SCAN family member owes: an element present for the WHOLE iteration is
// returned at least once, no matter how many times the table was resized underneath it. A cursor
// that is a raw slot number cannot keep that promise -- doubling the table moves a key from a slot
// AHEAD of the cursor to one BEHIND it, and the key is silently never emitted.
//
// The fix is Redis's dictScan trick, and it needs exactly one property our tables already have:
// the home index is `hash & (2^k - 1)`, so a key's home in the small table is its home in the big
// table with the extra high bits masked off. Count the cursor in BIT-REVERSED order and the high
// bits therefore move FASTEST, which makes every family of homes that share the small table's low
// bits a contiguous block of the visiting order. A resize can then only move a key between homes
// that are on the same side of the cursor -- both already visited, or both still to come.
//
// The counter is: set every bit ABOVE the mask, reverse, add one, reverse back. The carry runs
// downward through the high bits and the result is again inside the mask, so any cursor a client
// hands back is self-correcting, and the walk ends at exactly 0 after one full cycle.
inline uint64_t scan_cursor_reverse_bits(uint64_t v) {
    v = ((v & 0x5555555555555555ULL) << 1)  | ((v >> 1)  & 0x5555555555555555ULL);
    v = ((v & 0x3333333333333333ULL) << 2)  | ((v >> 2)  & 0x3333333333333333ULL);
    v = ((v & 0x0f0f0f0f0f0f0f0fULL) << 4)  | ((v >> 4)  & 0x0f0f0f0f0f0f0f0fULL);
    v = ((v & 0x00ff00ff00ff00ffULL) << 8)  | ((v >> 8)  & 0x00ff00ff00ff00ffULL);
    v = ((v & 0x0000ffff0000ffffULL) << 16) | ((v >> 16) & 0x0000ffff0000ffffULL);
    return (v << 32) | (v >> 32);
}

inline uint64_t scan_cursor_next(uint64_t cursor, uint64_t mask) {
    cursor |= ~mask;
    cursor = scan_cursor_reverse_bits(cursor);
    cursor++;
    return scan_cursor_reverse_bits(cursor);
}

class FlatStore;
struct FlatStoreLayoutLock;
struct ReadLocalTablePublication;
struct ReadLocalTableMetrics;

// Armed stores extend the already-cold atomic pending allocation. Keeping AtomicPendingState first
// preserves atomic_pending_ and every FlatStore member/offset; disabled stores allocate precisely
// the historical AtomicPendingState body and nothing else.
struct ReadLocalStoreState {
    AtomicPendingState atomic;
    // Semantic generation only: atomic physical exchanges and broad/enumerable scopes.
    // Table publication never advances it. Point GET and large MGET retain atomic validation.
    std::atomic<uint64_t> atomic_sequence{0};
    ReadLocalRetireSink retire_sink{};
    uint32_t atomic_mutation_depth = 0;
    uint32_t pending_count = 0;
    // Fits the existing alignment hole; present whenever the read-local lane is armed.
    ReadLocalTablePublication* published_table = nullptr;
    ForeignReadSafety foreign_reads{};
};
static_assert(std::is_standard_layout_v<ReadLocalStoreState>);
static_assert(offsetof(ReadLocalStoreState, atomic) == 0);
#if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
static_assert(offsetof(ReadLocalStoreState, foreign_reads) == 1408);
static_assert(sizeof(ReadLocalStoreState) == 50624);
#endif

// Hash-field TTLs, defined in src/cmd/t_hash_ttl.cc. The store owns the ATTENTION (which keys carry
// field deadlines, and the bounded cycle that revisits them); the hash lane owns the reap itself,
// because only it knows the two hash representations. Returns true when no live field is left and
// the caller must delete the key; `reaped` counts fields removed.
bool hash_ttl_active_reap(FlatStore& store, KvObj* object, int64_t now_ms, uint32_t& reaped);

class FlatStore {

public:
    static constexpr uint64_t kPtrMask = (1ULL << 48) - 1;
    static constexpr uint64_t kTombBit = 1ULL << 48;
    static constexpr int      kLoadPct = 70;
    static constexpr uint32_t kMinCap  = 64;

    // OLD slots examined per operation. Large enough that a rehash finishes well before the table
    // needs another, small enough that no single operation visibly stalls. Bounded per op is the
    // entire point.
    static constexpr uint32_t kRehashSlotsPerOp = 8;

    // Eight slot bytes at the 70% target load cost 11.43 bytes per live key. Accounting rounds
    // that stable-state estimate to 12; transient dual tables, tombstones and allocator metadata
    // are deliberately outside the maxmemory model and documented in NOTES-EVICT.md.
    static constexpr size_t   kSlotOverheadPerKey = 12;
    static constexpr uint32_t kEvictionsPerOp = 16;
    static constexpr uint32_t kSampleProbeAttempts = 16;

    enum class InsertResult : uint8_t { Inserted, MaxmemoryOom, Failed };
    enum class OverwriteResult : uint8_t { Updated, NotPossible, MaxmemoryOom };
    enum class ReadLocalProbeResult : uint8_t { Hit, Missing, AtomicPending };

    // Stack-local result of the batch prefetch walk. `slot` identifies the exact word whose
    // acquire load produced `object`; execute must never dereference it, because doing so would
    // reload a concurrent replacement and defeat capture-at-prefetch. Rotation QSBR keeps both
    // pointers allocated until the fused pass returns. Keep this a trivial, uninitialized aggregate:
    // hot-path capture arrays assign every consumed entry, so default member stores would be waste.
    struct ReadLocalPrefetchCapture {
        ReadLocalProbeResult result;
        const uint64_t* slot;
        const KvObj* object;
        uint64_t state;
    };

    struct ReadLocalTable {
        uint64_t* slots = nullptr;
        uint32_t cap = 0;
        uint32_t mask = 0;
    };
    struct ReadLocalTopology { ReadLocalTable tables[2]; };

    // Semantic word: bit 0 brackets atomic physical exchanges and safety-filter publication;
    // bit 1 is the whole-shard pending hint, and bits 2..63 are a saturating generation.
    // Topology publication and ownership moves never open or advance this word.
    static constexpr uint64_t kReadLocalAtomicMutationBit = uint64_t{1} << 0;
    static constexpr uint64_t kReadLocalPendingBit = uint64_t{1} << 1;
    static constexpr uint32_t kReadLocalGenerationShift = 2;
    static constexpr uint64_t kReadLocalGenerationMask =
        std::numeric_limits<uint64_t>::max() >> kReadLocalGenerationShift;

    class ReadLocalAtomicGuard {
    public:
        explicit ReadLocalAtomicGuard(FlatStore& store, bool active = true)
            : store_(active ? &store : nullptr) {
            if (store_) store_->read_local_atomic_mutation_begin();
        }
        ~ReadLocalAtomicGuard() {
            if (store_) store_->read_local_atomic_mutation_end();
        }
        ReadLocalAtomicGuard(const ReadLocalAtomicGuard&) = delete;
        ReadLocalAtomicGuard& operator=(const ReadLocalAtomicGuard&) = delete;
        ReadLocalAtomicGuard(ReadLocalAtomicGuard&& other) noexcept : store_(other.store_) {
            other.store_ = nullptr;
        }
        ReadLocalAtomicGuard& operator=(ReadLocalAtomicGuard&&) = delete;

    private:
        FlatStore* store_;
    };

    class ForeignReadKeyGuard {
    public:
        ForeignReadKeyGuard(FlatStore& store, uint64_t hash)
            : store_(store.read_local_enabled_ ? &store : nullptr), hash_(hash) {
            if (store_) store_->foreign_read_scope_open(hash_);
        }
        ~ForeignReadKeyGuard() {
            if (store_) store_->foreign_read_scope_close(hash_);
        }
        ForeignReadKeyGuard(const ForeignReadKeyGuard&) = delete;
        ForeignReadKeyGuard& operator=(const ForeignReadKeyGuard&) = delete;
        ForeignReadKeyGuard(ForeignReadKeyGuard&& other) noexcept
            : store_(other.store_), hash_(other.hash_) {
            other.store_ = nullptr;
        }
        ForeignReadKeyGuard& operator=(ForeignReadKeyGuard&&) = delete;

    private:
        FlatStore* store_ = nullptr;
        uint64_t hash_ = 0;
    };

    class ForeignReadPoisonGuard {
    public:
        explicit ForeignReadPoisonGuard(FlatStore& store, bool active = true)
            : store_(active && store.read_local_enabled_ ? &store : nullptr) {
            if (store_) store_->foreign_read_poison_open();
        }
        ~ForeignReadPoisonGuard() {
            if (store_) store_->foreign_read_poison_close();
        }
        ForeignReadPoisonGuard(const ForeignReadPoisonGuard&) = delete;
        ForeignReadPoisonGuard& operator=(const ForeignReadPoisonGuard&) = delete;
        ForeignReadPoisonGuard(ForeignReadPoisonGuard&& other) noexcept
            : store_(other.store_) {
            other.store_ = nullptr;
        }
        ForeignReadPoisonGuard& operator=(ForeignReadPoisonGuard&&) = delete;

    private:
        FlatStore* store_ = nullptr;
    };

    // KvObj allocations use alloc_raw(), so probe that exact backend before any shard is built.
    // A platform/allocator that can only supply wider virtual addresses cannot safely use the
    // packed slot format and must be rejected at boot rather than losing pointer bits later.
    static bool pointer_encoding_supported() {
        constexpr size_t kProbeBytes = 64;
        void* probe = alloc_raw(kProbeBytes);
        if (!probe) return false;
        const bool fits = (reinterpret_cast<uint64_t>(probe) & ~kPtrMask) == 0;
        free_sized(probe, kProbeBytes);
        return fits;
    }

    explicit FlatStore(uint32_t initial_cap = 1024) {
        const uint32_t cap = round_pow2(initial_cap);
        if (!cap || !alloc_table(0, cap)) throw std::bad_alloc();
    }
    ~FlatStore() {
        // At process teardown no reader survives. Collapse pending entries first so the ordinary
        // table destructor below remains the unique owner of each promoted winner.
        read_local_enabled_ = false;  // shutdown promotion/free is direct; no callback may outlive us
        atomic_promote_all_for_shutdown();
        expires_.clear();
        if (snapshot_new_tab_) std::free(snapshot_new_tab_);
        for (int t = 0; t < 2; t++)
            if (tab_[t]) {
                for (uint32_t i = 0; i < cap_[t]; i++)
                    if (KvObj* o = ptr_of(tab_[t][i])) kvobj_free(o);
                std::free(tab_[t]);
            }
        // Retired objects no longer appear in either table. They remain here only because an io
        // thread may still be handing their value bytes to the kernel.
        for (const Borrow& b : borrows_)
            if (b.retired) kvobj_free(b.retired);
    }
    FlatStore(const FlatStore&) = delete;
    FlatStore& operator=(const FlatStore&) = delete;

    // Boot-only allocation, before persistence replay or any foreign probe can exist.
    bool prepare_read_local(uint32_t readers) {
        if (!ensure_read_local_store_state()) return false;
        return prepare_read_local_table(readers);
    }

    // Every armed store publishes complete table images. Implementation and QSBR lifetime
    // rules live in flatstore_nowait.inc; unarmed stores allocate no publication state.
    bool prepare_read_local_table(uint32_t readers);
    void arm_read_local_table();
    void read_local_table_prepared_bytes(uint64_t bytes);
    void destroy_read_local_table();
    void read_local_table_metrics(ReadLocalTableMetrics& total) const;
    bool debug_read_local_table(uint64_t hash, uint32_t owner, uint32_t mode);
    ReadLocalPrefetchCapture read_local_table_capture(uint64_t hash, Slice key,
                                                    uint32_t reader) const;
    void read_local_table_prefetch(uint64_t hash) const;
    bool read_local_atomic_validate(uint64_t state, uint32_t reader) const;
    void read_local_table_served(uint64_t hash, uint32_t reader) const;
    bool rebuild_read_local_table(uint32_t capacity);
    bool snapshot_mark_read_local_table();
    void clear_read_local_table(bool snapshot);
    friend struct ReadLocalTablePublication;

    // Enabled is boot-latched. The sink may be rebound only at a quiesced fused ownership handoff;
    // false keeps the old store path and every installed writer hook predicted cold.
    void configure_read_local(bool enabled, ReadLocalRetireSink sink) {
        if (enabled && !sink.defer) std::abort();
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        if (enabled && !sink.diagnostics()) std::abort();
#endif
        // Persistence loading finishes before the fused executor arms this store. Prepared atomic
        // records cannot be retroactively marked in their entry headers, so fail closed if that
        // boot invariant ever changes instead of publishing a false zero-pending state.
        if (enabled && atomic_pending_entries() != 0) std::abort();
        ReadLocalStoreState* state = read_local_store_state();
        if (enabled && !state) std::abort();
        if constexpr (kReadLocalSetTaxAtomicRaw) {
            // Persistence/bootstrap may have used the ordinary overwrite path before the boot latch
            // is exposed. Establish fixed atomic payload cells for that final image while no foreign
            // probe can exist; every later Raw constructor performs the same preparation directly.
            if (enabled && !read_local_enabled_)
                for (int table = 0; table < 2; table++)
                    if (tab_[table])
                        for (uint32_t slot = 0; slot < cap_[table]; slot++)
                            if (KvObj* object = ptr_of(tab_[table][slot]))
                                kvobj_prepare_read_local_raw_cells(object);
        }
        if (enabled && !read_local_enabled_) arm_read_local_table();
        if (state) state->retire_sink = sink;
        read_local_enabled_ = enabled;
    }
    void rebind_read_local_retire_sink(ReadLocalRetireSink sink) {
        if (!read_local_enabled_ || !sink.defer) std::abort();
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        if (!sink.diagnostics()) std::abort();
#endif
        // The old owner has drained all tasks/read captures/retire entries. Rebind before the
        // router commits the ownership move; the root and all shard-owned state move together.
        // No generation change is needed: readers retain their image under the same QSBR domain.
        read_local_store_state_required().retire_sink = sink;
    }
    bool read_local_enabled() const { return read_local_enabled_; }
#ifdef TOMO_RL_CACHE_DEBUG
    // Debug builds only: lets Server assert that this store's sink still names its CURRENT owner.
    const ReadLocalRetireSink& read_local_retire_sink_debug() const {
        return read_local_store_state_required().retire_sink;
    }
#endif

    uint64_t read_local_state_acquire() const {
        return read_local_store_state_required().atomic_sequence.load(std::memory_order_acquire);
    }
    static bool read_local_atomic_mutating(uint64_t state) {
        return (state & kReadLocalAtomicMutationBit) != 0;
    }
    static uint32_t read_local_pending(uint64_t state) {
        return (state & kReadLocalPendingBit) ? 1u : 0u;
    }
    static uint64_t read_local_generation(uint64_t state) {
        return state >> kReadLocalGenerationShift;
    }
    // Wrap publishes this permanent fail-closed value rather than an equal "stable" generation.
    static bool read_local_generation_poisoned(uint64_t state) {
        return read_local_generation(state) == kReadLocalGenerationMask;
    }
    static bool read_local_state_eligible(uint64_t state) {
        return !read_local_atomic_mutating(state) && !read_local_generation_poisoned(state);
    }
    // Equal semantic generation regardless of the independent pending hint; callers compare two words
    // they have already proven even.
    static bool read_local_generation_equal(uint64_t first, uint64_t second) {
        return read_local_generation(first) == read_local_generation(second);
    }
    bool foreign_read_key_unsafe(uint64_t state, uint64_t hash) const {
        // Pending is also the cheap empty-filter hint. It stays latched if filter bookkeeping ever
        // permanently poisons, so short-circuiting here cannot turn that fail-closed state into a
        // false negative after the last enumerable entry drains.
        if (!read_local_pending(state)) return false;
        return read_local_store_state_required().foreign_reads.might_contain(hash);
    }
    bool foreign_read_key_unsafe(uint64_t hash) const {
        const uint64_t state = read_local_state_acquire();
        return foreign_read_key_unsafe(state, hash);
    }
    // Multi-key window validator. Load it only for a key whose shard word carries the pending bit,
    // and only to compare against a later load of the same cell. Called only on the armed lane.
    uint32_t foreign_read_cell_epoch(uint64_t hash) const {
        return read_local_store_state_required().foreign_reads.cell_epoch(hash);
    }
    static uint32_t foreign_read_filter_index(uint64_t hash) {
        return ForeignReadSafety::cell_index(hash);
    }
    static uint32_t foreign_read_filter_fingerprint(uint64_t hash) {
        return ForeignReadSafety::fingerprint(hash);
    }
    uint64_t foreign_read_unsafe_refs() const {
        const ReadLocalStoreState* state = read_local_store_state();
        return state ? state->foreign_reads.unsafe_total_refs() : 0;
    }
    uint64_t foreign_read_occupied_cells() const {
        const ReadLocalStoreState* state = read_local_store_state();
        return state ? state->foreign_reads.occupied_cells() : 0;
    }
    uint64_t foreign_read_wildcard_cells() const {
        const ReadLocalStoreState* state = read_local_store_state();
        return state ? state->foreign_reads.wildcard_cells() : 0;
    }
    uint64_t foreign_read_saturated_cells() const {
        const ReadLocalStoreState* state = read_local_store_state();
        return state ? state->foreign_reads.saturated_cells() : 0;
    }
    bool foreign_read_poisoned() const {
        const ReadLocalStoreState* state = read_local_store_state();
        return state && state->foreign_reads.poison_refs() != 0;
    }
    [[nodiscard]] ForeignReadKeyGuard foreign_read_key_guard(uint64_t hash) {
        return ForeignReadKeyGuard(*this, hash);
    }
    [[nodiscard]] ForeignReadPoisonGuard foreign_read_poison_guard(bool active = true) {
        return ForeignReadPoisonGuard(*this, active);
    }

    // Same-owner group/script paths retain their enumerated hashes through finish. Duplicate
    // occurrences are intentional and must be closed symmetrically.
    template <typename HashAt>
    void foreign_read_scope_open_span(uint32_t count, HashAt&& hash_at) {
        if (!read_local_enabled_) return;
        // Unlike an AtomicEntry, this scoped path may mutate through an ordinary immutable slot
        // replacement. Complete a brief sequence handshake around filter publication so a point
        // reader that already observed a negative cell cannot validate across the later handler.
        ReadLocalAtomicGuard publication(*this);
        ReadLocalStoreState& state = read_local_store_state_required();
        state.foreign_reads.add_span(count, std::forward<HashAt>(hash_at));
        foreign_read_pending_witness_open(state);
    }

    template <typename HashAt>
    void foreign_read_scope_close_span(uint32_t count, HashAt&& hash_at) {
        if (!read_local_enabled_) return;
        ReadLocalStoreState& state = read_local_store_state_required();
        state.foreign_reads.close_span(count, std::forward<HashAt>(hash_at));
        foreign_read_pending_witness_close(state);
    }

    void foreign_read_scope_open(uint64_t hash) {
        foreign_read_scope_open_span(1, [&](uint32_t) { return hash; });
    }

    void foreign_read_scope_close(uint64_t hash) {
        foreign_read_scope_close_span(1, [&](uint32_t) { return hash; });
    }

    // An enumerator that cannot name every key publishes a whole-shard wildcard. The caller must
    // retain and close this scope after declining or completing the write.
    void foreign_read_unenumerable_open() {
        if (read_local_enabled_) foreign_read_poison_open();
    }
    void foreign_read_unenumerable_close() {
        if (read_local_enabled_) foreign_read_poison_close();
    }

    bool     rehashing() const { return tab_[1] != nullptr; }
    uint32_t size() const { return live_[0] + live_[1]; }
    uint64_t capacity() const { return static_cast<uint64_t>(cap_[0]) + cap_[1]; }
    size_t   object_bytes() const { return obj_bytes_ + atomic_version_bytes_; }
    uint32_t expire_count() const { return expires_.size(); }
    // Hashes in this shard carrying at least one field deadline. THE gate for the whole hash-field
    // TTL feature: a shard that has never seen HEXPIRE reads zero here and every hash command pays
    // one predicted-false test, with all field-TTL machinery out of line behind it.
    uint32_t field_expire_count() const { return field_ttl_gate_; }
    // Registration is idempotent and keyed by key hash only, exactly like expires_. A stale entry
    // (key replaced, deleted, or persisted) is harmless: the cycle drops it on its next visit.
    void note_field_ttl(uint64_t h) {
        (void)field_expires_.insert(h);
        field_ttl_gate_ = field_expires_.size();
    }
    // FIRED-proof for the lazy reap: >0 means a hash field really was collected on an access path,
    // not merely filtered out of a reply. INFO reports it as expired_hash_fields.
    void note_field_expired(uint32_t n) { field_expired_ += n; }
    // Re-arms type-specific attention for an object that arrived from a snapshot, an AOF replay or
    // RESTORE rather than from a command. Load-only, so it costs the hot path nothing.
    void note_loaded_object(uint64_t h, const KvObj* o) {
        if (static_cast<Type>(o->type) != Type::Hash) return;
        if (static_cast<Enc>(o->enc) == Enc::Compact) return;
        if (static_cast<const HashVal*>(o->external_ptr())->ttls) note_field_ttl(h);
    }
    void bind_aof(AofManager* manager, int32_t sid, uint32_t next_sequence) {
        aof_.init(manager, sid, next_sequence);
    }
    AofProducer& aof() { return aof_; }
    const AofProducer& aof() const { return aof_; }
    // AOF group fragments describe the private candidate installed by this owner before its
    // cross-shard visibility ticket is published. Logical find() must hide that candidate; this
    // owner-only tap intentionally reads the newest physical representation instead.
    KvObj* aof_physical(uint64_t hash, Slice key) const {
        if (KvObj* object = find_in(0, hash, key)) return object;
        return rehashing() ? find_in(1, hash, key) : nullptr;
    }
    size_t   accounted_bytes() const {
        const size_t keys = size();
        const size_t objects = obj_bytes_ + atomic_version_bytes_;
        if (keys > (std::numeric_limits<size_t>::max() - objects) / kSlotOverheadPerKey)
            return std::numeric_limits<size_t>::max();
        return objects + static_cast<uint64_t>(keys) * kSlotOverheadPerKey;
    }

    // ==== epoch-MVCC atomics (see src/store/flatstore_atomic.inc) ====
    #include "flatstore_atomic.inc"

    enum class SnapshotWriteResult : uint8_t { Ready, Pending, Error };
    enum class EraseEvent : uint8_t { Del, Evicted, None };

    // Phase 1 of the cut barrier.  Existing incremental rehashing is completed in its ordinary
    // bounded steps; only then is a fresh post-cut table allocated.  Resize remains suppressed
    // while prepared, so the owner can keep serving until the later freeze barrier without making
    // this allocation stale.  snapshot_mark() is then only a pointer swap at the epoch boundary.
    SnapshotWriteResult snapshot_prepare(uint64_t epoch, int64_t cut_ms) {
        if (snapshot_active_) return SnapshotWriteResult::Error;
        if (rehashing()) {
            rehash_step();
            return SnapshotWriteResult::Pending;
        }
        if (snapshot_prepared_) return SnapshotWriteResult::Ready;
        uint64_t wanted = static_cast<uint64_t>(std::max(cap_[0], kMinCap)) * 2;
        if (wanted > UINT32_MAX) return SnapshotWriteResult::Error;
        const uint32_t cap = round_pow2(static_cast<uint32_t>(wanted));
        if (!cap) return SnapshotWriteResult::Error;
        auto* fresh = static_cast<uint64_t*>(flatstore_table_calloc(cap, sizeof(uint64_t)));
        if (!fresh) return SnapshotWriteResult::Error;
        snapshot_new_tab_ = fresh;
        snapshot_new_cap_ = cap;
        if (read_local_enabled_)
            read_local_table_prepared_bytes(uint64_t(cap) * sizeof(uint64_t));
        snapshot_epoch_ = epoch;
        snapshot_cut_ms_ = cut_ms;
        snapshot_prepared_ = true;
        return SnapshotWriteResult::Ready;
    }

    // Phase 2 of the cut barrier: no owner is executing an operation while these swaps run.  The
    // old table becomes the immutable-layout snapshot table (values may change only after their
    // pre-image has been serialized), and all new keys land in the fresh current table.
    bool snapshot_mark(int32_t shard_id, int64_t cut_ms) {
        if (__builtin_expect(read_local_enabled_, false))
            return snapshot_mark_read_local(shard_id, cut_ms);
        if (!snapshot_prepared_ || snapshot_active_ || rehashing()) return false;
        tab_[1] = tab_[0]; cap_[1] = cap_[0]; mask_[1] = mask_[0];
        live_[1] = live_[0]; tombs_[1] = tombs_[0];
        tab_[0] = snapshot_new_tab_; cap_[0] = snapshot_new_cap_; mask_[0] = cap_[0] - 1;
        live_[0] = tombs_[0] = 0;
        snapshot_new_tab_ = nullptr; snapshot_new_cap_ = 0; snapshot_prepared_ = false;
        rehash_pos_ = 0;
        snapshot_active_ = true;
        snapshot_shard_id_ = shard_id;
        snapshot_cut_ms_ = cut_ms;
        snapshot_pos_ = 0;
        snapshot_sequence_ = 0;
        snapshot_failed_ = false;
        snapshot_finished_ = false;
        snapshot_build_ = make_snapshot_chunk(SnapshotFrameBegin);
        snapshot_ready_.reset();
        snapshot_record_ = {};
        return snapshot_build_ != nullptr;
    }

    bool snapshot_active() const { return snapshot_active_; }
    bool snapshot_failed() const { return snapshot_failed_; }

    // Called by the owner before a Write command.  A slot behind the traversal cursor was already
    // dumped.  A slot ahead of it is serialized incrementally and marked with kTombBit; traversal
    // later sees that mark, clears it, and skips the now-post-cut value.
    uint64_t snapshot_preimages() const { return snapshot_preimages_; }

    SnapshotWriteResult snapshot_prepare_write(uint64_t h, Slice key) {
        if (__builtin_expect(read_local_enabled_, false))
            return snapshot_prepare_write_read_local(h, key);
        if (!snapshot_active_) return SnapshotWriteResult::Ready;
        if (snapshot_failed_) return SnapshotWriteResult::Error;
        if (find_in(0, h, key)) return SnapshotWriteResult::Ready;  // born/moved after the cut
        uint32_t slot = 0;
        KvObj* object = find_slot_in(1, h, key, slot);
        if (!object || slot < snapshot_pos_ || (tab_[1][slot] & kTombBit))
            return SnapshotWriteResult::Ready;
        if (deadline_elapsed(h, object, snapshot_cut_ms_)) {
            tab_[1][slot] |= kTombBit;             // absent at the cut; traversal must skip it
            return SnapshotWriteResult::Ready;
        }
        if (snapshot_record_.active) return SnapshotWriteResult::Pending;
        if (!snapshot_start_record(object, slot, true)) return SnapshotWriteResult::Error;
        snapshot_preimages_++;   // FIRED-proof: >0 in any real mutate-during-capture run
        return SnapshotWriteResult::Pending;
    }

    // CPU work on the owner, bounded by both bytes and examined slots.  It never writes a file.
    uint32_t snapshot_progress(uint32_t byte_budget, uint32_t slot_budget) {
        if (__builtin_expect(read_local_enabled_, false))
            return snapshot_progress_read_local(byte_budget, slot_budget);
        if (!snapshot_active_ || snapshot_failed_ || snapshot_ready_) return 0;
        uint32_t work = 0;
        while (byte_budget && !snapshot_ready_ && !snapshot_failed_) {
            if (snapshot_record_.active) {
                const uint32_t before = byte_budget;
                snapshot_progress_record(byte_budget);
                work += before - byte_budget;
                if (snapshot_record_.active || snapshot_ready_) break;
                continue;
            }
            if (snapshot_pos_ >= cap_[1]) {
                snapshot_finish_stream();
                break;
            }
            if (!slot_budget) break;
            const uint32_t slot = snapshot_pos_;
            const uint64_t word = tab_[1][slot];
            KvObj* object = ptr_of(word);
            slot_budget--; work++;
            if (!object) { snapshot_pos_++; continue; }
            if (word & kTombBit) {
                tab_[1][slot] = word & ~kTombBit;
                snapshot_pos_++;
                continue;
            }
            if (deadline_elapsed(hash_key(object->key()), object, snapshot_cut_ms_)) {
                snapshot_pos_++;
                continue;
            }
            if (!snapshot_start_record(object, slot, false)) break;
        }
        return work;
    }

    std::unique_ptr<SnapshotChunk> snapshot_take_chunk() {
        return std::move(snapshot_ready_);
    }

    void snapshot_handoff_complete() {
        if (!snapshot_finished_) return;
        snapshot_active_ = false;
        snapshot_shard_id_ = -1;
        snapshot_build_.reset();
        snapshot_ready_.reset();
        snapshot_record_ = {};
        // tab_[1] is now an ordinary old table.  Subsequent operations merge it with the existing
        // bounded rehash step; successful post-cut inserts were capacity-gated so the destination
        // can hold the complete logical set.
    }

    void snapshot_cancel() {
        if (read_local_enabled_) read_local_table_prepared_bytes(0);
        if (snapshot_new_tab_) std::free(snapshot_new_tab_);
        snapshot_new_tab_ = nullptr; snapshot_new_cap_ = 0; snapshot_prepared_ = false;
        snapshot_active_ = false; snapshot_failed_ = false; snapshot_finished_ = false;
        snapshot_build_.reset(); snapshot_ready_.reset(); snapshot_record_ = {};
        snapshot_shard_id_ = -1;
        // Any pre-image marks still live in tab_[1] are harmless to normal lookup and are cleared
        // as the ordinary rehash moves those slot words back through make_word().
    }

    // What a migration of this shard would cost the NEW domain to re-pull through the fabric. Not a
    // copy cost — nothing is copied — but an L3 domain is filled by access, so a shard that moves
    // has to be read back in on the other side.
    size_t resident_estimate() const {
        return static_cast<size_t>((static_cast<uint64_t>(cap_[0]) + cap_[1]) *
                                   sizeof(uint64_t)) + obj_bytes_ +
               atomic_version_bytes_ + pending_bytes_ +
               expires_.memory_bytes() + field_expires_.memory_bytes();
    }

    // Collection values grow and shrink behind a stable KvObj header. The shard owner brackets
    // such a mutation with kvobj_size() samples and reports the delta here so later erase/rehash
    // subtracts the same footprint that is currently charged. Single-owner: no atomic or lock.
    void note_object_size_change(size_t before, size_t after) {
        if (after >= before) obj_bytes_ += after - before;
        else obj_bytes_ -= before - after;
    }

    KvObj* find(uint64_t h, Slice key) {
        if (rehashing() && !snapshot_active_) rehash_step();
        KvObj* found = nullptr;
        if (__builtin_expect(atomic_pending_ != nullptr, false) &&
            __builtin_expect(atomic_pending_->live != 0, false)) {
            found = atomic_resolve(h, key, atomic_read_epoch_);
        } else {
            if (KvObj* o = find_in(0, h, key)) found = live_or_expire(0, h, key, o);
            if (rehashing()) {
                if (!found)
                    if (KvObj* o = find_in(1, h, key)) found = live_or_expire(1, h, key, o);
            }
        }
        // The entire disabled-feature read tax: one predicted branch, no metadata write.
        // CLIENT NO-TOUCH rides INSIDE that branch -- with maxmemory off (the default) the
        // no-touch byte is never even loaded, because && short-circuits.
        if (__builtin_expect(maxmemory_enabled_ && !no_touch_, false) && found) touch(found);
        return found;
    }

    KvObj* find_notify(uint64_t h, Slice key, FlatNotifySink* sink) {
        KvObj* candidate = find_in(0, h, key);
        if (!candidate && rehashing()) candidate = find_in(1, h, key);
        const bool expired = candidate && deadline_elapsed(h, candidate, cached_now_ms_) &&
                             !(snapshot_active_ && candidate == find_in(1, h, key));
        if (expired) notify_emit(sink, NOTIFY_EXPIRED, NotifyEventId::Expired, candidate->key());
        KvObj* found = find(h, key);
        if (!found) notify_emit(sink, NOTIFY_KEY_MISS, NotifyEventId::Keymiss, key);
        return found;
    }

    // Same-size-CLASS overwrite, allocation-free. Asking for 88 bytes gets 96, so a value that grew
    // or shrank a little still fits what was already paid for. The test is equality of CLASS rather
    // than "new <= old" because good_size() is recomputed from the header — letting the real
    // allocation and the implied one diverge would silently break the resident estimate.
    OverwriteResult try_overwrite(uint64_t h, Slice key, Slice val) {
        if (__builtin_expect(read_local_enabled_, false))
            return try_overwrite_read_local(h, key, val);
        KvObj* o = find_without_touch(h, key);
        if (!o) return OverwriteResult::NotPossible;
        if (o->encoding() != Enc::Raw) return OverwriteResult::NotPossible;
        if (o->flags & KvObjFlags::HasTtl) return OverwriteResult::NotPossible;  // SET clears TTL
        if (val.n > kEmbedThreshold) return OverwriteResult::NotPossible;       // becomes Extern
        const size_t want = kvobj_alloc_size(o->klen(), val.n, false, Enc::Raw);
        if (good_size(want) != kvobj_capacity(o)) return OverwriteResult::NotPossible;

        // In-place overwrite is the one mutation that would change bytes without retiring their
        // allocation. With no outstanding borrows this is one predicted branch and no lookup.
        if (outstanding_borrows_ && is_borrowed(o->str_data()))
            return OverwriteResult::NotPossible;

        // The entire disabled-feature write tax is this branch. When enabled, the target key is
        // protected while make_room_for() evicts other candidates.
        if (__builtin_expect(maxmemory_enabled_, false)) {
            if (!make_room_for(key, good_size(want))) return OverwriteResult::MaxmemoryOom;
            touch(o);
        }

        // Same length means the same class and the same footprint: the accounting delta is exactly
        // zero, so do not compute it (kvobj_size was 7.7% of SET-cell cycles before this).
        if (val.n == kvobj_read_local_raw_length(o)) {
            std::memcpy(o->val_ptr(), val.p, val.n);
            return OverwriteResult::Updated;
        }
        obj_bytes_ -= kvobj_size(o);
        o->store_raw_length_relaxed(val.n);
        std::memcpy(o->val_ptr(), val.p, val.n);
        obj_bytes_ += kvobj_size(o);
        return OverwriteResult::Updated;
    }

    OverwriteResult try_overwrite_notify(uint64_t h, Slice key, Slice val,
                                         FlatNotifySink*) {
        return try_overwrite(h, key, val);
    }

    // ARMED WRITES ALLOCATE. A published object is immutable while read-local is armed (readers
    // hold no lock and must never observe a half-written object), so a SET may not overwrite in
    // place: it builds a fresh block, publishes it, and retires the old one through QSBR. Rather
    // than round-trip that block through the allocator, the armed path takes it from the fused
    // owner's post-grace block cache and falls back to mallocx only on a miss. Extern values keep an
    // independent value allocation and stay on the baseline path. Unarmed builds see one
    // boot-latched, predicted-false branch and the identical allocator call they have today.
    KvObj* make_set_string(Slice key, Slice value, int64_t expire_at_ms = -1,
                           bool reserve_ttl_slot = false) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        ReadLocalSetTaxStats* stats = read_local_enabled_ ? &settax_stats() : nullptr;
        if (stats) {
            if (value.n <= kEmbedThreshold) stats->init_raw_calls++;
            else stats->init_extern_calls++;
            stats->init_key_bytes += key.n;
            stats->init_value_bytes += value.n;
        }
#endif
        if (__builtin_expect(read_local_enabled_, false) && value.n <= kEmbedThreshold) {
            const bool has_ttl_slot = reserve_ttl_slot || expire_at_ms >= 0;
            const size_t allocation = good_size(
                kvobj_alloc_size(key.n, value.n, has_ttl_slot, Enc::Raw));
            void* memory = read_local_cache_take(allocation);
            if (!memory) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                if (stats) stats->fresh_allocation_attempts++;
#endif
                memory = alloc_raw(allocation);
            }
            if (!memory) {
                // The allocator refused. The cache is holding physical memory nothing needs;
                // hand it back and ask exactly once more before reporting OOM.
                read_local_cache_release_all();
                memory = alloc_raw(allocation);
            }
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            if (stats && memory) stats->init_cell_prepare_calls++;
#endif
            return memory ? kvobj_init_raw_string(memory, key, value, expire_at_ms,
                                                  reserve_ttl_slot) : nullptr;
        }
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        KvObj* object = kvobj_new_string(
            key, value, expire_at_ms, reserve_ttl_slot,
            stats ? &stats->fresh_allocation_attempts : nullptr);
        if (stats && object && value.n <= kEmbedThreshold) stats->init_cell_prepare_calls++;
        return object;
#else
        return kvobj_new_string(key, value, expire_at_ms, reserve_ttl_slot);
#endif
    }

    KvObj* make_set_int(Slice key, int64_t value, int64_t expire_at_ms = -1,
                        bool reserve_ttl_slot = false) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        ReadLocalSetTaxStats* stats = read_local_enabled_ ? &settax_stats() : nullptr;
        if (stats) {
            stats->init_int_calls++;
            stats->init_key_bytes += key.n;
            stats->init_value_bytes += sizeof(value);
        }
#endif
        if (__builtin_expect(read_local_enabled_, false)) {
            const bool has_ttl_slot = reserve_ttl_slot || expire_at_ms >= 0;
            const size_t allocation = good_size(
                kvobj_alloc_size(key.n, 0, has_ttl_slot, Enc::Int));
            void* memory = read_local_cache_take(allocation);
            if (!memory) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                if (stats) stats->fresh_allocation_attempts++;
#endif
                memory = alloc_raw(allocation);
            }
            if (!memory) {
                read_local_cache_release_all();
                memory = alloc_raw(allocation);
            }
            return memory ? kvobj_init_int(memory, key, value, expire_at_ms,
                                           reserve_ttl_slot) : nullptr;
        }
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        return kvobj_new_int(
            key, value, expire_at_ms, reserve_ttl_slot,
            stats ? &stats->fresh_allocation_attempts : nullptr);
#else
        return kvobj_new_int(key, value, expire_at_ms, reserve_ttl_slot);
#endif
    }

    // A failed insertion never published the replacement, so no grace period is owed. It is NOT
    // offered to the block cache: every caller reaches here because the store refused the write
    // (OOM or a maxmemory refusal), and holding memory back from the allocator is exactly the
    // wrong move under that pressure. Return it immediately.
    void discard_set_value(KvObj* object) {
        if (!object) return;
        kvobj_free(object);
    }

    // Called by GET on the shard owner before publishing the Op. Pointer identity is sufficient:
    // an allocation cannot be reused while it is either table-owned or retained as `retired`.
    void borrow(const char* ptr) {
        const uint32_t at = borrow_find(ptr);
        if (at != kNoBorrow) { borrows_[at].refs++; outstanding_borrows_++; return; }
        borrows_.push_back(Borrow{ptr, 1, nullptr});
        // Publish the count only after push_back succeeds. A failed registry growth must not leave
        // an unreturnable phantom borrow behind (cross-shard MGET can recover as an OOM reply).
        borrow_index_added(ptr, static_cast<uint32_t>(borrows_.size() - 1));
        outstanding_borrows_++;
    }

    // Called only by the shard owner after an io-thread release crosses back through its channel.
    // The last reference is also the point at which a logically removed object may be destroyed.
    void unborrow(const char* ptr) {
        const uint32_t at = borrow_find(ptr);
        if (at == kNoBorrow) return;
        Borrow& b = borrows_[at];
        if (b.refs == 0 || outstanding_borrows_ == 0) return;
        b.refs--;
        outstanding_borrows_--;
        if (b.refs) return;
        if (b.retired) {
            const size_t capacity = kvobj_capacity(b.retired);
            pending_bytes_ -= capacity + kvobj_external_bytes(b.retired);
            free_retired_obj_now(b.retired, capacity);
        }
        borrow_index_dropped(ptr);
        const uint32_t last = static_cast<uint32_t>(borrows_.size() - 1);
        if (at != last) {
            borrows_[at] = borrows_[last];
            // The moved entry keeps its identity but changes slot; relabel BEFORE pop_back, while
            // borrows_[last] is still readable for the pointer comparison inside the index.
            borrow_index_put(borrows_[at].ptr, at);
        }
        borrows_.pop_back();
        if (borrows_.empty()) borrow_index_release();
    }

    uint32_t outstanding_borrows() const { return outstanding_borrows_; }

    void set_cached_now_ms(int64_t now_ms) { cached_now_ms_ = now_ms; }
    void set_cached_lru_clock(uint8_t clock) { cached_lru_clock_ = clock; }
    // Lifetime high-water mark for successful active reaps.  The deadline is known logical here;
    // still reject a negative value defensively so a retained-but-persisted slot cannot report a
    // multi-billion-millisecond lag.  Four bytes are enough for almost 50 days and larger stalls
    // saturate instead of wrapping.
    void note_active_expire_reap(int64_t deadline_ms) {
        if (deadline_ms < 0 || cached_now_ms_ <= deadline_ms) return;
        const uint64_t lag = static_cast<uint64_t>(cached_now_ms_ - deadline_ms);
        const uint32_t bounded = lag > std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(lag);
        if (bounded > active_expire_reap_lag_ms_max_)
            active_expire_reap_lag_ms_max_ = bounded;
    }
    uint32_t active_expire_reap_lag_ms_max() const {
        return active_expire_reap_lag_ms_max_;
    }
    // Owner-thread scratch: the current task's CLIENT NO-TOUCH answer. Written only when
    // maxmemory is enabled, so it costs nothing in the default configuration.
    void set_no_touch(bool value) { no_touch_ = value; }

    // OBJECT IDLETIME/FREQ report the same five-bit eviction metadata the victim chooser reads, so
    // they need the policy, the arming flag and the clock that gives the bits their meaning. All
    // three are owner-thread reads of owner-thread state; nothing here is on the lookup path.
    MaxmemoryPolicy maxmemory_policy() const { return maxmemory_policy_; }
    bool maxmemory_enabled() const { return maxmemory_enabled_; }
    uint8_t published_lru_clock() const { return cached_lru_clock_; }
    // OBJECT must not count as an access: reporting idle time through find() would reset the very
    // metadata being reported. Redis solves the same problem with LOOKUP_NOTOUCH.
    KvObj* find_no_touch(uint64_t h, Slice key) { return find_without_touch(h, key); }
    // MEMORY USAGE asks what an entry costs, not whether it is readable. Redis answers it from a
    // raw dictionary lookup with no expire check, so an elapsed-but-unreaped key still reports its
    // footprint -- which is the honest answer, since those bytes are still resident.
    KvObj* find_resident(uint64_t h, Slice key) const {
        if (KvObj* object = find_in(0, h, key)) return object;
        return rehashing() ? find_in(1, h, key) : nullptr;
    }
    // Owner-only deadline accessor. With the prototype selector enabled, a physically TTL-capable
    // object pays one ExpireIndex probe; objects without the slot return before touching sidecar
    // memory. Atomic/snapshot versions retain their inline transport value and bypass the hash-only
    // prototype because one index entry cannot describe multiple versions of the same key.
    int64_t deadline(uint64_t h, const KvObj* object) const {
        if (!object || !object->has_ttl_slot()) return kNoTtlDeadline;
        const int64_t inline_deadline = object->expire_at_ms();
        if constexpr (kTtlDeadlineSidecar) {
            if (snapshot_active_ || (atomic_pending_ && atomic_pending_->live != 0))
                return inline_deadline;
            return expires_.deadline(h, inline_deadline);
        }
        return inline_deadline;
    }
    bool deadline_elapsed(uint64_t h, const KvObj* object, int64_t now_ms) const {
        const int64_t at = deadline(h, object);
        return at >= 0 && at <= now_ms;
    }
    // The deadline WATCH pins on an armed key. Redis probes the key with LOOKUP_NOTOUCH and does
    // not reap it, so an elapsed-but-unreaped key must stay physically counted here too. A key that
    // is ALREADY past its deadline when WATCH runs is redis's `wk->expired`: its later removal is
    // not a change the transaction should see, so it captures -1 exactly like a TTL-free key.
    int64_t watch_deadline(uint64_t h, Slice key) const {
        const KvObj* object = find_resident(h, key);
        if (!object) return -1;
        const int64_t at = deadline(h, object);
        return at > cached_now_ms_ ? at : -1;
    }
    void bind_expired_counter(uint64_t* counter) { expired_counter_ = counter; }
    void bind_evicted_counter(uint64_t* counter) { evicted_counter_ = counter; }
    // Table rebuilds started. Cold (once per resize, never on the hot path) and it exists so a
    // SCAN-correctness test can PROVE the hazard it guards actually occurred: a run in which this
    // does not move has not exercised resize-during-iteration at all, and its "0 keys missed"
    // would be vacuous.
    void bind_rehash_counter(uint64_t* counter) { rehash_counter_ = counter; }
    void configure_maxmemory(bool enabled, uint64_t shard_limit, MaxmemoryPolicy policy,
                             uint32_t samples) {
        maxmemory_enabled_ = enabled;
        maxmemory_limit_ = shard_limit;
        maxmemory_policy_ = policy;
        maxmemory_samples_ = samples == 0 ? 1 : (samples > 64 ? 64 : samples);
    }

    // An atomic script can roll back every declared key, but an eviction victim is deliberately
    // outside that declaration. Keep maxmemory admission enabled while making it non-evicting for
    // the duration of the one owner task, so a failed script cannot leave an unrelated victim
    // behind. The owner restores the live policy before it takes another task.
    MaxmemoryPolicy script_suspend_eviction() {
        const MaxmemoryPolicy previous = maxmemory_policy_;
        if (maxmemory_enabled_) maxmemory_policy_ = MaxmemoryPolicy::NoEviction;
        return previous;
    }
    void script_restore_eviction(MaxmemoryPolicy policy) { maxmemory_policy_ = policy; }

    enum class TtlResult : uint8_t { Missing, NoChange, Updated, Oom, MaxmemoryOom };

    TtlResult set_expire(uint64_t h, Slice key, int64_t expire_at_ms) {
        if (__builtin_expect(read_local_enabled_, false))
            return set_expire_read_local(h, key, expire_at_ms);
        KvObj* old = find(h, key);
        if (!old) return TtlResult::Missing;
        if (old->has_ttl_slot()) {
            old->set_expire_at_ms(expire_at_ms);
            (void)track_expire(h, old);
            return TtlResult::Updated;
        }
        return rewrite_expire(h, old, expire_at_ms);
    }

    TtlResult set_expire_notify(uint64_t h, Slice key, int64_t expire_at_ms,
                                FlatNotifySink* sink) {
        if (__builtin_expect(read_local_enabled_, false))
            return set_expire_notify_read_local(h, key, expire_at_ms, sink);
        KvObj* old = find_notify(h, key, sink);
        if (!old) return TtlResult::Missing;
        if (old->has_ttl_slot()) {
            old->set_expire_at_ms(expire_at_ms);
            (void)track_expire(h, old);
            return TtlResult::Updated;
        }
        return rewrite_expire(h, old, expire_at_ms);
    }

    TtlResult persist(uint64_t h, Slice key) {
        KvObj* old = find(h, key);
        if (!old) return TtlResult::Missing;
        if (deadline(h, old) < 0) return TtlResult::NoChange;
        if (__builtin_expect(read_local_enabled_, false))
            return rewrite_expire_read_local(h, old, kNoTtlDeadline);
        old->set_expire_at_ms(kNoTtlDeadline);
        untrack_expire(h);
        return TtlResult::Updated;
    }

    TtlResult persist_notify(uint64_t h, Slice key, FlatNotifySink* sink) {
        KvObj* old = find_notify(h, key, sink);
        if (!old) return TtlResult::Missing;
        if (deadline(h, old) < 0) return TtlResult::NoChange;
        if (__builtin_expect(read_local_enabled_, false))
            return rewrite_expire_read_local(h, old, kNoTtlDeadline);
        old->set_expire_at_ms(kNoTtlDeadline);
        untrack_expire(h);
        return TtlResult::Updated;
    }

    // Returns a WORK count, while `budget` bounds examined expire-index slots. The count is the
    // number of expired keys removed, plus one while a sidecar move is still in flight: half the
    // index is then parked in the old table, and an owner that treats a barren sampling pass as
    // "nothing to do" parks with those deadlines unsampled until the next command wakes it.
    // Finding an object from its full hash follows only that hash's FlatStore probe run; it never
    // scans the table or keyspace.
    uint32_t active_expire(uint32_t budget) {
        // Expiry after the cut is a post-cut deletion.  Leaving the object physically present lets
        // traversal serialize its absolute deadline; find() still reports it logically absent.
        if (snapshot_active_) return 0;
        if (rehashing()) rehash_step();
        uint32_t removed = 0;
        expires_.sample(budget, [&](uint64_t h) {
            KvObj* o = find_hash_in(0, h);
            if (!o && rehashing()) o = find_hash_in(1, h);
            if (!o || !o->has_ttl_slot()) {
                untrack_expire(h);       // stale tracker after a replacement or collision
                return;
            }
            if (atomic_has_record(h, o->key())) return;  // promotion resolves the winning TTL
            const int64_t at = deadline(h, o);
            if (at < 0) { untrack_expire(h); return; }
            if (at > cached_now_ms_) return;
            const Slice key = o->key();
            notify_flat_store_emit(this, NOTIFY_EXPIRED, NotifyEventId::Expired, key);
            (void)aof_.record_delete(key);
            if (erase_in(0, h, key) || (rehashing() && erase_in(1, h, key))) {
                removed++;
                note_active_expire_reap(at);
                if (expired_counter_) (*expired_counter_)++;
            }
        });
        if (__builtin_expect(field_expires_.size() != 0, false))
            removed += active_expire_fields(budget);
        if (__builtin_expect(expires_.migrating() || field_expires_.migrating(), false)) removed++;
        return removed;
    }

    // Hash-field deadlines ride the SAME attention mechanism as key deadlines: a per-shard index of
    // key hashes, a persistent cursor, and a slot budget that counts empty slots so a pass can never
    // degenerate into a keyspace walk. Reaping is the ex thread touching its own shard, so it is
    // legal here for exactly the reason active_expire() is.
    uint32_t active_expire_fields(uint32_t budget) {
        if (snapshot_active_) return 0;
        uint32_t removed = 0;
        field_expires_.sample(budget, [&](uint64_t h) {
            KvObj* o = find_any_hash_in(0, h);
            if (!o && rehashing()) o = find_any_hash_in(1, h);
            if (!o || static_cast<Type>(o->type) != Type::Hash ||
                static_cast<Enc>(o->enc) == Enc::Compact ||
                !static_cast<const HashVal*>(o->external_ptr())->ttls) {
                // Key gone, replaced, re-typed, or every field TTL removed. Self-healing here is
                // what lets registration stay a cheap unconditional insert on the write path.
                field_expires_.erase(h);
                field_ttl_gate_ = field_expires_.size();
                return;
            }
            if (atomic_has_record(h, o->key())) return;
            uint32_t reaped = 0;
            const size_t before = kvobj_size(o);
            const bool empty = hash_ttl_active_reap(*this, o, cached_now_ms_, reaped);
            if (!reaped && !empty) return;
            const Slice key = o->key();
            notify_flat_store_emit(this, NOTIFY_HASH, NotifyEventId::Hexpired, key);
            field_expired_ += reaped;
            if (!empty) {
                note_object_size_change(before, kvobj_size(o));
                (void)aof_.record_post_image_buffered(*this, h, key);
                removed += reaped;
                return;
            }
            (void)aof_.record_delete(key);
            notify_flat_store_emit(this, NOTIFY_GENERIC, NotifyEventId::Del, key);
            if (erase_in(0, h, key) || (rehashing() && erase_in(1, h, key))) removed += reaped;
            field_expires_.erase(h);
            field_ttl_gate_ = field_expires_.size();
        });
        return removed;
    }
    uint64_t field_expired() const { return field_expired_; }

    // Pre-execution budget gate for growth commands (CmdFlags::DenyOom), redis-style: over-budget
    // shards evict (policy permitting) until under, else the command is refused. The mutation is
    // NOT sized -- like redis, one op may overshoot and the next gated op pays it back. Suspended
    // during capture for the same reason admission is (victims are not pre-imaged).
    bool budget_admit(Slice protected_key) {
        if (!__builtin_expect(maxmemory_enabled_, false)) return true;
        if (snapshot_active_) return true;
        // NOT make_room_for: projected_bytes subtracts the protected key's own size (replacement
        // semantics), so the very key being grown always "fits". The gate asks the raw question:
        // is the shard over budget NOW; evict others, never the op's key; else refuse.
        if (accounted_bytes() <= maxmemory_limit_) return true;
        if (maxmemory_policy_ == MaxmemoryPolicy::NoEviction) return refuse_over_budget();
        uint32_t budget = kEvictionsPerOp;
        while (budget-- && accounted_bytes() > maxmemory_limit_) {
            KvObj* victim = choose_victim(protected_key);
            if (!victim) return refuse_over_budget();
            const uint64_t hash = hash_key(victim->key());
            const Slice key = victim->key();
            (void)aof_.record_delete(key);
            const uint32_t before = size();
            const bool live = erase(hash, key);
            if (size() == before) return refuse_over_budget();
            if (live && evicted_counter_) (*evicted_counter_)++;
        }
        if (accounted_bytes() <= maxmemory_limit_) return true;
        return refuse_over_budget();
    }


    // Takes ownership of `o` only on success; frees anything it displaces.
    InsertResult insert(uint64_t h, KvObj* o) {
        if (__builtin_expect(read_local_enabled_, false)) return insert_read_local(h, o);
        const bool capturing = rehashing() && snapshot_active_;
        if (rehashing()) {
            if (!capturing) rehash_step();
        } else {
            if (!maybe_start_grow()) return InsertResult::Failed;
        }
        // Preparation normally has the same cost as the original insert path.  Only an actually
        // full prepared table consults this state and refuses a new key rather than resizing it.
        if (static_cast<uint64_t>(live_[0]) + tombs_[0] + 1 >= cap_[0] &&
            snapshot_prepared_ &&
            !find_in(0, h, o->key())) return InsertResult::Failed;
        if (capturing) {
            const bool exists = find_in(0, h, o->key()) || find_in(1, h, o->key());
            // The fresh table is deliberately overprovisioned at the cut.  Refuse only genuinely
            // new keys once the complete logical set would no longer fit; replacements preserve
            // cardinality.  This is an ordinary insert failure, never snapshot corruption.
            // Count current-table tombstones as promised destination slots too.  Otherwise a
            // churn-heavy capture could leave enough logical capacity but no EMPTY terminating
            // slot, and the post-capture merge would be unable to place a frozen pointer.
            if (!exists && static_cast<uint64_t>(live_[0]) + tombs_[0] + live_[1] + 1 >=
                               cap_[0])
                return InsertResult::Failed;
        }
        // Disabled maxmemory pays one predicted branch and does no metadata write or accounting
        // work. Enabled admission is bounded to kEvictionsPerOp victim deletions.  Admission is
        // SUSPENDED while a capture is active: make_room_for deletes sampled victims, and the
        // snapshot write-gate pre-images only the incoming command's key — an evicted, unvisited
        // frozen victim would vanish from the dump.  The capture window is short and bounded.
        if (__builtin_expect(maxmemory_enabled_, false) && !snapshot_active_) {
            if (!make_room_for(o->key(), kvobj_size(o))) return InsertResult::MaxmemoryOom;
            if (o->eviction_meta() == 0) initialize_meta(o);
        }
        // Evict any copy still in the old table FIRST, or it outlives a later delete of the new one
        // and the key resurrects — see the header.
        if (rehashing()) {
            bool expired = false;
            if (erase_in(1, h, o->key(), &expired) && expired && expired_counter_)
                (*expired_counter_)++;
        }
        return insert_into(0, h, o, true) ? InsertResult::Inserted : InsertResult::Failed;
    }

    InsertResult insert_notify(uint64_t h, KvObj* o, FlatNotifySink* sink) {
        KvObj* candidate = find_in(0, h, o->key());
        if (!candidate && rehashing()) candidate = find_in(1, h, o->key());
        const bool expired = candidate && deadline_elapsed(h, candidate, cached_now_ms_);
        const bool report_new = !candidate || expired;
        if (expired)
            notify_emit(sink, NOTIFY_EXPIRED, NotifyEventId::Expired, candidate->key());
        const InsertResult result = insert(h, o);
        if (result == InsertResult::Inserted && report_new)
            notify_emit(sink, NOTIFY_NEW, NotifyEventId::New, o->key());
        return result;
    }

    bool erase(uint64_t h, Slice key) {
        if (__builtin_expect(read_local_enabled_, false)) return erase_read_local(h, key);
        if (rehashing() && !snapshot_active_) rehash_step();
        bool expired = false;
        if (erase_in(0, h, key, &expired)) {
            maybe_start_shrink();
            if (expired && expired_counter_) (*expired_counter_)++;
            return !expired;
        }
        if (rehashing() && erase_in(1, h, key, &expired)) {
            maybe_start_shrink();
            if (expired && expired_counter_) (*expired_counter_)++;
            return !expired;
        }
        return false;
    }

    bool erase_notify(uint64_t h, Slice key, FlatNotifySink* sink,
                      EraseEvent event = EraseEvent::Del) {
        KvObj* candidate = find_in(0, h, key);
        if (!candidate && rehashing()) candidate = find_in(1, h, key);
        if (candidate) {
            const bool expired = deadline_elapsed(h, candidate, cached_now_ms_);
            if (expired)
                notify_emit(sink, NOTIFY_EXPIRED, NotifyEventId::Expired, candidate->key());
            else if (event == EraseEvent::Del)
                notify_emit(sink, NOTIFY_GENERIC, NotifyEventId::Del, candidate->key());
            else if (event == EraseEvent::Evicted)
                notify_emit(sink, NOTIFY_EVICTED, NotifyEventId::Evicted, candidate->key());
        }
        return erase(h, key);
    }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (int t = 0; t < 2; t++)
            if (tab_[t])
                for (uint32_t i = 0; i < cap_[t]; i++)
                    if (KvObj* o = ptr_of(tab_[t][i])) fn(o);
    }

    // FLUSH is intentionally proportional to the table capacity it discards. Borrowed string
    // values move to the existing retirement list, so a send already in flight remains valid.
    void clear() {
        if (__builtin_expect(read_local_enabled_, false)) {
            clear_read_local();
            return;
        }
        // Allocate the small replacement before touching either live table. If that cold allocation
        // fails, retain and zero table 0 after retiring its objects; FLUSH still has a valid empty
        // table and never exposes a half-demoted state.
        uint64_t* fresh = allocate_table(1024);
        expires_.clear();
        for (int t = 0; t < 2; t++) {
            if (!tab_[t]) continue;
            for (uint32_t i = 0; i < cap_[t]; i++)
                if (KvObj* o = ptr_of(tab_[t][i])) retire_obj(o);
            if (t == 0 && !fresh) {
                std::memset(tab_[0], 0, static_cast<size_t>(
                    static_cast<uint64_t>(cap_[0]) * sizeof(uint64_t)));
                live_[0] = tombs_[0] = 0;
                continue;
            }
            std::free(tab_[t]);
            tab_[t] = nullptr;
            cap_[t] = mask_[t] = live_[t] = tombs_[t] = 0;
        }
        field_expires_.clear();
        field_ttl_gate_ = 0;
        rehash_pos_ = 0;
        if (fresh) install_empty_table(0, fresh, 1024);
    }

    // FLUSH after its scatter gate has prepared every frozen pre-image.  Preserve both table
    // allocations and their slot numbering until the capture walker releases its cursor; turn all
    // live entries into ordinary tombstones instead of freeing the tables as clear() does.
    void clear_during_snapshot() {
        if (__builtin_expect(read_local_enabled_, false)) {
            clear_during_snapshot_read_local();
            return;
        }
        expires_.clear();
        for (int t = 0; t < 2; t++) {
            if (!tab_[t]) continue;
            for (uint32_t i = 0; i < cap_[t]; i++) {
                if (KvObj* object = ptr_of(tab_[t][i])) {
                    retire_obj(object);
                    tab_[t][i] = kTombBit;
                    live_[t]--;
                    tombs_[t]++;
                }
            }
        }
        field_expires_.clear();
        field_ttl_gate_ = 0;
    }

    // RANDOMKEY starts from an owner-private draw, independent of the IO-side draw that selected
    // this shard. Reusing that routing draw correlates its low bits with the shard id and leaves
    // physical-slot residue classes unreachable when table capacities are powers of two. Reservoir
    // selection across the one wrapped walk keeps adjacent live slots from inheriting a tiny share
    // of a sparse table's probability. Lazy expiry is performed before a key becomes a candidate.
    KvObj* random_live() {
        const uint64_t total = static_cast<uint64_t>(cap_[0]) + cap_[1];
        if (!total || size() == 0) return nullptr;
        const uint64_t start_pos = next_random() % total;
        KvObj* chosen = nullptr;
        uint64_t live_seen = 0;
        for (uint64_t step = 0; step < total; step++) {
            uint64_t pos = start_pos + step;
            if (pos >= total) pos -= total;
            const int t = pos < cap_[0] ? 0 : 1;
            const uint32_t slot = static_cast<uint32_t>(pos - (t ? cap_[0] : 0));
            KvObj* o = ptr_of(tab_[t][slot]);
            if (!o) continue;
            const uint64_t h = hash_key(o->key());
            if (!deadline_elapsed(h, o, cached_now_ms_)) {
                if (next_random() % ++live_seen == 0) chosen = o;
                continue;
            }
            notify_flat_store_emit(this, NOTIFY_EXPIRED, NotifyEventId::Expired, o->key());
            (void)aof_.record_delete(o->key());
            erase_in(t, h, o->key());
            if (expired_counter_) (*expired_counter_)++;
        }
        return chosen;
    }

    // The cursor is a bit-reversed HOME index (see scan_cursor_next above), not a physical slot and
    // not a table selector -- there is nothing left in it that a resize can invalidate. One step
    // covers a whole logical home bucket, which under linear probing is the run of non-EMPTY slots
    // starting at that home, so a key's probe displacement no longer decides whether it is seen.
    // While a rehash is draining, the step covers the smaller table's home AND every home in the
    // larger table that expands from it before the cursor moves, exactly as Redis does.
    // COUNT keeps its Redis meaning -- HOMES visited per call, so a call still yields about COUNT
    // entries at our load factor and a full walk costs the same number of round trips it always
    // did. A second, looser budget of 10*COUNT examined SLOTS is what keeps one call bounded when
    // tombstones stretch the probe runs; it is the same pair of budgets SSCAN documents.
    // `expire_on_visit` distinguishes the two legitimate walkers. SCAN/KEYS keep redis semantics:
    // a visited dead key is logically expired on the spot (event, AOF delete, erase). ACCOUNTING
    // walks (the LB bucket-byte census) must pass false: they run from the executor loop at census
    // cadence, not inside any logical operation, and a census that expires keys performs the
    // expiry OUTSIDE every in-flight operation's pinned cut -- it physically deleted keys between
    // the fragments of an MGET fan-out and tore the one-cut-per-logical-operation law (caught by
    // the expwide battery the first time the census shipped default-on).
    template <typename Fn>
    uint64_t scan(uint64_t cursor, uint32_t count, Fn&& fn, bool expire_on_visit = true) {
        if (!tab_[0]) return 0;
        const uint64_t slot_budget = static_cast<uint64_t>(count) * 10;
        uint32_t homes = 0;
        uint64_t slots = 0;
        do {
            if (!rehashing()) {
                slots += scan_home(0, static_cast<uint32_t>(cursor) & mask_[0], fn,
                                   expire_on_visit);
                homes++;
                cursor = scan_cursor_next(cursor, mask_[0]);
            } else if (mask_[0] == mask_[1]) {
                // Same-size rebuild (tombstone reclaim): identical homes, so one visit each.
                const uint32_t home = static_cast<uint32_t>(cursor) & mask_[0];
                slots += scan_home(0, home, fn, expire_on_visit);  // separate statements: both
                slots += scan_home(1, home, fn, expire_on_visit);  // emit, order must be defined
                homes += 2;
                cursor = scan_cursor_next(cursor, mask_[0]);
            } else {
                const int small = cap_[0] < cap_[1] ? 0 : 1;   // grow puts the old table in 1,
                const int large = small ^ 1;                   // shrink puts it in 0
                const uint64_t small_mask = mask_[small];
                const uint64_t large_mask = mask_[large];
                slots += scan_home(small, static_cast<uint32_t>(cursor & small_mask), fn,
                                   expire_on_visit);
                homes++;
                do {
                    slots += scan_home(large, static_cast<uint32_t>(cursor & large_mask), fn,
                                       expire_on_visit);
                    homes++;
                    cursor = scan_cursor_next(cursor, large_mask);
                } while (cursor & (small_mask ^ large_mask));
            }
        } while (cursor && homes < count && slots < slot_budget);
        return cursor;
    }

    // Warm the slots this key will probe, for a whole batch before any of it executes, so the DRAM
    // round trips overlap. A plain prefetch pass, not an AMAC-style interleaved state machine.
    void prefetch(uint64_t h) const {
        if (tab_[0]) __builtin_prefetch(&tab_[0][slot_start(0, h)], 0, 3);
        if (tab_[1]) __builtin_prefetch(&tab_[1][slot_start(1, h)], 0, 3);
    }

    // One hash for the whole server: the router takes its bucket from the low bits and FlatStore
    // mixes for its index, so both must agree and it lives here. Word-at-a-time, because FNV-1a
    // costs one DEPENDENT multiply per byte and a 20-character key is then a 20-long chain.
    static uint64_t hash_key(Slice k) {
        if (g_hash_kind == HashKind::SipHash12) return siphash12(k.p, k.n);
        return hash_key_mix(k);
    }

    static uint64_t hash_key_mix(Slice k) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(k.p);
        uint32_t n = k.n;
        uint64_t h = (0x9e3779b97f4a7c15ULL ^ (static_cast<uint64_t>(n) * 0xff51afd7ed558ccdULL)) ^ g_hash_seed;
        auto rd8 = [](const uint8_t* q) { uint64_t v; std::memcpy(&v, q, 8); return v; };
        auto rd4 = [](const uint8_t* q) { uint32_t v; std::memcpy(&v, q, 4); return v; };
        while (n >= 8) { h = mix64(h ^ rd8(p)); p += 8; n -= 8; }
        if (n >= 4)    { h = mix64(h ^ ((static_cast<uint64_t>(rd4(p)) << 32) | rd4(p + n - 4))); }
        else if (n)    { uint64_t t = p[0];
                         t = (t << 16) | (static_cast<uint64_t>(p[n >> 1]) << 8) | p[n - 1];
                         h = mix64(h ^ t); }
        return mix64(h);
    }

private:
    static constexpr uint32_t kSnapshotRecordTag = 0x44434552;  // "RECD", little endian
    static constexpr uint32_t kSnapshotRecordHeader = 32;

    // Key-expiry tracker registration. The index is hash-only: an entry states that this hash may
    // carry a deadline, so registration is driven by LOGICAL volatility and a retained-but-
    // persisted slot must not leave one behind.
    bool track_expire(uint64_t hash, KvObj* object) {
        if (!object) return true;
        const int64_t at = object->expire_at_ms();
        if (at >= 0) return expires_.insert(hash, at);
        // The un-TTL'd store never reaches the index at all. Repeated here rather than left to
        // ExpireIndex::erase() so the CALL goes too, which is most of what it cost.
        //
        // The erase itself STAYS, and it is load-bearing. `SET k v EX 10` then `SET k v` clears
        // the deadline (redis semantics) and this is what takes the hash back out. Two things
        // break if a stale entry is allowed to survive instead:
        //   - INFO keyspace `expires` is expires_.size(); tests/expireindex.py asserts that count
        //     EXACTLY (== n, == 0), so it would over-report until active expiry happened to
        //     resample the hash;
        //   - with TOMO_TTL_DEADLINE_SIDECAR=1 (src/store/store_ttl.h) deadline() reads the
        //     deadline back OUT of this index, so a stale entry resurrects the TTL the SET just
        //     cleared -- a wrong answer, not a slow one.
        // Reaping alone would tolerate a false positive (active_expire() cleans stale trackers,
        // and the index is documented hash-only, "may carry a deadline"); those two do not.
        if (expires_.size() == 0) return true;
        expires_.erase(hash);
        return true;
    }

    void untrack_expire(uint64_t hash) { expires_.erase(hash); }

    struct SnapshotRecordState {
        bool active = false;
        bool preimage = false;
        uint32_t slot = 0;
        uint8_t header[kSnapshotRecordHeader] = {};
        uint32_t header_offset = 0;
        uint32_t key_offset = 0;
        SnapshotSaveCursor value;
        SnapshotTypeHooks hooks{};
    };

    std::unique_ptr<SnapshotChunk> make_snapshot_chunk(uint32_t flags) {
        try {
            auto chunk = std::make_unique<SnapshotChunk>();
            chunk->sid = snapshot_shard_id_;
            chunk->sequence = snapshot_sequence_++;
            chunk->flags = flags;
            chunk->bytes.reserve(kSnapshotChunkBytes);
            return chunk;
        } catch (const std::bad_alloc&) {
            snapshot_failed_ = true;
            return nullptr;
        }
    }

    void snapshot_seal(uint32_t flags) {
        if (!snapshot_build_ || snapshot_ready_) return;
        snapshot_build_->flags |= flags;
        snapshot_ready_ = std::move(snapshot_build_);
    }

    uint32_t snapshot_emit(const uint8_t* source, uint32_t length, uint32_t& budget) {
        uint32_t emitted = 0;
        while (length && budget && !snapshot_ready_ && !snapshot_failed_) {
            if (!snapshot_build_) snapshot_build_ = make_snapshot_chunk(0);
            if (!snapshot_build_) break;
            const uint32_t room = kSnapshotChunkBytes -
                                  static_cast<uint32_t>(snapshot_build_->bytes.size());
            const uint32_t take = std::min({length, budget, room});
            try {
                snapshot_build_->bytes.insert(snapshot_build_->bytes.end(), source, source + take);
            } catch (const std::bad_alloc&) {
                snapshot_failed_ = true;
                break;
            }
            source += take; length -= take; budget -= take; emitted += take;
            if (snapshot_build_->bytes.size() == kSnapshotChunkBytes) snapshot_seal(0);
        }
        return emitted;
    }

    bool snapshot_start_record(const KvObj* object, uint32_t slot, bool preimage) {
        SnapshotRecordState state;
        state.active = true;
        state.preimage = preimage;
        state.slot = slot;
        state.hooks = snapshot_type_hooks(static_cast<Type>(object->type));
        uint8_t encoding = 0;
        const SnapshotHookStatus status = state.hooks.begin_save(*object, state.value, encoding);
        if (status != SnapshotHookStatus::Ok) {
            snapshot_failed_ = true;
            return false;
        }
        snapshot_put_u32(state.header + 0, kSnapshotRecordTag);
        state.header[4] = object->type;
        state.header[5] = encoding;
        state.header[6] = state.header[7] = 0;
        snapshot_put_u32(state.header + 8, object->klen());
        snapshot_put_u32(state.header + 12, 0);
        snapshot_put_u64(state.header + 16, state.value.total);
        snapshot_put_u64(state.header + 24, static_cast<uint64_t>(object->expire_at_ms()));
        snapshot_record_ = state;
        return true;
    }

    void snapshot_progress_record(uint32_t& budget) {
        if (__builtin_expect(read_local_enabled_, false)) {
            snapshot_progress_record_read_local(budget);
            return;
        }
        SnapshotRecordState& state = snapshot_record_;
        const KvObj* object = state.value.object;
        if (!state.active || !object) { snapshot_failed_ = true; return; }

        if (state.header_offset < kSnapshotRecordHeader) {
            const uint32_t n = snapshot_emit(state.header + state.header_offset,
                                             kSnapshotRecordHeader - state.header_offset, budget);
            state.header_offset += n;
            if (state.header_offset != kSnapshotRecordHeader || snapshot_ready_) return;
        }
        if (state.key_offset < object->klen()) {
            const uint32_t n = snapshot_emit(
                reinterpret_cast<const uint8_t*>(object->key_ptr()) + state.key_offset,
                object->klen() - state.key_offset, budget);
            state.key_offset += n;
            if (state.key_offset != object->klen() || snapshot_ready_) return;
        }
        while (state.value.offset < state.value.total && budget && !snapshot_ready_) {
            if (!snapshot_build_) snapshot_build_ = make_snapshot_chunk(0);
            if (!snapshot_build_) return;
            const size_t room = kSnapshotChunkBytes - snapshot_build_->bytes.size();
            const size_t capacity = std::min<size_t>(room, budget);
            const size_t old_size = snapshot_build_->bytes.size();
            try {
                snapshot_build_->bytes.resize(old_size + capacity);
            } catch (const std::bad_alloc&) {
                snapshot_failed_ = true;
                return;
            }
            size_t written = 0;
            const SnapshotHookStatus status = state.hooks.read_save(
                state.value, snapshot_build_->bytes.data() + old_size, capacity, written);
            if (status != SnapshotHookStatus::Ok || written > capacity ||
                (written == 0 && state.value.offset < state.value.total)) {
                snapshot_build_->bytes.resize(old_size);
                snapshot_failed_ = true;
                return;
            }
            snapshot_build_->bytes.resize(old_size + written);
            budget -= static_cast<uint32_t>(written);
            if (snapshot_build_->bytes.size() == kSnapshotChunkBytes) snapshot_seal(0);
        }
        if (state.value.offset != state.value.total || snapshot_ready_) return;

        const bool preimage = state.preimage;
        const uint32_t slot = state.slot;
        snapshot_record_ = {};
        if (preimage) tab_[1][slot] |= kTombBit;
        else          snapshot_pos_++;
    }

    void snapshot_finish_stream() {
        if (snapshot_finished_ || snapshot_ready_ || snapshot_record_.active) return;
        if (!snapshot_build_) snapshot_build_ = make_snapshot_chunk(0);
        if (!snapshot_build_) return;
        snapshot_seal(SnapshotFrameEnd);
        snapshot_finished_ = true;
    }

    struct Borrow {
        const char* ptr;
        uint32_t    refs;
        KvObj*      retired;
    };

    // BORROW REGISTRY LOOKUP.
    // borrows_ is the registry; every borrow, release, in-place-overwrite check and retirement has
    // to find one entry by pointer identity in it. That was a linear scan, so one MGET whose values
    // all take the borrow path registered K pointers with a scan each -- quadratic in K -- and a
    // shard holding B concurrent borrows paid O(B) on every further borrowed read. Measured: a
    // borrowed GET went 2903 -> 3185 ns/op as B went 0 -> ~1100 while the identical non-borrowed
    // GET stayed flat at ~1100 ns.
    //
    // borrow_idx_ is an OPTIONAL open-addressed pointer -> slot index over the same vector. It is
    // never the source of truth: correctness only ever depends on borrows_, and every failure path
    // simply releases the index and falls back to the scan. Below kBorrowIndexMin entries the scan
    // is faster than hashing, so an ordinary connection with a handful of borrows never builds one.
    static constexpr uint32_t kNoBorrow  = 0xffffffffu;   // also the empty-slot sentinel
    static constexpr uint32_t kBorrowTomb = 0xfffffffeu;
    static constexpr uint32_t kBorrowIndexMin = 32;

    static size_t borrow_hash(const char* ptr) {
        return static_cast<size_t>(mix64(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr))));
    }

    uint32_t borrow_find(const char* ptr) const {
        if (borrow_idx_.empty()) {
            for (uint32_t i = 0; i < borrows_.size(); i++)
                if (borrows_[i].ptr == ptr) return i;
            return kNoBorrow;
        }
        const size_t mask = borrow_idx_.size() - 1;
        size_t pos = borrow_hash(ptr) & mask;
        for (size_t probes = 0; probes <= mask; probes++) {
            const uint32_t v = borrow_idx_[pos];
            if (v == kNoBorrow) return kNoBorrow;
            if (v != kBorrowTomb && borrows_[v].ptr == ptr) return v;
            pos = (pos + 1) & mask;
        }
        return kNoBorrow;
    }

    void borrow_index_release() {
        std::vector<uint32_t>().swap(borrow_idx_);
        borrow_tombs_ = 0;
    }

    void borrow_index_put(const char* ptr, uint32_t at) {
        if (borrow_idx_.empty()) return;
        const size_t cap = borrow_idx_.size();
        const size_t mask = cap - 1;
        size_t pos = borrow_hash(ptr) & mask;
        size_t first_tomb = cap;
        for (size_t probes = 0; probes < cap; probes++) {
            const uint32_t v = borrow_idx_[pos];
            if (v == kNoBorrow) {
                if (first_tomb != cap) { pos = first_tomb; borrow_tombs_--; }
                borrow_idx_[pos] = at;
                return;
            }
            if (v == kBorrowTomb) {
                if (first_tomb == cap) first_tomb = pos;
            } else if (borrows_[v].ptr == ptr) {
                borrow_idx_[pos] = at;
                return;
            }
            pos = (pos + 1) & mask;
        }
        borrow_index_release();   // full: cannot happen at the 70% rebuild trigger
    }

    void borrow_index_dropped(const char* ptr) {
        if (borrow_idx_.empty()) return;
        const size_t mask = borrow_idx_.size() - 1;
        size_t pos = borrow_hash(ptr) & mask;
        for (size_t probes = 0; probes <= mask; probes++) {
            const uint32_t v = borrow_idx_[pos];
            if (v == kNoBorrow) return;
            if (v != kBorrowTomb && borrows_[v].ptr == ptr) {
                borrow_idx_[pos] = kBorrowTomb;
                borrow_tombs_++;
                return;
            }
            pos = (pos + 1) & mask;
        }
    }

    bool borrow_index_rebuild() {
        uint64_t cap = 64;
        const uint64_t occupancy = static_cast<uint64_t>(borrows_.size());
        if (occupancy > UINT64_MAX / uint64_t{200}) return false;
        const uint64_t wanted = occupancy * uint64_t{200};
        for (;;) {                                             // land near 35% loaded
            if (cap > UINT64_MAX / uint64_t{70}) return false;
            if (cap * uint64_t{70} >= wanted) break;
            if (cap > static_cast<uint64_t>(borrow_idx_.max_size()) / 2) {
                return false;
            }
            cap *= uint64_t{2};
        }
        std::vector<uint32_t> fresh;
        try {
            fresh.assign(static_cast<size_t>(cap), kNoBorrow);
        } catch (const std::bad_alloc&) {
            return false;
        }
        const size_t mask = fresh.size() - 1;
        for (uint32_t i = 0; i < borrows_.size(); i++) {
            size_t pos = borrow_hash(borrows_[i].ptr) & mask;
            while (fresh[pos] != kNoBorrow) pos = (pos + 1) & mask;
            fresh[pos] = i;
        }
        borrow_idx_.swap(fresh);
        borrow_tombs_ = 0;
        return true;
    }

    void borrow_index_added(const char* ptr, uint32_t at) {
        if (borrow_idx_.empty()) {
            if (borrows_.size() < kBorrowIndexMin) return;    // short scan beats a hash
            (void)borrow_index_rebuild();                     // allocation failure stays scan mode
            return;
        }
        if ((static_cast<uint64_t>(borrows_.size()) + borrow_tombs_) * 100 >=
            static_cast<uint64_t>(borrow_idx_.size()) * 70) {
            if (borrow_index_rebuild()) return;
            // The old index remains valid on allocation failure and still has the trigger's 30%
            // headroom. Add this borrow there; borrow_index_put() falls back to scan mode if a
            // pathological tombstone layout nevertheless leaves no usable slot.
        }
        borrow_index_put(ptr, at);
    }

    static uint32_t round_pow2(uint32_t v) {
        uint32_t p = kMinCap;
        while (p < v) {
            const uint64_t next = static_cast<uint64_t>(p) * 2;
            if (next > UINT32_MAX) return 0;
            p = static_cast<uint32_t>(next);
        }
        return p;
    }
    static uint16_t tag_of(uint64_t h)      { return static_cast<uint16_t>((h >> 49) & 0x7fff); }
    static uint16_t tag_of_word(uint64_t w) { return static_cast<uint16_t>((w >> 49) & 0x7fff); }
    static KvObj*   ptr_of(uint64_t w)      { return reinterpret_cast<KvObj*>(w & kPtrMask); }
    static uint64_t make_word(uint16_t tag, KvObj* o) {
        const uint64_t pointer = reinterpret_cast<uint64_t>(o);
        assert((pointer & ~kPtrMask) == 0 && "KvObj pointer exceeds FlatStore's 48-bit encoding");
        return (static_cast<uint64_t>(tag) << 49) | pointer;
    }
    uint32_t slot_start(int t, uint64_t h) const { return static_cast<uint32_t>(mix64(h)) & mask_[t]; }

    KvObj* find_without_touch(uint64_t h, Slice key) {
        // During capture tab_[1] is the positional frozen image. Moving even an unrelated entry
        // here can carry it past the snapshot cursor and omit it from the BASE.
        if (rehashing() && !snapshot_active_) rehash_step();
        if (KvObj* o = find_in(0, h, key)) return live_or_expire(0, h, key, o);
        if (rehashing())
            if (KvObj* o = find_in(1, h, key)) return live_or_expire(1, h, key, o);
        return nullptr;
    }

    uint64_t next_random() {
        // xorshift64*: shard-owner-only state used by cold random sampling paths.
        uint64_t x = random_state_;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        random_state_ = x;
        return x * 2685821657736338717ULL;
    }

    uint8_t lru_clock() const { return cached_lru_clock_; }

    void initialize_meta(KvObj* o) {
        if (__builtin_expect(read_local_enabled_, false)) {
            initialize_meta_read_local(o);
            return;
        }
        if (maxmemory_policy_is_lru(maxmemory_policy_)) {
            o->set_eviction_meta(lru_clock());
        } else if (maxmemory_policy_is_lfu(maxmemory_policy_)) {
            o->set_eviction_meta(5);
        }
    }

    void touch(KvObj* o) {
        if (__builtin_expect(read_local_enabled_, false)) {
            touch_read_local(o);
            return;
        }
        if (maxmemory_policy_is_lru(maxmemory_policy_)) {
            o->set_eviction_meta(lru_clock());
            return;
        }
        if (!maxmemory_policy_is_lfu(maxmemory_policy_)) return;

        uint8_t count = o->eviction_meta();
        if (count == 0) count = 5;
        // Redis-style logarithmic increment with its default factor 10, compressed to five bits.
        const uint32_t base = count > 5 ? static_cast<uint32_t>(count - 5) : 0;
        const uint32_t denominator = base * 10 + 1;
        if (count < 31 && next_random() % denominator == 0) count++;
        o->set_eviction_meta(count);
    }

    KvObj* random_allkeys_candidate() {
        const uint64_t total_cap = static_cast<uint64_t>(cap_[0]) + cap_[1];
        if (total_cap == 0 || size() == 0) return nullptr;
        for (uint32_t attempt = 0; attempt < kSampleProbeAttempts; attempt++) {
            uint64_t pos = next_random() % total_cap;
            const int table = pos < cap_[0] ? 0 : 1;
            if (table == 1) pos -= cap_[0];
            if (KvObj* o = ptr_of(tab_[table][pos])) return o;
        }
        // Sparse-table backstop. It remains bounded and advances between calls rather than
        // restarting at slot zero and repeatedly missing the same empty prefix.
        for (uint32_t attempt = 0; attempt < kSampleProbeAttempts; attempt++) {
            if (sample_cursor_ >= total_cap) sample_cursor_ = 0;
            uint64_t pos = sample_cursor_++;
            const int table = pos < cap_[0] ? 0 : 1;
            if (table == 1) pos -= cap_[0];
            if (KvObj* o = ptr_of(tab_[table][pos])) return o;
        }
        return nullptr;
    }

    KvObj* random_volatile_candidate() {
        uint64_t hash = 0;
        if (!expires_.random_hash(next_random(), kSampleProbeAttempts, hash)) return nullptr;
        KvObj* o = find_hash_in(0, hash);
        if (!o && rehashing()) o = find_hash_in(1, hash);
        if (o && deadline(hash, o) < 0) { untrack_expire(hash); return nullptr; }
        return o;
    }

    KvObj* choose_victim(Slice protected_key) {
        if (__builtin_expect(read_local_enabled_, false))
            return choose_victim_read_local(protected_key);
        KvObj* best = nullptr;
        KvObj* seen[64];
        uint32_t seen_count = 0;
        uint64_t best_score = 0;
        for (uint32_t i = 0; i < maxmemory_samples_; i++) {
            KvObj* candidate = maxmemory_policy_is_volatile(maxmemory_policy_)
                ? random_volatile_candidate() : random_allkeys_candidate();
            if (!candidate || candidate->key().key_eq(protected_key)) continue;
            if (atomic_has_record(hash_key(candidate->key()), candidate->key())) continue;
            bool duplicate = false;
            for (uint32_t j = 0; j < seen_count; j++)
                if (seen[j] == candidate) { duplicate = true; break; }
            if (duplicate) continue;
            seen[seen_count++] = candidate;

            uint64_t score = 0;
            switch (maxmemory_policy_) {
                case MaxmemoryPolicy::AllKeysRandom:
                case MaxmemoryPolicy::VolatileRandom:
                    score = next_random();
                    break;
                case MaxmemoryPolicy::AllKeysLru:
                case MaxmemoryPolicy::VolatileLru: {
                    const uint8_t age = static_cast<uint8_t>(
                        (lru_clock() - candidate->eviction_meta()) & 0x1f);
                    score = (static_cast<uint64_t>(age) << 56) | (next_random() & ((1ULL << 56) - 1));
                    break;
                }
                case MaxmemoryPolicy::AllKeysLfu:
                case MaxmemoryPolicy::VolatileLfu: {
                    // With only five free header bits, sampling is also the bounded aging event:
                    // one count is forgotten whenever a key competes for eviction.
                    uint8_t count = candidate->eviction_meta();
                    if (count) candidate->set_eviction_meta(--count);
                    score = (static_cast<uint64_t>(31 - count) << 56) |
                            (next_random() & ((1ULL << 56) - 1));
                    break;
                }
                case MaxmemoryPolicy::VolatileTtl:
                    score = std::numeric_limits<uint64_t>::max() -
                            static_cast<uint64_t>(deadline(hash_key(candidate->key()), candidate));
                    break;
                case MaxmemoryPolicy::NoEviction:
                    return nullptr;
            }
            if (!best || score > best_score) { best = candidate; best_score = score; }
        }
        if (best) {
            const bool expired = deadline_elapsed(hash_key(best->key()), best, cached_now_ms_);
            notify_flat_store_emit(this,
                expired ? NOTIFY_EXPIRED : NOTIFY_EVICTED,
                expired ? NotifyEventId::Expired : NotifyEventId::Evicted, best->key());
        }
        return best;
    }

    size_t projected_bytes(Slice key, size_t incoming_bytes) const {
        const uint64_t hash = hash_key(key);
        KvObj* old = find_in(0, hash, key);
        if (!old && rehashing()) old = find_in(1, hash, key);

        size_t used = accounted_bytes();
        if (old) {
            const size_t old_bytes = kvobj_size(old);
            used = used >= old_bytes ? used - old_bytes : 0;
        } else if (used > std::numeric_limits<size_t>::max() - kSlotOverheadPerKey) {
            return std::numeric_limits<size_t>::max();
        } else {
            used += kSlotOverheadPerKey;
        }
        if (incoming_bytes > std::numeric_limits<size_t>::max() - used)
            return std::numeric_limits<size_t>::max();
        return used + incoming_bytes;
    }

    // The write is about to be REFUSED. Eviction either could not run or did not get this shard
    // under budget, so the armed-write block cache is the last physical memory we can hand back
    // before telling the client no. It is deliberately NOT released while eviction is succeeding:
    // the cache is already bounded by the shard's own live footprint, so releasing it on every
    // over-budget write would only make the next write allocate again -- the same allocator call
    // count as before the cache existed, plus the walk. Always returns false, so the refusing
    // call sites read as one statement.
    bool refuse_over_budget() {
        read_local_cache_trim_on_pressure();
        return false;
    }

    bool make_room_for(Slice protected_key, size_t incoming_bytes) {
        if (projected_bytes(protected_key, incoming_bytes) <= maxmemory_limit_) return true;
        if (maxmemory_policy_ == MaxmemoryPolicy::NoEviction) return refuse_over_budget();

        uint32_t budget = kEvictionsPerOp;
        while (budget-- && projected_bytes(protected_key, incoming_bytes) > maxmemory_limit_) {
            KvObj* victim = choose_victim(protected_key);
            if (!victim) return refuse_over_budget();
            const uint64_t hash = hash_key(victim->key());
            const Slice key = victim->key();
            (void)aof_.record_delete(key);
            const uint32_t before = size();
            const bool live = erase(hash, key);
            if (size() == before) return refuse_over_budget();
            if (live && evicted_counter_) (*evicted_counter_)++;
        }
        if (projected_bytes(protected_key, incoming_bytes) <= maxmemory_limit_) return true;
        return refuse_over_budget();
    }

    static uint64_t* allocate_table(uint32_t cap) {
        return static_cast<uint64_t*>(
            flatstore_table_calloc(cap, sizeof(uint64_t)));  // EMPTY == 0
    }

    void install_empty_table(int t, uint64_t* table, uint32_t cap) {
        // All armed structural changes go through the complete-image publisher.
        if (read_local_enabled_) std::abort();
        tab_[t]   = table;
        cap_[t]   = cap;
        mask_[t]  = cap - 1;
        live_[t]  = 0;
        tombs_[t] = 0;
    }

    bool alloc_table(int t, uint32_t cap) {
        uint64_t* fresh = allocate_table(cap);
        if (!fresh) return false;
        std::free(tab_[t]);
        install_empty_table(t, fresh, cap);
        return true;
    }

    // Emit every key whose HOME slot is `home`, wherever linear probing actually put it. Those keys
    // all sit in the run of non-EMPTY words starting at `home`: an insert probes forward from the
    // home and stops at the first EMPTY, and nothing ever writes EMPTY back over an occupied word
    // (erase, rehash and flush all leave a TOMBSTONE), so no gap can open between a key and its
    // home. Returns slots examined, charged against COUNT, and never zero -- an empty home must
    // still cost the caller one unit or a sparse table would walk itself out in a single call.
    template <typename Fn>
    uint32_t scan_home(int t, uint32_t home, Fn& fn, bool expire_on_visit) {
        uint32_t examined = 0;
        uint32_t i = home;
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) break;                              // EMPTY — the only stop
            examined++;
            KvObj* o = ptr_of(w);
            if (o) {
                const uint64_t h = hash_key(o->key());
                if (slot_start(t, h) == home) scan_visit(t, h, o, fn, expire_on_visit);
            }
            i = (i + 1) & mask_[t];
        }
        return examined ? examined : 1;
    }

    // The per-key half of a scan step, unchanged from the physical walk it replaces. A walker
    // with expire_on_visit=false sees the dead entry as-is: accounting walks must have no
    // logical side effects (see the scan() comment).
    template <typename Fn>
    void scan_visit(int t, uint64_t h, KvObj* o, Fn& fn, bool expire_on_visit) {
        if (!expire_on_visit) { fn(o); return; }
        if (deadline_elapsed(h, o, cached_now_ms_)) {
            // An epoch record, not this physical candidate, owns logical expiry and pointer
            // lifetime. The walker callback resolves it at its registered cut.
            if (atomic_has_record(h, o->key())) { fn(o); return; }
            // During capture the frozen table is the cut, not a normal mutable scan source.
            // Report the key logically absent but leave its slot for snapshot traversal, just
            // like find()/active_expire().  KEYS uses this bounded scan while capture runs.
            if (snapshot_active_ && t == 1) return;
            notify_flat_store_emit(this, NOTIFY_EXPIRED, NotifyEventId::Expired, o->key());
            (void)aof_.record_delete(o->key());
            erase_in(t, h, o->key());
            if (expired_counter_) (*expired_counter_)++;
            return;
        }
        fn(o);
    }

    KvObj* find_in(int t, uint64_t h, Slice key) const {
        if (!tab_[t]) return nullptr;
        const uint16_t tag = tag_of(h);
        uint32_t i = slot_start(t, h);
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) return nullptr;                     // EMPTY — the only stop
            KvObj* o = ptr_of(w);
            if (o && tag_of_word(w) == tag && o->key().key_eq(key)) return o;
            i = (i + 1) & mask_[t];
        }
        return nullptr;
    }

    KvObj* find_slot_in(int t, uint64_t h, Slice key, uint32_t& slot) const {
        if (!tab_[t]) return nullptr;
        const uint16_t tag = tag_of(h);
        uint32_t i = slot_start(t, h);
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) return nullptr;
            KvObj* o = ptr_of(w);
            if (o && tag_of_word(w) == tag && o->key().key_eq(key)) { slot = i; return o; }
            i = (i + 1) & mask_[t];
        }
        return nullptr;
    }

    KvObj* find_hash_in(int t, uint64_t h) const {
        if (!tab_[t]) return nullptr;
        const uint16_t tag = tag_of(h);
        uint32_t i = slot_start(t, h);
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) return nullptr;
            KvObj* o = ptr_of(w);
            // The slot flag is a byte on the object line the hash would read anyway; testing it
            // first spares a full key hash on every slotless key that merely shares the 15-bit
            // tag. The flag is PHYSICAL now -- a retained slot may hold -1 -- so logical
            // volatility is decided by the deadline, after the hash has confirmed identity (the
            // sidecar prototype's probe is keyed by that hash and must not run on a mismatch).
            if (o && tag_of_word(w) == tag && o->has_ttl_slot() &&
                hash_key(o->key()) == h && deadline(h, o) >= 0) return o;
            i = (i + 1) & mask_[t];
        }
        return nullptr;
    }

    // Same probe as find_hash_in, minus its key-TTL filter. The field-TTL index tracks hashes that
    // usually carry NO key-level deadline, so reusing find_hash_in there silently found nothing and
    // the cycle deregistered every hash it visited (caught by hash_field_expires falling to 0).
    KvObj* find_any_hash_in(int t, uint64_t h) const {
        if (!tab_[t]) return nullptr;
        const uint16_t tag = tag_of(h);
        uint32_t i = slot_start(t, h);
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) return nullptr;
            KvObj* o = ptr_of(w);
            if (o && tag_of_word(w) == tag && hash_key(o->key()) == h) return o;
            i = (i + 1) & mask_[t];
        }
        return nullptr;
    }

    KvObj* live_or_expire(int t, uint64_t h, Slice key, KvObj* o) {
        // This is the non-expiring-key tax: one flags branch after the ordinary lookup. No clock
        // read occurs here; the executor refreshed cached_now_ms_ once for its loop pass.
        if (!o->has_ttl_slot()) return o;
        const int64_t at = deadline(h, o);
        if (at < 0 || at > cached_now_ms_) return o;
        if (snapshot_active_ && t == 1) return nullptr;
        (void)aof_.record_delete(key);
        erase_in(t, h, key);
        if (expired_counter_) (*expired_counter_)++;
        return nullptr;
    }

    // `fresh` means a newly published object: charge its bytes and (re)register its deadline.
    // The one false caller is rehash_step(), moving an object that is already charged and already
    // indexed -- the slot word moves, nothing else does.
    bool insert_into(int t, uint64_t h, KvObj* o, bool fresh) {
        if (__builtin_expect(read_local_enabled_, false))
            return insert_into_read_local(t, h, o, fresh);
        const uint16_t tag = tag_of(h);
        const Slice    key = o->key();
        uint32_t i = slot_start(t, h);
        int32_t  first_tomb = -1;
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) {
                if (first_tomb >= 0) { tab_[t][first_tomb] = make_word(tag, o); tombs_[t]--; }
                else                 { tab_[t][i] = make_word(tag, o); }
                live_[t]++;
                if (fresh) {
                    obj_bytes_ += kvobj_size(o);
                    (void)this->track_expire(h, o);
                }
                return true;
            }
            KvObj* cur = ptr_of(w);
            if (!cur) { if (first_tomb < 0) first_tomb = static_cast<int32_t>(i); }
            // The ONE key-equality site left on memcmp, and it is measured, not an oversight.
            // This loop holds enough live state that inlining the byte compare cost 13
            // instructions of extra spill on EVERY first insert -- more than the compare saves,
            // and the compare itself almost never runs (it needs a 15-bit tag match: a real
            // replacement or a collision). Both spellings are exact byte equality over the same
            // bytes, so no path can answer differently; only the inlining policy differs.
            else if (tag_of_word(w) == tag && cur->key() == key) {
                if (fresh && deadline_elapsed(h, cur, cached_now_ms_) && expired_counter_)
                    (*expired_counter_)++;
                if (fresh) (void)this->track_expire(h, o);
                retire_obj(cur);                            // replace in place; live_ unchanged
                tab_[t][i] = make_word(tag, o);
                if (fresh) obj_bytes_ += kvobj_size(o);
                return true;
            }
            i = (i + 1) & mask_[t];
        }
        return false;   // unreachable while the load factor holds
    }

    static void notify_emit(FlatNotifySink* sink, uint32_t cls,
                            NotifyEventId event, Slice key) {
        if (sink && sink->enabled && sink->enabled(sink->context, cls) && sink->emit)
            sink->emit(sink->context, cls, event, key);
    }

    bool erase_in(int t, uint64_t h, Slice key, bool* was_expired = nullptr) {
        if (__builtin_expect(read_local_enabled_, false))
            return erase_in_read_local(t, h, key, was_expired);
        if (!tab_[t]) return false;
        const uint16_t tag = tag_of(h);
        uint32_t i = slot_start(t, h);
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) return false;
            KvObj* o = ptr_of(w);
            if (o && tag_of_word(w) == tag && o->key().key_eq(key)) {
                if (was_expired) {
                    *was_expired = deadline_elapsed(h, o, cached_now_ms_);
                }
                untrack_expire(h);
                retire_obj(o);
                tab_[t][i] = kTombBit;                      // DEAD: non-zero, ptr == 0
                live_[t]--; tombs_[t]++;
                return true;
            }
            i = (i + 1) & mask_[t];
        }
        return false;
    }

    TtlResult rewrite_expire(uint64_t h, KvObj* old, int64_t expire_at_ms) {
        if (__builtin_expect(read_local_enabled_, false))
            return rewrite_expire_read_local(h, old, expire_at_ms);
        KvObj* replacement = kvobj_reheader(old, expire_at_ms);
        if (!replacement) return TtlResult::Oom;
        if (maxmemory_enabled_) replacement->set_eviction_meta(old->eviction_meta());

        const bool moves_collection = static_cast<Type>(old->type) != Type::String &&
                                      static_cast<Enc>(old->enc) == Enc::Extern;
        if (moves_collection) {
            old->flags &= static_cast<uint8_t>(~KvObjFlags::OwnsExtern);
            replacement->flags |= KvObjFlags::OwnsExtern;
        }
        const InsertResult inserted = insert(h, replacement);
        if (inserted != InsertResult::Inserted) {
            if (moves_collection) {
                old->flags |= KvObjFlags::OwnsExtern;
                replacement->flags &= static_cast<uint8_t>(~KvObjFlags::OwnsExtern);
            }
            kvobj_free(replacement);
            return inserted == InsertResult::MaxmemoryOom
                ? TtlResult::MaxmemoryOom : TtlResult::Oom;
        }
        return TtlResult::Updated;
    }

    OverwriteResult try_overwrite_read_local(uint64_t h, Slice key, Slice val) {
        if constexpr (!kReadLocalSetTaxAtomicRaw) {
            // OFF and variant B keep published values immutable for the whole grace period.
            return OverwriteResult::NotPossible;
        }

        KvObj* object = find_without_touch(h, key);
        if (!object) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            settax_stats().reject_missing++;
#endif
            return OverwriteResult::NotPossible;
        }
        if (object->encoding() != Enc::Raw) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            settax_stats().reject_encoding++;
#endif
            return OverwriteResult::NotPossible;
        }
        if (object->flags & KvObjFlags::HasTtl) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            settax_stats().reject_ttl++;
#endif
            return OverwriteResult::NotPossible;  // SET clears TTL
        }
        if (val.n > kEmbedThreshold) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            settax_stats().reject_oversize++;
#endif
            return OverwriteResult::NotPossible;  // becomes Extern
        }
        const size_t wanted = kvobj_alloc_size(object->klen(), val.n, false, Enc::Raw);
        if (good_size(wanted) != kvobj_capacity(object)) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            settax_stats().reject_size_class++;
#endif
            return OverwriteResult::NotPossible;
        }
        if (outstanding_borrows_ && is_borrowed(object->str_data())) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            settax_stats().reject_borrowed++;
#endif
            return OverwriteResult::NotPossible;
        }

        if (__builtin_expect(maxmemory_enabled_, false)) {
            if (!make_room_for(key, good_size(wanted))) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                settax_stats().overwrite_maxmemory_oom++;
#endif
                return OverwriteResult::MaxmemoryOom;
            }
            touch(object);
        }

        const uint32_t previous_length = kvobj_read_local_raw_length(object);
        if constexpr (kReadLocalSetTaxVariant == ReadLocalSetTaxVariant::SequenceOverwrite) {
            // Legacy selector 1 intentionally reuses the table publication word. Its unrelated-key
            // retry tax is the round-1 control; selector 3 below never touches table generation.
            ReadLocalAtomicGuard legacy_shard_sequence(*this);
            if (previous_length == val.n) {
                kvobj_write_read_local_raw(object, val);
                return OverwriteResult::Updated;
            }
            obj_bytes_ -= kvobj_size(object);
            kvobj_write_read_local_raw(object, val);
            std::atomic_ref<uint32_t>(object->vlen).store(val.n, std::memory_order_relaxed);
            obj_bytes_ += kvobj_size(object);
            return OverwriteResult::Updated;
        }

        // Selector 3 overlays Raw's otherwise-unneeded vlen word with a full u32 object sequence;
        // the bounded length occupies the byte freed by packing Type+Enc. Saturate into immutable
        // replacement rather than wrap: even a preempted reader can therefore never accept ABA.
        const uint32_t sequence = object->raw_sequence_relaxed();
        if (sequence & 1u) std::abort();       // one shard owner means no concurrent writer
        if (sequence >= UINT32_MAX - 1u) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            settax_stats().reject_sequence_saturated++;
#endif
            return OverwriteResult::NotPossible;
        }
        object->open_raw_sequence(sequence);
        // The release fence after publishing odd keeps every following relaxed cell store on the
        // far side of the open marker. One shard owner makes locked RMWs unnecessary here.
        std::atomic_thread_fence(std::memory_order_release);
        kvobj_write_read_local_raw(object, val);
        if (previous_length != val.n) object->store_raw_length_relaxed(val.n);
        object->close_raw_sequence(sequence);
        // The old/new request sizes are in the same allocator class by the eligibility check above;
        // Raw has no external allocation, so resident accounting is exactly unchanged.
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        settax_stats().overwrite_hits++;
#endif
        return OverwriteResult::Updated;
    }

    bool snapshot_mark_read_local(int32_t shard_id, int64_t cut_ms) {
        if (!snapshot_prepared_ || snapshot_active_ || rehashing()) return false;
        if (!snapshot_mark_read_local_table()) return false;
        snapshot_new_tab_ = nullptr; snapshot_new_cap_ = 0; snapshot_prepared_ = false;
        rehash_pos_ = 0;
        snapshot_active_ = true;
        snapshot_shard_id_ = shard_id;
        snapshot_cut_ms_ = cut_ms;
        snapshot_pos_ = 0;
        snapshot_sequence_ = 0;
        snapshot_failed_ = false;
        snapshot_finished_ = false;
        snapshot_build_ = make_snapshot_chunk(SnapshotFrameBegin);
        snapshot_ready_.reset();
        snapshot_record_ = {};
        return snapshot_build_ != nullptr;
    }

    SnapshotWriteResult snapshot_prepare_write_read_local(uint64_t h, Slice key) {
        if (!snapshot_active_) return SnapshotWriteResult::Ready;
        if (snapshot_failed_) return SnapshotWriteResult::Error;
        if (find_in(0, h, key)) return SnapshotWriteResult::Ready;
        uint32_t slot = 0;
        KvObj* object = find_slot_in(1, h, key, slot);
        if (!object || slot < snapshot_pos_ || (tab_[1][slot] & kTombBit))
            return SnapshotWriteResult::Ready;
        if (deadline_elapsed(h, object, snapshot_cut_ms_)) {
            read_local_slot_store(&tab_[1][slot], tab_[1][slot] | kTombBit);
            return SnapshotWriteResult::Ready;
        }
        if (snapshot_record_.active) return SnapshotWriteResult::Pending;
        if (!snapshot_start_record(object, slot, true)) return SnapshotWriteResult::Error;
        snapshot_preimages_++;
        return SnapshotWriteResult::Pending;
    }

    uint32_t snapshot_progress_read_local(uint32_t byte_budget, uint32_t slot_budget) {
        if (!snapshot_active_ || snapshot_failed_ || snapshot_ready_) return 0;
        uint32_t work = 0;
        while (byte_budget && !snapshot_ready_ && !snapshot_failed_) {
            if (snapshot_record_.active) {
                const uint32_t before = byte_budget;
                snapshot_progress_record_read_local(byte_budget);
                work += before - byte_budget;
                if (snapshot_record_.active || snapshot_ready_) break;
                continue;
            }
            if (snapshot_pos_ >= cap_[1]) {
                snapshot_finish_stream();
                break;
            }
            if (!slot_budget) break;
            const uint32_t slot = snapshot_pos_;
            const uint64_t word = tab_[1][slot];
            KvObj* object = ptr_of(word);
            slot_budget--; work++;
            if (!object) { snapshot_pos_++; continue; }
            if (word & kTombBit) {
                read_local_slot_store(&tab_[1][slot], word & ~kTombBit);
                snapshot_pos_++;
                continue;
            }
            if (deadline_elapsed(hash_key(object->key()), object, snapshot_cut_ms_)) {
                snapshot_pos_++;
                continue;
            }
            if (!snapshot_start_record(object, slot, false)) break;
        }
        return work;
    }

    void snapshot_progress_record_read_local(uint32_t& budget) {
        SnapshotRecordState& state = snapshot_record_;
        const KvObj* object = state.value.object;
        if (!state.active || !object) { snapshot_failed_ = true; return; }

        if (state.header_offset < kSnapshotRecordHeader) {
            const uint32_t n = snapshot_emit(state.header + state.header_offset,
                                             kSnapshotRecordHeader - state.header_offset, budget);
            state.header_offset += n;
            if (state.header_offset != kSnapshotRecordHeader || snapshot_ready_) return;
        }
        if (state.key_offset < object->klen()) {
            const uint32_t n = snapshot_emit(
                reinterpret_cast<const uint8_t*>(object->key_ptr()) + state.key_offset,
                object->klen() - state.key_offset, budget);
            state.key_offset += n;
            if (state.key_offset != object->klen() || snapshot_ready_) return;
        }
        while (state.value.offset < state.value.total && budget && !snapshot_ready_) {
            if (!snapshot_build_) snapshot_build_ = make_snapshot_chunk(0);
            if (!snapshot_build_) return;
            const size_t room = kSnapshotChunkBytes - snapshot_build_->bytes.size();
            const size_t capacity = std::min<size_t>(room, budget);
            const size_t old_size = snapshot_build_->bytes.size();
            try {
                snapshot_build_->bytes.resize(old_size + capacity);
            } catch (const std::bad_alloc&) {
                snapshot_failed_ = true;
                return;
            }
            size_t written = 0;
            const SnapshotHookStatus status = state.hooks.read_save(
                state.value, snapshot_build_->bytes.data() + old_size, capacity, written);
            if (status != SnapshotHookStatus::Ok || written > capacity ||
                (written == 0 && state.value.offset < state.value.total)) {
                snapshot_build_->bytes.resize(old_size);
                snapshot_failed_ = true;
                return;
            }
            snapshot_build_->bytes.resize(old_size + written);
            budget -= static_cast<uint32_t>(written);
            if (snapshot_build_->bytes.size() == kSnapshotChunkBytes) snapshot_seal(0);
        }
        if (state.value.offset != state.value.total || snapshot_ready_) return;

        const bool preimage = state.preimage;
        const uint32_t slot = state.slot;
        snapshot_record_ = {};
        if (preimage) {
            read_local_slot_store(&tab_[1][slot], tab_[1][slot] | kTombBit);
        } else {
            snapshot_pos_++;
        }
    }

    TtlResult set_expire_read_local(uint64_t h, Slice key, int64_t expire_at_ms) {
        KvObj* old = find(h, key);
        if (!old) return TtlResult::Missing;
        return rewrite_expire_read_local(h, old, expire_at_ms);
    }

    TtlResult set_expire_notify_read_local(uint64_t h, Slice key, int64_t expire_at_ms,
                                           FlatNotifySink* sink) {
        KvObj* old = find_notify(h, key, sink);
        if (!old) return TtlResult::Missing;
        return rewrite_expire_read_local(h, old, expire_at_ms);
    }

    InsertResult insert_read_local(uint64_t h, KvObj* o) {
        const bool capturing = rehashing() && snapshot_active_;
        if (rehashing()) {
            if (!capturing) rehash_step_read_local();
        } else {
            if (!maybe_start_grow()) return InsertResult::Failed;
        }
        if (static_cast<uint64_t>(live_[0]) + tombs_[0] + 1 >= cap_[0] &&
            snapshot_prepared_ &&
            !find_in(0, h, o->key())) return InsertResult::Failed;
        if (capturing) {
            const bool exists = find_in(0, h, o->key()) || find_in(1, h, o->key());
            if (!exists && static_cast<uint64_t>(live_[0]) + tombs_[0] + live_[1] + 1 >=
                               cap_[0])
                return InsertResult::Failed;
        }
        if (__builtin_expect(maxmemory_enabled_, false) && !snapshot_active_) {
            if (!make_room_for(o->key(), kvobj_size(o))) return InsertResult::MaxmemoryOom;
            if (o->eviction_meta() == 0) initialize_meta_read_local(o);
        }
        // Keep snapshot preimage slot numbers and the search chain. Published arrays never
        // move an existing key between old and current; replacement remains one slot store.
        if (rehashing() && find_in(1, h, o->key()))
            return insert_into_read_local(1, h, o, true)
                ? InsertResult::Inserted : InsertResult::Failed;
        return insert_into_read_local(0, h, o, true)
            ? InsertResult::Inserted : InsertResult::Failed;
    }

    bool erase_read_local(uint64_t h, Slice key) {
        if (rehashing() && !snapshot_active_) rehash_step_read_local();
        bool expired = false;
        if (erase_in_read_local(0, h, key, &expired)) {
            maybe_start_shrink();
            if (expired && expired_counter_) (*expired_counter_)++;
            return !expired;
        }
        if (rehashing() && erase_in_read_local(1, h, key, &expired)) {
            maybe_start_shrink();
            if (expired && expired_counter_) (*expired_counter_)++;
            return !expired;
        }
        return false;
    }

    void clear_read_local() { clear_read_local_table(false); }

    void clear_during_snapshot_read_local() { clear_read_local_table(true); }

    void initialize_meta_read_local(KvObj* o) {
        if (maxmemory_policy_is_lru(maxmemory_policy_)) {
            write_eviction_meta_read_local(o, lru_clock());
        } else if (maxmemory_policy_is_lfu(maxmemory_policy_)) {
            write_eviction_meta_read_local(o, 5);
        }
    }

    void touch_read_local(KvObj* o) {
        if (maxmemory_policy_is_lru(maxmemory_policy_)) {
            write_eviction_meta_read_local(o, lru_clock());
            return;
        }
        if (!maxmemory_policy_is_lfu(maxmemory_policy_)) return;

        uint8_t count = o->eviction_meta();
        if (count == 0) count = 5;
        const uint32_t base = count > 5 ? static_cast<uint32_t>(count - 5) : 0;
        const uint32_t denominator = base * 10 + 1;
        if (count < 31 && next_random() % denominator == 0) count++;
        write_eviction_meta_read_local(o, count);
    }

    KvObj* choose_victim_read_local(Slice protected_key) {
        KvObj* best = nullptr;
        KvObj* seen[64];
        uint32_t seen_count = 0;
        uint64_t best_score = 0;
        for (uint32_t i = 0; i < maxmemory_samples_; i++) {
            KvObj* candidate = maxmemory_policy_is_volatile(maxmemory_policy_)
                ? random_volatile_candidate() : random_allkeys_candidate();
            if (!candidate || candidate->key().key_eq(protected_key)) continue;
            if (atomic_has_record(hash_key(candidate->key()), candidate->key())) continue;
            bool duplicate = false;
            for (uint32_t j = 0; j < seen_count; j++)
                if (seen[j] == candidate) { duplicate = true; break; }
            if (duplicate) continue;
            seen[seen_count++] = candidate;

            uint64_t score = 0;
            switch (maxmemory_policy_) {
                case MaxmemoryPolicy::AllKeysRandom:
                case MaxmemoryPolicy::VolatileRandom:
                    score = next_random();
                    break;
                case MaxmemoryPolicy::AllKeysLru:
                case MaxmemoryPolicy::VolatileLru: {
                    const uint8_t age = static_cast<uint8_t>(
                        (lru_clock() - candidate->eviction_meta()) & 0x1f);
                    score = (static_cast<uint64_t>(age) << 56) |
                            (next_random() & ((1ULL << 56) - 1));
                    break;
                }
                case MaxmemoryPolicy::AllKeysLfu:
                case MaxmemoryPolicy::VolatileLfu: {
                    uint8_t count = candidate->eviction_meta();
                    if (count) write_eviction_meta_read_local(candidate, --count);
                    score = (static_cast<uint64_t>(31 - count) << 56) |
                            (next_random() & ((1ULL << 56) - 1));
                    break;
                }
                case MaxmemoryPolicy::VolatileTtl:
                    score = std::numeric_limits<uint64_t>::max() -
                            static_cast<uint64_t>(deadline(hash_key(candidate->key()), candidate));
                    break;
                case MaxmemoryPolicy::NoEviction:
                    return nullptr;
            }
            if (!best || score > best_score) { best = candidate; best_score = score; }
        }
        if (best) {
            const bool expired = deadline_elapsed(hash_key(best->key()), best, cached_now_ms_);
            notify_flat_store_emit(this,
                expired ? NOTIFY_EXPIRED : NOTIFY_EVICTED,
                expired ? NotifyEventId::Expired : NotifyEventId::Evicted, best->key());
        }
        return best;
    }

    bool insert_into_read_local(int t, uint64_t h, KvObj* o, bool track_expire) {
        const uint16_t tag = tag_of(h);
        const Slice    key = o->key();
        uint32_t i = slot_start(t, h);
        int32_t  first_tomb = -1;
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) {
                if (first_tomb >= 0) {
                    read_local_slot_store(&tab_[t][first_tomb], make_word(tag, o));
                    tombs_[t]--;
                } else {
                    read_local_slot_store(&tab_[t][i], make_word(tag, o));
                }
                live_[t]++;
                const size_t added_bytes = kvobj_capacity(o) + read_local_external_bytes(o);
                obj_bytes_ += added_bytes;
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                settax_stats().accounting_add_calls++;
                settax_stats().accounting_bytes += added_bytes;
#endif
                if (track_expire) {
                    (void)this->track_expire(h, o);
                    if (o->expire_at_ms() < 0) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                        settax_stats().expire_erases++;
#endif
                    }
                }
                return true;
            }
            KvObj* cur = ptr_of(w);
            if (!cur) { if (first_tomb < 0) first_tomb = static_cast<int32_t>(i); }
            // Same measured exception as insert_into: memcmp here, not the inline compare.
            else if (tag_of_word(w) == tag && cur->key() == key) {
                if (track_expire && deadline_elapsed(h, cur, cached_now_ms_) && expired_counter_)
                    (*expired_counter_)++;
                // An acquiring reader that starts after the retirement stamp must no longer be
                // able to acquire the displaced pointer.
                if (track_expire) (void)this->track_expire(h, o);
                uint64_t next = make_word(tag, o);
                if (t == 1 && snapshot_active_)
                    next |= w & kTombBit; // the snapshot already serialized this slot's preimage
                read_local_slot_store(&tab_[t][i], next);
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                settax_stats().slot_replacements++;
#endif
                retire_obj_read_local(cur);
                const size_t added_bytes = kvobj_capacity(o) + read_local_external_bytes(o);
                obj_bytes_ += added_bytes;
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                settax_stats().accounting_add_calls++;
                settax_stats().accounting_bytes += added_bytes;
#endif
                if (track_expire && o->expire_at_ms() < 0) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
                    settax_stats().expire_erases++;
#endif
                }
                return true;
            }
            i = (i + 1) & mask_[t];
        }
        return false;
    }

    bool erase_in_read_local(int t, uint64_t h, Slice key, bool* was_expired = nullptr) {
        if (!tab_[t]) return false;
        const uint16_t tag = tag_of(h);
        uint32_t i = slot_start(t, h);
        for (uint32_t probes = 0; probes <= cap_[t]; probes++) {
            const uint64_t w = tab_[t][i];
            if (w == 0) return false;
            KvObj* o = ptr_of(w);
            if (o && tag_of_word(w) == tag && o->key().key_eq(key)) {
                if (was_expired) {
                    *was_expired = deadline_elapsed(h, o, cached_now_ms_);
                }
                untrack_expire(h);
                read_local_slot_store(&tab_[t][i], kTombBit);
                retire_obj_read_local(o);
                live_[t]--; tombs_[t]++;
                return true;
            }
            i = (i + 1) & mask_[t];
        }
        return false;
    }

    TtlResult rewrite_expire_read_local(uint64_t h, KvObj* old, int64_t expire_at_ms) {
        KvObj* replacement = kvobj_reheader(old, expire_at_ms);
        if (!replacement) return TtlResult::Oom;
        if (maxmemory_enabled_) replacement->set_eviction_meta(old->eviction_meta());

        const bool moves_collection = static_cast<Type>(old->type) != Type::String &&
                                      static_cast<Enc>(old->enc) == Enc::Extern;
        if (moves_collection) {
            write_object_flags_read_local(old, static_cast<uint8_t>(
                old->flags & static_cast<uint8_t>(~KvObjFlags::OwnsExtern)));
            write_object_flags_read_local(
                replacement, static_cast<uint8_t>(replacement->flags | KvObjFlags::OwnsExtern));
        }
        const InsertResult inserted = insert_read_local(h, replacement);
        if (inserted != InsertResult::Inserted) {
            if (moves_collection) {
                write_object_flags_read_local(
                    old, static_cast<uint8_t>(old->flags | KvObjFlags::OwnsExtern));
                write_object_flags_read_local(replacement, static_cast<uint8_t>(
                    replacement->flags & static_cast<uint8_t>(~KvObjFlags::OwnsExtern)));
            }
            kvobj_free(replacement);
            return inserted == InsertResult::MaxmemoryOom
                ? TtlResult::MaxmemoryOom : TtlResult::Oom;
        }
        return TtlResult::Updated;
    }

    // kvobj_external_bytes() is out of line and switches on type. One byte of the header (already
    // hot: the key compare loaded it) decides whether anything lives outside the block, so test it
    // here and pay the call only for Enc::Extern. Same value as kvobj_size(o) - kvobj_capacity(o).
    static size_t read_local_external_bytes(const KvObj* object) {
        return static_cast<Enc>(object->enc) == Enc::Extern ? kvobj_external_bytes(object) : 0;
    }

    // Decode the header ONCE, exactly as unarmed retire_obj() does: the size class computed for
    // accounting is also the sized-free length. It travels to the reclaim callback as the ring's
    // auxiliary word, so the free after the grace period does not re-derive it from a header that
    // has gone cold. A published object is immutable (variant 0), and eviction-meta updates keep
    // the layout bits, so the class cannot change between retire and reclaim.
    void retire_obj_read_local(KvObj* object) {
        const size_t capacity = kvobj_capacity(object);
        const size_t bytes = capacity + read_local_external_bytes(object);
        obj_bytes_ -= bytes;
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        settax_stats().accounting_sub_calls++;
        settax_stats().accounting_bytes += bytes;
#endif
        read_local_store_state_armed().retire_sink.retire(
            this, object, capacity, &FlatStore::read_local_reclaim_object);
    }

    bool start_rehash_read_local(uint32_t newcap) {
        if (rehashing()) return true;
        return rebuild_read_local_table(std::max(newcap, kMinCap));
    }

    void rehash_step_read_local() {
        // Only snapshot finish/cancel leaves two tables. Failure leaves both searchable;
        // later owner work retries the allocation, never a foreign reader.
        (void)rebuild_read_local_table(cap_[0]);
    }

    // Slot words publish immutable objects with a release store plus QSBR lifetime. Structural
    // changes publish a separate complete image. Every caller is an *_read_local body
    // reached through a read_local_enabled_ dispatch, so the latch is not re-tested here (it was one
    // load + branch per publication, and a release store on an unarmed table would be harmless).
    static void read_local_slot_store(uint64_t* location, uint64_t value) {
        __atomic_store_n(location, value, __ATOMIC_RELEASE);
    }

    static uint64_t read_local_slot_load(const uint64_t* location) {
        return __atomic_load_n(location, __ATOMIC_ACQUIRE);
    }

    const KvObj* read_local_capture_in(const ReadLocalTable& table, uint64_t hash, Slice key,
                                       const uint64_t*& captured_slot) const {
        captured_slot = nullptr;
        if (!table.slots || !table.cap) return nullptr;
        const uint16_t tag = tag_of(hash);
        uint32_t slot = static_cast<uint32_t>(mix64(hash)) & table.mask;
        for (uint32_t probes = 0; probes <= table.cap; probes++) {
            captured_slot = table.slots + slot;
            const uint64_t word = read_local_slot_load(captured_slot);
            if (word == 0) return nullptr;
            const KvObj* object = ptr_of(word);
            if (object && tag_of_word(word) == tag) {
                const uint8_t flags = object->read_local_flags();
                // The measured insert_into exception also applies here: memcmp keeps the
                // bounded probe's live state from being spilled by an inline compare.
                if (object->read_local_key(flags) == key) return object;
            }
            slot = (slot + 1) & table.mask;
        }
        return nullptr;
    }

    static void read_local_prefetch_object(const KvObj* object) {
        __builtin_prefetch(object, 0, 1);
        if (static_cast<Type>(object->type) != Type::String) return;

        const uint8_t flags = object->read_local_flags();
        const char* value = object->read_local_key_ptr(flags) +
                            object->read_local_klen(flags);
        if (object->encoding() != Enc::Extern) {
            __builtin_prefetch(value, 0, 1);
            return;
        }

        const void* external = nullptr;
        std::memcpy(&external, value, sizeof(external));
        if (external) __builtin_prefetch(external, 0, 1);
    }

    void write_eviction_meta_read_local(KvObj* object, uint8_t meta) {
        object->set_eviction_meta_atomic(meta);
    }

    void write_object_flags_read_local(KvObj* object, uint8_t flags) {
        object->store_flags_atomic(flags);
    }

#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
    ReadLocalSetTaxStats& settax_stats() const {
        ReadLocalSetTaxStats* stats =
            read_local_store_state_required().retire_sink.diagnostics();
        if (!stats) std::abort();
        return *stats;
    }
#endif

    // `capacity` is kvobj_capacity(object) as decoded at retire time (the ring's auxiliary word).
    // THE GRACE FLOOR HAS PASSED: no reader can still be holding this pointer and only this shard's
    // owner may touch the block, which is exactly the licence the old code used to call free() on
    // it. Offer it to the shard's block cache first; anything the cache refuses is destroyed on the
    // unchanged path (including the borrowed-value retention destroy_retired_obj owns).
    static void read_local_reclaim_object(const ReadLocalRetireSink&, void* owner,
                                          void* payload, size_t capacity) {
        FlatStore* store = static_cast<FlatStore*>(owner);
        KvObj* object = static_cast<KvObj*>(payload);
        if (!store->read_local_cache_put(object, capacity))
            store->destroy_retired_obj(object, capacity);
    }

    static void read_local_reclaim_atomic_object(const ReadLocalRetireSink&, void* owner,
                                                 void* payload, size_t capacity) {
        FlatStore* store = static_cast<FlatStore*>(owner);
        KvObj* object = static_cast<KvObj*>(payload);
        if (store->atomic_recycle_value(object)) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
            store->settax_stats().recycle_atomic_pool_accepts++;
#endif
            return;
        }
        if (!store->read_local_cache_put(object, capacity))
            store->destroy_retired_obj(object, capacity);
    }

    void read_local_atomic_mutation_begin() {
        if (__builtin_expect(!read_local_enabled_, true)) return;
        ReadLocalStoreState& state = read_local_store_state_required();
        if (state.atomic_mutation_depth++ == 0) {
            read_local_advance_generation(state.atomic_sequence, false);
        }
    }

    void read_local_atomic_mutation_end() {
        if (__builtin_expect(!read_local_enabled_, true)) return;
        ReadLocalStoreState& state = read_local_store_state_required();
        if (!state.atomic_mutation_depth) std::abort();
        if (--state.atomic_mutation_depth == 0) {
            read_local_advance_generation(state.atomic_sequence, true);
        }
    }

    // Ownership supplies the only writer, so the semantic word needs no locked RMW. The acq_rel fence
    // after the release odd-store keeps that marker before later data stores; the final release
    // store publishes the incremented even generation. Wrap becomes permanently fail-closed.
    static void read_local_advance_generation(std::atomic<uint64_t>& sequence, bool ending) {
        if (!ending) {
            const uint64_t previous = sequence.load(std::memory_order_relaxed);
            if (previous & kReadLocalAtomicMutationBit) std::abort();
            sequence.store(previous | kReadLocalAtomicMutationBit, std::memory_order_release);
            std::atomic_thread_fence(std::memory_order_acq_rel);
            return;
        }
        const uint64_t observed = sequence.load(std::memory_order_relaxed);
        if (!(observed & kReadLocalAtomicMutationBit)) std::abort();
        const uint64_t generation = observed >> kReadLocalGenerationShift;
        const uint64_t next = generation == kReadLocalGenerationMask
            ? generation : generation + 1;
        const uint64_t desired = (next << kReadLocalGenerationShift) |
                                 (observed & kReadLocalPendingBit);
        sequence.store(desired, std::memory_order_release);
    }

    void read_local_pending_publish(AtomicEntry& entry) {
        if (!read_local_enabled_) std::abort();
        if (entry.foreign_read_unsafe_published) std::abort();
        ReadLocalStoreState& state = read_local_store_state_required();
        state.foreign_reads.add_span(entry.capacity, [&](uint32_t index) {
            return atomic_entry_hash(entry, index);
        });
        foreign_read_pending_witness_open(state);
        entry.foreign_read_unsafe_published = true;
    }

    void read_local_pending_unpublish(AtomicEntry& entry) {
        if (__builtin_expect(!entry.foreign_read_unsafe_published, true)) return;
        ReadLocalStoreState& state = read_local_store_state_required();
        state.foreign_reads.close_span(entry.capacity, [&](uint32_t index) {
            return atomic_entry_hash(entry, index);
        });
        foreign_read_pending_witness_close(state);
        entry.foreign_read_unsafe_published = false;
    }

    void foreign_read_pending_witness_open(ReadLocalStoreState& state) {
        if (state.pending_count == UINT32_MAX) {
            state.foreign_reads.fail_closed_permanently();
            state.atomic_sequence.fetch_or(kReadLocalPendingBit, std::memory_order_release);
            return;
        }
        if (state.pending_count++ != 0) return;
        const uint64_t previous = state.atomic_sequence.fetch_or(
            kReadLocalPendingBit, std::memory_order_acq_rel);
        if ((previous & kReadLocalPendingBit) &&
            !state.foreign_reads.permanently_poisoned()) std::abort();
    }

    void foreign_read_pending_witness_close(ReadLocalStoreState& state) {
        if (!state.pending_count) {
            if (state.foreign_reads.permanently_poisoned()) return;
            std::abort();
        }
        state.pending_count--;
        if (state.pending_count != 0 || state.foreign_reads.permanently_poisoned()) return;
        const uint64_t previous = state.atomic_sequence.fetch_and(
            ~kReadLocalPendingBit, std::memory_order_release);
        if (!(previous & kReadLocalPendingBit)) std::abort();
    }

    void foreign_read_poison_open() {
        ReadLocalAtomicGuard publication(*this);
        ReadLocalStoreState& state = read_local_store_state_required();
        state.foreign_reads.poison_open();
        foreign_read_pending_witness_open(state);
    }

    void foreign_read_poison_close() {
        ReadLocalStoreState& state = read_local_store_state_required();
        state.foreign_reads.poison_close();
        foreign_read_pending_witness_close(state);
    }

    bool is_borrowed(const char* ptr) const { return borrow_find(ptr) != kNoBorrow; }

    // The experiment targets the physical cache lines jemalloc may return from its LIFO tcache to
    // the next SET. Bound hints to the KvObj allocation itself (kvobj_size may include an external
    // value allocation), and issue them only at the point where ownership really passes to free.
    // `capacity` is kvobj_capacity(object), decoded by whoever retired it; the sized free needs it.
    void free_retired_obj_now(KvObj* object, size_t capacity) {
        if constexpr (kReadLocalReclaimPrefetchw) {
            if (read_local_enabled_) {
                constexpr size_t kLineBytes = 64;
                constexpr uint32_t kMaxLines = 3;
                const char* const base = reinterpret_cast<const char*>(object);
                __builtin_prefetch(base, 1, 3);
                size_t offset = kLineBytes -
                    (reinterpret_cast<uintptr_t>(base) & (kLineBytes - 1));
                for (uint32_t line = 1; line < kMaxLines && offset < capacity;
                     line++, offset += kLineBytes)
                    __builtin_prefetch(base + offset, 1, 3);
            }
        }
        kvobj_free_with_capacity(object, capacity);
    }

    // Post-grace destruction. Only the rare borrowed path needs the full footprint again, and it
    // rebuilds it from the retire-time capacity plus the external block (== kvobj_size).
    void destroy_retired_obj(KvObj* object, size_t capacity) {
        if (outstanding_borrows_ == 0) { free_retired_obj_now(object, capacity); return; }
        const char* ptr = (static_cast<Type>(object->type) == Type::String && !object->is_int())
                              ? object->str_data() : nullptr;
        const uint32_t at = ptr ? borrow_find(ptr) : kNoBorrow;
        if (at != kNoBorrow) {
            borrows_[at].retired = object;
            pending_bytes_ += capacity + read_local_external_bytes(object);
            return;
        }
        free_retired_obj_now(object, capacity);
    }

    // Logical removal updates the live-store footprint immediately. Physical destruction is the
    // common case and pays one branch; registry work exists only while some wire borrow is live.
    // The header is decoded once: the class computed for accounting is the sized-free length.
    void retire_obj(KvObj* o) {
        const size_t capacity = kvobj_capacity(o);
        const size_t bytes = capacity + kvobj_external_bytes(o);
        obj_bytes_ -= bytes;
        if (outstanding_borrows_ == 0) { kvobj_free_with_capacity(o, capacity); return; }
        const char* ptr = (static_cast<Type>(o->type) == Type::String && !o->is_int())
                              ? o->str_data() : nullptr;
        const uint32_t at = ptr ? borrow_find(ptr) : kNoBorrow;
        if (at != kNoBorrow) {
            borrows_[at].retired = o;
            pending_bytes_ += bytes;
            return;
        }
        kvobj_free_with_capacity(o, capacity);
    }

    // ---- incremental resize -----------------------------------------------------------------
    bool maybe_start_grow() {
        if ((static_cast<uint64_t>(live_[0]) + tombs_[0] + 1) * 100 <
            static_cast<uint64_t>(cap_[0]) * kLoadPct) return true;
        if (snapshot_prepared_) return true;
        // Double only when the LIVE set alone justifies it. The trigger counts tombstones, so a
        // delete-heavy workload trips it with almost no live keys and doubling there would inflate
        // the table forever. Otherwise rehash at the same size, which costs the same walk and
        // reclaims every tombstone.
        const bool double_it = static_cast<uint64_t>(live_[0]) * 200 >=
                               static_cast<uint64_t>(cap_[0]) * kLoadPct;
        uint64_t wanted = cap_[0];
        if (double_it) wanted *= uint64_t{2};
        if (wanted > UINT32_MAX) return false;
        return start_rehash(static_cast<uint32_t>(wanted));
    }

    void maybe_start_shrink() {
        if (rehashing() || cap_[0] <= kMinCap) return;
        // Hysteresis: grow triggers at kLoadPct and leaves the table at kLoadPct/2, so shrinking
        // only below kLoadPct/4 keeps the two far enough apart that a workload sitting near a
        // boundary cannot rebuild on every other operation.
        if (static_cast<uint64_t>(live_[0]) * 400 >
            static_cast<uint64_t>(cap_[0]) * kLoadPct) return;
        if (snapshot_prepared_) return;
        (void)start_rehash(cap_[0] / 2);  // shrinking is opportunistic; OOM keeps the old table
    }

    // Demote the current table to the old slot and install a fresh one. NOTHING is copied here —
    // that is the whole point; the slot-word migration is spread across later operations.
    bool start_rehash(uint32_t newcap) {
        if (__builtin_expect(read_local_enabled_, false)) return start_rehash_read_local(newcap);
        if (rehashing()) return true;                       // one at a time; finish before starting
        if (newcap < kMinCap) newcap = kMinCap;
        uint64_t* fresh = allocate_table(newcap);
        if (!fresh) return false;
        if (rehash_counter_) (*rehash_counter_)++;
        tab_[1]  = tab_[0];  cap_[1] = cap_[0];  mask_[1] = mask_[0];
        live_[1] = live_[0]; tombs_[1] = tombs_[0];
        install_empty_table(0, fresh, newcap);
        rehash_pos_ = 0;
        return true;
    }

    // Move a BOUNDED number of SLOT WORDS from the old table to the current one. Called at the head
    // of every operation, so the cost is amortised and no single operation stalls. The KvObjs those
    // words point at are not touched.
    void rehash_step() {
        if (__builtin_expect(read_local_enabled_, false)) {
            rehash_step_read_local();
            return;
        }
        // The window's slot words share a cache line; the objects behind them do not, and every
        // move needs one (hash_key(o->key()) -- the hash is not stored). Warm them together so the
        // misses overlap instead of serializing, eight deep, inside one operation.
        const uint32_t end = std::min(rehash_pos_ + kRehashSlotsPerOp, cap_[1]);
        for (uint32_t i = rehash_pos_; i < end; i++)
            if (const KvObj* o = ptr_of(tab_[1][i])) __builtin_prefetch(o, 0, 1);
        while (rehash_pos_ < end) {
            const uint64_t w = tab_[1][rehash_pos_];
            if (KvObj* o = ptr_of(w)) {
                // TOMBSTONE, not EMPTY. Writing 0 here would terminate any probe run passing
                // through this slot, making every key that probed past it unreachable in the old
                // table for the rest of the rehash — a silent, transient, load-dependent miss.
                tab_[1][rehash_pos_] = kTombBit;
                live_[1]--; tombs_[1]++;
                // Already charged and already indexed: fresh=false moves only the slot word.
                // A failure here would lose the key silently (its old slot is already a tomb);
                // the load bound makes it unreachable, so fail loud, as the atomic exchange does.
                if (!insert_into(0, hash_key(o->key()), o, false)) std::abort();
            }
            rehash_pos_++;
        }
        if (rehash_pos_ >= cap_[1]) {
            std::free(tab_[1]);
            tab_[1] = nullptr; cap_[1] = 0; mask_[1] = 0; live_[1] = 0; tombs_[1] = 0;
            rehash_pos_ = 0;
        }
    }

    friend struct FlatStoreLayoutLock;

    // ============================================================================================
    // THE READER BLOCK vs TWO SETS OF OWNER WRITES — NONE OF WHICH MAY SHARE A 64-BYTE LINE.
    //
    // Layout history: current foreign reads use atomic_pending_ and the boot latch here, then
    // acquire their complete table tuples from the publication sidecar. The separation below
    // remains locked; the original validating design that motivated it is described next.
    //
    // The original --read-local FOREIGN GET loaded tab_/cap_/mask_ (both tables) and the two
    // boot-latched gates below straight out of this object, while the OWNER writes live_, tombs_,
    // rehash_pos_ on every insert of a NEW key and every DEL, and obj_bytes_ on top of that.
    // Declared adjacently — which they were, tab_ at +208 and live_ at +240 — those two sets landed
    // on ONE line, so every owner insert invalidated the exact line every remote reader was loading:
    // a write obstructing reads in a store whose entire premise is that it never does. Nothing in
    // the source showed it; only the byte offsets did.
    //
    // atomic_pending_ is the SECOND half of the same defect, on the atomic side. Every foreign
    // probe dereferences it — read_local_state_acquire() reaches atomic_sequence through it, and it
    // is reloaded at each acquire because that load stops the compiler reusing the previous one:
    // seven static sites in read_local_probe alone — yet it was declared as the second word of the
    // atomic block, eight bytes past atomic_version_bytes_ (owner-written by
    // every atomic_admit / atomic_gauge_sub / atomic_install_plain) and immediately ahead of the
    // three collapse scratch vectors, whose control blocks the owner rewrites on every collapse
    // pass. The vectors were the live collision: at +16/+40/+64 against a foreign read at +8 they
    // shared one line at EVERY alignment the allocator can return. atomic_version_bytes_ at +0 was
    // the latent one, separated from +8 only by ShardLayoutLock::store_offset landing on 56 and by
    // the Shard happening to be 64-byte aligned — with plain 16-byte-aligned storage three of the
    // four possible alignments put them back on one line. Same accident class, same fix: the read
    // moved out, and the distance is now stated in offsets.
    //
    // The separation is expressed in OFFSETS, not addresses (see FlatStoreLayoutLock below). Two
    // bytes whose offsets differ by >= 64 cannot share a line for ANY base address, so the property
    // survives whatever alignment the allocator hands `new Shard` (operator new promises 16, not
    // 64) and whatever ShardLayoutLock::store_offset becomes. The old layout only looked split
    // because store_offset happened to be 56 and the allocator happened to over-align the Shard.
    // ============================================================================================

    // ---- SHARED BLOCK. Foreign probes read the boot latch and atomic_pending_; the table
    // aliases now belong to the owner alone. Preserve the mainline layout and its separation
    // from the ordinary insert/delete accounting below. -------------------------------------
    //
    // Null until the first atomic group reaches this owner. The object stays as a pool after the
    // list drains; its zero live count is the common ON read test. KvObj remains byte-identical.
    // It belongs to the READER, not to the atomic block it is declared beside in
    // flatstore_atomic.inc: every foreign probe loads it before it can reach atomic_sequence, while
    // the owner writes it only in atomic_ensure_pending / ensure_read_local_store_state /
    // atomic_destroy_pending — arming and teardown, never a per-operation write.
    AtomicPendingState* atomic_pending_ = nullptr;
    uint64_t* tab_[2]   = {nullptr, nullptr};
    // Capacities are power-of-two and intentionally stop at 2^31: slot/probe indices are uint32_t
    // and insert_into's tombstone sentinel is int32_t. A requested next doubling (2^32 slots,
    // already 32 GiB for one shard's slot words) returns InsertResult::Failed before any swap.
    // Tables beyond four billion slots are therefore outside this store's representation rather
    // than a legitimate reason to widen these hot fields.
    uint32_t  cap_[2]   = {0, 0};
    uint32_t  mask_[2]  = {0, 0};
    // Boot-latched, and the first byte a foreign probe tests before it touches anything else.
    // It belongs ON the topology line rather than 200 bytes past it: co-located, the whole foreign
    // read reduces to one line of FlatStore. Armed state itself stays sidecarred.
    bool      read_local_enabled_ = false;
    uint8_t   reader_reserved_ = 0; // retain the measured reader/owner line separation

    // ---- SEPARATOR. Read-mostly only: config, bind-once counter bindings, and the snapshot
    // scalars that are latched once when a capture is prepared. NOTHING here is written by an
    // ordinary read or write of a key, so a foreign reader sharing a line with any of it still
    // never sees an owner invalidation. The live snapshot cursor is deliberately NOT here.
    bool      maxmemory_enabled_ = false;
    MaxmemoryPolicy maxmemory_policy_ = MaxmemoryPolicy::NoEviction;
    // THE hash-field-TTL gate: read by every hash command, so it must share the first cache line
    // that find() touches rather than sit next to its own (cold) index hundreds of bytes further
    // down. Four bytes behind mask_ keeps it on that line and off the owner's.
    uint32_t  field_ttl_gate_ = 0;
    uint32_t  maxmemory_samples_ = 5;
    int32_t   snapshot_shard_id_ = -1;
    uint64_t  maxmemory_limit_ = 0;
    uint64_t* snapshot_new_tab_ = nullptr;
    uint64_t* evicted_counter_ = nullptr;
    uint64_t* rehash_counter_ = nullptr;
    uint64_t  snapshot_epoch_ = 0;
    int64_t   snapshot_cut_ms_ = 0;
    uint32_t  snapshot_new_cap_ = 0;
    // Explicit tail of the separator. The read-mostly fields above fall 28 bytes short of the 64
    // that make the guarantee base-independent, and this is that balance — it is also why the
    // object still measures 944 bytes instead of 916. Repurposing it is fine only for a field that
    // is never written on the key path, and only if FlatStoreLayoutLock still passes.
    char      reader_owner_gap_[28] = {};

    // ---- OWNER BLOCK. Written by the single owner on the ordinary insert/DEL path. The first
    // eight fields are 48 bytes, so the whole per-operation counter set is one line for the owner
    // (it used to straddle two), and that line is now private to the owner.
    uint32_t  live_[2]  = {0, 0};
    uint32_t  tombs_[2] = {0, 0};
    uint32_t  rehash_pos_ = 0;
    uint32_t  active_expire_reap_lag_ms_max_ = 0;
    size_t    obj_bytes_  = 0;
    size_t    pending_bytes_ = 0;
    uint32_t  outstanding_borrows_ = 0;
    uint32_t  borrow_tombs_ = 0;
    // The live snapshot cursor is owner-written under ordinary write traffic, so it belongs on this
    // side of the split even though the rest of the capture's scalars are latched once and sit in
    // the separator above.
    uint32_t snapshot_pos_ = 0;
    uint32_t snapshot_sequence_ = 0;
    // SECOND OWNER LINE, and its composition is deliberate: every field an insert reads outside the
    // counters above — the clock pair, the expiry counter binding, the snapshot gates — is inside
    // one 64-byte window here, which is what keeps the owner's per-insert line count at what it was
    // before the split (the counters simply moved from the reader's line to their own).
    int64_t     cached_now_ms_ = 0;
    uint8_t     cached_lru_clock_ = 0;
    bool        no_touch_ = false;      // per-task, owner-written; see set_no_touch
    // Snapshot state is owner-only.  No atomics or locks enter FlatStore, and the ordinary lookup
    // still searches exactly t_[0] then t_[1] — during capture those already-existing tables mean
    // "post-cut" and "frozen cut" respectively.
    bool snapshot_prepared_ = false;
    bool snapshot_active_ = false;
    bool snapshot_failed_ = false;
    bool snapshot_finished_ = false;
    uint64_t*   expired_counter_ = nullptr;
    uint64_t    random_state_ = 0x9e3779b97f4a7c15ULL;
    uint64_t    sample_cursor_ = 0;
    uint64_t snapshot_preimages_ = 0;   // pre-images emitted ahead of the cursor (write-gate fired)
    std::vector<Borrow> borrows_;
    std::vector<uint32_t> borrow_idx_;   // empty == scan mode; see kBorrowIndexMin
    ExpireIndex expires_;
    SnapshotRecordState snapshot_record_;
    std::unique_ptr<SnapshotChunk> snapshot_build_;
    std::unique_ptr<SnapshotChunk> snapshot_ready_;
    AofProducer aof_;
    // COLD TAIL. The hash-field-TTL index and read-local publication stay after the established hot
    // fields on purpose: placing cold state mid-struct pushed cached_now_ms_ / maxmemory_ / snapshot_
    // further out, which showed up as a measurable instr/op regression on workloads that never use
    // those features. The TTL index allocates nothing until the first HEXPIRE in the shard.
    ExpireIndex field_expires_;
    uint64_t    field_expired_ = 0;
};

// THE READER/OWNER LINE SPLIT, pinned in offsets so nothing about it depends on where a Shard
// happens to land in memory. `new Shard` promises 16-byte alignment, not 64, and the store sits at
// ShardLayoutLock::store_offset inside it; both are free to change. Two bytes whose OFFSETS differ
// by at least 64 are on different lines for every possible base address, which is the only form of
// this guarantee that a static_assert can actually make.
struct FlatStoreLayoutLock {
    // Every byte a foreign reader loads out of FlatStore on the GET path, and nothing else.
    // atomic_pending_ is the FIRST of them: a foreign capture cannot reach atomic_sequence, the
    // read-local filter or the retire sink without loading it.
    static constexpr size_t reader_first = offsetof(FlatStore, atomic_pending_);
    static constexpr size_t reader_last  = offsetof(FlatStore, read_local_enabled_);
    // Every byte the owner writes on the ordinary insert-a-new-key / DEL path.
    static constexpr size_t owner_first  = offsetof(FlatStore, live_);
    static constexpr size_t owner_last   = offsetof(FlatStore, borrow_tombs_) + 3;
    // Every byte the owner writes on the ATOMIC path, declared ahead of the reader block:
    // atomic_version_bytes_ is the first word of the object, the three collapse scratch vectors and
    // the seen-key vector follow, and the per-operation read context closes the range.
    static constexpr size_t atomic_owner_first = offsetof(FlatStore, atomic_version_bytes_);
    static constexpr size_t atomic_owner_last  =
        offsetof(FlatStore, atomic_read_origin_conn_id_) + 7;
    // First word of the bind-once separator that buys the distance below it.
    static constexpr size_t atomic_separator_first = offsetof(FlatStore, atomic_ticket_fn_);
    static constexpr size_t line         = 64;
    static constexpr size_t gap_bytes    = sizeof(FlatStore::reader_owner_gap_);
};

// THE INVARIANT, STATED TWICE BECAUSE THE READER BLOCK HAS OWNER WRITES ON BOTH SIDES OF IT: no
// word the owner writes per operation may share a cache line with a word a foreign GET reads, for
// any Shard base address. Adding a per-operation counter above or below the separators, or a
// foreign read inside either owner block, breaks this build rather than quietly reintroducing the
// false sharing.
static_assert(FlatStoreLayoutLock::owner_first - FlatStoreLayoutLock::reader_last >=
                  FlatStoreLayoutLock::line,
              "owner-written counters may share a cache line with the reader topology words");
static_assert(FlatStoreLayoutLock::reader_first - FlatStoreLayoutLock::atomic_owner_last >=
                  FlatStoreLayoutLock::line,
              "atomic accounting words may share a cache line with the reader's probe words");
// Both blocks stay compact enough to be one line each when the Shard is 64-byte aligned, which is
// what the allocator does today: the reader pays one line per foreign GET, the owner one line per
// insert. These are the budgets the split was bought with.
static_assert(FlatStoreLayoutLock::reader_last - FlatStoreLayoutLock::reader_first <
                  FlatStoreLayoutLock::line, "reader topology block no longer fits one line");
static_assert(FlatStoreLayoutLock::owner_last - FlatStoreLayoutLock::owner_first <
                  FlatStoreLayoutLock::line, "owner counter block no longer fits one line");
// The distance above is bought entirely by the bind-once bindings, which start where the atomic
// owner block ends. An owner-written field appended to that block would extend it PAST the assert's
// named last word and stay invisible to it; this is what refuses that edit. Widening the separator
// with more bind-once state is fine — move atomic_separator_first onto the new first field.
static_assert(FlatStoreLayoutLock::atomic_separator_first ==
                  FlatStoreLayoutLock::atomic_owner_last + 1,
              "a field slipped in between the atomic owner block and its read-mostly separator");
// The gap is padding on purpose; if a future field shrinks it below the line, say so here.
static_assert(FlatStoreLayoutLock::gap_bytes == 28);

// atomic_torn's disabled geometry is contractual: armed state must never grow this baseline object.
static_assert(sizeof(FlatStore) == 944);

#include "flatstore_nowait.inc"


// RAII bracket for any mutation of an EXISTING object: samples kvobj_size before, reports the
// delta to the owning store after, so obj_bytes_ tracks external collection growth/shrink. Every
// family handler that mutates a found object must hold one (missing brackets were invisible on
// the way up and UNDERFLOWED obj_bytes_ on delete -- caught 2026-08-25). Single-owner: no atomics.
class ObjectSizeTracker {
public:
    ObjectSizeTracker(FlatStore& store, KvObj* object)
        : store_(store), object_(object), before_(object ? kvobj_size(object) : 0) {}
    ~ObjectSizeTracker() { finish(); }
    ObjectSizeTracker(const ObjectSizeTracker&) = delete;
    ObjectSizeTracker& operator=(const ObjectSizeTracker&) = delete;

    void finish() {
        if (!object_) return;
        store_.note_object_size_change(before_, kvobj_size(object_));
        object_ = nullptr;
    }

    // For paths that erase the object inside the bracket: the store already subtracted the full
    // current size in erase(); reporting a delta on top would double-count.
    void cancel() { object_ = nullptr; }

private:
    FlatStore& store_;
    KvObj*     object_;
    size_t     before_;
};

}  // namespace tomo
