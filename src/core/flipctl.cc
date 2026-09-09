#include "flipctl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "server.h"

namespace tomo {

namespace {

// A real workload must not wait forever merely because its stationary noise never presents a
// quiet absolute-rate band. Thirty seconds is long enough to reject ordinary connection ramps but
// short enough to make automatic placement useful during a container/cell warm-up. The gate below
// converts this duration to controller ticks, so the internal tick preserves the same wall-clock bound.
constexpr uint64_t kBootMaxDeferralMs = 30'000;

// Anchored drift is deliberately much slower than both the half-weight maneuver EWMA and the
// two-observation surge detector. Only in-band readings reach this learner, and a 64-observation
// time constant lets it follow real long-lived drift without turning ordinary hold jitter into a
// new reference edge.
constexpr double kAnchorRateEwmaAlpha = 1.0 / 64.0;

double relative_distance(double left, double right) {
    const double scale = std::max(std::abs(left), std::abs(right));
    return scale > 0 ? std::abs(left - right) / scale : 0;
}

double unit_normalized_distance(double left, double right) {
    // A one-byte/one-key measurement quantum keeps a zero anchor from turning one rare admin
    // command's infinitesimal average into distance 1. Large workload changes still approach 1.
    return std::abs(left - right) / (1.0 + std::max(std::abs(left), std::abs(right)));
}

template <size_t N>
void normalize(const std::array<uint64_t, N>& input, std::array<double, N>& output) {
    uint64_t total = 0;
    for (uint64_t value : input) total += value;
    if (!total) return;
    for (size_t i = 0; i < N; i++)
        output[i] = static_cast<double>(input[i]) / static_cast<double>(total);
}

void add_delta(uint64_t current, uint64_t previous, uint64_t& destination) {
    if (current >= previous) destination += current - previous;
}

}  // namespace

FlipSignature flip_signature(const FlipFingerprintWindow& sample) {
    FlipSignature signature;
    if (!sample.commands) return signature;
    normalize(sample.pass_depth, signature.pass_depth);
    normalize(sample.command_class, signature.command_class);
    signature.keys_per_multikey = sample.multikey_ops
        ? static_cast<double>(sample.multikey_keys) / sample.multikey_ops : 0;
    signature.value_bytes_per_command =
        static_cast<double>(sample.value_bytes) / sample.commands;
    signature.commands = sample.commands;
    signature.valid = true;
    return signature;
}

double flip_signature_distance(const FlipSignature& left, const FlipSignature& right) {
    if (!left.valid || !right.valid) return 0;
    double class_l1 = 0;
    for (size_t i = 0; i < left.command_class.size(); i++)
        class_l1 += std::abs(left.command_class[i] - right.command_class[i]);
    // Three families, all of them properties of WHAT THE CLIENTS ASKED FOR: the command-class mix,
    // the keys each multi-key command names, and the value bytes each command carries. The
    // probability-vector L1 is in [0,2] and each relative scalar distance is in [0,1], so dividing
    // by the family count keeps the distance normalized to [0,1].
    //
    // pass_depth is NOT here. Measured on the multi-key regime this defect was found in: one
    // controller step from io=5 to io=7 moved the pass-depth distance by 0.0476 -- eight to forty
    // times its own learned trigger band -- while the three mix families moved by 4.2e-7, 0 and
    // 5.4e-7 across the same seconds. Parse-pass occupancy describes how the io threads happened
    // to batch arrivals, which is exactly what the actuator changes when it moves connections
    // between owners. Judging placement with it is the sweep-abandon livelock: the controller
    // re-fires on its own last move. Pipeline-depth changes that matter still reach the controller
    // through the rate detector, which owns volume.
    return (class_l1 * 0.5 +
            unit_normalized_distance(left.keys_per_multikey, right.keys_per_multikey) +
            unit_normalized_distance(left.value_bytes_per_command,
                                     right.value_bytes_per_command)) / 3.0;
}

double flip_signature_pass_distance(const FlipSignature& left, const FlipSignature& right) {
    if (!left.valid || !right.valid) return 0;
    double pass_l1 = 0;
    for (size_t i = 0; i < left.pass_depth.size(); i++)
        pass_l1 += std::abs(left.pass_depth[i] - right.pass_depth[i]);
    return pass_l1 * 0.5;
}

void FlipShiftDetector::reset() {
    signature_noise_.reset();
    smoothed_ = {};
    previous_ = {};
    learning_origin_ = {};
    anchored_signature_ = {};
    jitter_ = 0;
    band_ = 0;
    last_distance_ = 0;
    have_previous_ = false;
    have_jitter_ = false;
    anchored_ = false;
}

void FlipShiftDetector::update_band() {
    if (configured_band_ == 0) {
        band_ = 0;
        return;
    }
    // SAMPLING SCALE, not counting resolution. Every family in the distance is a proportion or a
    // per-command mean estimated from the N commands this window observed, so two windows drawn
    // from one stationary workload differ by the ESTIMATOR'S OWN NOISE, which falls as 1/sqrt(N)
    // -- never as 1/N. A Bernoulli proportion's standard error is at most 0.5/sqrt(N) and the
    // difference of two independent estimates at most sqrt(2) of that, so 1/sqrt(N) bounds the
    // per-family noise with room for the few terms an L1 sums. The old 1/N quantum understated it
    // by sqrt(N) -- twentyfold at 13.5k commands a window -- which is how a window that merely
    // drew 60 MSETs where the last one drew 50 could read as a workload change. A window that
    // observed no commands supplies no evidence: saturate at one command (quantum 1.0) instead of
    // collapsing to zero, or an anchor cut from an idle window freezes a zero band and judges the
    // first busy wobble as a mix shift. Rate triggers, not fingerprints, own idle->busy.
    //
    // N IS WHAT THE SIGNATURE WAS ESTIMATED FROM, so when the writer samples the request stream the
    // quantum grows with the sampling: at one command in a hundred a 100-command window resolves
    // the mix to about 0.1, not to the 0.009 the underlying 10k commands would give.
    const double quantum =
        1.0 / std::sqrt(static_cast<double>(std::max<uint64_t>(smoothed_.commands, 1)));
    // A TYPED BAND IS A FLOOR, NOT A CEILING, AND IT CANNOT BUY RESOLUTION THE SIGNAL HAS NOT GOT.
    // The internal detector's explicit-band unit-test control sets the mix change worth a maneuver;
    // it says nothing about how
    // still the signature holds, and it used to return here consulting neither the estimator's
    // resolution nor the signal's measured movement. Measured by the gate-hygiene lane on a driver
    // whose rate held to 0.07% across 34 samples: a fingerprint distance of 0.2518 against a flat
    // 0.0200 band -- 12.6x -- fired a maneuver on a stationary load. Simulated here on a 1-in-100
    // sampled stream, a typed 2% band produced three two-consecutive exceedances in 600 stationary
    // windows (a spurious maneuver each); with the quantum applied it produces none, and a real mix
    // change still clears the band by 3.3x. So both branches take the same two floors.
    const double floor = std::max(2.0 * quantum, signature_noise_.bound());
    if (configured_band_ > 0) {
        band_ = std::max(static_cast<double>(configured_band_) / 100.0, floor);
        return;
    }
    // ... and the learned band adds the frozen learning-window jitter, floored the same way: that
    // jitter is the largest adjacent step seen in the few windows before the anchor, so a learning
    // window that happened to be still freezes a band the hold then trips over.
    band_ = std::max(2.0 * jitter_, floor);
}

bool FlipShiftDetector::observe(const FlipFingerprintWindow& sample) {
    const FlipSignature incoming = flip_signature(sample);
    if (!incoming.valid) return false;
    if (!smoothed_.valid) {
        smoothed_ = incoming;
        learning_origin_ = incoming;
    } else {
        for (size_t i = 0; i < smoothed_.pass_depth.size(); i++)
            smoothed_.pass_depth[i] =
                (smoothed_.pass_depth[i] + incoming.pass_depth[i]) * 0.5;
        for (size_t i = 0; i < smoothed_.command_class.size(); i++)
            smoothed_.command_class[i] =
                (smoothed_.command_class[i] + incoming.command_class[i]) * 0.5;
        smoothed_.keys_per_multikey =
            (smoothed_.keys_per_multikey + incoming.keys_per_multikey) * 0.5;
        smoothed_.value_bytes_per_command =
            (smoothed_.value_bytes_per_command + incoming.value_bytes_per_command) * 0.5;
        smoothed_.commands = incoming.commands;
        smoothed_.valid = true;
    }

    const double adjacent = have_previous_
        ? flip_signature_distance(smoothed_, previous_) : 0;
    if (anchored_) {
        last_distance_ = flip_signature_distance(smoothed_, anchored_signature_);
        const bool fired = configured_band_ != 0 && last_distance_ > band_;
        // LEARN THE STATISTIC THAT IS ACTUALLY TESTED. The trigger compares the distance from the
        // ANCHOR; the adjacent-window distance is a different quantity -- smoothed and correlated,
        // so systematically smaller -- and flooring an anchor-distance test with it underestimates.
        // Measured on a simulated 1-in-100 sampled stream: adjacent-learned bound 0.054 against a
        // quiet-state anchor distance reaching 0.101, which fired 15 times in 60 stationary windows.
        // So the noise estimate is the anchored distance's own null distribution.
        //
        // JUDGE FIRST, FOLD ONLY IN-BAND WINDOWS. An excursion must never widen the threshold that
        // judges it -- that is why this branch froze the band originally, and it stands. Folding
        // every window instead cost exactly that: with the decaying maximum below, the FIRST window
        // of a real mix change lifted the band to the excursion's own size, so the second window was
        // judged against a band that had already absorbed it and the change never cleared its band
        // (tests/flip_multikey_hold.py positive phase, reached 0.80x). A window that came back IN
        // band is quiet-state evidence and is safe to learn from; one that did not is the thing
        // under test. The estimate cannot get stuck too small, because the quantum below floors the
        // band at the estimator's own resolution without needing any observation at all.
        if (!fired) {
            signature_noise_.add(last_distance_);
            update_band();
        }
        previous_ = smoothed_;
        have_previous_ = true;
        return fired;
    }

    if (have_previous_) {
        // Quiet jitter is a band, not a trend estimate: retain the largest adjacent EWMA movement
        // observed at the anchor instead of letting one unusually quiet pair collapse the band.
        jitter_ = have_jitter_ ? std::max(jitter_, adjacent) : adjacent;
        have_jitter_ = true;
        signature_noise_.add(adjacent);
    }
    if (!anchored_ && learning_origin_.valid) {
        jitter_ = std::max(jitter_,
                           flip_signature_distance(smoothed_, learning_origin_));
        have_jitter_ = true;
    }
    previous_ = smoothed_;
    have_previous_ = true;
    update_band();
    return false;
}

void FlipShiftDetector::anchor() {
    if (!smoothed_.valid) return;
    anchored_signature_ = smoothed_;
    anchored_ = true;
    last_distance_ = 0;
    update_band();
}

bool FlipController::init(bool enabled, uint32_t nthreads) {
    enabled_ = enabled;
    signature_learning_windows_ = std::max<uint32_t>(1, nthreads);
    maneuver_learning_windows_ = 0;
    for (uint32_t value = nthreads; value > 1; value >>= 1)
        maneuver_learning_windows_++;
    maneuver_learning_windows_ = std::max<uint32_t>(1, maneuver_learning_windows_);
    // The signature noise estimate's time constant is the same window count the controller already
    // uses to learn a signature at an anchor -- derived from the live pool, not typed.
    shift_detector_ = FlipShiftDetector(-1, signature_learning_windows_);
    if (!enabled) {
        phase_ = Phase::Disabled;
        return true;
    }
    try {
        fingerprint_last_.resize(nthreads);
        maneuver_start_.resize(nthreads);
        maneuver_mark_.resize(nthreads);
        // At most one observation per split is needed for the seek itself, followed by up to one
        // thread-count-derived set of final anchor readings. Reserve both cold phases up front.
        readings_.reserve(static_cast<size_t>(nthreads) * 2);
    } catch (const std::bad_alloc&) {
        return false;
    }
    phase_ = Phase::BootPending;
    return true;
}

const char* FlipController::phase_name(Phase phase) {
    switch (phase) {
        case Phase::Disabled: return "disabled";
        case Phase::BootPending: return "boot-pending";
        case Phase::Measuring: return "measuring";
        case Phase::WaitingFlip: return "waiting-flip";
        case Phase::Seeking: return "seeking";
        case Phase::Settling: return "settling";
        case Phase::Anchored: return "anchored";
    }
    return "disabled";
}

const char* FlipController::reason_name(FlipctlTriggerReason reason) {
    switch (reason) {
        case FlipctlTriggerReason::None: return "none";
        case FlipctlTriggerReason::Boot: return "boot";
        case FlipctlTriggerReason::FingerprintShift: return "fingerprint-shift";
        case FlipctlTriggerReason::AnchorRateSurge: return "anchor-rate-surge";
        case FlipctlTriggerReason::AnchorRateCollapse: return "anchor-rate-collapse";
        case FlipctlTriggerReason::Forced: return "forced";
    }
    return "none";
}

uint64_t FlipController::total_commands(const Server& server) const {
    uint64_t total = 0;
    for (uint32_t tid = 0; tid < server.nthreads(); tid++)
        total += server.thread(tid).total_commands();
    return total;
}

FlipController::MovementStamp FlipController::movement_stamp(const Server& server) const {
    return MovementStamp{
        server.flip_completed(), server.flip_refused(), server.flip_clients_transferred(),
        server.lb_bucket_moves(), server.lb_client_moves()};
}

bool FlipController::sample_fingerprint(Server& server) {
    fingerprint_sampled_this_tick_ = false;
    if (!enabled_) return false;
    FlipFingerprintWindow aggregate;
    bool any = false;
    for (uint32_t tid = 0; tid < server.nthreads(); tid++) {
        const FlipFingerprintWindow current = server.thread(tid).flip_fingerprint().published();
        FlipFingerprintWindow& previous = fingerprint_last_[tid];
        if (current.closed_windows == previous.closed_windows) continue;
        add_delta(current.commands, previous.commands, aggregate.commands);
        add_delta(current.multikey_keys, previous.multikey_keys, aggregate.multikey_keys);
        add_delta(current.multikey_ops, previous.multikey_ops, aggregate.multikey_ops);
        add_delta(current.value_bytes, previous.value_bytes, aggregate.value_bytes);
        add_delta(current.closed_windows, previous.closed_windows, aggregate.closed_windows);
        for (size_t i = 0; i < aggregate.pass_depth.size(); i++)
            add_delta(current.pass_depth[i], previous.pass_depth[i], aggregate.pass_depth[i]);
        for (size_t i = 0; i < aggregate.command_class.size(); i++)
            add_delta(current.command_class[i], previous.command_class[i],
                      aggregate.command_class[i]);
        previous = current;
        any = true;
    }
    if (!any) return false;
    fingerprint_sampled_this_tick_ = true;
    if (phase_ == Phase::Settling) anchor_signature_samples_++;
    const bool shifted = shift_detector_.observe(aggregate);
    if (phase_ != Phase::Anchored && phase_ != Phase::Disabled &&
        phase_ != Phase::BootPending && !shift_detector_.anchored()) {
        maneuver_signature_samples_++;
        const uint32_t learning_windows = phase_ == Phase::Settling
            ? signature_learning_windows_ : maneuver_learning_windows_;
        if (maneuver_signature_samples_ >= learning_windows)
            shift_detector_.anchor();
    }
    return shifted;
}

double FlipController::automatic_rate_band(double pair_delta, double rate) const {
    const double quantum = rate > 0 ? 1.0 / rate : 0;
    return 2.0 * std::max(pair_delta, quantum);
}

bool FlipController::sample_rate(Server& server, uint64_t now_ms, double& rate) {
    const uint64_t commands = total_commands(server);
    const MovementStamp movement = movement_stamp(server);
    const bool quiescent = server.flip_stage() == FlipStage::Idle &&
                           server.lb_stage() == LbStage::Idle &&
                           !server.snapshot().in_progress() && !server.loading();
    if (!rate_window_ms_ || !quiescent || !(movement == rate_window_movement_)) {
        rate_window_ms_ = now_ms;
        rate_window_commands_ = commands;
        rate_window_movement_ = movement;
        previous_subwindow_valid_ = false;
        return false;
    }
    if (now_ms <= rate_window_ms_) return false;
    const uint64_t elapsed_ms = now_ms - rate_window_ms_;
    const uint64_t completed = commands - rate_window_commands_;
    rate = static_cast<double>(completed) * 1000.0 / static_cast<double>(elapsed_ms);
    rate_window_ms_ = now_ms;
    rate_window_commands_ = commands;
    rate_window_movement_ = movement;
    // The maneuver alone arms stamps. Derive their rate over the same three-tick decision
    // window; keep this computation behind the armed check, including its clock/window inputs.
    if (signal_sample_rate_.load(std::memory_order_relaxed))
        signal_sample_rate_.store(LbAutotune::sample_every(
            completed, elapsed_ms, server.flipctl_tick_ms() * kSettleTicks),
            std::memory_order_release);
    // Every rate sample, selected or not, feeds the long-window noise estimate; a tick at the
    // near-idle floor (one command per thread per tick, the boot gate's own idle line) is not load.
    {
        const uint64_t tick_ms = std::max<uint32_t>(1, server.flipctl_tick_ms());
        if (rate_ew_.alpha <= 0)
            rate_ew_.configure(static_cast<uint32_t>(std::max<uint64_t>(1, kBootMaxDeferralMs / tick_ms)));
        const double idle_rate = static_cast<double>(server.nthreads()) * 1000.0 /
                                 static_cast<double>(tick_ms);
        rate_ew_.add(rate, idle_rate);
    }
    return true;
}

bool FlipController::sample_stabilized_rate(Server& server, uint64_t now_ms, double& rate) {
    if (!sample_rate(server, now_ms, rate)) return false;
    if (!previous_subwindow_valid_) {
        previous_subwindow_rate_ = rate;
        previous_subwindow_valid_ = true;
        return false;
    }
    const double prior_rate = previous_subwindow_rate_;
    stable_pair_delta_ = relative_distance(rate, prior_rate);
    previous_subwindow_rate_ = rate;
    double band = anchor_rate_band_;
    if (band <= 0) band = automatic_rate_band(stable_pair_delta_, rate);
    // A pair is "stable" relative to how the load itself moves tick to tick, never to a band a
    // lucky settle window learned below that (0.02% measured: no pair ever fit and Measuring stuck).
    band = std::max(band, 2.0 * rate_ew_.sigma());
    if (stable_pair_delta_ > band) return false;
    // A stabilized reading represents the pair, not whichever of its two subwindows happened to
    // come last. This avoids anchoring on one edge of otherwise accepted quiet jitter.
    rate = (rate + prior_rate) * 0.5;
    return true;
}

bool FlipController::sample_anchored_rate(Server& server, uint64_t now_ms, double& rate) {
    if (!sample_rate(server, now_ms, rate)) return false;
    if (!previous_subwindow_valid_) {
        previous_subwindow_rate_ = rate;
        previous_subwindow_valid_ = true;
        return false;
    }
    const double prior_rate = previous_subwindow_rate_;
    previous_subwindow_rate_ = rate;
    stable_pair_delta_ = relative_distance(rate, prior_rate);
    rate = (rate + prior_rate) * 0.5;
    return true;
}

bool FlipController::boot_load_stable(Server& server, uint64_t now_ms) {
    const auto reset_learning = [this]() {
        boot_rate_ewma_ = 0;
        boot_rate_jitter_ = 0;
        boot_rate_slope_ = 0;
        boot_rate_slope_threshold_ = 0;
        boot_previous_ewma_change_ = 0;
        boot_rate_ewma_valid_ = false;
        boot_previous_ewma_change_valid_ = false;
        boot_rate_jitter_valid_ = false;
        boot_work_observed_ = false;
        boot_rate_history_.fill(0);
        boot_rate_samples_ = 0;
        boot_nonidle_ticks_ = 0;
        stationary_since_ms_ = 0;
    };
    double rate = 0;
    if (!sample_rate(server, now_ms, rate)) return false;
    const uint64_t tick_ms = std::max<uint32_t>(1, server.flipctl_tick_ms());
    // A health check or controller INFO poll is not a workload worth optimizing. One command per
    // provisioned thread per controller interval is the self-scaled near-idle floor.
    const double idle_rate = static_cast<double>(server.nthreads()) * 1000.0 /
                             static_cast<double>(tick_ms);
    if (rate <= idle_rate) {
        // Forget stale startup work so an idle interval cannot be joined to later traffic and
        // mistaken for either a flat trend or elapsed non-idle deferral.
        reset_learning();
        return false;
    }
    boot_nonidle_ticks_++;
    // The workload's stationarity clock: the cost gate credits a move only over the time the
    // workload has already held still, and at boot that is since the first non-idle tick.
    if (!stationary_since_ms_) stationary_since_ms_ = now_ms;
    // A command-rate sample made only of controller observability is not enough when work
    // fingerprinting is armed. Once real work closes a window, keep counting all non-idle
    // rate ticks: a workload below one fingerprint window per tick must still reach the cap.
    boot_work_observed_ = boot_work_observed_ || fingerprint_sampled_this_tick_;
    if (!boot_work_observed_) return false;
    const uint64_t max_deferral_ticks = std::max<uint64_t>(
        1, (kBootMaxDeferralMs + tick_ms - 1) / tick_ms);
    if (boot_nonidle_ticks_ >= max_deferral_ticks) return true;

    if (!boot_rate_ewma_valid_) {
        boot_rate_ewma_ = rate;
        boot_rate_ewma_valid_ = true;
        boot_rate_history_[0] = rate;
        boot_rate_samples_ = 1;
        return false;
    }

    const double previous_ewma = boot_rate_ewma_;
    boot_rate_ewma_ = (boot_rate_ewma_ + rate) * 0.5;
    const double scale = std::max(std::abs(previous_ewma), std::abs(boot_rate_ewma_));
    const double ewma_change = scale > 0
        ? (boot_rate_ewma_ - previous_ewma) / scale : 0;
    if (!boot_previous_ewma_change_valid_) {
        boot_previous_ewma_change_ = ewma_change;
        boot_previous_ewma_change_valid_ = true;
        return false;
    }

    // Jitter is variation in the EWMA's adjacent movement. A monotone connection ramp therefore
    // remains directional drift instead of widening its own stability band and blessing itself.
    const double jitter_sample = std::abs(ewma_change - boot_previous_ewma_change_);
    boot_rate_jitter_ = boot_rate_jitter_valid_
        ? (boot_rate_jitter_ + jitter_sample) * 0.5 : jitter_sample;
    boot_rate_jitter_valid_ = true;
    boot_previous_ewma_change_ = ewma_change;

    const uint32_t history_slot = boot_rate_samples_ % kBootTrendTicks;
    const bool have_trend = boot_rate_samples_ >= kBootTrendTicks;
    const double trend_origin = boot_rate_history_[history_slot];
    boot_rate_history_[history_slot] = boot_rate_ewma_;
    boot_rate_samples_++;
    if (!have_trend) return false;

    const double trend_scale = std::max(std::abs(trend_origin), std::abs(boot_rate_ewma_));
    boot_rate_slope_ = trend_scale > 0
        ? std::abs(boot_rate_ewma_ - trend_origin) /
              (trend_scale * static_cast<double>(kBootTrendTicks))
        : 0;
    // Adjacent EWMA-change jitter estimates the noise in one slope observation. Its standard
    // error over the N-tick trend supplies a workload-derived threshold; one command per tick is
    // retained only as a measurement-quantum floor. A monotone ramp has slope but little change
    // jitter, while stationary rate noise cancels across the longer trend.
    const double command_rate_quantum = 1000.0 / static_cast<double>(tick_ms);
    const double relative_quantum = command_rate_quantum / boot_rate_ewma_;
    boot_rate_slope_threshold_ =
        2.0 * std::max(boot_rate_jitter_, relative_quantum) /
        std::sqrt(static_cast<double>(kBootTrendTicks));
    return boot_rate_slope_ <= boot_rate_slope_threshold_;
}

void FlipController::start_maneuver(Server& server, FlipctlTriggerReason reason,
                                    uint64_t now_ms) {
    last_trigger_ = reason;
    triggers_++;
    if (reason == FlipctlTriggerReason::Boot) boot_triggers_++;
    else if (reason == FlipctlTriggerReason::FingerprintShift) fingerprint_triggers_++;
    else if (reason == FlipctlTriggerReason::AnchorRateSurge) rate_surge_triggers_++;
    else if (reason == FlipctlTriggerReason::AnchorRateCollapse) rate_collapse_triggers_++;
    else if (reason == FlipctlTriggerReason::Forced) forced_triggers_++;

    phase_ = Phase::Measuring;
    // Where this maneuver started. If the search comes back here, the trigger that started it
    // demanded a move and the measurements refused one: that excursion carries no placement
    // information and anchor() turns it into a band floor.
    maneuver_origin_io_ = server.role_count(Role::Ifid);
    shift_streak_ = 0;
    readings_.clear();
    demand_window_.reset();
    model_readings_ = 0;
    // Readings, not ticks: each stabilized reading spans at least two ticks, and the deferral
    // bound is the one wall-clock allowance this controller already grants a boot.
    {
        const uint64_t tick_ms = std::max<uint32_t>(1, server.flipctl_tick_ms());
        model_readings_cap_ = static_cast<uint32_t>(std::max<uint64_t>(
            kMinModelSamples, (kBootMaxDeferralMs / tick_ms) / 2));
    }
    model_gain_mean_ = model_gain_low_ = model_gain_high_ = 0;
    model_required_gain_ = 0;
    model_target_io_ = 0;
    model_last_decision_ = "measuring";
    maneuver_flips_ = 0;
    maneuver_rate_jitter_ = 0;
    seek_ = Seek::None;
    seek_target_io_ = 0;
    seek_origin_io_ = maneuver_origin_io_;
    refining_ = false;
    refine_decision_ = "none";
    refine_steps_ = 0;
    refine_bar_ = 0;
    origin_rate_ = 0;
    origin_window_.reset();
    target_window_.reset();
    model_cost_ = FlipCostVerdict{};
    model_verify_readings_ = 0;
    model_kappa_ = cost_.kappa();
    hyp_predicted_ = hyp_sigma_ = hyp_delivered_ = hyp_threshold_ = 0;
    hyp_origin_samples_ = 0;
    pending_miss_ = false;
    flip_measure_armed_ = false;
    // A change trigger means the workload just changed: its stationarity starts now. A forced
    // trigger (DEBUG) re-examines an unchanged workload and keeps the clock; boot set it at the
    // first non-idle tick.
    if (reason == FlipctlTriggerReason::FingerprintShift ||
        reason == FlipctlTriggerReason::AnchorRateSurge ||
        reason == FlipctlTriggerReason::AnchorRateCollapse || !stationary_since_ms_)
        stationary_since_ms_ = now_ms;
    maneuver_mark_ms_ = now_ms;
    surge_streak_ = 0;
    collapse_streak_ = 0;
    shift_detector_.reset();
    anchor_signature_samples_ = 0;
    anchor_sampling_disabled_ = false;
    maneuver_signature_samples_ = 0;
    anchor_learning_rate_jitter_ = 0;
    anchor_learning_rate_sum_ = 0;
    anchor_learning_rate_min_ = 0;
    anchor_learning_rate_max_ = 0;
    anchor_learning_rate_samples_ = 0;
    retrigger_after_flip_ = false;
    pending_retrigger_ = FlipctlTriggerReason::None;
    // Derive the initial maneuver rate from the last traffic observation, then adapt it on
    // each completed rate window. Idle and anchored states leave stamping off entirely.
    signal_sample_rate_.store(
        LbAutotune::sample_every(rate_ew_.prev1 > 0 ? rate_ew_.prev1 : boot_rate_ewma_,
                                1000, server.flipctl_tick_ms() * kSettleTicks),
        std::memory_order_release);
    for (uint32_t tid = 0; tid < server.nthreads(); tid++) {
        const LoopSignals& signal = server.thread(tid).sig();
        maneuver_start_[tid] = ThreadMeasure{signal.ops, signal.busy_ns, signal.idle_ns};
        maneuver_mark_[tid] = maneuver_start_[tid];
    }
    rate_window_ms_ = now_ms;
    rate_window_commands_ = total_commands(server);
    rate_window_movement_ = movement_stamp(server);
    previous_subwindow_valid_ = false;
}

void FlipController::record(uint32_t split, double rate) {
    // Preserve every stabilized observation: settle() takes its argmax over the complete evidence
    // and DEBUG FLIPCTL dumps the trail.
    readings_.push_back(Reading{split, rate});
}

// Every path that stops seeking lands here: leave the split alone, forget the maneuver's learning
// state, and let Settling cut a fresh anchor from wherever the server is now.
void FlipController::enter_settling() {
    phase_ = Phase::Settling;
    shift_detector_.reset();
    anchor_signature_samples_ = 0;
    maneuver_signature_samples_ = 0;
    anchor_learning_rate_jitter_ = 0;
    anchor_learning_rate_sum_ = 0;
    anchor_learning_rate_min_ = 0;
    anchor_learning_rate_max_ = 0;
    anchor_learning_rate_samples_ = 0;
    rate_window_ms_ = 0;
    previous_subwindow_valid_ = false;
}

bool FlipController::issue_flip(Server& server, uint32_t coordinator, uint32_t target_io,
                                Phase after_flip, uint64_t now_ms, double rate_before) {
    // A flip onto the live split is a no-op that flip_begin nevertheless counts as completed, and
    // issue_flip would then report failure because the stage never left Idle -- an ownership
    // transaction in the counters that the controller does not believe it made. The seek arithmetic
    // does not produce one today (every wall case reverses direction first), so this is a guard,
    // not the cause of the multi-key thrash; but the controller must be structurally incapable of
    // spending a FLIP on the split it is already running.
    if (target_io == server.role_count(Role::Ifid)) return false;
    std::string error;
    pending_completed_ = server.flip_completed();
    pending_refused_ = server.flip_refused();
    pending_target_io_ = target_io;
    after_flip_ = after_flip;
    // The flip's COST is measured from here: commands the server would have served at the rate it
    // ran at before, minus the commands it did serve until the stage returns to Idle, per
    // connection the planner moved. The naive transfer count is what the role change alone
    // requires; the planner's re-plan moves more, and the ratio is learned.
    flip_issue_ms_ = now_ms;
    flip_issue_commands_ = total_commands(server);
    flip_issue_transfers_ = server.flip_clients_transferred();
    flip_issue_rate_ = rate_before;
    flip_issue_naive_ = flip_naive_transfers(io_clients(server), server.role_count(Role::Ifid),
                                             target_io);
    if (!server.flip_begin(target_io, server.nthreads() - target_io, coordinator, error))
        return false;
    phase_ = Phase::WaitingFlip;
    const bool issued = server.flip_stage() != FlipStage::Idle;
    if (issued) maneuver_flips_++;
    flip_measure_armed_ = issued;
    return issued;
}

uint32_t FlipController::io_clients(const Server& server) const {
    uint32_t clients = 0;
    for (uint32_t tid = 0; tid < server.nthreads(); tid++)
        if (server.thread(tid).role() == Role::Ifid) clients += server.thread(tid).client_count();
    return clients;
}

// Books the flip that just returned to Idle into the cost model. Called at the first look that
// sees Idle, which wait_ms() makes a fraction of a tick after completion, so the served count is
// contaminated by at most that fraction of post-flip traffic at the new rate.
void FlipController::measure_flip(Server& server, uint64_t now_ms) {
    if (!flip_measure_armed_) return;
    flip_measure_armed_ = false;
    // A refused flip moved nothing and cost nothing worth learning from.
    if (server.flip_completed() <= pending_completed_) return;
    const uint64_t tick_ms = std::max<uint32_t>(1, server.flipctl_tick_ms());
    const double elapsed_s = now_ms > flip_issue_ms_
        ? static_cast<double>(now_ms - flip_issue_ms_) / 1000.0 : 0;
    const uint64_t served = total_commands(server) - flip_issue_commands_;
    const double lost = flip_issue_rate_ > 0
        ? std::max(0.0, flip_issue_rate_ * elapsed_s - static_cast<double>(served)) : 0;
    const uint64_t moved = server.flip_clients_transferred() - flip_issue_transfers_;
    const double ticks = std::max(1.0, elapsed_s * 1000.0 / static_cast<double>(tick_ms));
    last_flip_lost_ = lost;
    last_flip_moved_ = moved;
    rate_ew_.reset();
    // A flip that moved no connection (an empty server) prices nothing per client; it still
    // counts toward the flip duration the blackout term uses.
    cost_.record_flip(lost, static_cast<double>(moved), flip_issue_naive_, ticks);
}

// One windowed observation of how the two roles split the server's real work, plus each role's
// HEADROOM: the idle share of its busiest thread. Returns false while the window carries no
// evidence; the caller keeps measuring rather than inventing a prior.
//
// WORK IS WALL MINUS IDLE. The loops' busy_ns is not the whole of their work: the io loop closes
// its busy span before ring_.submit_and_reap(), so the io_uring_enter syscall -- the kernel
// moving the bytes, which is io work -- was booked as neither busy nor idle. Measured on the
// guard's own snapshots (single-key 1:1, 40 s): one io thread booked 16.4 s busy + 0.2 s idle of
// 39.9 s on-CPU, 23.5 s unbooked; the busy share read io = 0.32 on a workload whose work share is
// 0.51, and the model moved 2:2 -> 1:3 (measured -52%) and came back. idle_ns is the quantity both
// loops book faithfully: the io loop's blocked wait after an empty sweep, the ex loop's empty
// passes and blocked wait (ex_loop.h). The wall is the controller's own tick clock, the same for
// every thread of the window, so nothing new is read on the hot path.
bool FlipController::sample_role_demand(Server& server, uint64_t now_ms, double& io_frac,
                                        double& io_headroom, double& ex_headroom) {
    const double wall = now_ms > maneuver_mark_ms_
        ? static_cast<double>(now_ms - maneuver_mark_ms_) * 1e6 : 0;
    double role_ops[2] = {};
    double role_work[2] = {};
    double headroom[2] = {1.0, 1.0};
    for (uint32_t tid = 0; tid < server.nthreads(); tid++) {
        const LoopSignals& signal = server.thread(tid).sig();
        const ThreadMeasure& start = maneuver_mark_[tid];
        const uint64_t ops = signal.ops - start.ops;
        const double idle = static_cast<double>(signal.idle_ns - start.idle_ns);
        const Role role = server.thread(tid).role();
        if (role != Role::Ifid && role != Role::Ex) continue;
        const size_t index = role == Role::Ifid ? 0 : 1;
        role_ops[index] += static_cast<double>(ops);
        role_work[index] += flip_role_work(wall, idle);
        if (wall > 0)
            headroom[index] = std::min(headroom[index], std::clamp(idle / wall, 0.0, 1.0));
    }
    for (uint32_t tid = 0; tid < server.nthreads(); tid++) {
        const LoopSignals& signal = server.thread(tid).sig();
        maneuver_mark_[tid] = ThreadMeasure{signal.ops, signal.busy_ns, signal.idle_ns};
    }
    maneuver_mark_ms_ = now_ms;
    // Capacity is unidentifiable without observed work on both sides. Keep measuring instead of
    // inventing a workload prior. The constraint guard deliberately examines each role's busiest
    // thread; idle peers therefore cannot veto a move demanded by the loaded owner.
    if (wall <= 0 || role_ops[0] <= 0 || role_ops[1] <= 0 ||
        role_work[0] <= 0 || role_work[1] <= 0) return false;

    // WORK CONSERVATION, WITH BOTH OPERANDS THE SAME QUANTITY. The throughput-optimal split gives
    // each role the thread fraction its PER-COMMAND service cost demands,
    //     n_io* / N = c_io / (c_io + c_ex),   c_role = (work ns that role spent) / (commands run),
    // and the command count is the same divisor on both sides, so it cancels: the estimator is the
    // work-time share, and it never has to name what an "op" is. (It used to divide each role's
    // busy time by that role's OWN op counter; the ex loop counts one op per SHARD TASK, 7.576 per
    // MGET8 command, so ex looked cheap and the model wanted io = 82% -- the 5:3 -> 7:1 -> 5:3
    // round trip the operator saw as three flips and 993 moved connections.)
    if (!std::isfinite(role_work[0]) || !std::isfinite(role_work[1])) return false;
    io_frac = role_work[0] / (role_work[0] + role_work[1]);
    io_headroom = headroom[0];
    ex_headroom = headroom[1];
    return true;
}

// The spread the ORIGIN split's own stabilized readings showed while the model was deciding: the
// baseline's movement between the two windows the outcome loop compares. It is a floor under every
// band this maneuver uses, the detector's explicit-band unit-test control included: it says how
// small a gain is worth chasing, not how still the workload is holding.
double FlipController::baseline_band() const {
    return origin_window_.samples >= 2 ? origin_window_.bracket_band() : 0;
}

// The throughput noise a projected gain has to beat before a flip could be VERIFIED: the band the
// last anchor learned, if any, or the maneuver's own stabilized-pair jitter, and never below what
// the baseline itself moved.
double FlipController::verification_band(double rate) const {
    const double floor = std::max(baseline_band(), 2.0 * rate_ew_.sigma());
    return std::max({anchor_rate_band_, automatic_rate_band(maneuver_rate_jitter_, rate), floor});
}

// The Measuring phase (flip_policy.h): one demand draw per stabilized reading into the policy
// window, then decide or keep measuring. The server serves at its live split throughout, so time
// spent here costs nothing but the maneuver's own sampling arm -- which is why every "not yet" below
// is a return to sampling, not a hold: the interval tightens, the origin's noise estimate firms up,
// and the stationarity horizon the cost gate credits grows, all for free.
bool FlipController::decide_placement(Server& server, uint32_t coordinator, uint64_t now_ms,
                                      double rate) {
    maneuver_rate_jitter_ = std::max(maneuver_rate_jitter_, stable_pair_delta_);
    double io_frac = 0, io_headroom = 0, ex_headroom = 0;
    if (!sample_role_demand(server, now_ms, io_frac, io_headroom, ex_headroom)) return false;
    demand_window_.add(io_frac);
    // Measuring never moves the split, so every reading here is a reading of the ORIGIN.
    if (rate > 0) origin_window_.add(rate);
    model_readings_++;
    model_io_frac_ = demand_window_.mean;
    model_io_headroom_ = io_headroom;
    model_ex_headroom_ = ex_headroom;

    const uint32_t unit = server.smt_units_enabled() ? 2u : 1u;
    const uint32_t total_units = server.nthreads() / unit;
    const uint32_t now_units = server.role_count(Role::Ifid) / unit;
    const double band = verification_band(rate);
    model_required_gain_ = std::min(1.0, band * static_cast<double>(model_margin_));
    // CALIBRATION. The chooser works in the model's own units; a projection the outcome record
    // says is worth kappa of itself has to clear a bar 1/kappa as high.
    model_kappa_ = cost_.kappa();
    const double bar_model_units = model_kappa_ > 0
        ? std::min(1.0, model_required_gain_ / model_kappa_) : 1.0;
    FlipPlacementChoice choice = flip_choose_split(
        now_units, total_units, demand_window_, bar_model_units, kMinModelSamples);
    // A DEBUG FLIPCTL SEEK request replaces the model's target with the operator's: judged by the
    // model's projection for THAT split under the same bars, or, forced, by the outcome loop alone.
    const bool debug_seek = debug_seek_io_ != 0 && debug_seek_io_ / unit != now_units &&
                            debug_seek_io_ / unit < total_units;
    const bool debug_force = debug_seek && debug_seek_force_;
    if (debug_seek && demand_window_.samples >= kMinModelSamples) {
        const uint32_t s = debug_seek_io_ / unit;
        choice.target_units = s;
        choice.gain_mean = flip_projected_gain(s, now_units, total_units, demand_window_.mean);
        choice.gain_low = std::min({choice.gain_mean,
            flip_projected_gain(s, now_units, total_units, choice.io_frac_low),
            flip_projected_gain(s, now_units, total_units, choice.io_frac_high)});
        choice.decided = true;
        choice.move = debug_force || choice.gain_low > bar_model_units;
    }
    model_io_frac_low_ = choice.io_frac_low;
    model_io_frac_high_ = choice.io_frac_high;
    model_gain_mean_ = choice.gain_mean;
    model_gain_low_ = choice.gain_low;
    model_gain_high_ = choice.gain_high;
    model_target_io_ = choice.target_units * unit;

    // SATURATION GATE. The capacity model above is a statement about CPU-bound roles: throughput
    // ~ min(io capacity, ex capacity) holds only while one role's busiest thread has no headroom.
    // When BOTH roles idle more than the noise band, the rate is set by something the split does
    // not touch -- a paced driver, the network, or the closed loop's own wake-up latency (measured
    // here: memtier at 27% of its cores, io 86% / ex 93% busy, and neither side waiting on the
    // other's capacity) -- and a flip can only cost. Read from the idle_ns the loops already
    // account, once per reading, off the hot path. A forced seek is exempt: it is a directed test.
    const bool externally_bound = !debug_force && io_headroom > band && ex_headroom > band;
    const bool capped = model_readings_ >= model_readings_cap_;

    // REFINEMENT stops at a split this maneuver already stood on: the landing is then the best
    // delivered split, and a chain that wants to go back is oscillation, not learning.
    bool visited = false;
    if (refining_ && choice.decided && choice.move && choice.target_units != now_units)
        for (const Reading& r : readings_)
            if (r.split == choice.target_units * unit) { visited = true; break; }
    // ... and stops when the projected gain is inside the model's own demonstrated error.
    bool within_error = false;
    if (refining_ && choice.decided && choice.move && choice.target_units != now_units && !visited)
        within_error = model_kappa_ * choice.gain_low <= refine_bar_;
    if (visited || within_error) { choice.move = false; }
    // COST GATE + VERIFICATION WINDOW (flip_policy.h). Only a target that already clears the noise
    // bar is priced. The window at the target is sized from the origin's own noise; the gate
    // credits the kappa-scaled gain over the stationarity the workload has demonstrated, less the
    // blind time, against the measured transfer cost and the blackout at a wrong split.
    bool pays = false;
    if (choice.decided && choice.move && choice.target_units != now_units) {
        const double g_low = model_kappa_ * choice.gain_low;
        const double g_mean = model_kappa_ * choice.gain_mean;
        const uint64_t tick_ms = std::max<uint32_t>(1, server.flipctl_tick_ms());
        const double tick_s = static_cast<double>(tick_ms) / 1000.0;
        model_verify_readings_ = flip_verify_window(
            std::max(origin_window_.sigma(), rate_ew_.sigma()), origin_window_.samples,
            std::max(g_mean, 0.0), model_readings_cap_);
        const double blackout_s = (cost_.flip_ticks() + kSettleTicks +
            (model_verify_readings_ ? model_verify_readings_ - 1 : 0)) * tick_s;
        const double stationary_s = (now_ms > stationary_since_ms_ && stationary_since_ms_)
            ? static_cast<double>(now_ms - stationary_since_ms_) / 1000.0 : 0;
        const double transfers = cost_.predicted_transfers(
            io_clients(server), now_units * unit, choice.target_units * unit);
        model_cost_ = flip_cost_gate(g_low, g_mean, rate, stationary_s, blackout_s,
                                     cost_.client_cost() * transfers, cost_.miss_probability(),
                                     model_margin_);
        pays = debug_force || (model_verify_readings_ > 0 && model_cost_.pays);
        if (!pays && !capped && !externally_bound) {
            // Not yet: the horizon grows and the origin's noise estimate tightens while sampling.
            const char* v = model_verify_readings_ ? "sampling-cost" : "sampling-unverifiable";
            if (refining_) refine_decision_ = v; else model_last_decision_ = v;
            return false;
        }
    }
    // A BAR LARGER THAN ANY AVAILABLE GAIN IS A MEASUREMENT VERDICT, NOT A PLACEMENT ONE. When the
    // model has picked a target and the only thing refusing it is the throughput bar, anchoring here
    // records "this split is fine" on the strength of noise. Measured: a 3:1 boot whose executors
    // were pinned (headroom_ex 0.0004, headroom_io 0.699) and whose demand interval was tight
    // (0.395-0.424) projected the +100% move that 3:1 -> 2:2 is, and was refused because a
    // neighbouring lane swung the box 2.8x per second, lifting the verification band past 1.0 --
    // where min(1.0, band x margin) caps it exactly on the gain. Sampling costs nothing and the box
    // may quieten, so keep measuring until the reading cap and let the cap deliver the honest
    // "too noisy to decide" hold. The live split is the optimum is a different verdict and still
    // decides immediately.
    const bool bar_bound = choice.decided && !choice.move && !visited &&
                           choice.target_units != now_units;
    if (bar_bound && !capped && !externally_bound) {
        model_last_decision_ = "sampling-bar";
        return false;
    }
    if (!choice.decided && !capped && !externally_bound) return false;  // keep measuring

    record(now_units * unit, rate);
    if (externally_bound || !choice.decided || !choice.move || !pays) {
        const char* verdict = externally_bound ? "hold-unsaturated"
            : !choice.decided ? "hold-unresolved"
            : choice.target_units == now_units ? "hold-optimum"
            : visited ? "hold-visited"
            : within_error ? "hold-within-error"
            : !choice.move ? "hold-below-bar"
            : !model_verify_readings_ ? "hold-unverifiable" : "hold-cost";
        if (refining_) {
            // The landing stands: the maneuver's outcome stays "moved-delivered" and the LOCAL
            // verdict is kept separately, so a trail can say whether the landing was its own argmax
            // or a further step that did not pay (hold-cost) -- the difference between a signal
            // question and a gate question.
            refine_decision_ = verdict;
        } else {
            model_last_decision_ = verdict;
            model_holds_++;
        }
        if (choice.decided && choice.move && choice.target_units != now_units && !externally_bound)
            cost_holds_++;
        debug_seek_io_ = 0;
        debug_seek_force_ = false;
        enter_settling();
        return false;
    }
    if (refining_) { refine_decision_ = "move"; refine_steps_++; }
    // MOVE: the hypothesis. R0 is the origin window's mean; the prediction is kappa-scaled; the
    // target window is judged against the origin's noise and sample count from here on.
    model_last_decision_ = debug_force ? "move-forced" : "move";
    origin_rate_ = origin_window_.mean > 0 ? origin_window_.mean : rate;
    hyp_predicted_ = model_kappa_ * choice.gain_mean;
    // The origin's noise for the verification: the window's own sigma or the long-window sigma,
    // whichever is larger -- two quiet readings inside one phase of a periodic load say nothing
    // about the swing the target reading will be compared across.
    hyp_sigma_ = std::max(origin_window_.sigma(), rate_ew_.sigma());
    hyp_origin_samples_ = origin_window_.samples;
    hyp_delivered_ = 0;
    hyp_threshold_ = 0;
    target_window_.reset();
    seek_target_io_ = model_target_io_;
    seek_origin_io_ = now_units * unit;
    seek_ = Seek::AtTarget;
    debug_seek_io_ = 0;
    debug_seek_force_ = false;
    if (!issue_flip(server, coordinator, seek_target_io_, Phase::Seeking, now_ms, origin_rate_)) {
        seek_ = Seek::None;
        enter_settling();
        return false;
    }
    return true;
}

// After a delivered move: measure the landing as a fresh origin and let the model say whether the
// landing is its own argmax. The workload's stationarity clock is untouched (nothing changed but
// the split); the demand and origin windows start over at the new split; a later miss returns to
// this landing, not to where the maneuver began.
void FlipController::begin_refinement(Server& server, uint64_t now_ms) {
    refining_ = true;
    refine_decision_ = "measuring";
    // The model's error on the step that just landed is the bar its next projection must clear.
    refine_bar_ = flip_refine_bar(hyp_predicted_, hyp_delivered_);
    phase_ = Phase::Measuring;
    demand_window_.reset();
    origin_window_.reset();
    model_readings_ = 0;
    model_cost_ = FlipCostVerdict{};
    model_verify_readings_ = 0;
    hyp_predicted_ = hyp_sigma_ = hyp_delivered_ = hyp_threshold_ = 0;
    hyp_origin_samples_ = 0;
    pending_miss_ = false;
    for (uint32_t tid = 0; tid < server.nthreads(); tid++) {
        const LoopSignals& signal = server.thread(tid).sig();
        maneuver_mark_[tid] = ThreadMeasure{signal.ops, signal.busy_ns, signal.idle_ns};
    }
    maneuver_mark_ms_ = now_ms;
    rate_window_ms_ = 0;
    previous_subwindow_valid_ = false;
}

bool FlipController::settle(Server& server, uint32_t coordinator, uint64_t now_ms) {
    if (readings_.empty()) {
        enter_settling();
        return false;
    }
    const Reading* best = &readings_.front();
    for (const Reading& reading : readings_)
        if (reading.rate > best->rate) best = &reading;
    const uint32_t current = server.role_count(Role::Ifid);
    if (current != best->split) {
        if (issue_flip(server, coordinator, best->split, Phase::Settling, now_ms,
                       readings_.back().rate)) return true;
    }
    enter_settling();
    return false;
}

// The Seeking phase: stabilized readings at the model's target, judged SEQUENTIALLY against the
// origin's window. The hypothesis planned n_t readings from the origin's noise; after each one the
// target's mean is compared at two standard errors of the difference (never below the origin's
// own bracket or the learned band). Better => anchor here, early if the evidence is already in.
// Clearly worse => straight back at once: no sitting at a split the model already lost on. At the
// planned count, not better => back to the origin, where anchor() books the miss and raises the
// bar. No walk, no overshoot: every extra visit is a flip plus a window at a split the model rated
// lower, and that walk is what cost 19.5% on multi-key with the target unchanged.
bool FlipController::seek_after_reading(Server& server, uint32_t coordinator, uint64_t now_ms,
                                        double rate) {
    maneuver_rate_jitter_ = std::max(maneuver_rate_jitter_, stable_pair_delta_);
    const uint32_t current = server.role_count(Role::Ifid);
    record(current, rate);
    if (seek_ == Seek::AtTarget && current == seek_target_io_) {
        target_window_.add(rate);
        const uint32_t k = target_window_.samples;
        hyp_threshold_ = flip_verify_threshold(hyp_sigma_, hyp_origin_samples_, k,
                                               verification_band(rate));
        hyp_delivered_ = origin_rate_ > 0 ? target_window_.mean / origin_rate_ - 1.0 : 0;
        const FlipOutcome verdict = flip_judge(hyp_delivered_, hyp_threshold_, k,
                                               model_verify_readings_);
        if (verdict == FlipOutcome::Pending) {
            model_last_decision_ = "verifying";
            return false;
        }
        if (verdict == FlipOutcome::Hit) {
            model_last_decision_ = "moved-delivered";
            cost_.record_outcome(hyp_predicted_, hyp_delivered_, true);
            seek_ = Seek::None;
            begin_refinement(server, now_ms);
            return false;
        }
        model_last_decision_ = "moved-reverted";
        pending_miss_ = true;
        seek_ = Seek::Returning;
        if (issue_flip(server, coordinator, seek_origin_io_, Phase::Settling, now_ms,
                       target_window_.mean)) return true;
    }
    // A refused return, or a reading at a split this seek did not ask for: stop on the best
    // evidence in hand.
    seek_ = Seek::None;
    return settle(server, coordinator, now_ms);
}

void FlipController::anchor(Server& server, double rate) {
    const uint32_t current = server.role_count(Role::Ifid);
    anchor_io_ = current;
    anchor_ex_ = server.nthreads() - current;
    anchor_rate_ = anchor_learning_rate_samples_
        ? anchor_learning_rate_sum_ / anchor_learning_rate_samples_ : rate;
    if (anchor_learning_rate_samples_ > 1) {
        anchor_learning_rate_jitter_ = std::max(
            anchor_learning_rate_jitter_,
            relative_distance(anchor_learning_rate_min_, anchor_learning_rate_max_));
    }
    anchor_rate_jitter_ = anchor_learning_rate_jitter_;
    // THE NULL-MANEUVER RULE, CLOSED ON THE OUTCOME. A maneuver that ends on the split it started
    // from moved ownership, paid every quiesce and changed nothing -- thrash by this project's own
    // definition. The trigger that started it was therefore not wrong about the workload, it was
    // wrong that the workload's move was worth a maneuver, so what has to grow is the EVIDENCE the
    // next such excursion must present, not the size it must reach. Double the confirming control
    // passes the responsible detector needs; halve back toward the floor whenever a maneuver
    // actually moves the split, so a workload that really is drifting stays cheap to follow.
    //
    // The rejected alternative was widening that detector's band by twice the excursion it just
    // proved uninformative. Measured: a paced 8-key -> single-key change scores distance 0.630 on
    // a metric whose maximum is 1.0, so one null result set a floor of 1.26 and the fingerprint
    // detector could never fire again at any workload -- an accidentally disabled trigger.
    // A threshold learned from an excursion can exceed every excursion; a window cannot.
    const bool null_maneuver = current == maneuver_origin_io_ &&
        (last_trigger_ == FlipctlTriggerReason::FingerprintShift ||
         last_trigger_ == FlipctlTriggerReason::AnchorRateSurge ||
         last_trigger_ == FlipctlTriggerReason::AnchorRateCollapse);
    const bool moved_split = current != maneuver_origin_io_;
    const uint32_t confirmation_cap =
        std::max(kMinConfirmations, signature_learning_windows_);
    uint32_t* responsible = last_trigger_ == FlipctlTriggerReason::FingerprintShift
        ? &shift_confirmations_
        : (last_trigger_ == FlipctlTriggerReason::AnchorRateSurge ||
           last_trigger_ == FlipctlTriggerReason::AnchorRateCollapse)
              ? &rate_confirmations_ : nullptr;
    if (null_maneuver) null_maneuvers_++;
    if (responsible) {
        if (null_maneuver)
            *responsible = std::min(confirmation_cap, *responsible * 2);
        else if (moved_split)
            *responsible = std::max(kMinConfirmations, *responsible / 2);
    }
    anchor_rate_band_ = std::max(automatic_rate_band(anchor_rate_jitter_, anchor_rate_),
                                 2.0 * rate_ew_.sigma());
    anchor_rate_band_floor_ = anchor_rate_band_;
    // The placement model's own outcome loop (flip_policy.h, rule 3): a maneuver that FLIPPED and
    // still ended where it began is a projection that did not deliver, so the bar rises; a move
    // that stuck lowers it again. A hold (no flip) is neither.
    // A ROUND TRIP is a hypothesis that missed and returned to the split it departed from -- not
    // "ended where the maneuver began": a refinement's second step that misses returns to the
    // first step's landing, which is a miss all the same, and a delivered first step followed by a
    // local hold is a move, not a trip.
    const bool round_trip = pending_miss_ && current == seek_origin_io_ && maneuver_flips_ > 0;
    if (round_trip) {
        // Was the test valid? Re-judge the hypothesis against the origin as it reads AFTER the
        // return: if the target's delivered gain, corrected for how far the baseline itself moved
        // during the excursion, would have cleared the threshold, the baseline moved out from
        // under the test -- voided, not refuted -- and the model's record must not carry a miss it
        // did not earn. A baseline wobble smaller than the miss changes nothing: measured here, a
        // closed-loop driver came back 2.2% faster after a round trip whose target had delivered
        // -48%, and that miss stands.
        bool baseline_moved = false;
        if (pending_miss_ && origin_rate_ > 0 && anchor_rate_ > 0)
            baseline_moved = flip_corrected_gain(hyp_delivered_, anchor_rate_ / origin_rate_ - 1.0)
                             > hyp_threshold_;
        if (baseline_moved) {
            invalidated_maneuvers_++;
            model_last_decision_ = "moved-reverted-invalidated";
        } else {
            round_trips_++;
            if (pending_miss_) cost_.record_outcome(hyp_predicted_, hyp_delivered_, false);
            if (anchor_rate_band_ * static_cast<double>(model_margin_) < 1.0) model_margin_ *= 2;
        }
    } else if (moved_split && !pending_miss_) {
        model_margin_ = std::max<uint32_t>(1, model_margin_ / 2);
    }
    pending_miss_ = false;
    refining_ = false;
    shift_streak_ = 0;
    shift_detector_.anchor();
    signal_sample_rate_.store(0, std::memory_order_release);
    surge_streak_ = 0;
    collapse_streak_ = 0;
    phase_ = Phase::Anchored;
    rate_window_ms_ = 0;
    previous_subwindow_valid_ = false;
}

bool FlipController::tick(Server& server, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return false;

    const bool shifted = sample_fingerprint(server);
    const bool forced = force_requested_.exchange(false, std::memory_order_acq_rel);
    if (phase_ == Phase::BootPending) {
        if (forced) start_maneuver(server, FlipctlTriggerReason::Forced, now_ms);
        else if (boot_load_stable(server, now_ms))
            start_maneuver(server, FlipctlTriggerReason::Boot, now_ms);
        return false;
    }
    if (phase_ == Phase::Anchored && forced) {
        start_maneuver(server, FlipctlTriggerReason::Forced, now_ms);
        return false;
    }
    // Mid-maneuver, ONLY a forced trigger may interrupt. The maneuver's own split changes move
    // the pass-depth signature, so honoring `shifted` here lets the actuator re-trigger itself
    // forever (the sweep-abandon law: a signal the actuator moves cannot police it). The
    // detector re-baselines at anchor; a real workload change re-fires from the anchored state.
    if (phase_ != Phase::Anchored && phase_ != Phase::BootPending && forced) {
        const FlipctlTriggerReason reason = FlipctlTriggerReason::Forced;
        if (phase_ == Phase::WaitingFlip && server.flip_stage() != FlipStage::Idle) {
            // The committed ownership transaction must finish, but none of its throughput samples
            // survive. Restart measurement immediately after Idle instead of overlapping a flip.
            retrigger_after_flip_ = true;
            pending_retrigger_ = reason;
            return false;
        }
        start_maneuver(server, reason, now_ms);
        return false;
    }

    uint32_t coordinator = UINT32_MAX;
    for (uint32_t tid = 0; tid < server.nthreads(); tid++)
        if (server.thread(tid).role() == Role::Ifid) { coordinator = tid; break; }
    if (coordinator == UINT32_MAX) return false;

    if (phase_ == Phase::WaitingFlip) {
        if (server.flip_stage() != FlipStage::Idle) return false;
        measure_flip(server, now_ms);
        if (retrigger_after_flip_) {
            const FlipctlTriggerReason reason = pending_retrigger_;
            start_maneuver(server, reason, now_ms);
            return false;
        }
        const bool completed = server.flip_completed() > pending_completed_ &&
                               server.role_count(Role::Ifid) == pending_target_io_;
        if (completed) {
            phase_ = after_flip_;
            if (phase_ == Phase::Settling) {
                shift_detector_.reset();
                anchor_signature_samples_ = 0;
                maneuver_signature_samples_ = 0;
                anchor_learning_rate_jitter_ = 0;
                anchor_learning_rate_sum_ = 0;
                anchor_learning_rate_min_ = 0;
                anchor_learning_rate_max_ = 0;
                anchor_learning_rate_samples_ = 0;
            }
            rate_window_ms_ = 0;
            previous_subwindow_valid_ = false;
            return false;
        }
        if (server.flip_refused() > pending_refused_) return settle(server, coordinator, now_ms);
        return false;
    }

    double rate = 0;
    if (phase_ == Phase::Measuring) {
        if (!sample_stabilized_rate(server, now_ms, rate)) return false;
        return decide_placement(server, coordinator, now_ms, rate);
    }
    if (phase_ == Phase::Seeking) {
        if (!sample_stabilized_rate(server, now_ms, rate)) return false;
        return seek_after_reading(server, coordinator, now_ms, rate);
    }
    if (phase_ == Phase::Settling) {
        if (!anchor_sampling_disabled_) {
            // Turning maneuver-only age sampling off changes the producer dispatch cost. Learn the
            // anchored fingerprint only after every owner has observed the dormant rate, otherwise
            // the controller detects its own disarm edge as a workload shift.
            signal_sample_rate_.store(0, std::memory_order_release);
            shift_detector_.reset();
            anchor_signature_samples_ = 0;
            maneuver_signature_samples_ = 0;
            anchor_learning_rate_jitter_ = 0;
            anchor_learning_rate_sum_ = 0;
            anchor_learning_rate_min_ = 0;
            anchor_learning_rate_max_ = 0;
            anchor_learning_rate_samples_ = 0;
            anchor_sampling_disabled_ = true;
            rate_window_ms_ = 0;
            previous_subwindow_valid_ = false;
            return false;
        }
        if (!sample_stabilized_rate(server, now_ms, rate)) return false;
        record(server.role_count(Role::Ifid), rate);
        anchor_learning_rate_jitter_ =
            std::max(anchor_learning_rate_jitter_, stable_pair_delta_);
        anchor_learning_rate_sum_ += rate;
        if (!anchor_learning_rate_samples_) {
            anchor_learning_rate_min_ = anchor_learning_rate_max_ = rate;
        } else {
            anchor_learning_rate_min_ = std::min(anchor_learning_rate_min_, rate);
            anchor_learning_rate_max_ = std::max(anchor_learning_rate_max_, rate);
        }
        anchor_learning_rate_samples_++;
        if (anchor_signature_samples_ < signature_learning_windows_) return false;
        anchor(server, rate);
        return false;
    }
    if (phase_ == Phase::Anchored) {
        // A fingerprint shift asks for the same evidence a rate trigger already asks for: TWO
        // consecutive out-of-band observations. One window can sit outside the band on ordinary
        // sampling luck; a workload that actually changed keeps presenting the new mix. Only
        // windows that closed real work vote -- a tick where no owner published is not evidence
        // either way, so it neither adds to the streak nor clears it.
        if (fingerprint_sampled_this_tick_) shift_streak_ = shifted ? shift_streak_ + 1 : 0;
        const bool sustained_shift = shift_streak_ >= shift_confirmations_;
        if (!sample_anchored_rate(server, now_ms, rate)) {
            if (sustained_shift) {
                last_shift_distance_ = shift_detector_.last_distance();
                last_shift_band_ = shift_detector_.band();
                start_maneuver(server, FlipctlTriggerReason::FingerprintShift, now_ms);
            }
            return false;
        }
        const double reference = anchor_rate_;
        const double band = anchor_rate_band_;
        if (reference > 0 &&
            rate > reference * (1.0 + band)) {
            surge_streak_++;
            collapse_streak_ = 0;
        } else if (reference > 0 &&
                   rate < reference * (1.0 - band)) {
            collapse_streak_++;
            surge_streak_ = 0;
        } else {
            surge_streak_ = 0;
            collapse_streak_ = 0;
        }
        // The same two-sub-window rule that makes a reading comparable supplies "sustained" here.
        if (surge_streak_ >= rate_confirmations_ ||
            collapse_streak_ >= rate_confirmations_) {
            const FlipctlTriggerReason reason = surge_streak_ >= 2
                ? FlipctlTriggerReason::AnchorRateSurge
                : FlipctlTriggerReason::AnchorRateCollapse;
            start_maneuver(server, reason, now_ms);
            return false;
        }

        // Pass-depth is rate-sensitive: a genuine volume step can cross both detectors on its
        // first tick. Preserve that first rate observation until the required second one instead
        // of letting the fingerprint preempt it and re-anchor at the new rate. A pure mix shift,
        // with no pending rate evidence, still starts immediately.
        if (sustained_shift && !surge_streak_ && !collapse_streak_) {
            last_shift_distance_ = shift_detector_.last_distance();
            last_shift_band_ = shift_detector_.band();
            start_maneuver(server, FlipctlTriggerReason::FingerprintShift, now_ms);
            return false;
        }

        // An out-of-band observation is trigger evidence, not baseline evidence. In particular,
        // do not let the first observation of the two-sample trigger pull the reference toward a
        // possible step or widen/narrow its band before the confirming observation arrives.
        if (surge_streak_ || collapse_streak_) return false;

        // This sample was wholly within an anchored, redistribution-free, in-band window. Fold its
        // innovation into a very slow live reference only after judging it against the prior
        // reference. Maneuver and pending-trigger windows never reach this branch.
        const double jitter_sample = relative_distance(rate, reference);
        anchor_rate_jitter_ +=
            (jitter_sample - anchor_rate_jitter_) * kAnchorRateEwmaAlpha;
        anchor_rate_ += (rate - anchor_rate_) * kAnchorRateEwmaAlpha;
        // The final settling windows define the minimum quiet jitter that this anchor already
        // proved it needs. Live learning may widen the band, but must never erase that floor.
        anchor_rate_band_ = std::max(
            anchor_rate_band_floor_, automatic_rate_band(anchor_rate_jitter_, anchor_rate_));
    }
    return false;
}

uint32_t FlipController::wait_ms(uint32_t tick_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ == Phase::WaitingFlip && flip_measure_armed_)
        return std::max<uint32_t>(1, tick_ms / kFlipPollDivisor);
    return std::max<uint32_t>(1, tick_ms);
}

bool FlipController::debug_seek(uint32_t target_io, bool force, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        error = "ERR flip controller is disabled; boot with --flip-auto 1";
        return false;
    }
    if (!target_io) {
        error = "ERR SEEK wants an io thread count of at least 1";
        return false;
    }
    if (phase_ != Phase::Anchored && phase_ != Phase::BootPending) {
        error = "ERR a maneuver is in progress; SEEK needs an anchored controller";
        return false;
    }
    debug_seek_io_ = target_io;
    debug_seek_force_ = force;
    force_requested_.store(true, std::memory_order_release);
    return true;
}

void FlipController::debug_cost(double commands_per_client) {
    std::lock_guard<std::mutex> lock(mutex_);
    cost_.injected_client_cost = commands_per_client;
}

FlipctlReport FlipController::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    FlipctlReport report;
    report.state = !enabled_ ? "disabled" : phase_ == Phase::BootPending
        ? "awaiting-load-stability" : phase_ == Phase::Anchored ? "anchored" : "maneuvering";
    report.phase = phase_name(phase_);
    report.last_trigger = reason_name(last_trigger_);
    report.anchor_io = anchor_io_;
    report.anchor_ex = anchor_ex_;
    report.anchor_rate = anchor_rate_;
    report.signature_band = shift_detector_.band();
    report.rate_band = anchor_rate_band_;
    report.triggers = triggers_;
    report.boot_triggers = boot_triggers_;
    report.fingerprint_triggers = fingerprint_triggers_;
    report.rate_surge_triggers = rate_surge_triggers_;
    report.rate_collapse_triggers = rate_collapse_triggers_;
    report.forced_triggers = forced_triggers_;
    report.null_maneuvers = null_maneuvers_;
    report.model_holds = model_holds_;
    report.shift_confirmations = shift_confirmations_;
    report.rate_confirmations = rate_confirmations_;
    report.round_trips = round_trips_;
    report.model_margin = model_margin_;
    report.model_last_decision = model_last_decision_;
    report.model_kappa = cost_.kappa();
    report.model_moves = cost_.moves;
    report.model_misses = cost_.misses;
    report.invalidated_maneuvers = invalidated_maneuvers_;
    report.cost_holds = cost_holds_;
    report.client_cost = cost_.client_cost();
    report.last_flip_lost = last_flip_lost_;
    report.last_flip_moved = last_flip_moved_;
    report.flip_ticks = cost_.flips ? cost_.flip_ticks() : 0;
    report.stationary_s = 0;
    report.refine_decision = refine_decision_;
    report.refine_steps = refine_steps_;
    return report;
}

std::string FlipController::debug_dump() const {
    std::lock_guard<std::mutex> lock(mutex_);
    char head[4096];
    const int n = std::snprintf(
        head, sizeof(head),
        "state=%s\nphase=%s\nanchor=%u:%u\nanchor_rate=%.3f\n"
        "signature_band=%.9f\nsignature_distance=%.9f\nsignature_jitter=%.9f\n"
        "signature_noise_bound=%.9f signature_noise_samples=%u\n"
        "rate_band=%.9f\nanchor_rate_jitter=%.9f\n"
        "last_shift_distance=%.9f\nlast_shift_band=%.9f\n"
        "boot_rate_ewma=%.3f\nboot_rate_jitter=%.9f\nboot_rate_slope=%.9f\n"
        "boot_rate_slope_threshold=%.9f\nboot_nonidle_ticks=%u\n"
        "model_io_frac=%.6f\nmodel_io_frac_low=%.6f\nmodel_io_frac_high=%.6f\n"
        "model_samples=%u model_readings_cap=%u\n"
        "model_headroom_io=%.4f model_headroom_ex=%.4f\n"
        "model_target_io=%u model_gain_mean=%.4f model_gain_low=%.4f model_gain_high=%.4f\n"
        "model_required_gain=%.4f model_margin=%u model_last_decision=%s\n"
        "model_holds=%llu round_trips=%llu maneuver_flips=%u\n"
        "shift_confirmations=%u rate_confirmations=%u\n"
        "shift_streak=%u null_maneuvers=%llu maneuver_origin_io=%u\n"
        "last_trigger=%s\ntriggers=%llu boot=%llu fingerprint=%llu "
        "rate_surge=%llu rate_collapse=%llu forced=%llu\n"
        "pending_io=%u seek=%s seek_target_io=%u origin_rate=%.3f\n"
        "origin_rate_readings=%u origin_rate_min=%.3f origin_rate_max=%.3f baseline_band=%.4f\n"
        "origin_sigma=%.5f model_kappa=%.4f model_verify_readings=%u rate_ew_sigma=%.5f rate_ew_n=%u\n"
        "cost_benefit=%.1f cost_cost=%.1f cost_transfer=%.1f cost_blackout_s=%.2f "
        "cost_horizon_s=%.2f cost_payback_s=%.2f cost_pays=%d\n"
        "cost_client=%.2f cost_reshuffle=%.3f cost_flip_ticks=%.2f cost_p_miss=%.3f "
        "cost_flips=%llu cost_moves=%u cost_misses=%u cost_lost_sum=%.1f cost_moved_sum=%.1f\n"
        "hyp_predicted=%.4f hyp_delivered=%.4f hyp_threshold=%.4f hyp_sigma=%.5f "
        "hyp_origin_samples=%u target_readings=%u pending_miss=%d\n"
        "last_flip_lost=%.1f last_flip_moved=%llu invalidated=%llu cost_holds=%llu\n"
        "refine_decision=%s refine_steps=%u seek_origin_io=%u refining=%d refine_bar=%.4f\n",
        !enabled_ ? "disabled" : phase_ == Phase::BootPending ? "awaiting-load-stability"
            : phase_ == Phase::Anchored ? "anchored" : "maneuvering",
        phase_name(phase_), anchor_io_, anchor_ex_, anchor_rate_, shift_detector_.band(),
        shift_detector_.last_distance(), shift_detector_.jitter(),
        shift_detector_.noise_bound(), shift_detector_.noise_samples(), anchor_rate_band_,
        anchor_rate_jitter_, last_shift_distance_, last_shift_band_, boot_rate_ewma_,
        boot_rate_jitter_, boot_rate_slope_, boot_rate_slope_threshold_, boot_nonidle_ticks_,
        model_io_frac_, model_io_frac_low_, model_io_frac_high_,
        demand_window_.samples, model_readings_cap_,
        model_io_headroom_, model_ex_headroom_,
        model_target_io_, model_gain_mean_, model_gain_low_, model_gain_high_,
        model_required_gain_, model_margin_, model_last_decision_,
        static_cast<unsigned long long>(model_holds_),
        static_cast<unsigned long long>(round_trips_), maneuver_flips_,
        shift_confirmations_, rate_confirmations_, shift_streak_,
        static_cast<unsigned long long>(null_maneuvers_), maneuver_origin_io_,
        reason_name(last_trigger_),
        static_cast<unsigned long long>(triggers_),
        static_cast<unsigned long long>(boot_triggers_),
        static_cast<unsigned long long>(fingerprint_triggers_),
        static_cast<unsigned long long>(rate_surge_triggers_),
        static_cast<unsigned long long>(rate_collapse_triggers_),
        static_cast<unsigned long long>(forced_triggers_), pending_target_io_,
        seek_ == Seek::AtTarget ? "at-target" : seek_ == Seek::Returning ? "returning" : "none",
        seek_target_io_, origin_rate_,
        origin_window_.samples, origin_window_.lo, origin_window_.hi, baseline_band(),
        origin_window_.sigma(), cost_.kappa(), model_verify_readings_,
        rate_ew_.sigma(), rate_ew_.samples,
        model_cost_.benefit, model_cost_.cost, model_cost_.transfer_cost, model_cost_.blackout_s,
        model_cost_.horizon_s, model_cost_.payback_s, model_cost_.pays ? 1 : 0,
        cost_.client_cost(), cost_.reshuffle(), cost_.flip_ticks(), cost_.miss_probability(),
        static_cast<unsigned long long>(cost_.flips), cost_.moves, cost_.misses,
        cost_.lost_sum, cost_.moved_sum,
        hyp_predicted_, hyp_delivered_, hyp_threshold_, hyp_sigma_, hyp_origin_samples_,
        target_window_.samples, pending_miss_ ? 1 : 0,
        last_flip_lost_, static_cast<unsigned long long>(last_flip_moved_),
        static_cast<unsigned long long>(invalidated_maneuvers_),
        static_cast<unsigned long long>(cost_holds_),
        refine_decision_, refine_steps_, seek_origin_io_, refining_ ? 1 : 0, refine_bar_);
    std::string out(head, n > 0 ? static_cast<size_t>(n) : 0);
    // Cold signature detail. The scalar `signature_distance` above says a shift happened; these
    // two vectors and the four family contributions say WHICH input moved, which is the only way
    // to tell a real mix change from one feature family's own quantization noise.
    const auto emit_signature = [&out](const char* tag, const FlipSignature& sig) {
        char line[512];
        const int m = std::snprintf(
            line, sizeof(line),
            "%s valid=%d cmds=%llu pass=%.6f,%.6f,%.6f,%.6f "
            "class=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f kpm=%.6f vbpc=%.6f\n",
            tag, sig.valid ? 1 : 0, static_cast<unsigned long long>(sig.commands),
            sig.pass_depth[0], sig.pass_depth[1], sig.pass_depth[2], sig.pass_depth[3],
            sig.command_class[0], sig.command_class[1], sig.command_class[2],
            sig.command_class[3], sig.command_class[4], sig.command_class[5],
            sig.command_class[6], sig.keys_per_multikey, sig.value_bytes_per_command);
        out.append(line, m > 0 ? static_cast<size_t>(m) : 0);
    };
    emit_signature("sig_now", shift_detector_.smoothed());
    emit_signature("sig_anchor", shift_detector_.anchored_signature());
    {
        const FlipSignature& now = shift_detector_.smoothed();
        const FlipSignature& anc = shift_detector_.anchored_signature();
        double pass_l1 = 0, class_l1 = 0;
        if (now.valid && anc.valid) {
            for (size_t i = 0; i < now.pass_depth.size(); i++)
                pass_l1 += std::abs(now.pass_depth[i] - anc.pass_depth[i]);
            for (size_t i = 0; i < now.command_class.size(); i++)
                class_l1 += std::abs(now.command_class[i] - anc.command_class[i]);
        }
        const double kpm = (now.valid && anc.valid)
            ? unit_normalized_distance(now.keys_per_multikey, anc.keys_per_multikey) : 0;
        const double vbpc = (now.valid && anc.valid)
            ? unit_normalized_distance(now.value_bytes_per_command,
                                       anc.value_bytes_per_command) : 0;
        char line[256];
        const int m = std::snprintf(
            line, sizeof(line),
            "dist_parts pass=%.9f class=%.9f kpm=%.9f vbpc=%.9f\n",
            pass_l1 * 0.5, class_l1 * 0.5 / 3.0, kpm / 3.0, vbpc / 3.0);
        out.append(line, m > 0 ? static_cast<size_t>(m) : 0);
    }
    out += "visited=";
    for (size_t i = 0; i < readings_.size(); i++) {
        if (i) out.push_back(',');
        out += std::to_string(readings_[i].split);
        out.push_back('@');
        out += std::to_string(readings_[i].rate);
    }
    out.push_back('\n');
    return out;
}

}  // namespace tomo
