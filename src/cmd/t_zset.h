#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../base/slice.h"

namespace tomo {

class Shard;
struct KvObj;

struct ZsetEntry {
    std::string member;
    double score = 0;
};

enum class ZsetOwnerResult : uint8_t {
    Ok,
    Missing,
    WrongType,
    Oom,
    Maxmemory,
    InsertFailed,
};

// The pinned Redis 7.4.10 oracle (f103d127b) rejects every negative LIMIT offset,
// for both compact and expanded zsets. Older 7.4 builds accidentally counted backwards
// in the skiplist path; preserving that quirk made cgaps seed 28 store a member where
// the oracle deleted the destination. Rank ranges still accept negative indices, and a
// negative LIMIT count still means unbounded: only this offset is invalid.
// Keep the encoding argument for the shared local/scatter callers; it cannot change
// the result. `available` counts entries inside the range, in either iteration order.
inline bool zset_resolve_limit_offset(int64_t offset, uint64_t available, bool /*expanded*/,
                                      uint64_t& resolved) {
    if (offset < 0) return false;
    resolved = static_cast<uint64_t>(offset);
    return resolved < available;
}

// Owner-thread-only bridge used by GEO. Entries are copied out so no pointer can escape the
// shard, and replacement is built completely before the live key is relinked.
ZsetOwnerResult zset_owner_read(Shard& shard, Slice key, uint64_t hash, bool notify,
                                bool read_stats,
                                std::vector<ZsetEntry>& entries, int64_t& expire_at_ms,
                                bool* reserve_ttl_slot = nullptr);
ZsetOwnerResult zset_owner_replace(Shard& shard, Slice key, uint64_t hash, bool notify,
                                   const std::vector<ZsetEntry>& entries, int64_t expire_at_ms,
                                   bool reserve_ttl_slot = false);

// SORT converts a zset source to the expanded encoding on the oracle and never converts back:
// SORT, SORT_RO and even a BY-nosort SORT all do it, because sorting wants indexed access. It
// produces no reply of its own, but OBJECT ENCODING exposes this change to clients.
//
// The key is looked up LIVE and in place: callers must not hand in a pointer they already hold,
// because on the scatter path that pointer can be an MVCC-tracked version rather than the store's
// current entry, and because externalising an embedded zset replaces the object. Call this BEFORE
// taking the pointer the command will read. Owner-thread only. Allocation failure leaves the key
// compact and is not reported: the encoding is a fidelity detail, never a reason to fail a read.
void zset_sort_promote(Shard& shard, uint64_t hash, Slice key, bool notify);

}  // namespace tomo
