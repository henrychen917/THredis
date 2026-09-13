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

// A NEGATIVE `LIMIT offset` is resolved against the END of the matched range -- but only for the
// expanded (skiplist) encoding. Redis 7.4 splits here and we match it per encoding, because the
// split is observable and the differ compares bytes. Probed on the oracle with the same logical
// zset {a:1,b:3,c:5,d:7} held in each encoding:
//
//   ZRANGE k 0 10 BYSCORE LIMIT -1 -1   listpack: (empty)   skiplist: d
//   ZRANGE k 0 10 BYSCORE LIMIT -3 -1   listpack: (empty)   skiplist: b c d
//   ZRANGE k 0 10 BYSCORE LIMIT -5 -1   listpack: (empty)   skiplist: (empty)
//   ZRANGE k 10 0 BYSCORE REV LIMIT -2 -1  listpack: (empty)  skiplist: b a
//
// So on the skiplist the start index is `available + offset` counted in ITERATION order (REV
// included, which is why callers pass the count of matched entries and not a rank), an index
// below zero selects nothing, and on the listpack every negative offset selects nothing.
//
// `available` is the number of entries inside the range. Returns false when the range selects
// nothing; otherwise `resolved` is the non-negative start index within the range.
inline bool zset_resolve_limit_offset(int64_t offset, uint64_t available, bool expanded,
                                      uint64_t& resolved) {
    if (offset >= 0) {
        resolved = static_cast<uint64_t>(offset);
        return resolved < available;
    }
    if (!expanded) return false;
    // available <= INT64_MAX for any real zset, so the signed add cannot overflow.
    const int64_t start = static_cast<int64_t>(available) + offset;
    if (start < 0) return false;
    resolved = static_cast<uint64_t>(start);
    return true;
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
// produces no reply of its own, but it is not cosmetic -- the encoding decides how a later
// negative LIMIT offset resolves (zset_resolve_limit_offset above), so skipping it made
// ZRANGESTORE diverge on any zset that had been SORTed.
//
// The key is looked up LIVE and in place: callers must not hand in a pointer they already hold,
// because on the scatter path that pointer can be an MVCC-tracked version rather than the store's
// current entry, and because externalising an embedded zset replaces the object. Call this BEFORE
// taking the pointer the command will read. Owner-thread only. Allocation failure leaves the key
// compact and is not reported: the encoding is a fidelity detail, never a reason to fail a read.
void zset_sort_promote(Shard& shard, uint64_t hash, Slice key, bool notify);

}  // namespace tomo
