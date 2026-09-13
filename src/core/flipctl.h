// flipctl.h -- allocation-free IO fingerprints and the single-owner FLIP controller.
//
// The writer is embedded in ThreadCtx, but only that physical thread mutates it while playing the
// IO role.  The monitor/controller reads completed cumulative windows; no per-operation atomic or
// shared-line write is introduced.  FlipController itself is written only by the main monitor
// thread.  INFO/DEBUG take its cold mutex and DEBUG's forced trigger is a single cold atomic flag.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "flip_policy.h"

namespace tomo {

class Server;

enum class FlipFingerprintClass : uint8_t {
    Read = 0,
    Write,
    MultiRead,
    MultiWrite,
    AtomicGrouped,
    Blocking,
    Other,
    Count,
};

inline constexpr size_t kFlipFingerprintClasses =
    static_cast<size_t>(FlipFingerprintClass::Count);

// Monotonic cumulative publication.  A controller subtraction therefore consumes every closed
// work window even when an IO thread closes many of them between monitor beats.
struct FlipFingerprintWindow {
    std::array<uint64_t, 4> pass_depth{};
    std::array<uint64_t, kFlipFingerprintClasses> command_class{};
    uint64_t commands = 0;
    uint64_t multikey_keys = 0;
    uint64_t multikey_ops = 0;
    uint64_t value_bytes = 0;
    uint64_t closed_windows = 0;
};

// SAMPLED BY PARSE PASS (DESIGN-flipfp.md). The io loop asks pass_sampled() once per dispatched
// frame -- one load of this writer's own word, on the line the old enabled() test already read,
// and one predicted-false branch; no store. Inside a sampled pass the full fingerprint body runs
// for every frame and note_command accumulates exactly what the exhaustive writer accumulated for
// that pass; at the pass end finish_parse_pass() publishes it and the caller draws the gap to the
// next sampled pass. `work_window_` keeps the knob's documented meaning, commands per fingerprint
// sample: one pass in W is sampled, so one frame in W is fingerprinted at every pipeline depth (a
// sampled pass contributes all of its d frames and costs the body d times, once per W passes).
// Gaps are uniform over [1, 2W-1] passes (mean W). A stride of exactly W would alias with any
// workload whose classes alternate with a period dividing W (memtier's 1:1 ratio is period 2);
// a uniform gap has 1 in its support, so the renewal process equidistributes over every period and
// each class is sampled in proportion. The draw is the io loop's own xorshift, taken only when a
// sampled pass ends, so the writer holds no generator state and the ThreadCtx footprint is
// unchanged (pass_frames_ became pass_countdown_; partial_.commands is the sampled pass's depth).
// W = 1 samples every pass: the exhaustive writer, byte for byte, and the unit test's oracle.
// Dark writer (work_window_ == 0: --flip-auto 0, every 1s boot): pass_countdown_ is 1 and nothing
// ever writes it, so the gate word reads false from a clean line and no other code runs.
class FlipFingerprintWriter {
public:
    void configure(uint32_t commands_per_sample) {
        work_window_ = commands_per_sample;
        // Armed: the first pass is sampled, so the controller's boot gate sees work after one pass
        // rather than one gap. Dark: the gate word must never read 0.
        pass_countdown_ = commands_per_sample ? 0 : 1;
        partial_ = {};
        published_ = {};
    }

    bool enabled() const { return work_window_ != 0; }
    uint32_t work_window() const { return work_window_; }

    // THE per-operation gate: true only inside a sampled parse pass.
    bool pass_sampled() const { return pass_countdown_ == 0; }

    // Called only inside a sampled pass, after the parsed frame has crossed its last
    // refusal/backpressure point.
    void note_command(FlipFingerprintClass command_class, uint32_t keys,
                      uint64_t value_bytes) {
        partial_.command_class[static_cast<size_t>(command_class)]++;
        partial_.commands++;
        if (keys > 1) {
            partial_.multikey_keys += keys;
            partial_.multikey_ops++;
        }
        partial_.value_bytes += value_bytes;
    }

    // Pass end, called only while enabled(). An unsampled pass counts down. A sampled pass buckets
    // its depth (partial_.commands is exactly this pass's frame count: the previous sampled pass
    // published and reset partial_) and publishes; the return value asks the caller for one random
    // draw for arm(). Detection latency stays work based: the next sample is a number of passes
    // away, never a wall-clock interval.
    bool finish_parse_pass() {
        if (pass_countdown_ != 0) {
            pass_countdown_--;
            return false;
        }
        const uint64_t frames = partial_.commands;
        if (frames) {
            const size_t bucket = frames == 1 ? 0 : frames <= 4 ? 1 : frames <= 16 ? 2 : 3;
            partial_.pass_depth[bucket]++;
            for (size_t i = 0; i < published_.pass_depth.size(); i++)
                published_.pass_depth[i] += partial_.pass_depth[i];
            for (size_t i = 0; i < published_.command_class.size(); i++)
                published_.command_class[i] += partial_.command_class[i];
            published_.commands += partial_.commands;
            published_.multikey_keys += partial_.multikey_keys;
            published_.multikey_ops += partial_.multikey_ops;
            published_.value_bytes += partial_.value_bytes;
            // Keep this as the last owner store. The exceptional monitor reader checks this field
            // before consuming the cumulative counters, matching the tree's other owner-local INFO
            // counters without adding an atomic publication to the request path.
            published_.closed_windows++;
            partial_ = {};
        }
        return true;
    }

    // Unsampled passes before the next sampled one: uniform over [0, 2W-2], so sampled passes are
    // 1..2W-1 apart with mean W. W = 1 gives 0 every time (every pass sampled).
    static uint32_t gap_for(uint64_t random, uint32_t work_window) {
        return static_cast<uint32_t>(random % (2ull * work_window - 1));
    }
    void arm(uint64_t random) { pass_countdown_ = gap_for(random, work_window_); }

    const FlipFingerprintWindow& published() const { return published_; }

private:
    uint32_t work_window_ = 0;
    uint32_t pass_countdown_ = 1;
    FlipFingerprintWindow partial_{};
    FlipFingerprintWindow published_{};
};

struct FlipSignature {
    std::array<double, 4> pass_depth{};
    std::array<double, kFlipFingerprintClasses> command_class{};
    double keys_per_multikey = 0;
    double value_bytes_per_command = 0;
    uint64_t commands = 0;
    bool valid = false;
};

FlipSignature flip_signature(const FlipFingerprintWindow& sample);
// The workload-mix distance. It deliberately EXCLUDES pass_depth: parse-pass occupancy is an
// arrival-batching observation the controller's own actuator moves (a measured io 5->7 step moved
// it by 0.048 while the mix families moved by 4e-7), and the sweep-abandon law says a signal the
// actuator moves cannot police the actuator. pass_depth stays measured, published and dumped for
// diagnosis; it is simply not trigger evidence.
double flip_signature_distance(const FlipSignature& left, const FlipSignature& right);
// Whole-signature distance including pass_depth. Diagnostics only -- never a trigger input.
double flip_signature_pass_distance(const FlipSignature& left, const FlipSignature& right);

// Small, deterministic detector used by both the controller and the unit test. In auto mode its
// band is twice the signature's own adjacent-window EWMA jitter, with one observed-command quantum
// as the floor. A numeric band is a percent; zero disables shift triggering.
class FlipShiftDetector {
public:
    explicit FlipShiftDetector(int32_t configured_band = -1,
                               uint32_t noise_windows = 1)
        : configured_band_(configured_band) {
        signature_noise_.configure(noise_windows);
    }

    void reset();
    bool observe(const FlipFingerprintWindow& sample);
    void anchor();

    bool anchored() const { return anchored_; }
    // Cold diagnostic readers (DEBUG FLIPCTL only): the two vectors whose distance is the
    // fingerprint trigger, so an operator can see WHICH feature family moved.
    const FlipSignature& smoothed() const { return smoothed_; }
    const FlipSignature& anchored_signature() const { return anchored_signature_; }
    double band() const { return band_; }
    double last_distance() const { return last_distance_; }
    double jitter() const { return jitter_; }
    // The signal's own adjacent-window movement, learned continuously from IN-BAND windows: the
    // floor under the band, typed or learned.
    double noise_bound() const { return signature_noise_.bound(); }
    uint32_t noise_samples() const { return signature_noise_.samples; }

private:
    void update_band();

    int32_t configured_band_ = -1;
    FlipSignature smoothed_{};
    FlipSignature previous_{};
    FlipSignature learning_origin_{};
    FlipSignature anchored_signature_{};
    double jitter_ = 0;
    // Every in-band window's adjacent-window distance, exponentially weighted. jitter_ is the max
    // over the pre-anchor learning windows and is then FROZEN (so an excursion cannot widen the
    // threshold that judges it); this keeps learning the quiet-state noise for as long as the
    // anchor holds, which is what a band cut from one quiet learning window misses.
    FlipEwBound signature_noise_{};
    double band_ = 0;
    double last_distance_ = 0;
    bool have_previous_ = false;
    bool have_jitter_ = false;
    bool anchored_ = false;
};

enum class FlipctlTriggerReason : uint8_t {
    None = 0,
    Boot,
    FingerprintShift,
    AnchorRateSurge,
    AnchorRateCollapse,
    Forced,
};

struct FlipctlReport {
    std::string state = "disabled";
    std::string phase = "disabled";
    std::string last_trigger = "none";
    uint32_t anchor_io = 0;
    uint32_t anchor_ex = 0;
    double anchor_rate = 0;
    double signature_band = 0;
    double rate_band = 0;
    uint64_t triggers = 0;
    uint64_t boot_triggers = 0;
    uint64_t fingerprint_triggers = 0;
    uint64_t rate_surge_triggers = 0;
    uint64_t rate_collapse_triggers = 0;
    uint64_t forced_triggers = 0;
    uint64_t null_maneuvers = 0;
    uint64_t model_holds = 0;
    uint32_t shift_confirmations = 0;
    uint32_t rate_confirmations = 0;
    uint64_t round_trips = 0;
    uint32_t model_margin = 0;
    // Redesign 2026-09-06: the decision the model last took, its calibration and outcome record,
    // and the measured transfer cost the gate charges.
    std::string model_last_decision = "none";
    double model_kappa = 1.0;
    uint32_t model_moves = 0;
    uint32_t model_misses = 0;
    uint64_t invalidated_maneuvers = 0;
    uint64_t cost_holds = 0;
    double client_cost = 0;       // commands lost per transferred client (measured or injected)
    double last_flip_lost = 0;    // commands the last flip cost
    uint64_t last_flip_moved = 0; // connections the last flip moved
    double flip_ticks = 0;        // mean controller ticks a flip stays in flight
    double stationary_s = 0;      // seconds the current workload has held still
    std::string refine_decision = "none";  // the local verdict at the last landing (see refine)
    uint32_t refine_steps = 0;    // extra steps taken after a delivered move, this maneuver
};

class FlipController {
public:
    FlipController() = default;
    FlipController(const FlipController&) = delete;
    FlipController& operator=(const FlipController&) = delete;

    bool init(bool enabled, int32_t configured_band, uint32_t nthreads);
    bool enabled() const { return enabled_; }
    uint32_t signal_sample_rate() const {
        return signal_sample_rate_.load(std::memory_order_relaxed);
    }
    void request_forced_trigger() {
        force_requested_.store(true, std::memory_order_release);
    }

    // Called only by the main monitor thread. Returns true when a new internal FLIP was issued.
    bool tick(Server& server, uint64_t now_ms);
    // How long the monitor should sleep before the next tick: the controller tick, except while a
    // flip it issued is in flight, when it looks kFlipPollDivisor times per tick so the flip's
    // cost (commands not served, duration) is measured to a fraction of a tick instead of being
    // quantized to whole ticks. Cold: the monitor thread only.
    uint32_t wait_ms(uint32_t tick_ms) const;
    FlipctlReport report() const;
    std::string debug_dump() const;
    // DEBUG FLIPCTL SEEK <io> [FORCE]: propose a split as the next maneuver's hypothesis. Without
    // FORCE it is judged by the model's projection for that split, the noise bar and the cost gate
    // like any model target; with FORCE the flip is issued regardless and only the outcome loop
    // judges it -- the directed test of "an induced miss must revert and raise the bar".
    bool debug_seek(uint32_t target_io, bool force, std::string& error);
    // DEBUG FLIPCTL COST <commands-per-client>: a typed per-client transfer cost replaces the
    // measured one (negative restores the measurement) -- the directed test of the cost gate.
    void debug_cost(double commands_per_client);

private:
    enum class Phase : uint8_t {
        Disabled = 0,
        BootPending,
        Measuring,
        WaitingFlip,
        Seeking,
        Settling,
        Anchored,
    };

    struct ThreadMeasure {
        uint64_t ops = 0;
        uint64_t busy_ns = 0;
        uint64_t idle_ns = 0;
    };

    struct Reading {
        uint32_t split = 0;
        double rate = 0;
    };

    struct MovementStamp {
        uint64_t flip_completed = 0;
        uint64_t flip_refused = 0;
        uint64_t flip_rebinds = 0;
        uint64_t bucket_moves = 0;
        uint64_t client_moves = 0;

        bool operator==(const MovementStamp&) const = default;
    };

    static const char* phase_name(Phase phase);
    static const char* reason_name(FlipctlTriggerReason reason);
    void start_maneuver(Server& server, FlipctlTriggerReason reason, uint64_t now_ms);
    bool sample_fingerprint(Server& server);
    bool sample_rate(Server& server, uint64_t now_ms, double& rate);
    bool sample_stabilized_rate(Server& server, uint64_t now_ms, double& rate);
    bool sample_anchored_rate(Server& server, uint64_t now_ms, double& rate);
    bool boot_load_stable(Server& server, uint64_t now_ms);
    MovementStamp movement_stamp(const Server& server) const;
    uint64_t total_commands(const Server& server) const;
    void enter_settling();
    bool sample_role_demand(Server& server, uint64_t now_ms, double& io_frac,
                            double& io_headroom, double& ex_headroom);
    double verification_band(double rate) const;
    double baseline_band() const;
    bool decide_placement(Server& server, uint32_t coordinator, uint64_t now_ms, double rate);
    bool issue_flip(Server& server, uint32_t coordinator, uint32_t target_io,
                    Phase after_flip, uint64_t now_ms, double rate_before);
    void measure_flip(Server& server, uint64_t now_ms);
    uint32_t io_clients(const Server& server) const;
    bool seek_after_reading(Server& server, uint32_t coordinator, uint64_t now_ms, double rate);
    bool settle(Server& server, uint32_t coordinator, uint64_t now_ms);
    void anchor(Server& server, double rate);
    void begin_refinement(Server& server, uint64_t now_ms);
    void record(uint32_t split, double rate);
    double automatic_rate_band(double pair_delta, double rate) const;

    mutable std::mutex mutex_;
    bool enabled_ = false;
    int32_t configured_band_ = -1;
    Phase phase_ = Phase::Disabled;
    Phase after_flip_ = Phase::Seeking;
    FlipctlTriggerReason last_trigger_ = FlipctlTriggerReason::None;
    std::atomic<bool> force_requested_{false};
    std::atomic<uint32_t> signal_sample_rate_{0};

    std::vector<FlipFingerprintWindow> fingerprint_last_;
    FlipShiftDetector shift_detector_{};
    std::vector<ThreadMeasure> maneuver_start_;
    std::vector<ThreadMeasure> maneuver_mark_;
    std::vector<Reading> readings_;

    uint64_t triggers_ = 0;
    uint64_t boot_triggers_ = 0;
    uint64_t fingerprint_triggers_ = 0;
    uint64_t rate_surge_triggers_ = 0;
    uint64_t rate_collapse_triggers_ = 0;
    uint64_t forced_triggers_ = 0;

    uint32_t anchor_io_ = 0;
    uint32_t anchor_ex_ = 0;
    double anchor_rate_ = 0;
    double anchor_rate_jitter_ = 0;
    double anchor_rate_band_ = 0;
    double anchor_rate_band_floor_ = 0;
    uint32_t surge_streak_ = 0;
    uint32_t collapse_streak_ = 0;

    uint32_t maneuver_origin_io_ = 0;
    // PLACEMENT POLICY STATE (flip_policy.h). The demand window accumulates one io-share draw per
    // stabilized reading; the choice is re-evaluated on every draw until it is decided, or the
    // reading cap -- the boot deferral bound expressed in two-tick readings -- forces a hold.
    // Two draws are the fewest that admit a variance, and the sequential test's 2-SE interval then
    // provides the distance-derived wait (flip_policy.h): a wildly wrong split's pessimistic gain
    // clears the bar at two readings, a marginal split's does not until the interval tightens. The
    // outcome loop catches a two-draw move that misses -- the wait is not the safety net.
    static constexpr uint32_t kMinModelSamples = 2;
    FlipDemandWindow demand_window_{};
    uint32_t model_readings_ = 0;
    uint32_t model_readings_cap_ = 0;
    double model_io_frac_ = 0;
    double model_io_frac_low_ = 0;
    double model_io_frac_high_ = 0;
    double model_io_headroom_ = 0;
    double model_ex_headroom_ = 0;
    double model_gain_mean_ = 0;
    double model_gain_low_ = 0;
    double model_gain_high_ = 0;
    double model_required_gain_ = 0;
    uint32_t model_target_io_ = 0;
    uint64_t model_holds_ = 0;
    const char* model_last_decision_ = "none";
    // OUTCOME MARGIN: how many throughput bands of projected gain the model must clear. Doubles
    // when a maneuver moved and came back (the projection did not deliver -- a biased estimator
    // does not get less biased with more samples, so the BAR rises), halves when a move delivered,
    // and never grows past the point where the required gain would exceed 100%, which would
    // retire the model for good. Derived at both ends: the band is learned, the cap is arithmetic.
    uint32_t model_margin_ = 1;
    uint64_t round_trips_ = 0;
    uint32_t maneuver_flips_ = 0;
    double maneuver_rate_jitter_ = 0;
    uint64_t null_maneuvers_ = 0;
    // OUTCOME BACKOFF. Every trigger already demands consecutive confirming control passes before
    // it starts a maneuver. When a maneuver ends on the split it started from it delivered nothing,
    // so the next excursion of that kind must present MORE passes of evidence, not clear a wider
    // threshold: widening a threshold on a distance normalized to [0,1] can put it out of reach of
    // any workload change at all and silently retire the detector, while a longer window keeps full
    // sensitivity and only costs time. Doubling per null result, halving back to the floor when a
    // maneuver actually moves the split, bounded by the window the controller already needs to
    // learn an anchor -- past that it would take longer to NOTICE a change than to characterize
    // one. Not a knob: both ends are derived.
    static constexpr uint32_t kMinConfirmations = 2;
    uint32_t shift_confirmations_ = kMinConfirmations;
    uint32_t rate_confirmations_ = kMinConfirmations;
    uint32_t shift_streak_ = 0;
    uint32_t pending_target_io_ = 0;
    uint64_t pending_completed_ = 0;
    uint64_t pending_refused_ = 0;
    // VERIFY-OR-REVERT SEEK: one model-directed flip, one stabilized reading judged against the
    // origin's stabilized reading, then anchor there or flip straight back.
    enum class Seek : uint8_t { None = 0, AtTarget, Returning };
    Seek seek_ = Seek::None;
    uint32_t seek_target_io_ = 0;
    // REFINEMENT. A projection from the origin's demand reading is only as good as the assumption
    // that per-command io and ex costs are the same at the target as at the origin; they are not
    // (the io side batches better with fewer threads), so a one-shot argmax from an io-heavy origin
    // lands short -- measured on the owner's box: 18:14 -> 13:19 settled 8% below 11:21. After a
    // DELIVERED move the maneuver therefore re-measures the local demand at the landing and takes
    // one more outcome-judged step while the local argmax differs from the live split and pays
    // (measured transfer cost now), stopping at the fixed point where the landing IS its own
    // argmax, or at a split already visited this maneuver. The departure split of the current
    // hypothesis is seek_origin_io_ (a miss returns there); maneuver_origin_io_ stays the split the
    // maneuver started from (the null-maneuver rule).
    uint32_t seek_origin_io_ = 0;
    bool refining_ = false;
    double refine_bar_ = 0;   // flip_refine_bar of the step that landed here
    const char* refine_decision_ = "none";
    uint32_t refine_steps_ = 0;
    double origin_rate_ = 0;
    // Every per-tick rate sample, whatever the phase, exponentially weighted over the deferral bound
    // (flip_policy.h): the floor under every band the controller learns from stabilized pairs.
    FlipEwVariance rate_ew_{};
    // The ORIGIN's stabilized readings while the model was deciding (Measuring never moves the
    // split): their mean is R0, their relative stdev is the noise the verification window is sized
    // from, and their bracket (min/max) floors every band of the maneuver -- a gain smaller than
    // the baseline's own movement is the baseline moving, which is how the gate's ramping driver
    // once anchored a boot maneuver on a rail.
    FlipRateWindow origin_window_{};
    // The TARGET's stabilized readings during the seek: judged sequentially against the origin.
    FlipRateWindow target_window_{};
    // What every flip cost and what every move delivered (flip_policy.h).
    FlipCostModel cost_{};
    FlipCostVerdict model_cost_{};
    uint32_t model_verify_readings_ = 0;   // planned target readings of the current hypothesis
    double model_kappa_ = 1.0;
    // The hypothesis under test: predicted gain (kappa-scaled), the origin's noise and sample
    // count it is judged against, planned readings, and what the target delivered so far.
    double hyp_predicted_ = 0;
    double hyp_sigma_ = 0;
    uint32_t hyp_origin_samples_ = 0;
    double hyp_delivered_ = 0;
    double hyp_threshold_ = 0;
    bool pending_miss_ = false;
    uint64_t invalidated_maneuvers_ = 0;
    uint64_t cost_holds_ = 0;
    // How long the workload has held still: the boot's first non-idle tick, or the trigger that
    // detected a change. A forced trigger does not reset it (the workload did not change).
    uint64_t stationary_since_ms_ = 0;
    uint64_t maneuver_mark_ms_ = 0;       // the tick clock at the last demand mark (wall per thread)
    // The flip in flight, for its cost: issue time, commands and transfers at issue, the rate the
    // server ran at before it, and the naive transfer count the role change requires.
    uint64_t flip_issue_ms_ = 0;
    uint64_t flip_issue_commands_ = 0;
    uint64_t flip_issue_transfers_ = 0;
    double flip_issue_rate_ = 0;
    double flip_issue_naive_ = 0;
    bool flip_measure_armed_ = false;
    double last_flip_lost_ = 0;
    uint64_t last_flip_moved_ = 0;
    // A DEBUG FLIPCTL SEEK request, consumed by the next decision.
    uint32_t debug_seek_io_ = 0;
    bool debug_seek_force_ = false;
    // The reading mechanics behind T_black: after a flip the rate window restarts (one tick) and a
    // stabilized reading needs two sub-windows -- three ticks before the first target reading.
    static constexpr uint32_t kSettleTicks = 3;
    // Looks per tick while a flip is in flight (wait_ms). A sampling ratio, not a machine
    // constant: it sets how finely a sub-tick event is resolved, and only the cold monitor pays.
    static constexpr uint32_t kFlipPollDivisor = 8;

    uint64_t rate_window_ms_ = 0;
    uint64_t rate_window_commands_ = 0;
    MovementStamp rate_window_movement_{};
    double previous_subwindow_rate_ = 0;
    double stable_pair_delta_ = 0;
    double boot_rate_ewma_ = 0;
    double boot_rate_jitter_ = 0;
    double boot_rate_slope_ = 0;
    double boot_rate_slope_threshold_ = 0;
    double boot_previous_ewma_change_ = 0;
    double anchor_learning_rate_jitter_ = 0;
    double anchor_learning_rate_sum_ = 0;
    double anchor_learning_rate_min_ = 0;
    double anchor_learning_rate_max_ = 0;
    uint32_t anchor_learning_rate_samples_ = 0;
    bool previous_subwindow_valid_ = false;
    bool boot_rate_ewma_valid_ = false;
    bool boot_previous_ewma_change_valid_ = false;
    bool boot_rate_jitter_valid_ = false;
    bool boot_work_observed_ = false;
    static constexpr uint32_t kBootTrendTicks = 5;
    std::array<double, kBootTrendTicks> boot_rate_history_{};
    uint32_t boot_rate_samples_ = 0;
    uint32_t boot_nonidle_ticks_ = 0;

    uint32_t anchor_signature_samples_ = 0;
    uint32_t signature_learning_windows_ = 1;
    uint32_t maneuver_learning_windows_ = 1;
    bool fingerprint_sampled_this_tick_ = false;
    bool anchor_sampling_disabled_ = false;
    uint32_t maneuver_signature_samples_ = 0;
    bool retrigger_after_flip_ = false;
    FlipctlTriggerReason pending_retrigger_ = FlipctlTriggerReason::None;
    double last_shift_distance_ = 0;
    double last_shift_band_ = 0;
};

}  // namespace tomo
