// The wavetable toolkit (plan 3.10, 6.1): every frame, drawing, harmonic, process, morph and
// sort operation against inputs whose result is known.
#include "../support/TestUtil.h"
#include "zederia/audio/WavFile.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;

namespace {

Frame sineFrame(int harmonic, double phase = 0.0, double level = 1.0) {
    Frame f;
    for (int n = 0; n < kFrameSize; ++n)
        f[static_cast<std::size_t>(n)] = static_cast<float>(level * std::sin(2.0 * kPi * harmonic * n / kFrameSize + phase));
    return f;
}

// Frames that can be told apart: frame k is a sine at harmonic k + 1.
Wavetable ladder(int frames) {
    Wavetable t;
    for (int k = 0; k < frames; ++k)
        t.frames.push_back(sineFrame(k + 1));
    return t;
}

// Which harmonic a ladder frame is.
int id(const Frame& f) {
    const Harmonics h = analyzeFrame(f);
    return static_cast<int>(std::max_element(h.magnitude.begin() + 1, h.magnitude.end()) - h.magnitude.begin());
}

std::vector<int> ids(const Wavetable& t) {
    std::vector<int> out;
    for (const Frame& f : t.frames)
        out.push_back(id(f));
    return out;
}

double maxDifference(const Frame& a, const Frame& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max<double>(worst, std::abs(a[i] - b[i]));
    return worst;
}

double level(const Frame& f, int harmonic) { return analyzeFrame(f).magnitude[static_cast<std::size_t>(harmonic)]; }

} // namespace

TEST_CASE("toolkit: frame strip operations", "[toolkit][frames]") {
    Wavetable t = ladder(5);
    using V = std::vector<int>;

    REQUIRE(insertFrame(t, 2, sineFrame(9)));
    CHECK(ids(t) == V{1, 2, 9, 3, 4, 5});
    REQUIRE(insertFrame(t, 6, sineFrame(8))); // at the end
    CHECK(ids(t) == V{1, 2, 9, 3, 4, 5, 8});
    CHECK_FALSE(insertFrame(t, 9, sineFrame(1)));
    CHECK_FALSE(insertFrame(t, -1, sineFrame(1)));

    REQUIRE(deleteFrames(t, {2, 2}));
    REQUIRE(deleteFrames(t, oneFrame(5)));
    CHECK(ids(t) == V{1, 2, 3, 4, 5});
    CHECK_FALSE(deleteFrames(t, allFrames())); // at least one frame stays

    REQUIRE(duplicateFrames(t, {1, 2}));
    CHECK(ids(t) == V{1, 2, 3, 2, 3, 4, 5});
    REQUIRE(deleteFrames(t, {3, 4}));

    REQUIRE(moveFrame(t, 0, 3));
    CHECK(ids(t) == V{2, 3, 4, 1, 5});
    REQUIRE(moveFrame(t, 3, 0));
    CHECK(ids(t) == V{1, 2, 3, 4, 5});
    CHECK_FALSE(moveFrame(t, 0, 5));

    reverseFrames(t);
    CHECK(ids(t) == V{5, 4, 3, 2, 1});
    reverseFrames(t);

    Wavetable thin = ladder(10);
    REQUIRE(thinFrames(thin, 3));
    CHECK(ids(thin) == V{1, 4, 7, 10});
    CHECK_FALSE(thinFrames(thin, 0));

    REQUIRE(cropFrames(t, {1, 3}));
    CHECK(ids(t) == V{2, 3, 4});
    CHECK_FALSE(cropFrames(t, {5, 7}));

    // The table never grows past 256 frames.
    Wavetable full = ladder(200);
    CHECK_FALSE(duplicateFrames(full, {0, 99}));
    CHECK(duplicateFrames(full, {0, 55}));
    CHECK(full.size() == 256);
    CHECK_FALSE(insertFrame(full, 0, sineFrame(1)));
}

TEST_CASE("toolkit: drawing tools", "[toolkit][draw]") {
    Frame f{};
    drawSegment(f, 0.0, 0.5, DrawShape::Flat, 0.5f, 0.0f);
    CHECK(f[0] == 0.5f);
    CHECK(f[1023] == 0.5f);
    CHECK(f[1024] == 0.0f);

    drawSegment(f, 0.5, 1.0, DrawShape::Line, -1.0f, 1.0f);
    CHECK(f[1024] == -1.0f);
    CHECK(f[2047] == 1.0f);
    CHECK(f[1536] == Approx(0.0f).margin(0.002));
    drawSegment(f, 0.0, 1.0, DrawShape::RampUp, -1.0f, 1.0f);
    CHECK(f[0] == -1.0f);
    CHECK(f[2047] == 1.0f);
    drawSegment(f, 0.0, 1.0, DrawShape::RampDown, -1.0f, 1.0f);
    CHECK(f[0] == 1.0f);
    CHECK(f[2047] == -1.0f);

    drawSegment(f, 0.0, 1.0, DrawShape::Sine, 0.0f, 1.0f);
    CHECK(f[512] == Approx(1.0f).margin(0.001));
    CHECK(f[1536] == Approx(-1.0f).margin(0.001));
    drawSegment(f, 0.0, 1.0, DrawShape::HalfSineUp, 0.0f, 1.0f);
    CHECK(f[0] == 0.0f);
    CHECK(f[1024] == Approx(1.0f).margin(0.001));
    CHECK(f[2047] == Approx(0.0f).margin(0.001));
    drawSegment(f, 0.0, 1.0, DrawShape::HalfSineDown, 0.0f, 1.0f);
    CHECK(f[1024] == Approx(-1.0f).margin(0.001));

    // Quarter curves: one starts flat, the other ends flat.
    drawSegment(f, 0.0, 1.0, DrawShape::QuarterIn, 0.0f, 1.0f);
    CHECK(f[2047] == Approx(1.0f).margin(0.001));
    CHECK(f[512] < 0.1f);
    drawSegment(f, 0.0, 1.0, DrawShape::QuarterOut, 0.0f, 1.0f);
    CHECK(f[2047] == Approx(1.0f).margin(0.001));
    CHECK(f[512] > 0.35f);
    // Curved interpolation between two points: level at both ends, halfway in the middle.
    drawSegment(f, 0.25, 0.75, DrawShape::Curve, -0.5f, 0.5f);
    CHECK(f[512] == -0.5f);
    CHECK(f[1535] == 0.5f);
    CHECK(std::abs(f[513] - f[512]) < 1e-4f); // flat where it starts
    CHECK(f[1024] == Approx(0.0f).margin(0.002));

    // Nudge and noise touch only their segment.
    Frame g{};
    nudgeSegment(g, 0.25, 0.5, 0.2f);
    CHECK(g[511] == 0.0f);
    CHECK(g[512] == 0.2f);
    CHECK(g[1023] == 0.2f);
    CHECK(g[1024] == 0.0f);
    Frame n1{}, n2{}, n3{};
    addNoise(n1, 0.0, 0.5, 0.1f, 5);
    addNoise(n2, 0.0, 0.5, 0.1f, 5);
    addNoise(n3, 0.0, 0.5, 0.1f, 6);
    CHECK(n1 == n2);        // seeded
    CHECK_FALSE(n1 == n3);
    CHECK(framePeak(n1) <= 0.1f);
    CHECK(framePeak(n1) > 0.05f);
    CHECK(n1[1500] == 0.0f);

    // Reversed ends mean the same segment.
    Frame a{}, b{};
    drawSegment(a, 0.2, 0.6, DrawShape::Flat, 1.0f, 0.0f);
    drawSegment(b, 0.6, 0.2, DrawShape::Flat, 1.0f, 0.0f);
    CHECK(a == b);

    CHECK(snapToGrid(0.26, 8) == Approx(0.25));
    CHECK(snapToGrid(0.32, 8) == Approx(0.375));
    CHECK(snapToGrid(0.99, 4) == Approx(1.0));
    CHECK(snapToGrid(0.123, 0) == Approx(0.123));
}

TEST_CASE("toolkit: harmonic operations", "[toolkit][harmonics]") {
    Frame saw{};
    applyHarmonicOp(saw, HarmonicOp::FillSaw);
    CHECK(level(saw, 1) == Approx(1.0).epsilon(1e-5));
    CHECK(level(saw, 7) == Approx(1.0 / 7.0).epsilon(1e-4));
    CHECK(level(saw, 1023) == Approx(1.0 / 1023.0).epsilon(1e-3));
    CHECK(saw[100] < saw[1900]); // a rising ramp

    Frame f = saw;
    applyHarmonicOp(f, HarmonicOp::ClearAbove, 10);
    CHECK(level(f, 10) == Approx(0.1).epsilon(1e-4));
    CHECK(level(f, 11) < 1e-7);
    f = saw;
    applyHarmonicOp(f, HarmonicOp::ClearBelow, 4);
    CHECK(level(f, 3) < 1e-7);
    CHECK(level(f, 4) == Approx(0.25).epsilon(1e-4));
    f = saw;
    applyHarmonicOp(f, HarmonicOp::ClearAll);
    CHECK(framePeak(f) < 1e-6f);

    f = saw;
    applyHarmonicOp(f, HarmonicOp::OddOnly);
    CHECK(level(f, 2) < 1e-7);
    CHECK(level(f, 3) == Approx(1.0 / 3.0).epsilon(1e-4));
    f = saw;
    applyHarmonicOp(f, HarmonicOp::EvenOnly);
    CHECK(level(f, 1) < 1e-7);
    CHECK(level(f, 2) == Approx(0.5).epsilon(1e-4));

    f = saw;
    applyHarmonicOp(f, HarmonicOp::OctaveUp);
    CHECK(level(f, 1) < 1e-7);
    CHECK(level(f, 2) == Approx(1.0).epsilon(1e-4));
    CHECK(level(f, 6) == Approx(1.0 / 3.0).epsilon(1e-4));
    applyHarmonicOp(f, HarmonicOp::OctaveDown);
    CHECK(level(f, 1) == Approx(1.0).epsilon(1e-4));
    CHECK(level(f, 3) == Approx(1.0 / 3.0).epsilon(1e-4));

    f = saw;
    applyHarmonicOp(f, HarmonicOp::ScaleByHarmonic, 0, 1.0); // 1/h times h: flat
    CHECK(level(f, 9) == Approx(1.0).epsilon(1e-3));
    f = saw;
    applyHarmonicOp(f, HarmonicOp::FadeHighs, 0, 1.0); // 1/h times 1/h
    CHECK(level(f, 4) == Approx(1.0 / 16.0).epsilon(1e-4));

    f = saw;
    applyHarmonicOp(f, HarmonicOp::SnapQuarters);
    CHECK(level(f, 1) == Approx(1.0).epsilon(1e-4));
    CHECK(level(f, 2) == Approx(0.5).epsilon(1e-4));
    CHECK(level(f, 3) == Approx(0.25).epsilon(1e-4)); // 0.33 snaps to a quarter
    CHECK(level(f, 9) < 1e-7);                        // 0.11 snaps to nothing

    // Randomizing: only the asked harmonics, within the asked height, and repeatable.
    Frame r1{}, r2{}, r3{};
    applyHarmonicOp(r1, HarmonicOp::RandomizeLowest, 8, 0.5, 3);
    applyHarmonicOp(r2, HarmonicOp::RandomizeLowest, 8, 0.5, 3);
    applyHarmonicOp(r3, HarmonicOp::RandomizeLowest, 8, 1.0, 4);
    CHECK(r1 == r2);
    CHECK_FALSE(r1 == r3);
    double tallest = 0.0;
    for (int k = 1; k <= 8; ++k)
        tallest = std::max(tallest, level(r1, k));
    CHECK(tallest <= 0.5);
    CHECK(tallest > 0.1);
    CHECK(level(r1, 9) < 1e-7);
    Frame all{};
    applyHarmonicOp(all, HarmonicOp::RandomizeAll, 0, 1.0, 9);
    int present = 0;
    for (int k = 1; k < kMaxHarmonics; ++k)
        present += level(all, k) > 1e-4;
    CHECK(present > 1000);
    Frame gaps = saw;
    applyHarmonicOp(gaps, HarmonicOp::RandomGaps, 0, 0.5, 2);
    int removed = 0;
    for (int k = 1; k <= 200; ++k)
        removed += level(gaps, k) < 1e-7;
    CHECK(removed > 60);
    CHECK(removed < 140);

    HarmonicOp op = HarmonicOp::ClearAll;
    CHECK(harmonicOpFromName("fade_highs", op));
    CHECK(op == HarmonicOp::FadeHighs);
    CHECK_FALSE(harmonicOpFromName("fade", op));
}

TEST_CASE("toolkit: process operations", "[toolkit][process]") {
    SECTION("normalize each or to a shared peak") {
        Wavetable t;
        t.frames = {sineFrame(1, 0, 0.5), sineFrame(2, 0, 0.25), Frame{}};
        Wavetable each = t;
        processFrames(each, allFrames(), ProcessOp::NormalizeEach);
        CHECK(framePeak(each.frames[0]) == Approx(1.0f).epsilon(1e-5));
        CHECK(framePeak(each.frames[1]) == Approx(1.0f).epsilon(1e-5));
        CHECK(framePeak(each.frames[2]) == 0.0f); // silence stays silence
        processFrames(t, allFrames(), ProcessOp::NormalizeShared);
        CHECK(framePeak(t.frames[0]) == Approx(1.0f).epsilon(1e-5));
        CHECK(framePeak(t.frames[1]) == Approx(0.5f).epsilon(1e-5)); // the balance between frames is kept
        // One frame only.
        Wavetable one;
        one.frames = {sineFrame(1, 0, 0.5), sineFrame(2, 0, 0.25)};
        processFrames(one, oneFrame(1), ProcessOp::NormalizeEach);
        CHECK(framePeak(one.frames[0]) == Approx(0.5f).epsilon(1e-5));
        CHECK(framePeak(one.frames[1]) == Approx(1.0f).epsilon(1e-5));
    }
    SECTION("remove DC, flip, mirror, clear") {
        Wavetable t;
        t.frames = {sineFrame(1)};
        for (float& s : t.frames[0])
            s += 0.25f;
        processFrames(t, allFrames(), ProcessOp::RemoveDc);
        CHECK(std::abs(analyzeFrame(t.frames[0]).dc) < 1e-6);
        CHECK(maxDifference(t.frames[0], sineFrame(1)) < 1e-5);

        processFrames(t, allFrames(), ProcessOp::FlipPolarity);
        CHECK(maxDifference(t.frames[0], sineFrame(1, kPi)) < 1e-5);

        t.frames = {sineFrame(1, 0.5)};
        const Frame before = t.frames[0];
        processFrames(t, allFrames(), ProcessOp::MirrorTime);
        for (std::size_t i = 0; i < before.size(); ++i)
            REQUIRE(t.frames[0][i] == before[before.size() - 1 - i]);

        processFrames(t, allFrames(), ProcessOp::Clear);
        CHECK(framePeak(t.frames[0]) == 0.0f);
    }
    SECTION("shift to zero crossing and align the fundamental") {
        Wavetable t;
        t.frames = {sineFrame(1, 1.0)}; // starts mid-wave
        processFrames(t, allFrames(), ProcessOp::ShiftToZeroCrossing);
        CHECK(t.frames[0][0] >= 0.0f);
        CHECK(t.frames[0][0] < 0.004f); // within one sample of the crossing
        CHECK(t.frames[0][kFrameSize - 1] < 0.0f);
        CHECK(level(t.frames[0], 1) == Approx(1.0).epsilon(1e-5)); // only rotated

        // Alignment is exact (a rotation by a fraction of a sample) and carries the harmonics along.
        Frame mixed;
        const double shift = 0.61803;
        for (int n = 0; n < kFrameSize; ++n) {
            const double x = 2.0 * kPi * n / kFrameSize + shift;
            mixed[static_cast<std::size_t>(n)] = static_cast<float>(0.6 * std::sin(x) + 0.3 * std::sin(3.0 * x + 0.4));
        }
        t.frames = {mixed};
        processFrames(t, allFrames(), ProcessOp::AlignFundamental);
        const Harmonics h = analyzeFrame(t.frames[0]);
        CHECK(h.phase[1] == Approx(0.0).margin(1e-5));
        CHECK(h.phase[3] == Approx(0.4).margin(1e-4));
        CHECK(h.magnitude[3] == Approx(0.3).epsilon(1e-4));
    }
    SECTION("fade and crossfade the edges") {
        Wavetable t;
        t.frames = {Frame{}};
        t.frames[0].fill(1.0f);
        processFrames(t, allFrames(), ProcessOp::FadeEdges, 100);
        CHECK(t.frames[0][0] == 0.0f);
        CHECK(t.frames[0][50] == Approx(0.5f));
        CHECK(t.frames[0][100] == 1.0f);
        CHECK(t.frames[0][kFrameSize - 1] == 0.0f);
        CHECK(t.frames[0][kFrameSize - 51] == Approx(0.5f));

        // A ramp jumps from +1 back to -1 at the loop point; crossfading the edges closes the gap.
        Frame ramp;
        for (int n = 0; n < kFrameSize; ++n)
            ramp[static_cast<std::size_t>(n)] = static_cast<float>(2.0 * n / kFrameSize - 1.0);
        t.frames = {ramp};
        processFrames(t, allFrames(), ProcessOp::CrossfadeEdges, 256);
        CHECK(std::abs(t.frames[0][kFrameSize - 1] - t.frames[0][0]) < 0.01f);
        CHECK(t.frames[0][1000] == ramp[1000]); // the rest is untouched
        float jump = 0.0f;
        for (std::size_t i = 1; i < t.frames[0].size(); ++i)
            jump = std::max(jump, std::abs(t.frames[0][i] - t.frames[0][i - 1]));
        CHECK(jump < 0.02f);
    }
    SECTION("low-pass by harmonic count, remove the fundamental, reduce the sample rate") {
        Wavetable t;
        t.frames = {Frame{}};
        applyHarmonicOp(t.frames[0], HarmonicOp::FillSaw);
        processFrames(t, allFrames(), ProcessOp::LowPass, 16);
        CHECK(level(t.frames[0], 16) == Approx(1.0 / 16.0).epsilon(1e-4));
        CHECK(level(t.frames[0], 17) < 1e-7);
        processFrames(t, allFrames(), ProcessOp::RemoveFundamental);
        CHECK(level(t.frames[0], 1) < 1e-7);
        CHECK(level(t.frames[0], 2) == Approx(0.5).epsilon(1e-4));

        t.frames = {sineFrame(1)};
        processFrames(t, allFrames(), ProcessOp::SampleRateReduce, 8);
        int values = 0;
        for (int n = 1; n < kFrameSize; ++n)
            values += t.frames[0][static_cast<std::size_t>(n)] != t.frames[0][static_cast<std::size_t>(n - 1)];
        CHECK(values == 7); // eight held steps
        CHECK(t.frames[0][256] == sineFrame(1)[256]);
        CHECK(t.frames[0][300] == sineFrame(1)[256]);
    }
    SECTION("names") {
        ProcessOp op = ProcessOp::Clear;
        for (const char* name : {"normalize_each", "normalize_shared", "remove_dc", "flip_polarity", "mirror_time",
                                 "shift_to_zero_crossing", "clear", "fade_edges", "crossfade_edges", "low_pass",
                                 "remove_fundamental", "sample_rate_reduce", "align_fundamental"})
            CHECK(processOpFromName(name, op));
        CHECK_FALSE(processOpFromName("normalise", op));
    }
}

TEST_CASE("toolkit: split, join and the pulse-width sweep", "[toolkit][process]") {
    // A frame with one cycle of harmonic 2 in its first half and harmonic 4 ... is simply a
    // frame holding two different waves side by side.
    Frame sideBySide;
    for (int n = 0; n < kFrameSize / 2; ++n) {
        sideBySide[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(2.0 * kPi * n / (kFrameSize / 2)));
        sideBySide[static_cast<std::size_t>(n + kFrameSize / 2)] = static_cast<float>(std::sin(2.0 * kPi * 3 * n / (kFrameSize / 2)));
    }
    Wavetable t;
    t.frames = {sideBySide};
    REQUIRE(splitFrames(t));
    REQUIRE(t.size() == 2);
    CHECK(maxDifference(t.frames[0], sineFrame(1)) < 1e-4);
    CHECK(maxDifference(t.frames[1], sineFrame(3)) < 1e-4);
    joinFramePairs(t);
    REQUIRE(t.size() == 1);
    CHECK(maxDifference(t.frames[0], sideBySide) < 1e-4);

    Wavetable odd = ladder(3);
    joinFramePairs(odd);
    REQUIRE(odd.size() == 2);
    CHECK(framePeak(odd.frames[1]) == Approx(1.0f).epsilon(1e-3));
    bool secondHalfSilent = true;
    for (int n = kFrameSize / 2; n < kFrameSize; ++n)
        secondHalfSilent = secondHalfSilent && odd.frames[1][static_cast<std::size_t>(n)] == 0.0f;
    CHECK(secondHalfSilent);
    Wavetable big = ladder(200);
    CHECK_FALSE(splitFrames(big));

    const Wavetable sweep = pulseWidthSweep(sineFrame(1), 5, 0.2);
    REQUIRE(sweep.size() == 5);
    CHECK(maxDifference(sweep.frames[0], sineFrame(1)) < 1e-5); // full width: unchanged
    const Frame& narrow = sweep.frames[4];                      // the cycle in the first fifth of the period
    CHECK(narrow[static_cast<std::size_t>(kFrameSize / 20)] == Approx(1.0f).margin(0.01)); // its peak
    for (int n = kFrameSize / 5 + 1; n < kFrameSize; ++n)
        REQUIRE(narrow[static_cast<std::size_t>(n)] == 0.0f);
    const Frame& half = sweep.frames[2]; // width 0.6
    CHECK(half[static_cast<std::size_t>(kFrameSize * 3 / 20)] == Approx(1.0f).margin(0.01));
}

TEST_CASE("toolkit: copy a spectrum or phases to all frames, subtract another table's spectrum", "[toolkit][process]") {
    Frame source;
    for (int n = 0; n < kFrameSize; ++n) {
        const double x = 2.0 * kPi * n / kFrameSize;
        source[static_cast<std::size_t>(n)] = static_cast<float>(0.7 * std::sin(x + 0.3) + 0.2 * std::sin(4.0 * x - 1.0));
    }
    Wavetable t;
    t.frames = {source, sineFrame(1, 2.0, 0.4), sineFrame(5, 0.5)};

    Wavetable levels = t;
    copySpectrumToAll(levels, 0);
    Harmonics h = analyzeFrame(levels.frames[1]);
    CHECK(h.magnitude[1] == Approx(0.7).epsilon(1e-4));
    CHECK(h.magnitude[4] == Approx(0.2).epsilon(1e-4));
    CHECK(h.phase[1] == Approx(2.0).margin(1e-4));  // its own phase where it had the harmonic
    CHECK(h.phase[4] == Approx(-1.0).margin(1e-4)); // the source's where it had none
    h = analyzeFrame(levels.frames[2]);
    CHECK(h.magnitude[5] < 1e-7);
    CHECK(maxDifference(levels.frames[0], source) < 1e-6);

    Wavetable phases = t;
    copyPhasesToAll(phases, 0);
    h = analyzeFrame(phases.frames[1]);
    CHECK(h.magnitude[1] == Approx(0.4).epsilon(1e-4)); // levels stay
    CHECK(h.phase[1] == Approx(0.3).margin(1e-4));

    Wavetable minus;
    minus.frames = {sineFrame(1, 0.0, 0.5)};
    Wavetable result = t;
    subtractSpectrum(result, minus);
    CHECK(level(result.frames[0], 1) == Approx(0.2).epsilon(1e-3));
    CHECK(level(result.frames[0], 4) == Approx(0.2).epsilon(1e-3));
    CHECK(level(result.frames[1], 1) < 1e-7); // never below zero
    CHECK(level(result.frames[2], 5) == Approx(1.0).epsilon(1e-4));
}

TEST_CASE("toolkit: morph fills between key frames and can be baked or removed", "[toolkit][morph]") {
    Wavetable t;
    t.frames = {sineFrame(1), sineFrame(1, kPi)};
    t.morph = MorphMode::Crossfade;
    Wavetable crossfade = t;
    bakeMorph(crossfade, 5);
    CHECK(crossfade.size() == 5);
    CHECK(crossfade.morph == MorphMode::None);
    CHECK(framePeak(crossfade.frames[2]) < 1e-5f); // the middle cancels
    CHECK(maxDifference(crossfade.frames[0], t.frames[0]) < 1e-6);
    CHECK(maxDifference(crossfade.frames[4], t.frames[1]) < 1e-6);

    for (const MorphMode mode : {MorphMode::Spectral, MorphMode::SpectralAligned, MorphMode::SpectralZero}) {
        Wavetable spectral = t;
        spectral.morph = mode;
        bakeMorph(spectral, 5);
        CHECK(level(spectral.frames[2], 1) == Approx(1.0).epsilon(1e-4)); // spectral morphs do not cancel
    }
    // Remove morph: the key frames are all there is.
    t.morph = MorphMode::None;
    bakeMorph(t, 256);
    CHECK(t.size() == 256);
}

TEST_CASE("toolkit: sorting frames", "[toolkit][sort]") {
    Frame dark = sineFrame(1), mid{}, bright{}, formant{};
    for (int n = 0; n < kFrameSize; ++n) {
        const double x = 2.0 * kPi * n / kFrameSize;
        mid[static_cast<std::size_t>(n)] = static_cast<float>(0.2 * std::sin(x) + std::sin(3.0 * x) + 0.05 * std::sin(5.0 * x));
        bright[static_cast<std::size_t>(n)] = static_cast<float>(0.5 * std::sin(x) + 0.3 * std::sin(40.0 * x) + 0.6 * std::sin(41.0 * x));
        formant[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(2.0 * x) + 0.4 * std::sin(12.0 * x) + 0.0005 * std::sin(90.0 * x));
    }
    CHECK(sortValue(dark, SortKey::StrongestPartial) == 1);
    CHECK(sortValue(mid, SortKey::StrongestPartial) == 3);
    CHECK(sortValue(bright, SortKey::StrongestPartial) == 41);
    CHECK(sortValue(mid, SortKey::ActivePartials) == 3);
    CHECK(sortValue(formant, SortKey::ActivePartials) == 2); // the faint harmonic 90 is below -60 dB
    CHECK(sortValue(mid, SortKey::HighestPartial) == 5);
    CHECK(sortValue(formant, SortKey::HighestPartial) == 12);
    CHECK(sortValue(formant, SortKey::BrightestPeak) == 12); // the highest prominent peak
    CHECK(sortValue(mid, SortKey::BrightestPeak) == 3);      // harmonic 5 is too weak to count
    CHECK(sortValue(dark, SortKey::Brightness) == Approx(1.0));
    CHECK(sortValue(mid, SortKey::Brightness) == Approx((0.2 + 3.0 + 0.25) / 1.25).epsilon(1e-3));
    CHECK(sortValue(dark, SortKey::Fundamental) == Approx(1.0));
    CHECK(sortValue(mid, SortKey::Fundamental) == Approx(0.04 / 1.0425).epsilon(1e-3));
    CHECK(sortValue(Frame{}, SortKey::Brightness) == 0.0);

    Wavetable t;
    t.frames = {bright, dark, formant, mid};
    sortFrames(t, SortKey::Brightness);
    CHECK(t.frames[0] == dark);
    CHECK(t.frames[1] == mid);
    CHECK(t.frames[3] == bright);
    sortFrames(t, SortKey::Brightness, true); // inverted order
    CHECK(t.frames[0] == bright);
    CHECK(t.frames[3] == dark);
    sortFrames(t, SortKey::Fundamental, true);
    CHECK(t.frames[0] == dark);

    SortKey key = SortKey::Brightness;
    for (const char* name : {"brightest_peak", "brightness", "strongest_partial", "active_partials", "highest_partial", "fundamental"})
        CHECK(sortKeyFromName(name, key));
    CHECK_FALSE(sortKeyFromName("loudness", key));
}

TEST_CASE("toolkit: trim silent frames and export single cycles", "[toolkit]") {
    Wavetable t;
    t.frames = {sineFrame(1), Frame{}, sineFrame(2, 0, 0.0001), sineFrame(3, 0, 0.5)};
    CHECK(trimSilentFrames(t) == 2); // silence and the frame 80 dB down
    CHECK(t.size() == 2);
    Wavetable silent = Wavetable::silent(3);
    CHECK(trimSilentFrames(silent) == 0); // never left empty
    CHECK(silent.size() == 3);

    const Wavetable table = ladder(4);
    const auto files = exportSingleCycles(table, {1, 2});
    REQUIRE(files.size() == 2);
    WavData wav;
    std::string error;
    REQUIRE(decodeWav(files[0], wav, error));
    CHECK(wav.channelCount() == 1);
    REQUIRE(wav.frames() == static_cast<std::size_t>(kFrameSize));
    CHECK(wav.channels[0][100] == table.frames[1][100]);
    CHECK(exportSingleCycles(table, allFrames()).size() == 4);
    // A whole table or a selection goes out as a table file.
    Wavetable selection = table;
    REQUIRE(cropFrames(selection, {2, 3}));
    Wavetable back;
    REQUIRE(decodeWavetable(encodeWavetable(selection), back, error));
    CHECK(back.size() == 2);
    CHECK(back.frames[0] == table.frames[2]);
}
