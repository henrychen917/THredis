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
#include "kv_block_cache.h"
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
    // Set after prepare publishes every key occurrence in the foreign-read safety filter (and the
    // legacy whole-shard pending witness). It remains set through install, linking and collapse.
    bool foreign_read_unsafe_published = false;
    // DEL/UNLINK groups own packed key bytes in this allocation instead of one empty KvObj per
    // key. The flag consumes header padding; AtomicEntry's locked size stays unchanged.
    bool copied_group_keys = false;

    KvObj** parked() { return reinterpret_cast<KvObj**>(this + 1); }
    KvObj* const* parked() const { return reinterpret_cast<KvObj* const*>(this + 1); }
    char* plain_key_data() { return reinterpret_cast<char*>(parked() + capacity); }
    const char* plain_key_data() const {
        return reinterpret_cast<const char*>(parked() + capacity);
    }
    uint32_t* group_key_offsets() {
        return reinterpret_cast<uint32_t*>(parked() + capacity);
    }
    const uint32_t* group_key_offsets() const {
        return reinterpret_cast<const uint32_t*>(parked() + capacity);
    }
    char* group_key_data() {
        return reinterpret_cast<char*>(group_key_offsets() + capacity);
    }
    const char* group_key_data() const {
        return reinterpret_cast<const char*>(group_key_offsets() + capacity);
    }
    bool plain() const { return group == nullptr; }
};

// The foreign-read lifetime marker consumes existing tail padding. Atomic entries are pooled by
// allocation class, so growing the header would change both the disabled allocation path and its
// cache geometry.
static_assert(sizeof(AtomicEntry) == 144);

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
    static constexpr uint32_t kPoolClasses = KvBlockCache::kClasses;
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
    // Read-local uses an extended allocation whose first member is this exact baseline state.
    // The discriminator consumes the pre-existing four-byte hole before cleanup_fast, so the
    // disabled allocation size and every following offset remain locked.
    bool read_local_extended = false;
    uint64_t cleanup_fast = 0;
    uint64_t cleanup_slow = 0;
    size_t cached_entry_bytes = 0;
    size_t cached_value_bytes = 0;
};

static_assert(sizeof(AtomicPendingState) == 1352);
static_assert(offsetof(AtomicPendingState, read_local_extended) == 1316);

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

// One definition, shared with the owner's block cache so the two size-class tables cannot drift.
static uint32_t atomic_pool_class(size_t allocation) { return kv_block_class(allocation); }

}  // namespace tomo
