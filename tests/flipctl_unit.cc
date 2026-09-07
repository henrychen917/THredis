#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "src/core/flipctl.h"

using namespace tomo;

namespace {

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "flipctl unit: %s\n", message);
    std::exit(1);
}

FlipFingerprintWindow quiet(uint64_t read, uint64_t write, uint64_t deep = 0) {
    FlipFingerprintWindow sample;
    sample.pass_depth = {read + write - deep, 0, deep, 0};
    sample.command_class[static_cast<size_t>(FlipFingerprintClass::Read)] = read;
    sample.command_class[static_cast<size_t>(FlipFingerprintClass::Write)] = write;
    sample.commands = read + write;
    sample.value_bytes = write * 32;
    sample.closed_windows = 1;
    return sample;
}

FlipFingerprintWindow multikey_mix() {
    FlipFingerprintWindow sample;
    sample.pass_depth = {0, 10, 90, 0};
    sample.command_class[static_cast<size_t>(FlipFingerprintClass::MultiRead)] = 80;
    sample.command_class[static_cast<size_t>(FlipFingerprintClass::MultiWrite)] = 20;
    sample.commands = 100;
    sample.multikey_ops = 100;
    sample.multikey_keys = 800;
    sample.value_bytes = 20 * 8 * 64;
    sample.closed_windows = 1;
    return sample;
}

// The io loop's generator (IoLoop::next_random), so the rows below draw what the server draws.
struct XorShift {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

// The EXHAUSTIVE writer as it shipped before DESIGN-flipfp (every frame fingerprinted, a window
// published once partial_.commands >= W). It is the oracle for the W = 1 equivalence row: the
// sampled writer at W = 1 must publish the same cumulative counters for the same frame stream.
class LegacyWriter {
public:
    void configure(uint32_t commands_per_window) {
        work_window_ = commands_per_window;
        partial_ = {};
        published_ = {};
        pass_frames_ = 0;
    }
    void note_command(FlipFingerprintClass command_class, uint32_t keys, uint64_t value_bytes) {
        partial_.command_class[static_cast<size_t>(command_class)]++;
        partial_.commands++;
        if (keys > 1) {
            partial_.multikey_keys += keys;
            partial_.multikey_ops++;
        }
        partial_.value_bytes += value_bytes;
        pass_frames_++;
    }
    void finish_parse_pass() {
        const uint32_t frames = pass_frames_;
        pass_frames_ = 0;
        if (!frames) return;
        const size_t bucket = frames == 1 ? 0 : frames <= 4 ? 1 : frames <= 16 ? 2 : 3;
        partial_.pass_depth[bucket]++;
        if (partial_.commands < work_window_) return;
        for (size_t i = 0; i < published_.pass_depth.size(); i++)
            published_.pass_depth[i] += partial_.pass_depth[i];
        for (size_t i = 0; i < published_.command_class.size(); i++)
            published_.command_class[i] += partial_.command_class[i];
        published_.commands += partial_.commands;
        published_.multikey_keys += partial_.multikey_keys;
        published_.multikey_ops += partial_.multikey_ops;
        published_.value_bytes += partial_.value_bytes;
        published_.closed_windows++;
        partial_ = {};
    }
    // Flush the open window (no phantom pass) so the cumulative totals are comparable at a cut.
    void flush() {
        for (size_t i = 0; i < published_.pass_depth.size(); i++)
            published_.pass_depth[i] += partial_.pass_depth[i];
        for (size_t i = 0; i < published_.command_class.size(); i++)
            published_.command_class[i] += partial_.command_class[i];
        published_.commands += partial_.commands;
        published_.multikey_keys += partial_.multikey_keys;
        published_.multikey_ops += partial_.multikey_ops;
        published_.value_bytes += partial_.value_bytes;
        partial_ = {};
    }
    const FlipFingerprintWindow& published() const { return published_; }

private:
    uint32_t work_window_ = 0;
    uint32_t pass_frames_ = 0;
    FlipFingerprintWindow partial_{};
    FlipFingerprintWindow published_{};
};

// One frame of a scripted stream: (class, keys, bytes) chosen by a cheap hash of its index so the
// stream has every class, single and multi key, and a spread of value sizes.
struct Frame {
    FlipFingerprintClass cls;
    uint32_t keys;
    uint64_t bytes;
};
Frame scripted_frame(uint64_t index) {
    const uint64_t h = index * 0x9E3779B97F4A7C15ull;
    const uint32_t which = static_cast<uint32_t>((h >> 40) % 7);
    Frame f{static_cast<FlipFingerprintClass>(which), 1, (h >> 20) % 300};
    if (which == 2 || which == 3 || which == 4) f.keys = 2 + static_cast<uint32_t>((h >> 12) % 8);
    return f;
}

bool same_counters(const FlipFingerprintWindow& a, const FlipFingerprintWindow& b) {
    for (size_t i = 0; i < a.pass_depth.size(); i++)
        if (a.pass_depth[i] != b.pass_depth[i]) return false;
    for (size_t i = 0; i < a.command_class.size(); i++)
        if (a.command_class[i] != b.command_class[i]) return false;
    return a.commands == b.commands && a.multikey_keys == b.multikey_keys &&
           a.multikey_ops == b.multikey_ops && a.value_bytes == b.value_bytes;
}

// Drive the sampled writer through `passes` passes of `depth` frames each; `frame_of(pass, i)`
// supplies the frame. Returns the number of sampled passes.
template <class FrameFn>
uint64_t drive(FlipFingerprintWriter& writer, XorShift& rng, uint64_t passes, uint32_t depth,
               FrameFn frame_of) {
    uint64_t sampled = 0;
    for (uint64_t p = 0; p < passes; p++) {
        const bool sampled_pass = writer.pass_sampled();
        for (uint32_t i = 0; i < depth; i++) {
            // The io loop's shell: the body runs only when the gate word says so.
            if (writer.pass_sampled()) {
                const Frame f = frame_of(p, i);
                writer.note_command(f.cls, f.keys, f.bytes);
            }
        }
        if (writer.finish_parse_pass()) {
            if (!sampled_pass) fail("finish_parse_pass reported a sample on an unsampled pass");
            writer.arm(rng.next());
            sampled++;
        } else if (sampled_pass) {
            fail("finish_parse_pass did not report the sampled pass");
        }
    }
    return sampled;
}

}  // namespace

int main() {
    // ---- detector rows (unchanged) --------------------------------------------------------------
    FlipShiftDetector detector(-1);
    if (detector.observe(quiet(80, 20)) ||
        detector.observe(quiet(79, 21, 1)) ||
        detector.observe(quiet(81, 19)))
        fail("learning windows fired before anchor");
    detector.anchor();
    if (!detector.anchored() || detector.band() <= 0)
        fail("auto band was not learned at anchor");
    const double anchored_band = detector.band();

    // Scripted quiet-state count noise stays inside the signature's own learned band.
    if (detector.observe(quiet(80, 20, 1)) || detector.observe(quiet(79, 21)))
        fail("quiet noise fired the shift detector");
    if (detector.band() != anchored_band)
        fail("post-anchor traffic changed the learned quiet-state band");

    // The same command count changes pipe-depth, command class, keys/op and value bytes together.
    if (!detector.observe(multikey_mix()))
        fail("scripted workload mix change did not fire");

    // THE TYPED BAND IS A FLOOR, NOT A CEILING. The gate-hygiene lane's instrumented failure: a
    // driver holding its rate to 0.07% across 34 samples, a fingerprint distance of 0.2518 against
    // a flat explicit band of 2% (0.0200) -- 12.6x its band -- fired a maneuver on a stationary load.
    // A detector whose signature genuinely moves between adjacent quiet windows must end up with a
    // band above that movement whatever the operator typed, and must still fire on a real change.
    {
        FlipShiftDetector typed(2, 4);   // internal explicit-band control: 2%
        // Stationary but NOISY: the read/write split alternates 35:65 / 65:35 window to window.
        // The detector smooths the signature, so the anchored distance this produces is ~0.09.
        const auto noisy = [](int k) { return quiet(k % 2 ? 65 : 35, k % 2 ? 35 : 65); };
        for (int k = 0; k < 8; k++)
            if (typed.observe(noisy(k))) fail("a pre-anchor window fired the detector");
        typed.anchor();
        if (!(typed.band() > 0.15))
            fail("a typed band ignored a signature that moves between quiet windows");
        int fires = 0;
        for (int k = 8; k < 24; k++) fires += typed.observe(noisy(k)) ? 1 : 0;
        if (fires) fail("the typed band still fired on the signal's own quiet-state noise");
        if (!(typed.band() > 0.09))
            fail("the band decayed below the distance its own quiet state produces");
        // ... and the feature still works: a real mix change clears the widened band.
        if (!typed.observe(multikey_mix()))
            fail("the widened typed band swallowed a real mix change");
    }
    // A STILL signature at a REAL window size keeps the operator's typed band exactly: the floors
    // are the signal's own movement and the estimator's resolution, not a tax on every deployment.
    // (Production windows are large -- the lane's dumps show 0.5M to 5M commands per window -- so
    // the 1/sqrt(N) quantum is far below a typed 2%.)
    {
        FlipShiftDetector typed(2, 4);
        for (int k = 0; k < 8; k++) typed.observe(quiet(8000, 2000, k % 2));
        typed.anchor();
        if (std::abs(typed.band() - 0.02) > 1e-9)
            fail("a still signature did not keep the typed 2% band");
        for (int k = 0; k < 8; k++)
            if (typed.observe(quiet(8000, 2000, k % 2)))
                fail("a still signature fired its typed band");
    }
    // A TYPED BAND CANNOT BUY RESOLUTION THE SIGNAL HAS NOT GOT. If the writer samples the request
    // stream, N is what the signature was ESTIMATED from: a 100-command window resolves the mix to
    // about 0.1, so a typed 2% is raised to the estimator's own 1/sqrt(N) scale. Simulated on a
    // 1-in-100 sampled stationary stream this took two-consecutive spurious exceedances from three
    // in 600 windows to none, while a real mix change still cleared the band by 3.3x.
    {
        FlipShiftDetector sampled(2, 8);
        for (int k = 0; k < 8; k++) sampled.observe(quiet(50, 50));
        sampled.anchor();
        if (!(sampled.band() >= 2.0 / std::sqrt(100.0) - 1e-9))
            fail("a typed band undercut the sampled estimator's own resolution");
        if (!sampled.observe(multikey_mix()))
            fail("the resolution floor swallowed a real mix change on a sampled stream");
    }
    // The learned (auto) band takes the same floor, and the max is what applies: on the same noisy
    // signature the band is the observed movement, not the count quantum under it.
    {
        FlipShiftDetector autob(-1, 4);
        const auto noisy = [](int k) { return quiet(k % 2 ? 6500 : 3500, k % 2 ? 3500 : 6500); };
        for (int k = 0; k < 8; k++) autob.observe(noisy(k));
        autob.anchor();
        if (!(autob.band() >= autob.noise_bound() - 1e-12))
            fail("the learned band did not take the noise floor");
        if (!(autob.band() > 2.0 / std::sqrt(10000.0)))
            fail("the learned band stayed at the count quantum under a noisy signature");
        int fires = 0;
        for (int k = 8; k < 24; k++) fires += autob.observe(noisy(k)) ? 1 : 0;
        if (fires) fail("the learned band fired on quiet-state movement");
    }
    // A TYPED BAND UNDER THE REAL SAMPLER. The shipped configuration cannot reach this -- the
    // controller runs the learned band. This internal explicit-band control preserves the earlier
    // regression: where the typed path bypassed the floor the fingerprint lane measured 73
    // two-consecutive exceedances in 600 stationary windows at K=100 and 208 at K=60: a spurious
    // maneuver every few seconds on a load that never changed. Driven here through THEIR writer, a
    // stationary 50/50 stream at depth 32, with the floor in place: none, and a real mix change
    // still clears. The window is what the writer actually published, so K is the SAMPLED count.
    {
        for (uint32_t window : {60u, 100u}) {
            XorShift rng;
            FlipShiftDetector typed(2, 8);   // internal explicit-band control: 2%
            int fires = 0, confirmed = 0, streak = 0;
            for (int w = 0; w < 60; w++) {
                FlipFingerprintWriter writer;
                writer.configure(1);          // sample every pass; `window` frames make one window
                drive(writer, rng, window, 1, [&rng](uint64_t, uint32_t) {
                    return Frame{rng.next() & 1 ? FlipFingerprintClass::Read
                                                : FlipFingerprintClass::Write, 1, 32};
                });
                const FlipFingerprintWindow& pub = writer.published();
                if (pub.commands != window) fail("the driven window did not publish its frames");
                const bool fired = w < 8 ? (typed.observe(pub), false) : typed.observe(pub);
                if (w == 7) typed.anchor();
                fires += fired ? 1 : 0;
                streak = fired ? streak + 1 : 0;
                if (streak >= 2) { confirmed++; streak = 0; }
            }
            if (confirmed) fail("a typed band under the real sampler fired on a stationary stream");
            if (!(typed.band() >= 2.0 / std::sqrt(static_cast<double>(window)) - 1e-12))
                fail("the typed band undercut the sampled estimator's resolution");
            if (!typed.observe(multikey_mix()))
                fail("the sampled typed band swallowed a real mix change");
        }
    }
    // AN EXCURSION MUST NOT WIDEN THE THRESHOLD THAT JUDGES IT. The regression this pins: folding
    // every post-anchor window into the noise estimate let the FIRST window of a real mix change
    // lift the band to the excursion's own size, so the change never cleared its band
    // (flip_multikey_hold positive phase, reached 0.80x). Only in-band windows may teach it.
    {
        FlipShiftDetector d(-1, 4);
        for (int k = 0; k < 8; k++) d.observe(quiet(8000, 2000, k % 2));
        d.anchor();
        const double band_before = d.band();
        if (!d.observe(multikey_mix())) fail("a real mix change did not clear its band");
        if (d.band() != band_before) fail("the excursion widened the band that judged it");
        // ... and it keeps clearing it for as long as it lasts, rather than being absorbed.
        if (!d.observe(multikey_mix())) fail("the mix change was absorbed on its second window");
    }
    // FlipEwBound itself: mean + 2 sd of a non-negative sample, zero before it has two.
    {
        FlipEwBound b; b.configure(8);
        if (b.bound() != 0) fail("an empty bound is not zero");
        for (int k = 0; k < 60; k++) b.add(k % 2 ? 0.30 : 0.20);
        if (!(b.bound() > 0.25 && b.bound() < 0.60)) fail("bound of a 0.2/0.3 alternation");
        FlipEwBound flat; flat.configure(8);
        for (int k = 0; k < 40; k++) flat.add(0.0);
        if (flat.bound() != 0) fail("a flat-zero signal has a bound");
    }

    // ---- writer: a sampled pass publishes what the exhaustive writer accumulated for it ---------
    FlipFingerprintWriter writer;
    writer.configure(4);
    if (!writer.enabled() || !writer.pass_sampled())
        fail("an armed writer must sample its first pass");
    for (unsigned i = 0; i < 4; i++)
        writer.note_command(FlipFingerprintClass::Read, 1, 0);
    if (!writer.finish_parse_pass()) fail("the sampled first pass did not report itself");
    {
        const FlipFingerprintWindow& published = writer.published();
        if (published.closed_windows != 1 || published.commands != 4 ||
            published.pass_depth[1] != 1)
            fail("sampled pass did not publish its frames and depth bucket");
    }
    // Depth buckets: 1 / <=4 / <=16 / more, exactly the exhaustive writer's.
    {
        FlipFingerprintWriter w;
        const uint32_t depths[4] = {1, 4, 16, 17};
        for (size_t b = 0; b < 4; b++) {
            w.configure(1);
            for (uint32_t i = 0; i < depths[b]; i++) w.note_command(FlipFingerprintClass::Write, 1, 8);
            w.finish_parse_pass();
            for (size_t k = 0; k < 4; k++)
                if (w.published().pass_depth[k] != (k == b ? 1u : 0u)) fail("depth bucket mismatch");
        }
    }
    // An empty sampled pass publishes nothing but still spends its sample.
    {
        FlipFingerprintWriter w;
        w.configure(3);
        if (!w.finish_parse_pass()) fail("empty sampled pass did not report itself");
        if (w.published().closed_windows != 0) fail("empty sampled pass published a window");
        w.arm(0);
        if (!w.pass_sampled()) fail("arm(0) must make the next pass sampled");
    }

    // ---- writer: dark ---------------------------------------------------------------------------
    writer.configure(0);
    if (writer.enabled()) fail("zero work window did not disable sampling");
    if (writer.pass_sampled()) fail("a dark writer's gate word must never read sampled");

    // ---- gap arithmetic: uniform over [0, 2W-2] unsampled passes, mean gap W, W = 1 exhaustive --
    for (uint32_t w : {1u, 2u, 3u, 100u, 1000u}) {
        const uint64_t span = 2ull * w - 1;
        long double sum = 0;
        for (uint64_t r = 0; r < span; r++) {
            const uint32_t gap = FlipFingerprintWriter::gap_for(r, w);
            if (gap > 2 * w - 2) fail("gap outside [0, 2W-2]");
            sum += gap + 1;   // passes between sampled passes, inclusive of the sampled one
        }
        if (std::fabs(static_cast<double>(sum / span) - static_cast<double>(w)) > 1e-9)
            fail("mean distance between sampled passes is not W");
        if (w == 1 && FlipFingerprintWriter::gap_for(0xDEADBEEFull, 1) != 0)
            fail("W = 1 must sample every pass");
    }

    // ---- sampling fraction: 1 in W passes, so 1 in W frames at any depth -----------------------
    {
        XorShift rng;
        for (uint32_t depth : {1u, 32u}) {
            FlipFingerprintWriter w;
            w.configure(100);
            const uint64_t passes = 1'000'000;
            const uint64_t sampled = drive(w, rng, passes, depth,
                                           [](uint64_t, uint32_t) { return Frame{FlipFingerprintClass::Read, 1, 0}; });
            // Renewal count over P passes with gap mean W and gap sd ~57.4: sd ~ sqrt(P) * 57.4 /
            // W^1.5 = 57 sampled passes; accept +-4 sd around P/W.
            if (sampled < 9'770 || sampled > 10'230) fail("sampled pass count is not 1/W of the passes");
            if (w.published().commands != sampled * depth)
                fail("a sampled pass must contribute all of its frames");
            if (w.published().closed_windows != sampled)
                fail("every non-empty sampled pass publishes exactly once");
        }
    }

    // ---- aliasing: classes alternating by pass are sampled in proportion (a stride of W would
    //      see one class only) ------------------------------------------------------------------
    {
        XorShift rng;
        FlipFingerprintWriter w;
        w.configure(2);
        const uint64_t passes = 400'000;
        drive(w, rng, passes, 4, [](uint64_t p, uint32_t) {
            return Frame{p % 2 == 0 ? FlipFingerprintClass::Read : FlipFingerprintClass::Write, 1, 0};
        });
        const FlipFingerprintWindow& pub = w.published();
        const double reads = static_cast<double>(pub.command_class[static_cast<size_t>(FlipFingerprintClass::Read)]);
        const double writes = static_cast<double>(pub.command_class[static_cast<size_t>(FlipFingerprintClass::Write)]);
        const double share = reads / (reads + writes);
        if (share < 0.47 || share > 0.53) fail("period-2 workload was not sampled in proportion");
        // The counter-example the uniform gap exists to avoid: a fixed stride of exactly W.
        uint64_t strided_reads = 0, strided_writes = 0;
        for (uint64_t p = 0; p < passes; p += 2) (p % 2 == 0 ? strided_reads : strided_writes) += 4;
        if (strided_writes != 0) fail("stride model is wrong");
    }

    // ---- W = 1 is the exhaustive writer: identical cumulative counters for the same stream -----
    {
        XorShift rng;
        FlipFingerprintWriter sampled;
        LegacyWriter legacy;
        sampled.configure(1);
        legacy.configure(100);
        uint64_t index = 0;
        for (uint64_t p = 0; p < 20'000; p++) {
            const uint32_t depth = 1 + static_cast<uint32_t>((p * 7919) % 40);
            if (!sampled.pass_sampled()) fail("W = 1 left a pass unsampled");
            for (uint32_t i = 0; i < depth; i++, index++) {
                const Frame f = scripted_frame(index);
                sampled.note_command(f.cls, f.keys, f.bytes);
                legacy.note_command(f.cls, f.keys, f.bytes);
            }
            if (!sampled.finish_parse_pass()) fail("W = 1 pass did not publish");
            sampled.arm(rng.next());
            legacy.finish_parse_pass();
        }
        legacy.flush();
        if (!same_counters(sampled.published(), legacy.published()))
            fail("W = 1 sampled writer diverged from the exhaustive writer");
        if (sampled.published().closed_windows != 20'000)
            fail("W = 1 must publish once per pass");
    }

    // ---- the signature is scale-free; only the quantum floor moves with the sample count -------
    // The scale-free half is the fingerprint lane's and stands: a 1/W sample of a stationary window
    // is the SAME signature. The floor's law is this lane's and supersedes the 4/K this row was
    // written against: every family in the distance is a proportion or a per-command mean estimated
    // from K commands, so two windows drawn from one stationary workload differ by the ESTIMATOR'S
    // noise, which falls as 1/sqrt(K), never as 1/K. The old form understated it by sqrt(K) --
    // twentyfold at 13.5k commands a window -- which is how a window that merely drew 60 MSETs
    // where the last drew 50 read as a workload change. Under sampling the two laws differ by W in
    // the sampled-vs-dispatched discrimination (a factor of sqrt(W) = 10 rather than W = 100 at
    // 1-in-100); both discriminate, and the measured adjacent jitter the sampler actually produces
    // sits about 4x under this floor at depth 32, so the design effect does not eat it.
    {
        const FlipSignature full = flip_signature(quiet(80, 20));
        const FlipSignature sampled = flip_signature(quiet(8, 2));
        if (flip_signature_distance(full, sampled) != 0)
            fail("a 1/W sample of a stationary window changed the signature");
        FlipShiftDetector k100(-1), k10(-1);
        k100.observe(quiet(80, 20)); k100.anchor();
        k10.observe(quiet(8, 2)); k10.anchor();
        // jitter is zero after one window and the noise bound needs two, so band = 2/sqrt(K).
        if (std::fabs(k100.band() - 2.0 / std::sqrt(100.0)) > 1e-12 ||
            std::fabs(k10.band() - 2.0 / std::sqrt(10.0)) > 1e-12)
            fail("quantum floor is not 2 / sqrt(commands)");
        if (!(k10.band() > k100.band()))
            fail("a thinner sample did not widen the floor");
    }
    std::puts("flipctl unit: ok");

    // ---- placement policy (flip_policy.h) -----------------------------------------------------
    // THE DEFECT, in the model's own arithmetic. The estimator then in use read io = 82% on 8-key
    // MGET/MSET at 5:3 of 8 threads; the old jump rounded 8 * 0.82 to 7 and overshot. Under work
    // conservation 7:1 is 0.69x the origin (ex saturates first) while 6:2 is +20%: the argmax must
    // be 6, never 7, and 7 must project as a LOSS against 5.
    if (flip_projected_gain(7, 5, 8, 0.82) >= 0) fail("7:1 did not project as a loss at io=0.82");
    {
        FlipDemandWindow w;
        w.add(0.82); w.add(0.82); w.add(0.82);
        const FlipPlacementChoice c = flip_choose_split(5, 8, w, 0.02, 3);
        if (!c.decided || !c.move || c.target_units != 6)
            fail("exact io=0.82 at 5:3 did not choose 6:2");
        if (std::abs(c.gain_mean - 0.2) > 0.01) fail("6:2 gain at io=0.82 is not +20%");
    }
    // A demand share that rounds to a different thread count is NOT a move when the live split
    // wins in throughput space: io=0.63 at 2:2 of 4 rounds to 3, but 3:1 leaves ex with a quarter
    // of the pool for 37% of the work (0.85x). Hold, decided.
    {
        FlipDemandWindow w;
        w.add(0.63); w.add(0.63); w.add(0.63);
        const FlipPlacementChoice c = flip_choose_split(2, 4, w, 0.02, 3);
        if (!c.decided || c.move || c.target_units != 2)
            fail("io=0.63 at 2:2 of 4 did not hold the live split");
    }
    // Single-key symmetry: one command is one task, so io=0.5 makes 4:4 the optimum of 8 and a
    // 5:3 origin a +33% move (8/6 - 1).
    if (std::abs(flip_projected_gain(4, 5, 8, 0.5) - 1.0 / 3.0) > 1e-9)
        fail("4:4 gain over 5:3 at io=0.5 is not 1/3");
    // WINDOW + VARIANCE, sequentially. Three draws spread across a thread's width of io share
    // straddle the bar: undecided, keep sampling. Draws that tighten around the mean resolve it.
    {
        FlipDemandWindow w;
        w.add(0.55); w.add(0.75); w.add(0.65);   // mean 0.65, half-width 0.115
        FlipPlacementChoice c = flip_choose_split(5, 8, w, 0.02, 3);
        if (c.decided) fail("a 0.20-wide window decided a one-thread move");
        for (int i = 0; i < 12; i++) w.add(0.65);
        c = flip_choose_split(5, 8, w, 0.02, 3);
        if (!c.decided) fail("a tightened window did not decide");
        // io=0.65 of 8: 5:3 projects 5/0.65=7.69 vs 3/0.35=8.57 -> io-bound at 7.69; 6:2 gives
        // 9.23 vs 5.71 -> 5.71 (worse). The live split IS the optimum: a hold, not a move.
        if (c.move || c.target_units != 5) fail("io=0.65 at 5:3 did not hold");
    }
    // Fewer than the minimum draws never decide, whatever they say.
    {
        FlipDemandWindow w;
        w.add(0.9); w.add(0.9);
        if (flip_choose_split(5, 8, w, 0.02, 3).decided) fail("two draws decided a move");
    }
    // The bar matters: a real +20% clears 2%, and a required gain above it refuses the move. (The
    // CONTROLLER treats that refusal as a measurement verdict and keeps sampling to its reading cap
    // rather than anchoring -- decide_placement, "sampling-bar" -- because a bar larger than any
    // available gain says the measurement is inadequate, not that the live split is right.)
    {
        FlipDemandWindow w;
        w.add(0.82); w.add(0.82); w.add(0.82);
        if (!flip_choose_split(5, 8, w, 0.19, 3).move) fail("+20% did not clear a 19% bar");
        const FlipPlacementChoice c = flip_choose_split(5, 8, w, 0.21, 3);
        if (!c.decided || c.move) fail("+20% cleared a 21% bar");
    }
    // BASELINE WANDER. The gate's ramping driver produced origin readings of 4899 and 6001 ops/s
    // at one split, and the boot maneuver then read a rail's +22% as "delivered" and anchored
    // there. Twice that 20% spread is the floor under the maneuver's bands, so the same +22%
    // cannot clear it and the seek reverts to the origin. A still baseline costs nothing.
    if (!(flip_baseline_band(4898.601, 6000.916) > 0.22))
        fail("a 20% baseline spread did not floor the band above the gain it produced");
    if (flip_baseline_band(500000, 500000) != 0) fail("a flat baseline moved the band");
    if (flip_baseline_band(0, 0) != 0) fail("an empty baseline moved the band");
    if (flip_baseline_band(500000, 505000) > 0.021)
        fail("a 1% baseline spread floored the band above 2%");
    // The estimator noise falls as 1/sqrt(n): the same three draws twice over halve nothing, but
    // the half-width shrinks by sqrt(2).
    {
        FlipDemandWindow a, b;
        for (double v : {0.6, 0.7, 0.65}) a.add(v);
        for (double v : {0.6, 0.7, 0.65, 0.6, 0.7, 0.65}) b.add(v);
        if (!(b.half_width() < a.half_width())) fail("more draws did not tighten the window");
    }
    // ---- REVISION 2026-09-06: signal, cost gate, verification window, outcome ------------------
    // THE SIGNAL DEFECT, in the guard's own numbers (single-key 1:1, 2:2 of 4, 40 s window). The
    // io thread booked 16.36 s busy + 0.17 s idle of 39.92 s on CPU; busy shares read io = 0.32 and
    // the model moved to 1:3, which measured 0.48x. Work = wall - idle reads 0.506 and holds.
    {
        const double wall = 40.0e9;
        const double io_work = flip_role_work(wall, 0.17e9) + flip_role_work(wall, 2.40e9);
        const double ex_work = flip_role_work(wall, 2.56e9) + flip_role_work(wall, 1.76e9);
        const double f = io_work / (io_work + ex_work);
        if (std::abs(f - 0.506) > 0.005) fail("wall-idle share of the sk1:1 window is not 0.506");
        FlipDemandWindow w;
        w.add(f); w.add(f); w.add(f);
        const FlipPlacementChoice c = flip_choose_split(2, 4, w, 0.012, 3);
        if (!c.decided || c.move || c.target_units != 2)
            fail("the wall-idle share did not hold 2:2 on the sk1:1 window");
        // The busy share the guard used projected +5.8% for 1:3 -- the round trip it took.
        FlipDemandWindow busy;
        busy.add(0.321); busy.add(0.321); busy.add(0.321);
        const FlipPlacementChoice b = flip_choose_split(2, 4, busy, 0.012, 3);
        if (!b.move || b.target_units != 1) fail("the busy share did not reproduce the 1:3 move");
        if (flip_role_work(0, 5) != 0 || flip_role_work(10, 12) != 0 || flip_role_work(10, -1) != 10)
            fail("flip_role_work bounds");
    }
    // The mk window: io 27.6+3.5 / 25.75+3.24 of 38.1 s, ex 38.7+1.2 / 37.5+1.45 of 40.0 s.
    {
        const double io_work = flip_role_work(38.1e9, 3.50e9) + flip_role_work(38.1e9, 3.24e9);
        const double ex_work = flip_role_work(40.0e9, 1.18e9) + flip_role_work(40.0e9, 1.45e9);
        const double f = io_work / (io_work + ex_work);
        if (std::abs(f - 0.472) > 0.005) fail("wall-idle share of the mk window is not 0.472");
        FlipDemandWindow w;
        w.add(f); w.add(f); w.add(f);
        if (flip_choose_split(2, 4, w, 0.02, 3).move) fail("mk at 2:2 moved on the work share");
    }
    // Naive transfers: every client of a converted thread when io shrinks, the new threads' share
    // when it grows (measured: 131 and 154 of 256 for 2->1 and 3->2 with the re-plan on top).
    if (flip_naive_transfers(256, 2, 1) != 128 || flip_naive_transfers(256, 1, 2) != 128)
        fail("naive transfers 2<->1 of 256 are not 128");
    if (std::abs(flip_naive_transfers(256, 3, 2) - 85.333) > 0.01)
        fail("naive transfers 3->2 of 256 are not 85.3");
    if (flip_naive_transfers(256, 2, 2) != 0 || flip_naive_transfers(0, 2, 1) != 0)
        fail("naive transfers of a no-op or an empty server are not zero");
    // VERIFICATION WINDOW from the origin's own noise: a big gain resolves in one reading, a small
    // one needs more, one too small against the origin's standard error cannot be verified yet,
    // and more origin readings shorten the target's window.
    if (flip_verify_window(0.01, 3, 0.20, 15) != 1) fail("+20% at 1% noise did not verify in one");
    {
        const uint32_t n3 = flip_verify_window(0.01, 3, 0.03, 15);   // (0.75)^2 - 1/3 = 0.229 -> 5
        const uint32_t n5 = flip_verify_window(0.01, 5, 0.03, 15);   // 0.5625 - 0.2 = 0.3625 -> 3
        if (n3 != 5) fail("+3% at 1% noise with 3 origin readings is not 5 target readings");
        if (n5 != 3) fail("+3% at 1% noise with 5 origin readings is not 3 target readings");
        if (!(n5 < n3)) fail("more origin readings did not shorten the target window");
    }
    if (flip_verify_window(0.05, 3, 0.03, 15) != 0)
        fail("a gain below the origin's own standard error verified");
    if (flip_verify_window(0.0, 3, 0.001, 15) != 1) fail("a noiseless origin needs more than one");
    if (flip_verify_window(0.01, 3, 0.0, 15) != 0) fail("a zero gain planned a window");
    // The threshold is two standard errors of the difference, floored.
    if (std::abs(flip_verify_threshold(0.01, 3, 1, 0) - 2 * 0.01 * std::sqrt(1.0 / 3 + 1)) > 1e-12)
        fail("verify threshold is not 2 sigma sqrt(1/n_o + 1/k)");
    if (flip_verify_threshold(0.01, 3, 1, 0.40) != 0.40) fail("threshold floor did not hold");
    // Sequential judgment: early accept, early reject, and the planned-count verdict.
    if (flip_judge(0.30, 0.10, 1, 5) != FlipOutcome::Hit) fail("a clear gain did not accept early");
    if (flip_judge(-0.30, 0.10, 1, 5) != FlipOutcome::Miss) fail("a clear loss did not reject early");
    if (flip_judge(0.05, 0.10, 1, 5) != FlipOutcome::Pending) fail("an unresolved reading decided");
    if (flip_judge(0.05, 0.10, 5, 5) != FlipOutcome::Miss) fail("the planned count did not reject");
    // COST GATE. Units: commands = gain x rate x seconds. The blackout risks the whole ORIGIN
    // throughput on a miss (R0 x T_black), NOT gain x R0 x T_black -- that is what makes the gate
    // asymmetric: benefit/cost ~ gain, so the stationarity a move needs falls as the gain rises.
    // The first flip has no transfer cost (it IS the measurement); it moves as soon as the credited
    // horizon covers the blackout risk. R0 = 236k/s (this rig at 3:1), gain +100%.
    {
        FlipCostModel m;
        if (m.client_cost() != 0 || m.kappa() != 1.0 || std::abs(m.miss_probability() - 0.5) > 1e-12)
            fail("fresh cost model priors");
        const FlipCostVerdict v = flip_cost_gate(1.0, 1.0, 236000, 12.0, 4.0, 0, m.miss_probability(), 1);
        // benefit 1.0 x 236k x (12-4) = 1.888M; cost 0.5 x 236k x 4 = 472k
        if (!v.pays || std::abs(v.benefit - 1.888e6) > 1 || std::abs(v.cost - 472000) > 1)
            fail("first-flip cost gate arithmetic");
        if (std::abs(v.payback_s - (4.0 + 472000.0 / 236000.0)) > 1e-9) fail("payback seconds");
    }
    // ASYMMETRY. A wildly wrong split (a 28:4 boot delivers ~0.53M against ~13M achievable, +2400%)
    // pays almost at the blackout floor: benefit 24 x 0.53M x (T-4), cost 0.5 x 0.53M x 4 = 1.06M,
    // so it pays once (T-4) > 1.06M/(24 x 0.53M) = 0.083 s -> T ~ 4.1 s, one blackout.
    {
        const FlipCostVerdict wrong = flip_cost_gate(24.0, 24.0, 530000, 4.1, 4.0, 0, 0.5, 1);
        if (!wrong.pays) fail("a +2400% wrong-split move did not pay within one blackout");
        if (!(wrong.payback_s < 4.5)) fail("the wrong-split move's payback was not ~one blackout");
    }
    // A marginal move on a HEALTHY split is refused for a long time: +5% off 500k risks the whole
    // 500k during the blackout. benefit 0.05 x 500k x (T-4); cost (first flip) 0.5 x 500k x 4 = 1M.
    // Pays only once (T-4) > 1M / 25k = 40 s -> T > 44 s.
    {
        const FlipCostVerdict no = flip_cost_gate(0.05, 0.05, 500000, 10.0, 4.0, 0, 0.5, 1);
        if (no.pays) fail("a +5% move off a healthy split paid at 10 s of stationarity");
        if (std::abs(no.cost - 1.0e6) > 1e-3) fail("the blackout cost did not risk the origin rate");
        const FlipCostVerdict yes = flip_cost_gate(0.05, 0.05, 500000, 45.0, 4.0, 0, 0.5, 1);
        if (!yes.pays) fail("the +5% move did not pay at 45 s of stationarity");
        // The bar doubles on a miss: at 45 s margin 1 pays, margin 2 does not.
        if (flip_cost_gate(0.05, 0.05, 500000, 45.0, 4.0, 0, 0.5, 2).pays)
            fail("a doubled margin did not refuse the marginal move");
    }
    // The measured transfer cost dominates once a flip has been priced (second move onward).
    {
        FlipCostModel m;
        const double naive = flip_naive_transfers(256, 3, 2);       // 85.33: the converted thread's share
        m.record_flip(250000, 154, naive, 1.0);     // lost 250k commands moving 154 clients
        if (std::abs(m.client_cost() - 250000.0 / 154) > 1e-6) fail("per-client cost");
        if (std::abs(m.reshuffle() - 154 / naive) > 1e-9) fail("re-plan ratio");
        const double transfers = m.predicted_transfers(256, 2, 3);   // naive 85.33 x 1.805 = 154
        if (std::abs(transfers - 154) > 1e-6) fail("predicted transfers did not reproduce the plan");
        const double xfer_cost = m.client_cost() * transfers;        // 250k commands
        // benefit 0.20 x 500k x (T-4); cost 250k x 1.5 + 0.5 x 500k x 4 = 375k + 1M = 1.375M.
        const FlipCostVerdict no = flip_cost_gate(0.20, 0.20, 500000, 10.0, 4.0, xfer_cost, 0.5, 1);
        if (no.pays || std::abs(no.cost - 1.375e6) > 1e-3) fail("+20% move priced with a measured flip");
        const FlipCostVerdict yes = flip_cost_gate(0.20, 0.20, 500000, 30.0, 4.0, xfer_cost, 0.5, 1);
        if (!yes.pays) fail("+20% move did not pay at 30 s with the measured flip cost");
    }
    // The pre-move wait is distance-derived: a huge, tight gain decides at the two-draw floor,
    // while a marginal one stays undecided until the interval tightens (flip_choose_split, above).
    {
        FlipDemandWindow w;
        w.add(0.11); w.add(0.13);   // 28:4-like: ex saturated, io idle -> move toward more ex
        const FlipPlacementChoice c = flip_choose_split(3, 4, w, 0.02, 2);
        if (!c.decided || !c.move) fail("a wildly wrong split did not decide at two draws");
        // A MARGINAL move: at mean io=0.72 of 8, 5:3 -> 6:2 is +2.9% (just over the 2% bar), but the
        // two-draw interval [0.68, 0.76] puts 6:2 at -15% at one end and +20% at the other, so its
        // pessimistic gain is below the bar -- keep sampling (undecided), the wait deriving from
        // how close the split is to the crossover, not from a fixed floor.
        FlipDemandWindow n;
        n.add(0.70); n.add(0.74);
        if (flip_choose_split(5, 8, n, 0.02, 2).decided)
            fail("a marginal two-draw window decided a one-thread move");
    }
    // OUTCOME LOOP. A miss halves the model's credence (kappa 1 -> 0.5: the bar doubles in
    // effect); a hit restores it; over-delivery never buys more than one; the miss rate follows
    // Laplace; an induced hypothesis with a non-positive prediction counts toward the rate only.
    {
        FlipCostModel m;
        m.record_outcome(0.20, 0.0, false);
        if (std::abs(m.kappa() - 0.5) > 1e-12) fail("one miss did not halve kappa");
        if (std::abs(m.miss_probability() - 2.0 / 3.0) > 1e-12) fail("Laplace after one miss");
        m.record_outcome(0.20, 0.20, true);
        if (std::abs(m.kappa() - 0.6667) > 1e-3) fail("hit after miss: kappa 2/3");
        FlipCostModel h;
        h.record_outcome(0.20, 0.50, true);
        if (h.kappa() != 1.0) fail("over-delivery bought credit above one");
        if (std::abs(h.miss_probability() - 1.0 / 3.0) > 1e-12) fail("Laplace after one hit");
        FlipCostModel partial;
        partial.record_outcome(0.20, 0.05, true);   // (0.05 + 0.2) / (0.2 + 0.2)
        if (std::abs(partial.kappa() - 0.625) > 1e-12) fail("partial delivery kappa");
        FlipCostModel induced;
        induced.record_outcome(-0.50, -0.60, false);
        if (induced.kappa() != 1.0 || induced.misses != 1 || induced.moves != 1)
            fail("an induced miss must count toward the rate, not the calibration");
    }
    // INVALIDATION. A miss is voided only when the baseline's own move during the excursion would
    // have turned the verdict: -48% delivered against a baseline that came back +2.2% is still
    // -49%; +5% delivered against a baseline that fell 10% is really +17% and the test was voided.
    if (!(flip_corrected_gain(-0.479, 0.022) < -0.48)) fail("a -48% miss survived a 2% wobble");
    if (!(flip_corrected_gain(0.05, -0.10) > 0.16)) fail("a fallen baseline did not void the miss");
    if (std::abs(flip_corrected_gain(0.10, 0.0) - 0.10) > 1e-12) fail("a still baseline changes the gain");
    // REFINEMENT BAR. Owner box, 18:14 boot: the first step projected +36% and delivered +26%, so
    // the model's demonstrated error is 10 points; the second step it would project (13:19 -> 11:21,
    // +10% by work conservation, +2.5% measured) does not clear it at kappa 0.86 and the chain stops
    // one step short (-2.5%) instead of round-tripping or crossing the peak (10:22, -6.6%). A first
    // step that delivered what it promised leaves no bar beyond the noise.
    if (std::abs(flip_refine_bar(0.36, 0.26) - 0.10) > 1e-12) fail("refine bar is not the error");
    if (!(0.86 * 0.10 <= flip_refine_bar(0.36, 0.26))) fail("a +10% projection cleared a 10-point error");
    if (flip_refine_bar(0.30, 0.31) != 0) fail("an over-delivering step left a bar");
    if (flip_refine_bar(4.0, 2.9) < 1.0) fail("a wildly over-projected first step left a small bar");
    // LONG-WINDOW NOISE, DETRENDED. The gate driver's six-second triangle (issue intervals x 0.8,
    // 0.95,1.1,1.2,1.05,0.9) puts per-tick rates at 1.25,1.05,0.91,0.83,0.95,1.11 of the mean:
    // per-reading sigma ~0.15. Two adjacent readings inside one phase read ~0.05%; the target's
    // +24% is that swing. The second-difference estimator must read the swing, and must NOT read
    // the boot ramp (a near-idle first sample, then the full rate) that blew the first form up.
    {
        FlipEwVariance ew; ew.configure(30);
        for (int cycle = 0; cycle < 4; cycle++)
            for (double r : {6050.0, 5100.0, 4400.0, 4030.0, 4600.0, 5370.0}) ew.add(r, 8.0);
        if (!(ew.sigma() > 0.08 && ew.sigma() < 0.40)) fail("the triangle's long-window sigma did not read the swing");
        // The verification threshold it floors refuses the +24% that fooled the pair bands.
        if (!(flip_verify_threshold(ew.sigma(), 2, 1, 0.02) > 0.24))
            fail("the long-window sigma did not floor the threshold above the driver's swing");
        // The boot ramp: idle ticks (skipped), 40/s (skipped as idle), then a fast ramp to a steady
        // 690k. A linear ramp is invisible; only the knee leaves a blip that the warm-up dilutes.
        FlipEwVariance ramp; ramp.configure(30);
        for (double r : {0.0, 3.0, 40.0}) ramp.add(r, 50.0);
        for (double r : {200000.0, 434000.0, 685000.0, 690000.0}) ramp.add(r, 50.0);
        for (int k = 0; k < 12; k++) ramp.add(690000.0 * (1.0 + 0.01 * ((k % 3) - 1)), 50.0);
        // The knee of a fast ramp is real curvature (-36% second difference once); the warm-up
        // dilutes it to ~0.1 within 14 ticks and the 1/30 decay carries it out from there.
        if (!(ramp.sigma() < 0.12)) fail("the boot ramp inflated the long-window sigma");
        // A x3 level step is a new regime: reset, not noise.
        FlipEwVariance step; step.configure(30);
        for (int k = 0; k < 10; k++) step.add(1000.0, 8.0);
        step.add(3000.0, 8.0); step.add(3000.0, 8.0);
        if (step.samples > 1 && step.sigma() > 0.05) fail("a x3 level step was folded in as noise");
        FlipEwVariance still; still.configure(30);
        for (int k = 0; k < 40; k++) still.add(500000.0 * (1.0 + 0.01 * ((k % 3) - 1)), 8.0);
        if (!(still.sigma() < 0.03)) fail("a still load read a large long-window sigma");
        if (FlipEwVariance{}.sigma() != 0) fail("an empty long window has a sigma");
    }
    // The rate window: mean, relative sigma and the bracket band the trend guard uses.
    {
        FlipRateWindow r;
        r.add(4898.601); r.add(6000.916);
        if (std::abs(r.mean - 5449.7585) > 1e-3) fail("rate window mean");
        if (!(r.bracket_band() > 0.22)) fail("rate window bracket did not floor the ramp");
        FlipRateWindow still;
        for (double v : {500000.0, 505000.0, 495000.0, 500000.0}) still.add(v);
        if (std::abs(still.sigma() - 0.00816) > 1e-4) fail("rate window relative sigma");
    }
    return 0;
}
