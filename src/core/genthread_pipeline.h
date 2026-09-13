// Compile-time geometry for the generalized-thread schedules. The boot knob selects a fixed loop
// shape; these remain build-time constants so no per-operation tuning state reaches a hot path.
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

// The measured iofused arm submits SEND-bearing network batches immediately, but may retain only
// non-SEND network/control work for at most this many outer rotations.
inline constexpr uint32_t kGenthreadIoFusedCoalesceRotations = 4;

}  // namespace tomo
