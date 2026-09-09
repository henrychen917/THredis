// read_local_reclaim.h — type-erased handoff from a shard store to its fused owner QSBR list.
//
// FlatStore cannot depend on ExLoop without reversing the core/store include graph. The store
// therefore publishes only this tiny boot-bound sink. Reclamation always runs later on the same
// physical owner thread; `owner` and `payload` are opaque until the callback is invoked after a
// grace period.
#pragma once
#include <cstddef>
#include <cstdint>
#include "read_local_settax.h"
#include "resize_retirement.h"

namespace tomo {

struct KvBlockCache;

// Capacity of the owner's deferred-retire ring (ReadLocalRetireRing, src/core/read_local.h), which
// static_asserts that it still equals this number. It lives here because the store side derives a
// bound from it and cannot include a core/ header: the ring already permits exactly this many
// retired-but-unreclaimed objects to be resident per owner, so it is the natural ceiling for any
// cache of blocks that ring produces (see KvBlockCache::kMaxBytes).
inline constexpr uint32_t kReadLocalRetireRingCapacity = 4096;

struct ReadLocalRetireSink {
    using ReclaimFn = void (*)(void* owner, void* payload, size_t auxiliary);
    using DeferFn = void (*)(void* context, void* owner, void* payload,
                            size_t auxiliary, ReclaimFn reclaim);

    void* context = nullptr;
    DeferFn defer = nullptr;
    // The owner's post-grace block cache (src/store/kv_block_cache.h). A DIRECT pointer, not a
    // third function pointer: the armed write path calls into it on every SET, and an indirect
    // call there would hand back part of the allocator call it exists to remove. Every armed
    // owner supplies its cache; null is only the unconfigured/off state.
    KvBlockCache* block_cache = nullptr;
    // Cold maintenance/lazy-expiry preflight and preallocated resize handoff. A null pair is
    // reserved for synchronous, serverless test sinks; real owners always bind both.
    bool (*available)(void* context) = nullptr;
    void (*defer_resize)(void* context, ResizeRetirement* record, uint64_t* table) = nullptr;
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
    ReadLocalSetTaxStats* settax_stats = nullptr;
#endif

    void retire(void* owner, void* payload, size_t auxiliary, ReclaimFn reclaim) const {
        defer(context, owner, payload, auxiliary, reclaim);
    }

    void bind_settax_stats(ReadLocalSetTaxStats* stats) {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        settax_stats = stats;
#else
        (void)stats;
#endif
    }
    ReadLocalSetTaxStats* diagnostics() const {
#if TOMO_READ_LOCAL_SET_TAX_VARIANT == 3
        return settax_stats;
#else
        return nullptr;
#endif
    }
};

#if TOMO_READ_LOCAL_SET_TAX_VARIANT != 3
static_assert(sizeof(ReadLocalRetireSink) == 5 * sizeof(void*),
              "retire sink plus cold capacity/resize hooks; no FlatStore/ThreadCtx layout change");
#endif

}  // namespace tomo
