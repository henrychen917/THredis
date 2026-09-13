// Compile-time geometry for the generalized-thread schedules. The boot knob selects a fixed loop
// shape; these remain build-time constants so no per-operation tuning state reaches a hot path.
#pragma once
#include <array>
#include <cstdint>

namespace tomo {

// Pipeline 0 is today's coarse fused loop. Keep its original quanta intact: it is the study's
// plain-loop baseline rather than a smaller spelling of either interwoven schedule.
inline constexpr uint32_t kGenthreadIfidBatchOps = 32;
inline constexpr uint32_t kGenthreadExBatchOps = 32;
inline constexpr uint32_t kGenthreadWbBatchConns = 16;

// Pipelines 1 and 2 share the measured generalized-thread batch geometry. These names are
// deliberately separate from the coarse constants above so selecting the study baseline cannot
// silently inherit the larger experimental batches.
inline constexpr uint32_t kGenthreadPipelineIfidBatchOps = 128;
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

// Legacy streams implementation retained below the overlap-2 dispatch for branch comparison.  It
// keeps one IFID context, exactly two EX contexts (A/D), and one WB context. The contexts are loop
// locals; streams may carry only a pre-I1 IFID batch or pre-E1 EX batch across one rotation. WB is
// empty at every outer boundary, and triple-buffered EX is forbidden.
inline constexpr uint32_t kGenthreadIfidContexts = 1;
inline constexpr uint32_t kGenthreadExContexts = 2;
inline constexpr uint32_t kGenthreadWbContexts = 1;
static_assert(kGenthreadExContexts == 2, "buffered schedules forbid EX triple buffering");

inline constexpr uint32_t kGenthreadStreamsMinBatchOccupancy = 8;
inline constexpr uint32_t kGenthreadStreamsResidualAgeCapRotations = 1;
inline constexpr uint32_t kGenthreadStreamsMaxChunksPerPass = 16;
static_assert(kGenthreadStreamsResidualAgeCapRotations > 0,
              "streams residual carry needs a positive, finite rotation cap");
static_assert(kGenthreadStreamsMaxChunksPerPass >= 8,
              "streams stage passes must drain at least eight batch chunks");

enum class GenthreadMicrostage : uint8_t {
    N0,
    I0,
    N1,
    E0,
    W0,
    I1,
    E1,
    W1,
    I2,
    E2,
    W2,
    N2,
};

// Owner-approved streams order. The loop body spells this order directly; the constant is the
// reviewable static contract rather than a function-pointer dispatch table.
inline constexpr std::array<GenthreadMicrostage, 12> kGenthreadStreamsSchedule = {
    GenthreadMicrostage::N0,
    GenthreadMicrostage::I0,
    GenthreadMicrostage::N1,
    GenthreadMicrostage::E0,
    GenthreadMicrostage::W0,
    GenthreadMicrostage::I1,
    GenthreadMicrostage::E1,
    GenthreadMicrostage::W1,
    GenthreadMicrostage::I2,
    GenthreadMicrostage::E2,
    GenthreadMicrostage::W2,
    GenthreadMicrostage::N2,
};

}  // namespace tomo
