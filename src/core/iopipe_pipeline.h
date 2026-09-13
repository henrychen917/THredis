// iopipe_pipeline.h -- fixed geometry for the split-IO micro-pipeline experiment.
//
// Keep every batch size, buffer count, prefetch window, and schedule entry in this one block.  The
// `--x-overlap 1` selects this one static loop shape; the geometry itself is not tunable. A
// stage is batch-granular and returns immediately when its stream buffer is empty; there are no
// fibers, request schedulers, or per-request stage machines.
#pragma once
#include <array>
#include <cstdint>

namespace tomo {

// Batch sizes follow the work already visible at the loop boundary: a stage takes min(backlog,
// cap), rather than advancing a fixed 16-client slice through another complete rotation.  The
// caps cover the 64-slot ROB/ordinary depth cell while keeping control and quiesce latency bounded.
// A shallow pass still contains only the one or few clients/frames that actually arrived.
inline constexpr uint32_t kIoPipeIfidBatchClients = 64;
inline constexpr uint32_t kIoPipeIfidBatchOpsPerClient = 64;

// WB buffers completion/serve requests by connection.  A ROB holds at most 64 operations, so this
// window covers every possible retireable prefix instead of letting an unprefetched tail leak into
// the warm retirement stage.  Borrow hints cover the first eight payload cache lines; they remain
// hints only and neither copy nor extend the store borrow's lifetime.
inline constexpr uint32_t kIoPipeWbBatchClients = 64;
inline constexpr uint32_t kIoPipeWbPrefetchOpsPerClient = 64;
inline constexpr uint32_t kIoPipeWbBorrowPrefetchBytes = 512;
inline constexpr uint32_t kIoPipeCacheLineBytes = 64;

// The ready mask is the ordinary WB selector. Once per this many rotations, IFID also nominates
// the clients it visits as the mask-independent completion backstop. This is the existing cadence,
// named here because it is part of the static stream schedule rather than a runtime policy.
inline constexpr uint32_t kIoPipeWbBackstopTurns = 64;

// The order gate is sampled exactly once at the outer loop-pass boundary.  Four completed passes
// form its window; distinct enter/leave levels avoid changing order around the threshold.  One ROB
// window per pass is already enough independent natural-order work to amortize cross-core latency,
// while many concurrent p1 connections remain below it.  The WB prefetch interleave is therefore
// reserved for the shallow regime it was built to help.
inline constexpr uint32_t kIoPipeDepthWindowPasses = 4;
inline constexpr uint32_t kIoPipeNaturalEnterFramesPerPass = 64;
inline constexpr uint32_t kIoPipeNaturalLeaveFramesPerPass = 32;

struct IoPipeDepthGate {
    void reset(uint64_t total_frames) {
        frames.fill(0);
        sum = 0;
        cursor = 0;
        last_total = total_frames;
        natural = false;
    }

    bool loop_boundary(uint64_t total_frames) {
        uint64_t delta = total_frames >= last_total ? total_frames - last_total : 0;
        last_total = total_frames;
        if (delta > UINT32_MAX) delta = UINT32_MAX;
        sum -= frames[cursor];
        frames[cursor] = static_cast<uint32_t>(delta);
        sum += frames[cursor];
        if (++cursor == frames.size()) cursor = 0;

        constexpr uint64_t kEnter = kIoPipeNaturalEnterFramesPerPass *
                                    kIoPipeDepthWindowPasses;
        constexpr uint64_t kLeave = kIoPipeNaturalLeaveFramesPerPass *
                                    kIoPipeDepthWindowPasses;
        if (natural) {
            if (sum <= kLeave) natural = false;
        } else if (sum >= kEnter) {
            natural = true;
        }
        return natural;
    }

    std::array<uint32_t, kIoPipeDepthWindowPasses> frames{};
    uint64_t last_total = 0;
    uint64_t sum = 0;
    uint32_t cursor = 0;
    bool natural = false;
};

enum class IoPipeStage : uint8_t {
    WbObserve,
    IfidRx,
    WbPrefetch,
    IfidParseHash,
    WbRetirePrepare,
    IfidPost,
    WbSubmitReclaim,
};

// THE HOT ROTATION.  This is expanded directly, in this order, by IoLoop::pipeline_pass().  Keeping
// the array here makes schedule changes reviewable beside the batch geometry instead of hiding
// them among control/cron maintenance in the outer loop.
inline constexpr std::array<IoPipeStage, 7> kIoPipeSchedule = {
    IoPipeStage::WbObserve,
    IoPipeStage::IfidRx,
    IoPipeStage::WbPrefetch,
    IoPipeStage::IfidParseHash,
    IoPipeStage::WbRetirePrepare,
    IoPipeStage::IfidPost,
    IoPipeStage::WbSubmitReclaim,
};

}  // namespace tomo
