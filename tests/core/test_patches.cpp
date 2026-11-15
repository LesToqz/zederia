// The hand-written test patches (a bass, a pad, a pluck and a lead) load from JSON without a
// complaint and render as such sounds should: in tune, at a sensible level, shaped by their
// envelopes and modulation, and silent when they are over.
#include "../support/Analysis.h"
#include "../support/TestUtil.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

Patch loadTestPatch(const char* name) {
    const std::string text = zedtest::readText(zedtest::dataPath(std::string("patches/") + name + ".zpreset"));
    REQUIRE_FALSE(text.empty());
    const LoadResult result = loadPatch(text);
    for (const Issue& issue : result.issues)
        WARN(name << ": " << issue.path << " " << issue.message);
    REQUIRE(result.ok());
    REQUIRE(result.issues.empty());
    // What was loaded is what a save writes back.
    const LoadResult again = loadPatch(savePatch(result.patch));
    REQUIRE(again.ok());
    REQUIRE(again.patch == result.patch);
    return result.patch;
}

RenderResult play(const Patch& patch, std::vector<NoteSpec> notes, double bpm = 120.0) {
    RenderSettings settings;
    settings.bpm = bpm;
    const RenderResult r = renderOffline(patch, sequenceFromNotes(notes, kRate), settings);
    bool finite = true;
    for (std::size_t i = 0; i < r.left.size(); ++i)
        finite = finite && std::isfinite(r.left[i]) && std::isfinite(r.right[i]);
    REQUIRE(finite);
    // Over when it is over: the last samples are silent.
    REQUIRE(zedtest::peakOf(r.left, r.left.size() - 64, r.left.size()) < 1.0e-4f);
    return r;
}

std::size_t at(double seconds) { return static_cast<std::size_t>(seconds * kRate); }

// The pitch of the lowest partial: the signal low-passed hard at the expected pitch, then its zero crossings.
double pitchCents(const std::vector<float>& x, double expectedHz, double fromSeconds, double toSeconds) {
    std::vector<float> y = x;
    const double k = 1.0 - std::exp(-2.0 * kPi * expectedHz / kRate);
    for (int pass = 0; pass < 4; ++pass) {
        double state = 0.0;
        for (float& s : y) {
            state += (s - state) * k;
            s = static_cast<float>(state);
        }
    }
    return zedtest::cents(zedtest::measureFrequency(y, at(fromSeconds), at(toSeconds), kRate), expectedHz);
}

} // namespace

TEST_CASE("test patches: the bass is a low, mono, legato sound with a filter that closes", "[patches][render]") {
    const Patch bass = loadTestPatch("bass");
    const RenderResult r = play(bass, {{36, 0.0, 1.5, 0.9}});
    CHECK(r.peak() > 0.1f);
    CHECK(r.peak() < 1.0f);
    // An octave below the key, with the Sub there as well.
    CHECK(std::abs(pitchCents(r.left, noteToHz(24.0), 0.5, 1.4)) < 6.0);
    // The filter envelope closes: the first tenth of a second is brighter than what follows.
    const auto bright = [&](double from) {
        const auto s = zedtest::spectrumOf(r.left, at(from), 4096, kRate);
        double high = 0.0, low = 0.0;
        for (std::size_t bin = 0; bin < s.magnitude.size(); ++bin)
            (static_cast<double>(bin) * kRate / 4096.0 > 400.0 ? high : low) += s.magnitude[bin] * s.magnitude[bin];
        return high / low;
    };
    CHECK(bright(0.0) > bright(0.6) * 3.0);
    // Harder notes open the filter further.
    const RenderResult soft = play(bass, {{36, 0.0, 1.5, 0.2}});
    CHECK(zedtest::rmsOf(r.left, at(0.3), at(1.2)) > zedtest::rmsOf(soft.left, at(0.3), at(1.2)) * 1.2);
    // Legato: a lower key pressed while the first is held slides down to it without a new attack
    // (the patch gives the low note priority); the first key comes back when the lower one is let go.
    const RenderResult legato = play(bass, {{36, 0.0, 2.0, 0.9}, {31, 0.8, 0.5, 0.9}});
    CHECK(std::abs(pitchCents(legato.left, noteToHz(19.0), 0.95, 1.28)) < 10.0);
    CHECK(std::abs(pitchCents(legato.left, noteToHz(24.0), 1.5, 1.9)) < 6.0);
    CHECK(zedtest::largestJump(legato.left, at(0.75), at(0.9)) < zedtest::largestJump(legato.left, at(0.3), at(0.7)) * 1.5f);
    // A higher key does not take the voice away from the low one.
    const RenderResult above = play(bass, {{36, 0.0, 2.0, 0.9}, {43, 0.8, 0.5, 0.9}});
    CHECK(std::abs(pitchCents(above.left, noteToHz(24.0), 1.0, 1.25)) < 6.0);
}

TEST_CASE("test patches: the pad swells, is wide and never the same on both sides", "[patches][render]") {
    const Patch pad = loadTestPatch("pad");
    const RenderResult r = play(pad, {{48, 0.0, 4.0, 0.8}, {52, 0.0, 4.0, 0.8}, {55, 0.0, 4.0, 0.8}, {59, 0.5, 3.5, 0.6}});
    CHECK(r.peak() > 0.05f);
    CHECK(r.peak() < 1.0f);
    // A slow attack and a long release.
    CHECK(zedtest::rmsOf(r.left, at(0.0), at(0.1)) < zedtest::rmsOf(r.left, at(1.5), at(2.5)) * 0.35);
    CHECK(zedtest::rmsOf(r.left, at(4.3), at(4.6)) > zedtest::rmsOf(r.left, at(1.5), at(2.5)) * 0.1);
    CHECK(r.left.size() > at(5.0));
    // Wide: the two sides carry different signals at about the same level.
    std::vector<float> side(r.left.size());
    for (std::size_t i = 0; i < side.size(); ++i)
        side[i] = 0.5f * (r.left[i] - r.right[i]);
    const double left = zedtest::rmsOf(r.left, at(1.0), at(3.5)), right = zedtest::rmsOf(r.right, at(1.0), at(3.5));
    CHECK(zedtest::rmsOf(side, at(1.0), at(3.5)) > 0.2 * left);
    CHECK(left / right == Approx(1.0).margin(0.5));
    // It moves: the level and colour drift from one half second to the next.
    const auto band = [&](double from) {
        const auto s = zedtest::spectrumOf(r.left, at(from), 16384, kRate);
        double high = 0.0;
        for (std::size_t bin = 0; bin < s.magnitude.size(); ++bin)
            if (static_cast<double>(bin) * kRate / 16384.0 > 1500.0)
                high += s.magnitude[bin] * s.magnitude[bin];
        return std::sqrt(high);
    };
    const double a = band(1.2), b = band(2.2), c = band(3.2);
    CHECK(std::max({a, b, c}) / std::min({a, b, c}) > 1.05);
    // The same seed gives the same pad; another seed another one (drift, variance, Flux).
    RenderSettings other;
    other.seed = 99;
    const std::vector<NoteSpec> one = {{48, 0.0, 1.0, 0.8}};
    const auto events = sequenceFromNotes(one, kRate);
    CHECK(renderOffline(pad, events, {}).hash() == renderOffline(pad, events, {}).hash());
    CHECK(renderOffline(pad, events, {}).hash() != renderOffline(pad, events, other).hash());
}

TEST_CASE("test patches: the pluck is short, in tune and follows the tempo", "[patches][render]") {
    const Patch pluck = loadTestPatch("pluck");
    const RenderResult r = play(pluck, {{60, 0.0, 1.0, 0.8}});
    CHECK(r.peak() > 0.1f);
    CHECK(r.peak() < 1.0f);
    // In tune (measured on one unison voice: three of them chorus by design).
    Patch single = pluck;
    single.set("osc1.unison.voices", 1);
    CHECK(std::abs(pitchCents(play(single, {{60, 0.0, 1.0, 0.8}}).left, noteToHz(60.0), 0.02, 0.3)) < 3.0);
    // It dies away while the key is still down: the decay is a quarter note, 500 ms at 120 bpm.
    const double start = zedtest::rmsOf(r.left, at(0.0), at(0.05));
    CHECK(zedtest::rmsOf(r.left, at(0.25), at(0.3)) < start * 0.6);
    CHECK(zedtest::rmsOf(r.left, at(0.45), at(0.5)) < start * 0.1);
    CHECK(zedtest::peakOf(r.left, at(0.55), at(1.0)) < 1.0e-3f);
    // Twice the tempo, half the decay.
    const RenderResult fast = play(pluck, {{60, 0.0, 1.0, 0.8}}, 240.0);
    CHECK(zedtest::peakOf(fast.left, at(0.3), at(1.0)) < 1.0e-3f);
    CHECK(zedtest::rmsOf(fast.left, at(0.15), at(0.2)) < zedtest::rmsOf(r.left, at(0.15), at(0.2)) * 0.7);
    // The multi-segment envelope opens the filter at the start: brighter early than late.
    const auto centroid = [&](double from) {
        const auto s = zedtest::spectrumOf(r.left, at(from), 2048, kRate);
        double weighted = 0.0, total = 0.0;
        for (std::size_t bin = 1; bin < s.magnitude.size(); ++bin) {
            weighted += static_cast<double>(bin) * s.magnitude[bin];
            total += s.magnitude[bin];
        }
        return weighted / total;
    };
    CHECK(centroid(0.0) > centroid(0.2) * 1.3);
    // Every note lands somewhere else in the stereo field.
    const RenderResult phrase = play(pluck, {{60, 0.0, 0.2, 0.8}, {64, 0.6, 0.2, 0.8}, {67, 1.2, 0.2, 0.8}, {72, 1.8, 0.2, 0.8}});
    double low = 1e9, high = 0.0;
    for (const double t : {0.0, 0.6, 1.2, 1.8}) {
        const double balance = zedtest::rmsOf(phrase.left, at(t), at(t + 0.15)) / zedtest::rmsOf(phrase.right, at(t), at(t + 0.15));
        low = std::min(low, balance);
        high = std::max(high, balance);
    }
    CHECK(high / low > 1.1);
}

TEST_CASE("test patches: the lead is mono, glides between notes and brings its vibrato in late", "[patches][render]") {
    const Patch lead = loadTestPatch("lead");
    const RenderResult r = play(lead, {{69, 0.0, 2.0, 0.9}});
    CHECK(r.peak() > 0.1f);
    CHECK(r.peak() < 1.0f);
    // Steady at first; after the delay the vibrato comes in and moves the pitch 24 cents either
    // way (12 % of the fine-tune range). Measured on the first oscillator alone, one unison voice.
    Patch solo = lead;
    solo.set("osc1.unison.voices", 1);
    solo.set("osc2.on", 0);
    const RenderResult v = play(solo, {{69, 0.0, 2.0, 0.9}});
    const auto pitchSpread = [&](double from, double to) {
        double low = 1e9, high = -1e9;
        for (double t = from; t + 0.04 <= to; t += 0.02) {
            const double c = pitchCents(v.left, 440.0, t, t + 0.04);
            low = std::min(low, c);
            high = std::max(high, c);
        }
        return high - low;
    };
    CHECK(pitchSpread(0.05, 0.24) < 4.0);
    CHECK(pitchSpread(1.0, 1.9) > 36.0);
    CHECK(pitchSpread(1.0, 1.9) < 56.0);
    CHECK(std::abs(pitchCents(v.left, 440.0, 1.0, 1.9)) < 5.0); // around the note
    // A second note takes the voice over and slides up to its pitch in 80 ms.
    const RenderResult two = play(lead, {{57, 0.0, 0.5, 0.9}, {69, 0.4, 0.4, 0.9}});
    const double early = pitchCents(two.left, 440.0, 0.405, 0.43), late = pitchCents(two.left, 440.0, 0.5, 0.6);
    CHECK(early < -150.0); // still on its way up from the lower note
    CHECK(std::abs(late) < 6.0);
    // The macro opens the filter.
    Patch bright = lead, dark = lead;
    bright.set("macro1.value", 1.0);
    dark.set("macro1.value", 0.0);
    const RenderResult open = play(bright, {{69, 0.0, 2.0, 0.9}}), closed = play(dark, {{69, 0.0, 2.0, 0.9}});
    const auto highs = [](const RenderResult& x) {
        const auto s = zedtest::spectrumOf(x.left, at(0.5), 16384, kRate);
        double sum = 0.0;
        for (std::size_t bin = 0; bin < s.magnitude.size(); ++bin)
            if (static_cast<double>(bin) * kRate / 16384.0 > 4000.0)
                sum += s.magnitude[bin] * s.magnitude[bin];
        return std::sqrt(sum);
    };
    CHECK(highs(open) > highs(closed) * 1.5);
}
