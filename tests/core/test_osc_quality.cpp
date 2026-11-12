// Oscillator quality (plan 5.7, 6.1): aliasing per quality mode, the click test on position
// sweeps, and the decimators the quality modes rest on.
#include "../support/Analysis.h"
#include "../support/TestUtil.h"
#include "zederia/dsp/HalfBand.h"
#include "zederia/util/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>

using namespace zed;

namespace {

constexpr double kRate = 44100.0;
constexpr int kFftSize = 131072;

int choiceOf(const char* id, const char* token) { return Registry::get().find(id)->choiceIndex(token); }

// A held note with nothing but the oscillator: full level, no envelope movement, fixed phase.
Patch plainPatch(const char* quality) {
    Patch p;
    p.set("osc1.phase_random", 0);
    p.set("osc1.level", 0);
    p.set("global.master", 0);
    p.set("env1.attack", 0);
    p.set("env1.sustain", 1);
    p.set("global.quality", choiceOf("global.quality", quality));
    p.set("global.quality_offline", choiceOf("global.quality_offline", quality));
    return p;
}

zedtest::HarmonicReport analyse(const Patch& patch, double note, double maxHz = 0.0) {
    Patch p = patch;
    const int key = static_cast<int>(std::floor(note));
    p.set("osc1.coarse", p.get("osc1.coarse") + (note - key));
    const double seconds = 0.2 + kFftSize / kRate + 0.05;
    const RenderResult r = zedtest::renderNote(p, key, seconds, kRate);
    const auto s = zedtest::spectrumOf(r.left, static_cast<std::size_t>(0.2 * kRate), kFftSize, kRate);
    const double f0 = noteToHz(note + patch.get("osc1.coarse") + patch.get("osc1.octave") * 12.0);
    return zedtest::nonHarmonicLevel(s, f0, maxHz);
}

// The notes of the sweep: C0 to C8, on and between the keys.
std::vector<double> sweepNotes() {
    std::vector<double> notes;
#ifdef NDEBUG
    for (int n = 12; n <= 108; ++n)
        notes.push_back(n + ((n * 37) % 100) / 100.0 * (n < 108 ? 1.0 : 0.0));
    for (int n = 12; n <= 108; n += 12)
        notes.push_back(n);
#else
    for (int n = 12; n <= 108; n += 8) // the unoptimized build measures a thinner set
        notes.push_back(n + ((n * 37) % 100) / 100.0 * (n < 108 ? 1.0 : 0.0));
#endif
    return notes;
}

double worstOverSweep(const Patch& patch, double* atNote = nullptr, double* atHz = nullptr) {
    double worst = -400.0;
    for (const double note : sweepNotes()) {
        const auto r = analyse(patch, note);
        if (r.worstDbc > worst) {
            worst = r.worstDbc;
            if (atNote != nullptr)
                *atNote = note;
            if (atHz != nullptr)
                *atHz = r.worstHz;
        }
    }
    return worst;
}

} // namespace

TEST_CASE("quality: a full-spectrum sawtooth swept C0 to C8 at 44.1 kHz stays clean in every mode", "[osc][aliasing]") {
    // Plan 5.7: non-harmonic components at or below -80 dBc in Standard and -96 dBc in High.
    struct Mode {
        const char* name;
        double limit;
    };
    for (const Mode mode : {Mode{"draft", -80.0}, Mode{"standard", -80.0}, Mode{"high", -96.0}, Mode{"ultra", -96.0}}) {
        double note = 0.0, hz = 0.0;
        const double worst = worstOverSweep(plainPatch(mode.name), &note, &hz);
        char line[160];
        std::snprintf(line, sizeof line, "aliasing, plain sawtooth, %s: worst non-harmonic component %.1f dBc (note %.2f, at %.0f Hz)",
                      mode.name, worst, note, hz);
        WARN(line);
        CHECK(worst <= mode.limit);
    }
}

TEST_CASE("quality: the sawtooth keeps its harmonics up to at least 17.5 kHz", "[osc][aliasing]") {
    // Band-limiting must not dull the sound: three levels per octave leave at most a third of
    // an octave below half the sample rate empty.
    const Patch patch = plainPatch("standard");
    for (const double note : {24.0, 36.3, 47.9, 60.0, 71.5, 84.2}) {
        Patch p = patch;
        const int key = static_cast<int>(std::floor(note));
        p.set("osc1.coarse", note - key);
        const RenderResult r = zedtest::renderNote(p, key, 0.2 + kFftSize / kRate + 0.05, kRate);
        const auto s = zedtest::spectrumOf(r.left, static_cast<std::size_t>(0.2 * kRate), kFftSize, kRate);
        const double f0 = noteToHz(note);
        const auto level = [&](int harmonic) { return zedtest::lineLevel(s, harmonic * f0); };
        const int top = static_cast<int>(17500.0 / f0); // the highest harmonic below 17.5 kHz
        INFO("note " << note << ", harmonic " << top << " at " << top * f0 << " Hz");
        // A sawtooth's harmonic h has 1/h of the fundamental's level; allow 0.5 dB either way.
        const double ratioDb = 20.0 * std::log10(level(top) * top / level(1));
        CHECK(std::abs(ratioDb) < 0.5);
        // And nothing at all above half the sample rate folds back: checked by the test above.
    }
}

TEST_CASE("quality: warped and modulated paths alias less as the quality rises", "[osc][aliasing]") {
    // Hard sync creates an edge that no table can band-limit; only oversampling helps. The
    // strongest non-harmonic component must fall from Draft to Standard to High.
    double previous = 0.0;
    for (const char* mode : {"draft", "standard", "high", "ultra"}) {
        Patch p = plainPatch(mode);
        p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "sync"));
        p.set("osc1.warp_a.amount", 0.37);
        double worst = -400.0;
        for (const double note : {45.3, 57.7, 69.1, 76.4})
            worst = std::max(worst, analyse(p, note, 20000.0).worstDbc);
        char line[160];
        std::snprintf(line, sizeof line, "aliasing, hard sync, %s: worst non-harmonic component below 20 kHz %.1f dBc", mode, worst);
        WARN(line);
        if (std::string(mode) != "draft" && std::string(mode) != "ultra")
            CHECK(worst < previous - 6.0);
        if (std::string(mode) == "ultra")
            CHECK(worst <= previous + 1.0);
        previous = worst;
    }
}

TEST_CASE("quality: a warp slot at zero amount leaves the sawtooth clean", "[osc][aliasing]") {
    // The oversampled path with nothing to do must not add anything of its own.
    Patch p = plainPatch("standard");
    p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "bend_plus"));
    p.set("osc1.warp_a.amount", 0.0);
    double worst = -400.0;
    for (const double note : {24.0, 40.6, 60.0, 77.3, 96.0})
        worst = std::max(worst, analyse(p, note).worstDbc);
    WARN("aliasing, oversampled path with a neutral warp: " << worst << " dBc");
    CHECK(worst <= -80.0);
}

TEST_CASE("quality: the decimators reject what would fold into the audible band", "[quality]") {
    for (const bool steep : {false, true}) {
        const auto& design = dsp::decimatorDesign(steep);
        const double wanted = steep ? -110.0 : -90.0;
        // Last stage, 2x to 1x: flat to 20 kHz at 44.1 kHz, stop band from 24.1 kHz.
        double ripple = 0.0, stop = 0.0;
        for (double f = 0.0; f <= 20000.0 / 88200.0; f += 0.0005)
            ripple = std::max(ripple, std::abs(20.0 * std::log10(dsp::halfBandMagnitude(design.stage[0], f))));
        for (double f = 24100.0 / 88200.0; f <= 0.5; f += 0.0005)
            stop = std::max(stop, dsp::halfBandMagnitude(design.stage[0], f));
        CHECK(ripple < 0.01);
        CHECK(20.0 * std::log10(stop) < wanted);
        // Earlier stages only have to remove what lands below half the base rate after folding.
        for (double f = 0.375; f <= 0.5; f += 0.0005) // 4x: 1.5 to 2 times the base rate
            CHECK(20.0 * std::log10(dsp::halfBandMagnitude(design.stage[1], f)) < wanted);
        for (double f = 0.4375; f <= 0.5; f += 0.0005) // 8x: 3.5 to 4 times the base rate
            CHECK(20.0 * std::log10(dsp::halfBandMagnitude(design.stage[2], f)) < wanted);
    }
    // The running filter matches its analytic response.
    const auto& k = dsp::decimatorDesign(false).stage[0];
    for (const double f : {0.05, 0.2, 0.3, 0.42}) {
        dsp::HalfBandState state;
        const int frames = 20000;
        std::vector<float> in(2 * frames), out(frames);
        for (int i = 0; i < 2 * frames; ++i)
            in[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(2.0 * kPi * f * i));
        dsp::halfBandDecimate(k, state, in.data(), out.data(), frames);
        const double measured = zedtest::rmsOf(out, 10000, 20000) * std::sqrt(2.0);
        const double expected = dsp::halfBandMagnitude(k, f);
        INFO("f = " << f);
        if (expected > 1e-3)
            CHECK(std::abs(measured / expected - 1.0) < 0.01);
        else
            CHECK(measured < 2e-5); // float arithmetic sets the floor
    }
}

TEST_CASE("quality: no mode adds latency", "[quality]") {
    // The same note in every mode starts at the same sample (plan 5.7: low-latency filters).
    for (const char* mode : {"draft", "standard", "high", "ultra"}) {
        Patch p = plainPatch(mode);
        p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "drive"));
        p.set("osc1.warp_a.amount", 0.5);
        Engine engine;
        engine.prepare(48000.0);
        engine.loadPatch(p);
        CHECK(engine.latencySamples() == 0);
        std::vector<float> left(256), right(256);
        const MidiEvent on[] = {MidiEvent::noteOn(100, 60, 1.0f)};
        engine.process({left.data(), right.data(), 256, on, {}});
        INFO(mode);
        CHECK(zedtest::peakOf(left, 0, 101) == 0.0f);
        CHECK(zedtest::peakOf(left, 101, 110) > 0.0f);
    }
}
