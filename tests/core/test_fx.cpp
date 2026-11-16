// The effects on their own (plan 3.8, 5.7, 6.1): level and tail expectations for every type, the
// reverb's decay, density and mono compatibility, the limiter's ceiling, and click-free changes.
#include "../support/EngineRig.h"
#include "../support/FxRig.h"
#include "zederia/dsp/Fft.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <complex>

using namespace zed;
using Catch::Approx;
using zedtest::FxRig;

namespace {

constexpr double kRate = 48000.0;

double levelDb(const std::vector<float>& x, std::size_t from, std::size_t to) { return zedtest::db(zedtest::rmsOf(x, from, to)); }

// Gain of an effect at one frequency: a sine through it, steady part compared.
double gainDb(FxRig& rig, double hz, double level = 0.25) {
    rig.effect->clear();
    rig.apply(true);
    auto [l, r] = rig.runMono(zedtest::sine(hz, 0.5, level));
    const std::size_t from = l.size() / 2;
    return levelDb(l, from, l.size()) - zedtest::db(level / std::sqrt(2.0));
}

double responseDb(const FxRig& rig, double hz) {
    std::complex<double> h;
    REQUIRE(fx::effectResponse(rig.type, rig.values, kRate, hz, h));
    return zedtest::db(std::abs(h));
}

// The largest sample step just after a change (50 ms), relative to the steady signal before and
// after it: a click stands far above both.
double jumpRatio(const std::vector<float>& x, std::size_t change) {
    const double before = zedtest::largestJump(x, change - 12000, change);
    const double after = zedtest::largestJump(x, change + 12000, std::min(change + 24000, x.size()));
    const double around = zedtest::largestJump(x, change, change + 2400);
    return around / std::max(std::max(before, after), 1e-9);
}

// Energy decay of an impulse response (both channels), backward-integrated; T60 from the slope
// between -5 and -35 dB.
double t60Seconds(const std::vector<float>& l, const std::vector<float>& r) {
    std::vector<double> edc(l.size());
    double sum = 0.0;
    for (std::size_t i = l.size(); i-- > 0;) {
        sum += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
        edc[i] = sum;
    }
    const double total = edc[0];
    double t5 = -1.0, t35 = -1.0;
    for (std::size_t i = 0; i < edc.size(); ++i) {
        const double level = 10.0 * std::log10(std::max(edc[i] / total, 1e-30));
        if (t5 < 0.0 && level <= -5.0)
            t5 = static_cast<double>(i) / kRate;
        if (t35 < 0.0 && level <= -35.0) {
            t35 = static_cast<double>(i) / kRate;
            break;
        }
    }
    return t5 >= 0.0 && t35 > t5 ? 2.0 * (t35 - t5) : -1.0;
}

} // namespace

TEST_CASE("fx: every implementation takes the registry's parameters in order", "[fx]") {
    const auto& reg = Registry::get();
    REQUIRE(reg.effectTypes().size() == 13);
    for (int type = 0; type < static_cast<int>(reg.effectTypes().size()); ++type) {
        const auto& effect = reg.effectTypes()[static_cast<std::size_t>(type)];
        const auto names = fx::implementedParams(type);
        INFO(effect.id);
        REQUIRE(names.size() == effect.params.size());
        for (std::size_t k = 0; k < names.size(); ++k)
            CHECK(effect.params[k].id == effect.id + "." + names[k]);
        CHECK(fx::makeEffect(type, kRate) != nullptr);
    }
}

TEST_CASE("fx: drive", "[fx][drive]") {
    FxRig rig("drive");
    // At no drive the tube passes a quiet signal at unit gain.
    rig.set("amount", 0.0);
    CHECK(gainDb(rig, 1000.0, 0.1) == Approx(0.0).margin(0.3));

    // Every mode, driven hard by a full-scale sine, stays finite and bounded, and is silent soon
    // after the input stops.
    const auto& modes = Registry::get().effectTypes()[static_cast<std::size_t>(rig.type)].params[0].choices;
    REQUIRE(modes.size() == 15);
    for (const std::string& mode : modes) {
        FxRig m("drive");
        m.choose("mode", mode.c_str());
        m.set("amount", 1.0);
        auto [l, r] = m.runMono(zedtest::withSilence(zedtest::sine(220.0, 0.5, 1.0), 0.5));
        INFO(mode);
        bool finite = true;
        for (const float s : l)
            finite = finite && std::isfinite(s);
        CHECK(finite);
        CHECK(zedtest::peakOf(l, 0, l.size()) < 4.0f);
        CHECK(zedtest::peakOf(l, 24000 + 12000, l.size()) < 1.0e-3f); // the DC blocker settles within a quarter second
        CHECK(levelDb(l, 4800, 24000) > -30.0);
    }

    // Oversampling keeps the products of a hard clip from folding back.
    double worst[2] = {};
    for (const int factor : {1, 4}) {
        FxRig hard("drive", kRate, factor);
        hard.choose("mode", "hard_clip");
        hard.set("amount", 0.5);
        auto [l, r] = hard.runMono(zedtest::sine(4987.0, 1.0, 0.5));
        const auto report = zedtest::nonHarmonicLevel(zedtest::spectrumOf(l, 8192, 32768, kRate), 4987.0, 20000.0);
        worst[factor == 1 ? 0 : 1] = report.worstDbc;
    }
    WARN("drive hard clip at 5 kHz: worst alias " << worst[0] << " dBc at 1x, " << worst[1] << " dBc at 4x");
    CHECK(worst[1] < worst[0] - 15.0);

    // A drawn transfer curve is what the custom mode plays.
    FxRig curve("drive");
    curve.choose("mode", "custom_curve");
    curve.set("amount", 0.0);
    std::vector<float> square(fx::kCurveSize + 1);
    for (int i = 0; i <= fx::kCurveSize; ++i)
        square[static_cast<std::size_t>(i)] = static_cast<float>(i) / fx::kCurveSize * static_cast<float>(i) / fx::kCurveSize;
    curve.effect->setCurves(square.data(), nullptr);
    auto [cl, cr] = curve.runMono(zedtest::sine(200.0, 0.5, 0.5));
    CHECK(zedtest::peakOf(cl, 12000, 24000) == Approx(0.25).margin(0.02)); // 0.5 through y = x^2
}

TEST_CASE("fx: drive changes its quality and its mode without a click", "[fx][drive][quality]") {
    for (const bool modeChange : {false, true}) {
        FxRig rig("drive", kRate, 2);
        rig.choose("mode", "tube");
        rig.set("amount", 0.5);
        std::vector<float> l = zedtest::sine(220.0, 1.0, 0.5), r = l;
        std::vector<float> first(l.begin(), l.begin() + 24000), firstR = first;
        std::vector<float> second(l.begin() + 24000, l.end()), secondR = second;
        rig.run(first, firstR);
        if (modeChange) {
            rig.choose("mode", "soft_clip", false);
        } else {
            rig.setQuality(4);
            rig.apply(false);
        }
        rig.run(second, secondR);
        first.insert(first.end(), second.begin(), second.end());
        INFO((modeChange ? "mode change" : "quality change"));
        CHECK(jumpRatio(first, 24000) < 1.5);
    }
}

TEST_CASE("fx: compressor", "[fx][dynamics]") {
    FxRig rig("compressor");
    rig.set("threshold", -18.0);
    rig.set("ratio", 4.0);
    rig.set("attack", 1.0);
    rig.set("release", 50.0);
    // A -6 dBFS sine sits 12 dB over the threshold: 3 dB over it after compression.
    auto [l, r] = rig.runMono(zedtest::sine(1000.0, 1.0, 0.5));
    CHECK(zedtest::db(zedtest::peakOf(l, 36000, 48000)) == Approx(-15.0).margin(1.5));
    // At the largest ratio it limits at the threshold; make-up lifts the result.
    rig.set("ratio", 20.0);
    rig.set("makeup", 6.0);
    rig.effect->clear();
    auto [l2, r2] = rig.runMono(zedtest::sine(1000.0, 1.0, 0.5));
    CHECK(zedtest::db(zedtest::peakOf(l2, 36000, 48000)) == Approx(-18.0 + 6.0).margin(1.5));

    // Three bands with nothing to do sum back to the input: the crossovers are flat.
    FxRig bands("compressor");
    bands.choose("mode", "multiband");
    for (const char* k : {"low_amount", "mid_amount", "high_amount", "upward"})
        bands.set(k, 0.0);
    for (const double hz : {60.0, 120.0, 700.0, 2500.0, 9000.0})
        CHECK(gainDb(bands, hz) == Approx(0.0).margin(0.3));
    // Upward compression lifts quiet material toward the band thresholds.
    bands.set("upward", 1.0);
    for (const char* k : {"low_amount", "mid_amount", "high_amount"})
        bands.set(k, 1.0);
    CHECK(gainDb(bands, 700.0, 0.01) > 3.0);
}

TEST_CASE("fx: EQ follows its analytic response", "[fx][eq]") {
    FxRig rig("eq");
    for (const double hz : {50.0, 500.0, 5000.0})
        CHECK(gainDb(rig, hz) == Approx(0.0).margin(0.05)); // flat at its defaults
    rig.set("mid1_gain", 12.0);
    rig.set("mid1_freq", 1000.0);
    CHECK(responseDb(rig, 1000.0) == Approx(12.0).margin(0.01));
    rig.choose("low_type", "high_pass");
    rig.set("low_freq", 200.0);
    rig.choose("high_type", "shelf");
    rig.set("high_gain", -6.0);
    for (const double hz : {60.0, 300.0, 1000.0, 3000.0, 12000.0}) {
        INFO(hz << " Hz");
        CHECK(gainDb(rig, hz) == Approx(responseDb(rig, hz)).margin(0.1));
    }
    CHECK(responseDb(rig, 50.0) < -20.0);
}

TEST_CASE("fx: filter is the voice filter in stereo", "[fx][filter]") {
    FxRig rig("filter");
    rig.set("cutoff", 1000.0);
    for (const double hz : {100.0, 800.0, 3000.0}) {
        INFO(hz << " Hz");
        CHECK(gainDb(rig, hz) == Approx(responseDb(rig, hz)).margin(0.5));
    }
    CHECK(gainDb(rig, 8000.0) < -20.0);
}

TEST_CASE("fx: chorus", "[fx][chorus]") {
    FxRig rig("chorus");
    const auto in = zedtest::noise(2.0, 0.25);
    auto [l, r] = rig.runMono(zedtest::withSilence(in, 0.5));
    const double inLevel = levelDb(in, 48000, 96000);
    CHECK(levelDb(l, 48000, 96000) == Approx(inLevel).margin(3.0));
    CHECK(levelDb(r, 48000, 96000) == Approx(inLevel).margin(3.0));
    CHECK(zedtest::correlation(l, r, 48000, 96000) < 0.9); // wide
    CHECK(zedtest::peakOf(l, 96000 + 2400, l.size()) < 1.0e-3f); // gone within 50 ms (feedback 0.1)

    // Feedback at its top stays stable.
    FxRig loud("chorus");
    loud.set("feedback", 1.0);
    loud.set("depth", 1.0);
    auto [fl, fr] = loud.runMono(zedtest::noise(3.0, 0.5));
    bool finite = true;
    for (const float s : fl)
        finite = finite && std::isfinite(s);
    CHECK(finite);
    CHECK(zedtest::peakOf(fl, 0, fl.size()) < 5.0f);

    // Moving the delay times does not click.
    FxRig moving("chorus");
    std::vector<float> a = zedtest::sine(110.0, 1.0, 0.5), b = a;
    std::vector<float> first(a.begin(), a.begin() + 24000), firstR = first, second(a.begin() + 24000, a.end()), secondR = second;
    moving.run(first, firstR);
    moving.set("delay1", 20.0, false);
    moving.set("delay2", 40.0, false);
    moving.run(second, secondR);
    first.insert(first.end(), second.begin(), second.end());
    CHECK(zedtest::largestJump(first, 24000, 48000) < 0.05f);
}

TEST_CASE("fx: flanger and phaser make their notches and stay stable", "[fx][flanger][phaser]") {
    // A still flanger is a comb: the 0.2 ms delay notches 2.5 kHz and passes 5 kHz.
    FxRig flanger("flanger");
    flanger.set("depth", 0.0);
    flanger.set("feedback", 0.0);
    CHECK(gainDb(flanger, 2500.0) < -20.0);
    CHECK(gainDb(flanger, 5000.0) == Approx(0.0).margin(0.5));
    for (const double feedback : {-1.0, 1.0}) {
        FxRig f("flanger");
        f.set("feedback", feedback);
        f.set("depth", 1.0);
        auto [l, r] = f.runMono(zedtest::withSilence(zedtest::noise(2.0, 0.5), 0.5));
        bool finite = true;
        for (const float s : l)
            finite = finite && std::isfinite(s);
        CHECK(finite);
        CHECK(zedtest::peakOf(l, 0, l.size()) < 10.0f);
        CHECK(zedtest::peakOf(l, 96000 + 9600, l.size()) < 1.0e-3f);
    }

    // A still six-stage phaser notches where its all-pass chain turns by 540 degrees: the centre.
    FxRig phaser("phaser");
    phaser.set("depth", 0.0);
    phaser.set("feedback", 0.0);
    phaser.set("center", 800.0);
    CHECK(gainDb(phaser, 800.0) < -20.0);
    CHECK(gainDb(phaser, 800.0 * std::tan(kPi / 12.0)) < -20.0);
    // Changing the number of stages crossfades.
    FxRig stages("phaser");
    std::vector<float> a = zedtest::sine(330.0, 1.0, 0.5);
    std::vector<float> first(a.begin(), a.begin() + 24000), firstR = first, second(a.begin() + 24000, a.end()), secondR = second;
    stages.run(first, firstR);
    stages.set("stages", 12.0, false);
    stages.run(second, secondR);
    first.insert(first.end(), second.begin(), second.end());
    CHECK(jumpRatio(first, 24000) < 1.5);
}

TEST_CASE("fx: delay modes, echo times and feedback", "[fx][delay]") {
    const auto impulse = [](double seconds) {
        std::vector<float> x(static_cast<std::size_t>(seconds * kRate), 0.0f);
        x[0] = 1.0f;
        return x;
    };
    FxRig dual("delay"); // synced 1/8 dotted at 120 bpm: 375 ms
    auto [l, r] = dual.runMono(impulse(1.0));
    const auto peakAt = [](const std::vector<float>& x, std::size_t from, std::size_t to) {
        std::size_t best = from;
        for (std::size_t i = from; i < to && i < x.size(); ++i)
            if (std::abs(x[i]) > std::abs(x[best]))
                best = i;
        return best;
    };
    CHECK(peakAt(l, 1000, 30000) == Approx(18000).margin(2));
    const double first = std::abs(l[peakAt(l, 1000, 30000)]), second = std::abs(l[peakAt(l, 30000, 47000)]);
    CHECK(peakAt(l, 30000, 47000) == Approx(36000).margin(4));
    CHECK(zedtest::db(second / first) == Approx(zedtest::db(0.35)).margin(0.5));

    // Ping-pong: the first echo on the left, the next on the right.
    FxRig pong("delay");
    pong.choose("mode", "ping_pong");
    auto [pl, pr] = pong.runMono(impulse(1.0));
    CHECK(std::abs(pl[peakAt(pl, 1000, 30000)]) > 0.5f);
    CHECK(zedtest::peakOf(pr, 1000, 30000) < 1.0e-3f);
    CHECK(peakAt(pr, 30000, 47000) == Approx(36000).margin(4));

    // Pre-tap: an early tap at the left time, then the echoes at the right time.
    FxRig pre("delay");
    pre.choose("mode", "pre_tap");
    pre.set("sync", 0.0);
    pre.set("link", 0.0);
    pre.set("time_l", 100.0);
    pre.set("time_r", 300.0);
    pre.set("feedback", 0.0);
    auto [ql, qr] = pre.runMono(impulse(0.5));
    CHECK(peakAt(ql, 1000, 10000) == Approx(4800).margin(2));
    CHECK(peakAt(ql, 10000, 20000) == Approx(14400).margin(2));

    // A new time glides: no click.
    FxRig moving("delay");
    moving.set("sync", 0.0);
    moving.set("time_l", 300.0);
    moving.set("feedback", 0.5);
    std::vector<float> a = zedtest::sine(50.0, 2.0, 0.5);
    std::vector<float> one(a.begin(), a.begin() + 48000), oneR = one, two(a.begin() + 48000, a.end()), twoR = two;
    moving.run(one, oneR);
    moving.set("time_l", 120.0, false);
    moving.run(two, twoR);
    one.insert(one.end(), two.begin(), two.end());
    CHECK(zedtest::largestJump(one, 48000, 96000) < 0.05f);

    // Ducking lowers the echoes while the input is loud.
    double level[2] = {};
    for (const double ducking : {0.0, 1.0}) {
        FxRig d("delay");
        d.set("ducking", ducking);
        auto [dl, dr] = d.runMono(zedtest::sine(440.0, 2.0, 0.5));
        level[ducking > 0.0 ? 1 : 0] = levelDb(dl, 48000, 96000);
    }
    CHECK(level[1] < level[0] - 20.0);
}

TEST_CASE("fx: reverb decays at its decay time in every mode", "[fx][reverb]") {
    for (const char* mode : {"plate", "hall", "room"}) {
        for (const double decayMs : {700.0, 2000.0, 5000.0}) {
            FxRig rig("reverb");
            rig.choose("mode", mode);
            rig.set("decay", decayMs);
            rig.set("damping", 0.0);
            rig.set("predelay", 0.0);
            rig.set("low_cut", 20.0);
            rig.set("high_cut", 20000.0);
            std::vector<float> l(static_cast<std::size_t>((decayMs / 1000.0 * 1.6 + 0.3) * kRate), 0.0f), r = l;
            l[0] = r[0] = 1.0f;
            rig.run(l, r);
            const double t60 = t60Seconds(l, r);
            INFO(mode << ", decay " << decayMs << " ms: measured T60 " << t60 << " s");
            WARN("reverb " << mode << " decay " << decayMs << " ms: T60 " << t60 << " s");
            CHECK(t60 == Approx(decayMs / 1000.0).epsilon(0.2));
        }
    }
}

TEST_CASE("fx: reverb has no metallic ringing and is mono-compatible", "[fx][reverb]") {
    for (const char* mode : {"plate", "hall", "room"}) {
        FxRig rig("reverb");
        rig.choose("mode", mode);
        rig.set("decay", 3000.0);
        rig.set("predelay", 0.0);
        std::vector<float> l(static_cast<std::size_t>(1.2 * kRate), 0.0f), r = l;
        l[0] = r[0] = 1.0f; // the click test
        rig.run(l, r);
        // The tail's spectrum, averaged over eight windows (each normalized to its own energy), is
        // compared with its local median: a mode that rings stands far above its neighbours.
        constexpr int kSize = 4096;
        std::vector<double> power(kSize / 2 + 1, 0.0);
        const std::size_t start = static_cast<std::size_t>(0.1 * kRate);
        for (int w = 0; w < 8; ++w) {
            std::vector<std::complex<double>> data(kSize);
            double energy = 0.0;
            for (int i = 0; i < kSize; ++i) {
                const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (kSize - 1));
                const double x = l[start + static_cast<std::size_t>(w * kSize + i)];
                data[static_cast<std::size_t>(i)] = x * hann;
                energy += x * x;
            }
            dsp::Fft::forSize(kSize).forward(data.data());
            for (std::size_t k = 0; k < power.size(); ++k)
                power[k] += std::norm(data[k]) / std::max(energy, 1e-30);
        }
        double worst = 0.0;
        for (int k = static_cast<int>(200.0 / (kRate / kSize)); k < static_cast<int>(10000.0 / (kRate / kSize)); ++k) {
            const int half = std::max(4, k / 12); // +/- a sixth of an octave
            std::vector<double> around;
            for (int j = k - half; j <= k + half; ++j)
                around.push_back(power[static_cast<std::size_t>(j)]);
            std::nth_element(around.begin(), around.begin() + static_cast<std::ptrdiff_t>(around.size() / 2), around.end());
            worst = std::max(worst, 10.0 * std::log10(power[static_cast<std::size_t>(k)] / around[around.size() / 2]));
        }
        INFO(mode << ": strongest line " << worst << " dB above its neighbourhood");
        WARN("reverb " << mode << ": strongest line in the tail " << worst << " dB above its neighbourhood");
        CHECK(worst < 10.0);

        // Mono compatibility: the two sides are uncorrelated and their sum keeps the energy.
        FxRig wide("reverb");
        wide.choose("mode", mode);
        auto [nl, nr] = wide.runMono(zedtest::noise(3.0, 0.25));
        const std::size_t from = 48000, to = nl.size();
        WARN("reverb " << mode << ": correlation of the sides " << zedtest::correlation(nl, nr, from, to));
        CHECK(std::abs(zedtest::correlation(nl, nr, from, to)) < 0.5);
        std::vector<float> mono(nl.size());
        for (std::size_t i = 0; i < mono.size(); ++i)
            mono[i] = 0.5f * (nl[i] + nr[i]);
        const double sides = 0.5 * (std::pow(zedtest::rmsOf(nl, from, to), 2) + std::pow(zedtest::rmsOf(nr, from, to), 2));
        WARN("reverb " << mode << ": mono sum against the sides " << 20.0 * std::log10(zedtest::rmsOf(mono, from, to)) - 10.0 * std::log10(sides) << " dB");
        CHECK(20.0 * std::log10(zedtest::rmsOf(mono, from, to)) - 10.0 * std::log10(sides) > -4.5);
    }
}

TEST_CASE("fx: reverb freeze holds the tail; size changes do not click", "[fx][reverb]") {
    FxRig rig("reverb");
    std::vector<float> l = zedtest::withSilence(zedtest::noise(1.0, 0.25), 10.0), r = l;
    std::vector<float> a(l.begin(), l.begin() + 48000), ar = a, b(l.begin() + 48000, l.end()), br = b;
    rig.run(a, ar);
    rig.set("freeze", 1.0, false);
    CHECK_FALSE(rig.effect->mayIdle());
    rig.run(b, br);
    // (the lines take about a second to settle on whole samples; from then on nothing is lost)
    const double early = levelDb(b, 72000, 96000), late = levelDb(b, 456000, 480000);
    WARN("reverb freeze: level after 1.5 s " << early << " dB, after 9.5 s " << late << " dB");
    CHECK(late == Approx(early).margin(0.5)); // frozen, the network loses nothing (the window levels wander a little)
    rig.set("freeze", 0.0, false);
    CHECK(rig.effect->mayIdle());

    FxRig sized("reverb");
    std::vector<float> s = zedtest::sine(220.0, 2.0, 0.3);
    std::vector<float> one(s.begin(), s.begin() + 48000), oneR = one, two(s.begin() + 48000, s.end()), twoR = two;
    sized.run(one, oneR);
    sized.set("size", 1.0, false);
    sized.run(two, twoR);
    one.insert(one.end(), two.begin(), two.end());
    INFO("largest jump at " << zedtest::largestJumpAt(one, 48000, 72000) << ", " << zedtest::largestJump(one, 48000, 72000));
    CHECK(jumpRatio(one, 48000) < 1.5);
}

TEST_CASE("fx: swarm detunes, widener and utility keep the mono sum", "[fx][stereo]") {
    FxRig swarm("swarm");
    swarm.set("detune", 1.0);
    const auto in = zedtest::sine(1000.0, 2.0, 0.25);
    auto [sl, sr] = swarm.runMono(in);
    CHECK(levelDb(sl, 48000, 96000) == Approx(levelDb(in, 48000, 96000)).margin(3.0));
    // The copies sit off the input's pitch: the line at 1 kHz carries only part of the energy.
    const auto spectrum = zedtest::spectrumOf(sl, 16000, 65536, kRate);
    CHECK(zedtest::lineLevel(spectrum, 1000.0) < 0.6 * 0.25);

    FxRig widener("widener");
    std::vector<float> wl = zedtest::noise(1.0, 0.25, 3), wr = zedtest::noise(1.0, 0.25, 4);
    const std::vector<float> inL = wl, inR = wr;
    widener.run(wl, wr);
    double worst = 0.0;
    for (std::size_t i = 0; i < wl.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(wl[i] + wr[i]) - (inL[i] + inR[i])));
    CHECK(worst < 1.0e-5); // the mono sum is untouched
    FxRig monoWide("widener");
    auto [ml, mr] = monoWide.runMono(zedtest::noise(1.0, 0.25));
    CHECK(zedtest::correlation(ml, mr, 4800, ml.size()) < 0.95); // a mono source becomes wider
    CHECK(monoWide.effect->meter() < 0.95f);
    CHECK(monoWide.effect->meter() > 0.0f); // and stays well clear of cancelling in mono

    FxRig utility("utility");
    utility.set("gain", 20.0 * std::log10(2.0));
    auto [ul, ur] = utility.runMono(std::vector<float>(480, 0.25f));
    CHECK(ul[400] == Approx(0.5f).margin(1e-5));
    utility.set("gain", 0.0);
    utility.set("width", 0.0);
    std::vector<float> xl = zedtest::noise(0.1, 0.25, 5), xr = zedtest::noise(0.1, 0.25, 6);
    utility.run(xl, xr);
    CHECK(zedtest::maxDifference(xl, xr) == 0.0);
    utility.set("width", 1.0);
    utility.choose("polarity", "invert_left");
    std::vector<float> pl(480, 0.25f), pr(480, 0.25f);
    utility.run(pl, pr);
    CHECK(pl[400] == Approx(-0.25f).margin(1e-6));
    CHECK(pr[400] == Approx(0.25f).margin(1e-6));
    // Mono below 200 Hz: the side at 50 Hz goes, the side at 2 kHz stays.
    FxRig mono("utility");
    mono.set("mono_below", 200.0);
    for (const double hz : {50.0, 2000.0}) {
        mono.effect->clear();
        mono.apply(true);
        std::vector<float> l = zedtest::sine(hz, 0.5, 0.25), r = l;
        for (float& s : r)
            s = -s;
        mono.run(l, r);
        INFO(hz << " Hz");
        if (hz < 100.0)
            CHECK(levelDb(l, 12000, 24000) < zedtest::db(0.25 / std::sqrt(2.0)) - 12.0);
        else
            CHECK(levelDb(l, 12000, 24000) == Approx(zedtest::db(0.25 / std::sqrt(2.0))).margin(1.0));
    }
    FxRig dc("utility");
    dc.set("dc_filter", 1.0);
    auto [dl, dr] = dc.runMono(std::vector<float>(96000, 0.5f));
    CHECK(std::abs(dl.back()) < 0.01f);
}

TEST_CASE("fx: limiter never passes its ceiling", "[fx][dynamics][limiter]") {
    FxRig rig("limiter");
    rig.set("ceiling", -6.0);
    auto [l, r] = rig.runMono(zedtest::noise(2.0, 2.0)); // peaks far above full scale
    const float ceiling = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    WARN("limiter at -6 dB: highest output " << zedtest::db(std::max(zedtest::peakOf(l, 0, l.size()), zedtest::peakOf(r, 0, r.size()))) << " dBFS");
    CHECK(zedtest::peakOf(l, 0, l.size()) <= ceiling);
    CHECK(zedtest::peakOf(r, 0, r.size()) <= ceiling);
    CHECK(zedtest::peakOf(l, 48000, 96000) > 0.9f * ceiling);
    // Quiet material that follows comes back to its own level after the release.
    std::vector<float> quiet = zedtest::sine(440.0, 1.0, 0.05), quietR = quiet;
    rig.run(quiet, quietR);
    CHECK(zedtest::peakOf(quiet, 36000, 48000) == Approx(0.05f).epsilon(0.02));
    // A ceiling that moves while the signal is loud is never passed either.
    FxRig moving("limiter");
    std::vector<float> loud = zedtest::noise(1.0, 2.0), loudR = loud;
    std::vector<float> a(loud.begin(), loud.begin() + 24000), ar = a, b(loud.begin() + 24000, loud.end()), br = b;
    moving.set("ceiling", -1.0);
    moving.run(a, ar);
    moving.set("ceiling", -12.0, false);
    moving.run(b, br);
    CHECK(zedtest::peakOf(a, 0, a.size()) <= static_cast<float>(std::pow(10.0, -1.0 / 20.0)));
    CHECK(zedtest::peakOf(b, 16, b.size()) <= static_cast<float>(std::pow(10.0, -12.0 / 20.0)));
}
