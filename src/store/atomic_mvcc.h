// atomic_mvcc.h — free-standing epoch-MVCC records and helpers used by FlatStore.
//
// These declarations do not depend on FlatStore internals and are normally includable. The
// store remains header-only: its member implementation is textually concatenated inside the
// FlatStore class by flatstore_atomic.inc, so this split creates no translation unit or ODR
// boundary.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "kvobj.h"

namespace tomo {

struct ScatterState;

struct AtomicEntry {
    AtomicEntry* next = nullptr;
    AtomicEntry* prev = nullptr;
    AtomicEntry* conn_next = nullptr;
    AtomicEntry* conn_prev = nullptr;
    AtomicEntry* pool_next = nullptr;
    ScatterState* group = nullptr;
    std::atomic<uint64_t>* group_epoch = nullptr;
    std::atomic<uint32_t>* group_refs = nullptr;
    std::atomic<bool>* group_aborted = nullptr;
    uint32_t* owner_refs = nullptr;
    uint64_t origin_conn_id = 0;
    uint64_t epoch = 0;                  // plain pseudo-entry; groups use group_epoch
    uint64_t plain_hash = 0;
    uint64_t membership = 0;
    size_t allocation = 0;
    uint32_t begin = 0;                  // key_order span in ScatterState
    uint32_t count = 0;                  // installed prefix of the span
    uint32_t capacity = 0;
    uint32_t key_len = 0;
    bool linked = false;

    KvObj** parked() { return reinterpret_cast<KvObj**>(this + 1); }
    KvObj* const* parked() const { return reinterpret_cast<KvObj* const*>(this + 1); }
    char* plain_key_data() { return reinterpret_cast<char*>(parked() + capacity); }
    const char* plain_key_data() const {
        return reinterpret_cast<const char*>(parked() + capacity);
    }
    bool plain() const { return group == nullptr; }
};

// Owner-local protection installed while a cross-owner script is staged.  It deliberately lives
// beside the cold MVCC list rather than in KvObj: ordinary databases that never execute a cross
// script allocate none, and a key's hot representation remains byte-identical.  The trailing key
// bytes make the intent independent of the IO-owned request buffer.
struct AtomicScriptIntent {
    AtomicScriptIntent* next = nullptr;
    uint64_t hash = 0;
    uint32_t refs = 0;
    uint32_t key_len = 0;
    char* key_data() { return reinterpret_cast<char*>(this + 1); }
    const char* key_data() const { return reinterpret_cast<const char*>(this + 1); }
};

struct AtomicPendingState {
    struct FreeValue {
        FreeValue* next;
        size_t allocation;
    };
    static constexpr uint32_t kPoolClasses = 48;
    AtomicEntry* head = nullptr;
    AtomicEntry* tail = nullptr;
    AtomicEntry* conn_heads[64] = {};
    uint32_t live = 0;
    // Deliberately adjacent to `live`. atomic_has_records() is the ONE branch on the plain
    // GET/SET owner path that reads this struct at all, and it now tests both words; keeping
    // them on one cache line means the reservation facility adds no second line touch there.
    uint32_t script_intent_count = 0;
    AtomicScriptIntent* script_intents = nullptr;
    AtomicEntry* free_entries[kPoolClasses] = {};
    FreeValue* free_values[kPoolClasses] = {};
    uint32_t cached_entries = 0;
    uint64_t cleanup_fast = 0;
    uint64_t cleanup_slow = 0;
    size_t cached_entry_bytes = 0;
    size_t cached_value_bytes = 0;
};

struct AtomicResolved {
    KvObj* value = nullptr;
    bool matched = false;
    bool physical = true;
};

struct AtomicCollapseKey {
    uint64_t hash = 0;
    Slice key;
    KvObj* base = nullptr;
    AtomicEntry* previous = nullptr;
    KvObj* winner = nullptr;
    KvObj* physical = nullptr;
    KvObj* physical_loser = nullptr;
    AtomicEntry* boundary = nullptr;
    uint32_t boundary_index = 0;
    uint64_t winner_epoch = 0;
    bool winner_set = false;
    // Program-order repair state for this key, carried along the collapse walk. `prev_epoch` is
    // the EFFECTIVE epoch assigned to the previous record for the key and `prev_conn_id` is the
    // connection that owned it; a following record from the same connection may never rank below
    // it, because one connection's records reach an owner in the order the client sent them.
    uint64_t prev_conn_id = 0;
    uint64_t prev_epoch = 0;
    bool prev_set = false;
};

struct AtomicCollapseSlot {
    uint64_t hash = 0;
    uint32_t index = 0;                  // collapse_keys_ index + 1; zero is empty
};

struct AtomicSeenKey {
    uint64_t hash;
    Slice key;
};

static uint64_t atomic_epoch(const AtomicEntry& entry) {
    return entry.group_epoch
        ? entry.group_epoch->load(std::memory_order_acquire) : entry.epoch;
}

static uint64_t atomic_membership_bit(uint64_t hash) {
    return uint64_t{1} << (hash & 63);
}

static uint32_t atomic_pool_class(size_t allocation) {
    if (allocation <= 8) return 0;
    if (allocation <= 128) return static_cast<uint32_t>(allocation / 16);
    const int k = 63 - __builtin_clzll(
        static_cast<unsigned long long>(allocation - 1));
    const size_t step = size_t{1} << (k - 2);
    const uint32_t quarter = static_cast<uint32_t>(allocation / step);
    return 9u + 4u * static_cast<uint32_t>(k - 7) + (quarter - 5u);
}

}  // namespace tomo
