// Compile-time batch caps for the generalized-thread schedules. The boot knob selects the loop
// shape; overlap_cache.h derives prefetch windows at boot, with no per-operation tuning state.
#pragma once
#include <cstdint>

namespace tomo {

// Pipeline 0 is today's coarse fused loop. Keep its original quanta intact: it is the study's
// plain-loop baseline rather than a smaller spelling of either interwoven schedule.
inline constexpr uint32_t kGenthreadIfidBatchOps = 32;
inline constexpr uint32_t kGenthreadExBatchOps = 32;
inline constexpr uint32_t kGenthreadWbBatchConns = 16;

// Fused overlap uses this generalized-thread geometry. These names are
// deliberately separate from the coarse constants above so selecting the study baseline cannot
// silently inherit the larger experimental batches.
inline constexpr uint32_t kGenthreadPipelineExBatchOps = 128;
inline constexpr uint32_t kGenthreadPipelineWbBatchConns = 64;
inline constexpr uint32_t kGenthreadWbPrefetchOpsPerConn = 64;
inline constexpr uint32_t kGenthreadWbBorrowPrefetchBytes = 512;
inline constexpr uint32_t kGenthreadCacheLineBytes = 64;

// The measured iofused arm submits SEND-bearing network batches immediately, but may retain only
// non-SEND network/control work for at most this many outer rotations.
inline constexpr uint32_t kGenthreadIoFusedCoalesceRotations = 4;

// The three-way arm starts in the ordinary coarse order and opens its interleave only after a
// completed pass exposes at least this much work in one whole IFID, EX, or WB batch.  A single
// threshold and one loop-local gate are the entire depth policy: a thin completed pass returns the
// next rotation to coarse order, and no hysteresis/residual state follows an operation across
// rotations.
inline constexpr uint32_t kGenthreadThreeWayMinBatchOccupancy = 8;

}  // namespace tomo
