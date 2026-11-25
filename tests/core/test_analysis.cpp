// The analysis module (Appendix C) on signals whose answers are known.
#include "zederia/analysis/Analysis.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

using namespace zed;
using Catch::Approx;

namespace {

struct Signal {
    std::vector<float> l, r;
    analysis::Audio audio() const { return {l, r, 48000.0}; }
};

Signal sine(double hz, double peak, double seconds, double rightGain = 1.0) {
    Signal s;
    const auto n = static_cast<std::size_t>(seconds * 48000.0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = static_cast<float>(peak * std::sin(2.0 * kPi * hz * static_cast<double>(i) / 48000.0));
        s.l.push_back(v);
        s.r.push_back(static_cast<float>(v * rightGain));
    }
    return s;
}

bool hasTag(const analysis::Descriptors& d, const char* tag) { return std::find(d.tags.begin(), d.tags.end(), tag) != d.tags.end(); }

} // namespace

TEST_CASE("analysis: level, loudness and pitch of a sine", "[analysis]") {
    const auto s = sine(1000.0, 0.1, 3.0);
    const auto d = analysis::analyze(s.audio());
    CHECK(d.peakDb == Approx(-20.0).margin(0.01));
    CHECK(d.rmsDb == Approx(-23.01).margin(0.02));
    CHECK(d.crestDb == Approx(3.01).margin(0.05));
    CHECK(d.loudnessLufs == Approx(-20.0).margin(0.15)); // ITU-R BS.1770: a 1 kHz stereo sine reads its peak level
    CHECK(d.clippedSamples == 0);
    REQUIRE(d.pitched);
    CHECK(d.fundamentalHz == Approx(1000.0).margin(0.5));
    CHECK(d.stabilityCents < 1.0);
    CHECK(d.centroidHz == Approx(1000.0).margin(30.0));
    CHECK(d.width < 1e-6);
    CHECK(d.correlation == Approx(1.0).margin(1e-6));
    CHECK(hasTag(d, "mono"));
    CHECK(hasTag(d, "static"));
    CHECK(d.bandShare[2] + d.bandShare[3] > 0.99); // 1 kHz sits on the low-mid / high-mid edge
}

TEST_CASE("analysis: pitch of an A3 saw, a plucky envelope and its tail", "[analysis]") {
    Signal s;
    const double hz = 220.0;
    for (int i = 0; i < 48000 * 2; ++i) {
        const double t = i / 48000.0;
        const double env = t < 0.002 ? t / 0.002 : std::exp(-(t - 0.002) / 0.08);
        double v = 0.0;
        for (int h = 1; h <= 40; ++h)
            v += std::sin(2.0 * kPi * hz * h * t) / h;
        s.l.push_back(static_cast<float>(0.4 * env * v));
        s.r.push_back(static_cast<float>(0.4 * env * v));
    }
    analysis::Options o;
    o.noteOffSeconds = 0.5;
    const auto d = analysis::analyze(s.audio(), o);
    REQUIRE(d.pitched);
    CHECK(d.note == "A3");
    CHECK(std::abs(d.centsOff) < 2.0);
    CHECK(d.attackMs < 5.0); // a 2 ms ramp, read through one 4.5 ms cycle of the tone
    CHECK(d.decayShape == "percussive");
    CHECK(hasTag(d, "plucky"));
    CHECK(d.inharmonicityCents < 5.0); // whole multiples, within what a 4096-point spectrum resolves
    CHECK(d.tailMs > 0.0);
}

TEST_CASE("analysis: stereo width, noise, movement and clipping", "[analysis]") {
    SECTION("opposite sides are wide and uncorrelated") {
        const auto s = sine(440.0, 0.5, 1.0, -1.0);
        const auto d = analysis::analyze(s.audio());
        CHECK(d.correlation == Approx(-1.0).margin(1e-6));
        CHECK(hasTag(d, "wide"));
    }
    SECTION("white noise is flat and noisy") {
        Signal s;
        Rng rng(3);
        for (int i = 0; i < 48000; ++i) {
            s.l.push_back(static_cast<float>(rng.uniform() * 0.6 - 0.3));
            s.r.push_back(static_cast<float>(rng.uniform() * 0.6 - 0.3));
        }
        const auto d = analysis::analyze(s.audio());
        CHECK(d.flatness > 0.5);
        CHECK(hasTag(d, "noisy"));
        CHECK(hasTag(d, "bright"));
    }
    SECTION("a tremolo at 4 Hz is found") {
        Signal s;
        for (int i = 0; i < 48000 * 4; ++i) {
            const double t = i / 48000.0;
            const auto v = static_cast<float>(0.3 * (1.0 + 0.5 * std::sin(2.0 * kPi * 4.0 * t)) * std::sin(2.0 * kPi * 330.0 * t));
            s.l.push_back(v);
            s.r.push_back(v);
        }
        const auto d = analysis::analyze(s.audio());
        CHECK(d.modulationRateHz == Approx(4.0).margin(0.3));
        CHECK(hasTag(d, "evolving"));
    }
    SECTION("full-scale samples are counted") {
        auto s = sine(100.0, 1.2, 0.5);
        for (auto& v : s.l)
            v = std::clamp(v, -1.0f, 1.0f);
        for (auto& v : s.r)
            v = std::clamp(v, -1.0f, 1.0f);
        CHECK(analysis::analyze(s.audio()).clippedSamples > 100);
    }
}

TEST_CASE("analysis: comparing a sound with itself and with others", "[analysis]") {
    const auto a = sine(440.0, 0.3, 1.0);
    const auto c = analysis::compare(a.audio(), a.audio());
    CHECK(c.distance == Approx(0.0).margin(1e-9));
    CHECK(c.similarity == Approx(1.0));
    const auto quieter = sine(440.0, 0.15, 1.0);
    const auto q = analysis::compare(quieter.audio(), a.audio());
    CHECK(q.loudnessDiffLu == Approx(-6.02).margin(0.1));
    CHECK(q.spectralDb < 0.5); // the colour is the same once loudness is aligned
    const auto higher = sine(880.0, 0.3, 1.0);
    const auto h = analysis::compare(higher.audio(), a.audio());
    CHECK(h.pitchDiffCents == Approx(1200.0).margin(2.0));
    CHECK(h.centroidDiffOct == Approx(1.0).margin(0.05));
    CHECK(h.distance > q.distance);
    const auto j = analysis::toJson(h);
    CHECK(j.contains("similarity"));
}

TEST_CASE("analysis: the mel spectrogram is a PNG of the requested size", "[analysis]") {
    const auto s = sine(440.0, 0.3, 1.0);
    const auto png = analysis::melSpectrogramPng(s.audio(), 320, 128);
    REQUIRE(png.size() > 100);
    CHECK(png[1] == 'P');
    CHECK(png[2] == 'N');
    CHECK(png[3] == 'G');
    const auto be32 = [&](std::size_t at) { return (png[at] << 24) | (png[at + 1] << 16) | (png[at + 2] << 8) | png[at + 3]; };
    CHECK(be32(16) == 320);
    CHECK(be32(20) == 128);
}

TEST_CASE("analysis: silence and empty input do not fail", "[analysis]") {
    Signal s;
    CHECK(analysis::analyze(s.audio()).durationSeconds == 0.0);
    s.l.assign(48000, 0.0f);
    s.r.assign(48000, 0.0f);
    const auto d = analysis::analyze(s.audio());
    CHECK(d.tags == std::vector<std::string>{"silent"});
    CHECK(analysis::toJson(d)["level"]["peak_dbfs"].get<double>() <= -199.0);
}

TEST_CASE("analysis: a held low note is sustained and a low pluck percussive", "[analysis]") {
    // The decay used to be read on a 1 ms envelope, which dips at each zero crossing of a low
    // note: a C3 saw held for three seconds read as a 4 ms percussive decay (milestone 7).
    const auto saw = [](double hz, double seconds, double decaySeconds) {
        Signal s;
        for (int i = 0; i < static_cast<int>(48000 * seconds); ++i) {
            const double t = i / 48000.0;
            const double env = decaySeconds > 0.0 ? std::exp(-t / decaySeconds) : 1.0;
            double v = 0.0;
            for (int h = 1; h <= 40; ++h)
                v += std::sin(2.0 * kPi * hz * h * t) / h;
            s.l.push_back(static_cast<float>(0.3 * env * v));
            s.r.push_back(static_cast<float>(0.3 * env * v));
        }
        return s;
    };
    analysis::Options o;
    o.noteOffSeconds = 3.0;
    for (const double hz : {130.81, 65.41, 41.2}) {
        INFO(hz << " Hz held");
        const auto d = analysis::analyze(saw(hz, 3.1, 0.0).audio(), o);
        CHECK(d.decayShape == "sustained");
        CHECK_FALSE(hasTag(d, "plucky"));
    }
    o.noteOffSeconds = 0.5;
    const auto pluck = analysis::analyze(saw(65.41, 1.5, 0.05).audio(), o);
    CHECK(pluck.decayShape == "percussive");
    CHECK(hasTag(pluck, "plucky"));
}

TEST_CASE("analysis: attack time of a linear ramp", "[analysis]") {
    Signal s;
    for (int i = 0; i < 48000; ++i) {
        const double t = i / 48000.0;
        const double env = std::min(1.0, t / 0.010); // 10 ms from silence to full level
        const auto v = static_cast<float>(0.5 * env * std::sin(2.0 * kPi * 2000.0 * t));
        s.l.push_back(v);
        s.r.push_back(v);
    }
    const auto d = analysis::analyze(s.audio());
    CHECK(d.attackMs == Approx(8.0).margin(1.0)); // 10 % to 90 % of a linear ramp
    CHECK(d.decayShape == "sustained");
}

TEST_CASE("analysis: a phrase of several notes is not called noisy", "[analysis]") {
    Signal s;
    for (int i = 0; i < 48000 * 2; ++i) {
        const double t = i / 48000.0;
        const double hz = t < 1.0 ? 220.0 : 659.26; // A3, then E5
        double v = 0.0;
        for (int h = 1; h <= 20; ++h)
            v += std::sin(2.0 * kPi * hz * h * t) / h;
        s.l.push_back(static_cast<float>(0.3 * v));
        s.r.push_back(static_cast<float>(0.3 * v));
    }
    const auto d = analysis::analyze(s.audio());
    REQUIRE(d.pitched);
    CHECK(d.pitchVaries);
    CHECK_FALSE(hasTag(d, "noisy"));
    CHECK(analysis::toJson(d)["pitch"]["varies"] == true);
}
