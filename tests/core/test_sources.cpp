// The other sources and what joins them (plan 3.1, 3.2): the Classic engine, the Sub oscillator,
// the Texture player, the per-source sends into the placeholder filter slots, and swapping
// tables and samples while notes play.
#include "../support/Analysis.h"
#include "../support/TestUtil.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/util/Math.h"

#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <thread>

using namespace zed;
using Catch::Approx;
using zedtest::idx;

namespace {

constexpr double kRate = 48000.0;

int choiceOf(const char* id, const char* token) { return Registry::get().find(id)->choiceIndex(token); }

// A flat envelope, full level, fixed phase, and no oscillator unless a test switches one on.
Patch quietPatch() {
    Patch p;
    p.set("osc1.on", 0);
    p.set("global.master", 0);
    p.set("env1.attack", 0);
    p.set("env1.sustain", 1);
    p.set("env1.release", 5);
    for (const char* osc : {"osc1", "osc2", "osc3"}) {
        p.set(std::string(osc) + ".phase_random", 0);
        p.set(std::string(osc) + ".level", 0);
    }
    p.set("sub.level", 0);
    p.set("texture.level", 0);
    return p;
}

Patch classicPatch(double shape) {
    Patch p = quietPatch();
    p.set("osc1.on", 1);
    p.set("osc1.engine", choiceOf("osc1.engine", "classic"));
    p.set("osc1.classic.shape", shape);
    return p;
}

struct Rig {
    Engine engine;
    std::vector<float> left, right;
    explicit Rig(const Patch& patch, double sampleRate = kRate) {
        engine.prepare(sampleRate);
        engine.loadPatch(patch);
    }
    void run(int frames, std::vector<MidiEvent> events = {}) {
        const std::size_t start = left.size();
        left.resize(start + static_cast<std::size_t>(frames));
        right.resize(start + static_cast<std::size_t>(frames));
        engine.process({left.data() + start, right.data() + start, frames, events, {}});
    }
};

Wavetable sineTable(int harmonic = 1, double level = 1.0) {
    Wavetable t;
    Frame f;
    for (int n = 0; n < kFrameSize; ++n)
        f[static_cast<std::size_t>(n)] = static_cast<float>(level * std::sin(2.0 * kPi * harmonic * n / kFrameSize));
    t.frames.push_back(f);
    return t;
}

// A stereo sample: a tone on the left, another (or silence) on the right.
std::shared_ptr<const SampleData> toneSample(double hz, double seconds, double sampleRate = 48000.0, double rightHz = -1.0) {
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> left(frames), right(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / sampleRate));
        right[i] = rightHz < 0.0 ? left[i] : rightHz == 0.0 ? 0.0f : static_cast<float>(std::sin(2.0 * kPi * rightHz * static_cast<double>(i) / sampleRate));
    }
    return SampleData::fromStereo(std::move(left), std::move(right), sampleRate, "tone");
}

RenderResult render(const Patch& patch, int note, double seconds, std::shared_ptr<const SampleData> texture = nullptr,
                    double sampleRate = kRate) {
    const NoteSpec notes[] = {{note, 0.0, seconds, 1.0}};
    RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.texture = std::move(texture);
    return renderOffline(patch, sequenceFromNotes(notes, sampleRate), settings);
}

// Pitch from the zero crossings of the fundamental alone. The harmonics are filtered away first
// (four gentle low-passes at the expected pitch): a bright wave near the top of the keyboard can
// cross zero more than once per cycle, and a filter moves no frequency.
double pitchCents(const std::vector<float>& x, double expectedHz, double sampleRate = kRate) {
    std::vector<float> y = x;
    const double k = 1.0 - std::exp(-2.0 * kPi * expectedHz / sampleRate);
    for (int pass = 0; pass < 4; ++pass) {
        double state = 0.0;
        for (float& s : y) {
            state += (s - state) * k;
            s = static_cast<float>(state);
        }
    }
    const auto from = static_cast<std::size_t>(0.2 * sampleRate), to = y.size() - static_cast<std::size_t>(0.1 * sampleRate);
    return zedtest::cents(zedtest::measureFrequency(y, from, to, sampleRate), expectedHz);
}

double maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        worst = std::max<double>(worst, std::abs(a[i] - b[i]));
    return worst;
}

} // namespace

// --- Classic engine ----------------------------------------------------------------------------

TEST_CASE("classic: in tune from C0 to C8", "[classic][tuning]") {
    double worst = 0.0;
    for (const double shape : {0.0, 1.0}) { // sawtooth and sine
        for (const double rate : {44100.0, 48000.0}) {
            for (int note = 12; note <= 108; note += 8) {
                const RenderResult r = render(classicPatch(shape), note, 2.5, nullptr, rate);
                const double error = pitchCents(r.left, noteToHz(note), rate);
                INFO("shape " << shape << ", note " << note << " at " << rate << " Hz: " << error << " cents");
                CHECK(std::abs(error) < 0.1);
                worst = std::max(worst, std::abs(error));
            }
        }
    }
    WARN("classic engine, worst tuning error C0..C8: " << worst << " cents");
}

TEST_CASE("classic: shape morphs sawtooth, square, triangle and sine; pulse width sets the duty", "[classic]") {
    const double level = std::sqrt(0.5); // centre pan
    const auto rms = [&](double shape) {
        const RenderResult r = render(classicPatch(shape), 45, 1.0); // 110 Hz
        return zedtest::rmsOf(r.left, 4800, 43200) / level;
    };
    CHECK(rms(0.0) == Approx(1.0 / std::sqrt(3.0)).epsilon(0.01));       // sawtooth
    CHECK(rms(1.0 / 3.0) == Approx(1.0).epsilon(0.01));                  // square
    CHECK(rms(2.0 / 3.0) == Approx(1.0 / std::sqrt(3.0)).epsilon(0.01)); // triangle
    CHECK(rms(1.0) == Approx(std::sqrt(0.5)).epsilon(0.005));            // sine

    // The four shapes have the harmonics they should.
    const auto harmonics = [&](double shape) {
        const RenderResult r = render(classicPatch(shape), 45, 1.0);
        const auto s = zedtest::spectrumOf(r.left, 4800, 32768, kRate);
        std::array<double, 4> h{};
        for (int k = 1; k <= 4; ++k)
            h[static_cast<std::size_t>(k - 1)] = zedtest::lineLevel(s, 110.0 * k) / level;
        return h;
    };
    auto h = harmonics(0.0);
    CHECK(h[0] == Approx(2.0 / kPi).epsilon(0.01));
    CHECK(h[1] == Approx(1.0 / kPi).epsilon(0.01));
    CHECK(h[2] == Approx(2.0 / (3.0 * kPi)).epsilon(0.01));
    h = harmonics(1.0 / 3.0);
    CHECK(h[0] == Approx(4.0 / kPi).epsilon(0.01));
    CHECK(h[1] < 0.003);
    CHECK(h[2] == Approx(4.0 / (3.0 * kPi)).epsilon(0.01));
    h = harmonics(2.0 / 3.0);
    CHECK(h[0] == Approx(8.0 / (kPi * kPi)).epsilon(0.01));
    CHECK(h[1] < 0.003);
    CHECK(h[2] == Approx(8.0 / (9.0 * kPi * kPi)).epsilon(0.02));
    h = harmonics(1.0);
    CHECK(h[0] == Approx(1.0).epsilon(0.002));
    CHECK(h[1] < 1e-5);
    CHECK(h[2] < 1e-5);
    // Halfway between two shapes is their mix.
    h = harmonics(1.0 / 6.0);
    CHECK(h[0] == Approx(0.5 * (2.0 / kPi + 4.0 / kPi)).epsilon(0.01));
    CHECK(h[1] == Approx(0.5 / kPi).epsilon(0.02));

    // Pulse width: the share of the cycle spent high.
    for (const double width : {0.1, 0.25, 0.5, 0.8}) {
        Patch p = classicPatch(1.0 / 3.0);
        p.set("osc1.classic.pulse_width", width);
        const RenderResult r = render(p, 33, 1.0); // 55 Hz
        int high = 0, total = 0;
        for (std::size_t i = 4800; i < 43200; ++i, ++total)
            high += r.left[i] > 0.0f;
        INFO("pulse width " << width);
        CHECK(static_cast<double>(high) / total == Approx(width).margin(0.01));
    }
}

TEST_CASE("classic: step correction keeps aliasing well below the naive wave", "[classic][aliasing]") {
    // The Classic engine trades some purity for cost (plan 3.2). Its sawtooth is measured the
    // same way as the wavetable one and reported; the bound here is what the two-sample
    // polynomial correction can be relied on to give in the audible band.
    struct Mode {
        const char* name;
        double low, high; // limits up to C5 and from C5 to A6
    };
    for (const Mode mode : {Mode{"draft", -40.0, -30.0}, Mode{"standard", -55.0, -45.0}, Mode{"high", -65.0, -55.0}}) {
        double worstLow = -400.0, worstHigh = -400.0;
        for (const double note : {33.0, 45.3, 57.6, 64.2, 69.0, 76.9, 81.5, 88.1, 93.7}) {
            Patch p = classicPatch(0.0);
            p.set("global.quality_offline", choiceOf("global.quality_offline", mode.name));
            const int key = static_cast<int>(std::floor(note));
            p.set("osc1.coarse", note - key);
            const RenderResult r = render(p, key, 3.2, nullptr, 44100.0);
            const auto s = zedtest::spectrumOf(r.left, 8820, 131072, 44100.0);
            const double dbc = zedtest::nonHarmonicLevel(s, noteToHz(note), 20000.0).worstDbc;
            (note < 72.0 ? worstLow : worstHigh) = std::max(note < 72.0 ? worstLow : worstHigh, dbc);
        }
        char line[200];
        std::snprintf(line, sizeof line, "aliasing, classic sawtooth below 20 kHz, %s: %.1f dBc up to C5, %.1f dBc from C5 to A6",
                      mode.name, worstLow, worstHigh);
        WARN(line);
        CHECK(worstLow < mode.low);
        CHECK(worstHigh < mode.high);
    }
}

TEST_CASE("classic: unison and the amplitude warps work; phase shaping needs a wavetable", "[classic]") {
    Patch p = classicPatch(0.0);
    p.set("global.quality_offline", choiceOf("global.quality_offline", "draft"));
    const RenderResult plain = render(p, 57, 0.5);

    // A phase-shaping warp is inactive in the Classic engine: nothing changes.
    Patch shaped = p;
    shaped.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "sync"));
    shaped.set("osc1.warp_a.amount", 0.6);
    CHECK(maxDifference(render(shaped, 57, 0.5).left, plain.left) == 0.0);

    // Drive and fold act on it.
    Patch driven = p;
    driven.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "drive"));
    driven.set("osc1.warp_a.amount", 0.8);
    const RenderResult d = render(driven, 57, 0.5);
    CHECK(zedtest::rmsOf(d.left, 4800, 20000) > zedtest::rmsOf(plain.left, 4800, 20000) * 1.3);

    // Another source can modulate it.
    Patch modulated = p;
    modulated.set("osc2.on", 1);
    modulated.set("osc2.level", kSilenceDb);
    modulated.set("osc2.coarse", 12.0);
    modulated.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "rm"));
    modulated.set("osc1.warp_a.amount", 1.0);
    CHECK(maxDifference(render(modulated, 57, 0.5).left, plain.left) > 0.1);

    // Unison: seven voices, the same loudness within the usual 1.5 dB.
    Patch wide = classicPatch(0.0);
    wide.set("osc1.phase_random", 1);
    wide.set("osc1.unison.voices", 7);
    const RenderResult u = render(wide, 57, 3.0);
    const RenderResult one = render(classicPatch(0.0), 57, 3.0);
    const double uRms = std::sqrt(0.5 * (std::pow(zedtest::rmsOf(u.left, 24000, 140000), 2) + std::pow(zedtest::rmsOf(u.right, 24000, 140000), 2)));
    CHECK(std::abs(20.0 * std::log10(uRms / zedtest::rmsOf(one.left, 24000, 140000))) < 1.5);
    CHECK(maxDifference(u.left, u.right) > 0.05); // spread across the stereo field
}

// --- Sub ---------------------------------------------------------------------------------------

TEST_CASE("sub: an octave below by default, in tune, with six shapes", "[sub]") {
    Patch p = quietPatch();
    p.set("sub.on", 1);
    CHECK(std::abs(pitchCents(render(p, 69, 2.0).left, 220.0)) < 0.1); // one octave below A4
    for (int octave = -4; octave <= 0; ++octave) {
        p.set("sub.octave", octave);
        INFO("octave " << octave);
        CHECK(std::abs(pitchCents(render(p, 84, 2.0).left, noteToHz(84 + 12 * octave))) < 0.1);
    }
    p.set("sub.octave", 0);
    p.set("sub.coarse", 7.0);
    p.set("sub.fine", -25.0);
    CHECK(std::abs(pitchCents(render(p, 60, 2.0).left, noteToHz(60 + 7.0 - 0.25))) < 0.1);
    Patch tuned = quietPatch();
    tuned.set("sub.on", 1);
    tuned.set("sub.octave", 0);
    tuned.set("global.reference", 432.0);
    tuned.set("global.master_tune", 50.0);
    CHECK(std::abs(pitchCents(render(tuned, 69, 2.0).left, 432.0 * std::exp2(0.5 / 12.0))) < 0.1);

    // Shapes, by their loudness and their duty.
    const double level = std::sqrt(0.5);
    const auto shapeRms = [&](const char* shape) {
        Patch q = quietPatch();
        q.set("sub.on", 1);
        q.set("sub.shape", choiceOf("sub.shape", shape));
        const RenderResult r = render(q, 57, 1.0); // 110 Hz
        return zedtest::rmsOf(r.left, 4800, 43200) / level;
    };
    CHECK(shapeRms("sine") == Approx(std::sqrt(0.5)).epsilon(0.005));
    CHECK(shapeRms("triangle") == Approx(1.0 / std::sqrt(3.0)).epsilon(0.01));
    CHECK(shapeRms("saw") == Approx(1.0 / std::sqrt(3.0)).epsilon(0.01));
    CHECK(shapeRms("square") == Approx(1.0).epsilon(0.01));
    CHECK(shapeRms("pulse25") == Approx(1.0).epsilon(0.01));
    const double rounded = shapeRms("rounded_square");
    CHECK(rounded > 0.8); // fuller than a sine ...
    CHECK(rounded < 0.97); // ... softer than a square
    Patch pulse = quietPatch();
    pulse.set("sub.on", 1);
    pulse.set("sub.shape", choiceOf("sub.shape", "pulse25"));
    const RenderResult r = render(pulse, 57, 1.0);
    int high = 0;
    for (std::size_t i = 4800; i < 43200; ++i)
        high += r.left[i] > 0.0f;
    CHECK(high / 38400.0 == Approx(0.25).margin(0.01));
    // The rounded square keeps a square's odd harmonics but lets them fall away faster.
    Patch round = quietPatch();
    round.set("sub.on", 1);
    round.set("sub.shape", choiceOf("sub.shape", "rounded_square"));
    const auto s = zedtest::spectrumOf(render(round, 57, 1.0).left, 4800, 32768, kRate);
    CHECK(zedtest::lineLevel(s, 220.0) < 1e-3);
    CHECK(zedtest::lineLevel(s, 330.0) > 0.05);
    CHECK(zedtest::lineLevel(s, 330.0) / zedtest::lineLevel(s, 110.0) < 1.0 / 3.0);
}

TEST_CASE("sub: level, pan and joining the oscillators", "[sub]") {
    Patch p = quietPatch();
    p.set("sub.on", 1);
    const RenderResult centre = render(p, 57, 0.3);
    CHECK(maxDifference(centre.left, centre.right) == 0.0);
    p.set("sub.level", -6.0);
    CHECK(render(p, 57, 0.3).peak() / centre.peak() == Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.001));
    p.set("sub.level", 0.0);
    p.set("sub.pan", 1.0);
    const RenderResult right = render(p, 57, 0.3);
    CHECK(zedtest::peakOf(right.left, 0, right.left.size()) < 1e-6f);
    CHECK(zedtest::peakOf(right.right, 0, right.right.size()) == Approx(centre.peak() * std::sqrt(2.0f)).epsilon(0.001));
    p.set("sub.on", 0);
    CHECK(render(p, 57, 0.3).peak() == 0.0f);

    // Oscillator and Sub together: the sum of each alone.
    Patch both = quietPatch();
    both.set("osc1.on", 1);
    both.set("sub.on", 1);
    Patch oscOnly = both, subOnly = both;
    oscOnly.set("sub.on", 0);
    subOnly.set("osc1.on", 0);
    const RenderResult sum = render(both, 57, 0.3), a = render(oscOnly, 57, 0.3), b = render(subOnly, 57, 0.3);
    for (std::size_t i = 0; i < sum.left.size(); i += 5)
        REQUIRE(sum.left[i] == Approx(a.left[i] + b.left[i]).margin(1e-5));
}

// --- Texture -----------------------------------------------------------------------------------

TEST_CASE("texture: plays the built-in noise or a loaded sample at the right pitch", "[texture]") {
    Patch p = quietPatch();
    p.set("texture.on", 1);
    // Nothing loaded: stereo white noise.
    const RenderResult noise = render(p, 60, 1.0);
    CHECK(zedtest::rmsOf(noise.left, 4800, 43200) == Approx(0.5 / std::sqrt(3.0)).epsilon(0.05)); // uniform noise of peak 0.5
    CHECK(maxDifference(noise.left, noise.right) > 0.2);

    const auto tone = toneSample(1000.0, 1.0);
    CHECK(std::abs(pitchCents(render(p, 60, 0.9, tone).left, 1000.0)) < 0.5);
    // Key tracking is off by default: every key plays the sample as recorded.
    CHECK(std::abs(pitchCents(render(p, 84, 0.9, tone).left, 1000.0)) < 0.5);
    p.set("texture.pitch", 12.0);
    CHECK(std::abs(pitchCents(render(p, 60, 0.9, tone).left, 2000.0)) < 0.5);
    p.set("texture.pitch", -7.0);
    p.set("texture.fine", 50.0);
    CHECK(std::abs(pitchCents(render(p, 60, 0.9, tone).left, 1000.0 * std::exp2(-6.5 / 12.0))) < 0.5);
    p.set("texture.pitch", 0.0);
    p.set("texture.fine", 0.0);
    p.set("texture.keytrack", 1);
    CHECK(std::abs(pitchCents(render(p, 60, 0.9, tone).left, 1000.0)) < 0.5); // middle C is the sample's own pitch
    CHECK(std::abs(pitchCents(render(p, 72, 0.9, tone).left, 2000.0)) < 0.5);
    CHECK(std::abs(pitchCents(render(p, 53, 0.9, tone).left, 1000.0 * std::exp2(-7.0 / 12.0))) < 0.5);
    p.set("texture.keytrack", 0);
    // A sample recorded at another rate keeps its pitch.
    CHECK(std::abs(pitchCents(render(p, 60, 0.9, toneSample(500.0, 1.0, 22050.0)).left, 500.0)) < 0.5);
    CHECK(std::abs(pitchCents(render(p, 60, 0.9, tone, 96000.0).left, 1000.0, 96000.0)) < 0.5);
    // Level and both channels: the sample's left goes left, its right goes right.
    const RenderResult stereo = render(p, 60, 0.5, toneSample(1000.0, 1.0, 48000.0, 0.0));
    CHECK(zedtest::peakOf(stereo.left, 4800, 20000) == Approx(1.0f).epsilon(0.01));
    CHECK(zedtest::peakOf(stereo.right, 0, stereo.right.size()) == 0.0f);
    p.set("texture.pan", -1.0);
    p.set("texture.level", -12.0);
    const RenderResult panned = render(p, 60, 0.5, tone);
    CHECK(zedtest::peakOf(panned.left, 4800, 20000) == Approx(std::sqrt(2.0) * std::pow(10.0, -12.0 / 20.0)).epsilon(0.01));
    CHECK(zedtest::peakOf(panned.right, 0, panned.right.size()) < 1e-6f);
}

TEST_CASE("texture: one-shot and loop, loop points, start and random start", "[texture]") {
    Patch p = quietPatch();
    p.set("texture.on", 1);
    const auto shortTone = toneSample(1000.0, 0.25);

    p.set("texture.mode", choiceOf("texture.mode", "one_shot"));
    RenderResult r = render(p, 60, 1.0, shortTone);
    CHECK(zedtest::rmsOf(r.left, 2400, 9600) == Approx(std::sqrt(0.5)).epsilon(0.01));
    CHECK(zedtest::peakOf(r.left, 12100, 48000) == 0.0f); // the sample is over after a quarter second

    p.set("texture.mode", choiceOf("texture.mode", "loop"));
    r = render(p, 60, 1.0, shortTone);
    CHECK(zedtest::rmsOf(r.left, 24000, 43200) == Approx(std::sqrt(0.5)).epsilon(0.01)); // still going
    // 250 cycles fit the sample exactly, so the loop is seamless.
    CHECK(zedtest::largestJump(r.left, 4800, 43200) <= static_cast<float>(2.0 * kPi * 1000.0 / kRate * 1.01));

    // Loop points stored in the file: half a second of silence, then a tone that loops.
    WavData wav;
    wav.sampleRate = 48000.0;
    wav.channels.assign(2, std::vector<float>(36000, 0.0f));
    for (std::size_t i = 24000; i < 36000; ++i)
        wav.channels[0][i] = wav.channels[1][i] = static_cast<float>(std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / 48000.0));
    wav.hasLoop = true;
    wav.loopStart = 24000;
    wav.loopEnd = 36000;
    const auto looped = SampleData::fromWav(wav, "looped");
    CHECK(looped->hasLoop);
    r = render(p, 60, 2.0, looped);
    CHECK(zedtest::peakOf(r.left, 100, 23900) == 0.0f);                                   // the lead-in plays once
    CHECK(zedtest::rmsOf(r.left, 36000, 90000) == Approx(std::sqrt(0.5)).epsilon(0.01)); // then only the loop: no gaps
    // Without loop points the whole file loops, silence included.
    wav.hasLoop = false;
    r = render(p, 60, 2.0, SampleData::fromWav(wav, "whole"));
    CHECK(zedtest::peakOf(r.left, 36100, 59900) == 0.0f);

    // Start moves the playback position: halfway in, the tone begins at once.
    p.set("texture.start", 0.7);
    r = render(p, 60, 0.5, SampleData::fromWav(wav, "whole"));
    CHECK(zedtest::rmsOf(r.left, 100, 4800) > 0.5);
    // Random start: each note begins somewhere else.
    p.set("texture.start", 0.0);
    p.set("texture.random_start", 1.0);
    Rig rig(p);
    rig.engine.setTexture(SampleData::builtin());
    std::vector<float> firsts;
    for (int n = 0; n < 5; ++n) {
        const std::size_t at = rig.left.size();
        rig.run(2400, {MidiEvent::noteOn(0, 60, 1.0f)});
        rig.run(2400, {MidiEvent::noteOff(0, 60)});
        firsts.push_back(rig.left[at + 40]);
    }
    int different = 0;
    for (const float f : firsts)
        different += f != firsts[0];
    CHECK(different >= 3);
    p.set("texture.random_start", 0.0);
    Rig fixed(p);
    for (int n = 0; n < 3; ++n) {
        const std::size_t at = fixed.left.size();
        fixed.run(2400, {MidiEvent::noteOn(0, 60, 1.0f)});
        fixed.run(2400, {MidiEvent::noteOff(0, 60)});
        CHECK(fixed.left[at + 40] == fixed.left[40]);
    }
    // An empty or missing sample falls back to the built-in noise instead of failing.
    Rig fallback(p);
    fallback.engine.setTexture(SampleData::fromStereo({}, {}, 48000.0, "empty"));
    fallback.run(4800, {MidiEvent::noteOn(0, 60, 1.0f)});
    CHECK(zedtest::rmsOf(fallback.left, 480, 4800) > 0.1);
    CHECK(fallback.engine.texture()->name == "White Noise");
}

// --- Sends -------------------------------------------------------------------------------------

TEST_CASE("sends: every source reaches the output through the filter slots or directly", "[sends]") {
    // The two filter slots pass their input through until milestone 3, so wherever a source is
    // sent - filter 1, filter 2, the direct path, in series or in parallel - the result is the
    // same sound. This pins the routing arithmetic the filters will be dropped into.
    Patch base = quietPatch();
    base.set("osc1.on", 1);
    base.set("osc2.on", 1);
    base.set("osc2.coarse", 7.0);
    base.set("osc2.unison.voices", 3);
    base.set("sub.on", 1);
    base.set("texture.on", 1);
    const RenderResult reference = render(base, 57, 0.4);
    REQUIRE(reference.peak() > 0.3f);

    Rng rng(21);
    for (int trial = 0; trial < 12; ++trial) {
        Patch p = base;
        for (const char* id : {"osc1.sends.filter_balance", "osc1.sends.direct", "osc2.sends.filter_balance", "osc2.sends.direct",
                               "osc3.sends.direct", "sub.sends.filter_balance", "texture.sends.filter_balance", "voice.filter_routing"})
            p.set(id, trial < 2 ? trial : rng.uniform());
        p.set("sub.direct", static_cast<double>(rng.next() % 2));
        p.set("texture.direct", static_cast<double>(rng.next() % 2));
        const RenderResult r = render(p, 57, 0.4);
        INFO("trial " << trial);
        CHECK(maxDifference(r.left, reference.left) < 2e-5);
        CHECK(maxDifference(r.right, reference.right) < 2e-5);
    }
    // Turning a filter slot "on" changes nothing yet either.
    Patch filters = base;
    filters.set("filter1.on", 1);
    filters.set("filter2.on", 1);
    CHECK(maxDifference(render(filters, 57, 0.4).left, reference.left) < 2e-5);

    // Moving the sends while a note sounds is smooth.
    Rig rig(base);
    rig.run(4800, {MidiEvent::noteOn(0, 57, 1.0f)});
    Rig moved(base);
    moved.run(4800, {MidiEvent::noteOn(0, 57, 1.0f)});
    moved.engine.params().set(idx("osc1.sends.direct"), 1.0);
    moved.engine.params().set(idx("osc2.sends.filter_balance"), 1.0);
    moved.engine.params().set(idx("voice.filter_routing"), 1.0);
    rig.run(4800);
    moved.run(4800);
    CHECK(maxDifference(rig.left, moved.left) < 2e-5);
}

TEST_CASE("sources: an off source costs nothing and all five can sound together", "[sources]") {
    Patch all = quietPatch();
    for (const char* id : {"osc1.on", "osc2.on", "osc3.on", "sub.on", "texture.on"})
        all.set(id, 1);
    all.set("osc2.engine", choiceOf("osc2.engine", "classic"));
    all.set("osc2.coarse", 4.0);
    all.set("osc3.coarse", 7.0);
    all.set("osc3.unison.voices", 5);
    const RenderResult together = render(all, 57, 1.0);
    const auto s = zedtest::spectrumOf(together.left, 4800, 32768, kRate);
    CHECK(zedtest::lineLevel(s, noteToHz(57)) > 0.2);      // oscillator 1
    CHECK(zedtest::lineLevel(s, noteToHz(61)) > 0.2);      // oscillator 2 (Classic)
    CHECK(zedtest::lineLevel(s, noteToHz(57 - 12)) > 0.4); // Sub
    bool finite = true;
    for (const float v : together.left)
        finite = finite && std::isfinite(v);
    CHECK(finite);

    // With everything off a held note is silent.
    Patch none = quietPatch();
    CHECK(render(none, 57, 0.3).peak() == 0.0f);
}

// --- Swapping tables and samples while notes play ----------------------------------------------

TEST_CASE("hot swap: a new table fades in without a click while a note plays", "[hotswap][click]") {
    const auto tableA = TableData::build(sineTable(1));
    const auto tableB = TableData::build(sineTable(2));
    Patch p = quietPatch();
    p.set("osc1.on", 1);
    const double level = std::sqrt(0.5);
    const double slopeA = level * 2.0 * kPi * 110.0 / kRate, slopeB = 2.0 * slopeA;

    Rig rig(p);
    rig.engine.setTable(0, tableA);
    rig.run(4800, {MidiEvent::noteOn(0, 45, 1.0f)});
    rig.engine.setTable(0, tableB);
    CHECK(rig.engine.table(0).get() == tableB.get()); // what was offered last
    rig.run(24000);
    // During the crossfade both tables sound; the output never moves faster than the two waves
    // and the fade itself allow.
    const double bound = slopeA + slopeB + 2.0 * level / Engine::kSwapFadeSamples;
    CHECK(zedtest::largestJump(rig.left, 4700, 28800) <= bound);
    // Before the swap: the first table; once the fade is over: the second, at the same phase.
    auto before = zedtest::spectrumOf(rig.left, 480, 4096, kRate);
    CHECK(zedtest::lineLevel(before, 110.0) == Approx(level).epsilon(0.01));
    auto after = zedtest::spectrumOf(rig.left, 4800 + Engine::kSwapFadeSamples + 16, 16384, kRate);
    CHECK(zedtest::lineLevel(after, 220.0) == Approx(level).epsilon(0.01));
    CHECK(zedtest::lineLevel(after, 110.0) < 0.01);
    // Halfway through the fade both are there.
    const std::size_t middle = 4800 + Engine::kSwapFadeSamples / 2;
    const double expected = 0.5 * level * std::sin(2.0 * kPi * 110.0 * static_cast<double>(middle) / kRate) +
                            0.5 * level * std::sin(2.0 * kPi * 220.0 * static_cast<double>(middle) / kRate);
    CHECK(rig.left[middle] == Approx(expected).margin(0.01));

    // A note that starts after the swap plays only the new table.
    rig.run(2400, {MidiEvent::noteOff(0, 45)});
    const std::size_t next = rig.left.size();
    rig.run(20000, {MidiEvent::noteOn(0, 45, 1.0f)});
    auto fresh = zedtest::spectrumOf(rig.left, next + 480, 16384, kRate);
    CHECK(zedtest::lineLevel(fresh, 110.0) < 0.01);
    CHECK(zedtest::lineLevel(fresh, 220.0) == Approx(level).epsilon(0.01));

    // Tables of different sizes and frame counts swap just the same.
    Wavetable many;
    for (int k = 0; k < 40; ++k)
        many.frames.push_back(sineTable(1 + k % 5).frames[0]);
    many.morph = MorphMode::Spectral;
    rig.engine.params().set(idx("osc1.position"), 0.83);
    rig.engine.setTable(0, TableData::build(many));
    rig.run(4800);
    rig.engine.setTable(0, tableA);
    rig.run(4800);
    bool finite = true;
    for (const float v : rig.left)
        finite = finite && std::isfinite(v);
    CHECK(finite);
    CHECK(zedtest::largestJump(rig.left, next + 4800, rig.left.size()) < 0.2f);

    // Null selects the built-in table again.
    rig.engine.setTable(0, nullptr);
    CHECK(rig.engine.table(0).get() == TableData::builtin().get());
    rig.run(1024);
}

TEST_CASE("hot swap: the texture sample fades over as well", "[hotswap][click]") {
    Patch p = quietPatch();
    p.set("texture.on", 1);
    Rig rig(p);
    rig.engine.setTexture(toneSample(500.0, 1.0));
    rig.run(4800, {MidiEvent::noteOn(0, 60, 1.0f)});
    rig.engine.setTexture(toneSample(750.0, 1.0));
    rig.run(9600);
    const double slope = 2.0 * kPi / kRate;
    CHECK(zedtest::largestJump(rig.left, 4700, 14400) <= (500.0 + 750.0) * slope + 2.0 / Engine::kSwapFadeSamples);
    const auto after = zedtest::spectrumOf(rig.left, 4800 + 512, 8192, kRate);
    CHECK(zedtest::lineLevel(after, 750.0) == Approx(1.0).epsilon(0.02));
    CHECK(zedtest::lineLevel(after, 500.0) < 0.01);
}

TEST_CASE("hot swap: tables, samples and curves change from another thread while audio runs", "[hotswap][rt]") {
    // The audio thread renders busy blocks while a second thread keeps building and handing over
    // tables, samples and curves. The guard watches the audio thread for allocations, frees and
    // locks; at the end everything that was replaced has been freed, by the other thread.
    rt::resetViolations();
    Patch p;
    p.set("osc2.on", 1);
    p.set("osc3.on", 1);
    p.set("texture.on", 1);
    p.set("osc1.unison.voices", 5);
    p.set("osc2.warp_a.mode", choiceOf("osc2.warp_a.mode", "curve_full"));
    p.set("osc2.warp_a.amount", 0.7);
    p.set("global.polyphony", 12);
    Engine engine;
    engine.prepare(kRate);
    engine.loadPatch(p);

    std::atomic<bool> stop{false};
    std::vector<std::weak_ptr<const TableData>> handed;
    std::weak_ptr<const SampleData> lastSample;
    int swaps = 0;
    std::thread worker([&] {
        Rng rng(77);
        while (!stop.load()) {
            Wavetable t;
            const int frames = 1 + static_cast<int>(rng.next() % 6);
            for (int f = 0; f < frames; ++f)
                t.frames.push_back(sineTable(1 + static_cast<int>(rng.next() % 9), 0.2 + 0.8 * rng.uniform()).frames[0]);
            t.morph = rng.next() % 2 ? MorphMode::Crossfade : MorphMode::None;
            t.stepped = rng.next() % 4 == 0;
            auto data = TableData::build(t);
            handed.push_back(data);
            engine.setTable(static_cast<int>(rng.next() % 3), std::move(data));
            if (rng.next() % 3 == 0) {
                auto sample = toneSample(200.0 + 800.0 * rng.uniform(), 0.05 + 0.2 * rng.uniform());
                lastSample = sample;
                engine.setTexture(std::move(sample));
            }
            if (rng.next() % 3 == 0) {
                const float curve[] = {0.0f, static_cast<float>(rng.uniform()), static_cast<float>(rng.uniform()), 1.0f};
                engine.setWarpCurve(1, 0, curve);
            }
            ++swaps;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    Rng rng(5);
    std::vector<float> left(512), right(512);
    std::vector<MidiEvent> events;
    events.reserve(8);
    bool finite = true;
    float peak = 0.0f;
    for (int block = 0; block < 3000; ++block) {
        events.clear();
        if (block % 7 == 0)
            events.push_back(MidiEvent::noteOn(0, 40 + static_cast<int>(rng.next() % 40), 0.8f));
        if (block % 11 == 0)
            events.push_back(MidiEvent::noteOff(0, 40 + static_cast<int>(rng.next() % 40)));
        const int frames = 64 + static_cast<int>(rng.next() % 448);
        engine.process({left.data(), right.data(), frames, events, {}});
        for (int i = 0; i < frames; ++i) {
            finite = finite && std::isfinite(left[static_cast<std::size_t>(i)]);
            peak = std::max(peak, std::abs(left[static_cast<std::size_t>(i)]));
        }
    }
    stop.store(true);
    worker.join();
    INFO("last violation: " << rt::lastViolation());
    CHECK(rt::violationCount() == 0);
    CHECK(finite);
    CHECK(peak > 0.01f);
    CHECK(peak < 16.0f);
    CHECK(swaps > 20);

    // Let the audio side finish its fades and give everything back, then collect.
    for (int block = 0; block < 8; ++block)
        engine.process({left.data(), right.data(), 512, {}, {}});
    engine.collectGarbage();
    const auto current = {engine.table(0), engine.table(1), engine.table(2)};
    int alive = 0;
    for (const auto& weak : handed) {
        const auto strong = weak.lock();
        if (strong == nullptr)
            continue;
        bool inUse = false;
        for (const auto& table : current)
            inUse = inUse || table.get() == strong.get();
        alive += inUse ? 0 : 1;
    }
    CHECK(alive == 0); // nothing leaked, nothing still held by the engine except what it plays
    CHECK(rt::violationCount() == 0);
}
