// The wavetable oscillator (plan 3.2 to 3.4, 6.1): frame morphing and the click test, unison,
// the wide kernels against the scalar reference, phase controls, warp modes and cross-modulation.
#include "../support/Analysis.h"
#include "../support/TestUtil.h"
#include "zederia/engine/OscKernel.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>

using namespace zed;
using Catch::Approx;
using zedtest::idx;

namespace {

constexpr double kRate = 48000.0;

int choiceOf(const char* id, const char* token) { return Registry::get().find(id)->choiceIndex(token); }

// Nothing but oscillator 1 at full level with a flat envelope and a fixed start phase.
Patch basePatch() {
    Patch p;
    p.set("osc1.phase_random", 0);
    p.set("osc1.level", 0);
    p.set("global.master", 0);
    p.set("env1.attack", 0);
    p.set("env1.sustain", 1);
    p.set("env1.release", 5);
    return p;
}

// A table whose frame k is a sine at harmonic k + 1: smooth frames that differ a lot.
Wavetable harmonicLadder(int frames) {
    Wavetable t;
    t.name = "ladder";
    for (int k = 0; k < frames; ++k) {
        Frame f;
        for (int n = 0; n < kFrameSize; ++n)
            f[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(2.0 * kPi * (k + 1) * n / kFrameSize));
        t.frames.push_back(f);
    }
    return t;
}

Wavetable sineTable() { return harmonicLadder(1); }

// Drives an engine block by block and keeps everything it produced.
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

RenderResult renderWith(const Patch& patch, int note, double seconds, std::shared_ptr<const TableData> table = nullptr,
                        double sampleRate = kRate) {
    const NoteSpec notes[] = {{note, 0.0, seconds, 1.0}};
    RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.tables[0] = table;
    settings.tables[1] = table;
    settings.tables[2] = table;
    return renderOffline(patch, sequenceFromNotes(notes, sampleRate), settings);
}

struct ForcedKernel {
    explicit ForcedKernel(int width) { osc::forceKernelWidth(width); }
    ~ForcedKernel() { osc::forceKernelWidth(0); }
};

} // namespace

// --- Frame morphing ----------------------------------------------------------------------------

TEST_CASE("osc: a two-second position sweep has no discontinuity", "[osc][click]") {
    // Plan 5.7 and 6.1: the largest sample-to-sample jump of the sweep is compared with the
    // bound for the signal itself, taken from steady notes at fixed positions.
    const auto table = TableData::build(harmonicLadder(8));
    Patch patch = basePatch();
    const int note = 45; // 110 Hz
    const int total = static_cast<int>(2.2 * kRate);

    double steady = 0.0;
    for (int k = 0; k <= 7; ++k) {
        patch.set("osc1.position", k / 7.0);
        const RenderResult r = renderWith(patch, note, 0.2, table);
        steady = std::max<double>(steady, zedtest::largestJump(r.left, 2400, 9000));
    }
    // The steepest frame is a sine at harmonic 8: its largest step is amplitude * 2 pi f / rate.
    const double level = std::sqrt(0.5); // centre pan
    CHECK(steady == Approx(level * 2.0 * kPi * 8.0 * 110.0 / kRate).epsilon(0.02));

    for (const int block : {64, 512, 2048}) { // automation as a host delivers it, fine and coarse
        Rig rig(patch);
        rig.engine.setTable(0, table);
        rig.engine.params().set(idx("osc1.position"), 0.0);
        rig.run(4800, {MidiEvent::noteOn(0, note, 1.0f)});
        for (int done = 0; done < total; done += block) {
            rig.engine.params().set(idx("osc1.position"), std::min(1.0, done / (2.0 * kRate)));
            rig.run(block);
        }
        const float jump = zedtest::largestJump(rig.left, 100, rig.left.size());
        INFO("automation every " << block << " samples: largest jump " << jump << ", steady bound " << steady);
        CHECK(jump <= steady * 1.05);
    }

    // The same sweep on a table flagged Stepped must show its staircase: this proves the
    // measure can see a step when there is one.
    Wavetable stepped = harmonicLadder(8);
    stepped.stepped = true;
    Rig rig(patch);
    rig.engine.setTable(0, TableData::build(stepped));
    rig.engine.params().set(idx("osc1.position"), 0.0);
    rig.run(4800, {MidiEvent::noteOn(0, note, 1.0f)});
    for (int done = 0; done < total; done += 64) {
        rig.engine.params().set(idx("osc1.position"), std::min(1.0, done / (2.0 * kRate)));
        rig.run(64);
    }
    const float steppedJump = zedtest::largestJump(rig.left, 100, rig.left.size());
    WARN("click test: smooth sweep stays within the steady bound " << steady << "; a stepped table jumps by " << steppedJump);
    CHECK(steppedJump > steady * 1.5);
}

TEST_CASE("osc: position blends neighbouring frames and reaches both ends", "[osc]") {
    const auto table = TableData::build(harmonicLadder(3)); // harmonics 1, 2, 3
    Patch patch = basePatch();
    const auto harmonicLevels = [&](double position) {
        patch.set("osc1.position", position);
        const RenderResult r = renderWith(patch, 57, 0.6, table); // 220 Hz
        const auto s = zedtest::spectrumOf(r.left, 4800, 16384, kRate);
        std::array<double, 3> out{};
        for (int h = 1; h <= 3; ++h)
            out[static_cast<std::size_t>(h - 1)] = zedtest::lineLevel(s, h * 220.0);
        return out;
    };
    const double full = std::sqrt(0.5);
    auto l = harmonicLevels(0.0);
    CHECK(l[0] == Approx(full).epsilon(0.002));
    CHECK(l[1] < 1e-5);
    l = harmonicLevels(0.25); // halfway between frames 1 and 2
    CHECK(l[0] == Approx(full * 0.5).epsilon(0.005));
    CHECK(l[1] == Approx(full * 0.5).epsilon(0.005));
    CHECK(l[2] < 1e-5);
    l = harmonicLevels(1.0);
    CHECK(l[2] == Approx(full).epsilon(0.002));
    CHECK(l[0] < 1e-5);
}

TEST_CASE("osc: the band-limited level changes with pitch without a click", "[osc][click]") {
    // A glide over two octaves crosses six levels. With a sine in the table every level holds
    // the same wave, so the glide must be as smooth as the sine itself: a wrong read while the
    // level changes would show as a jump.
    Patch patch = basePatch();
    patch.set("global.bend_up", 24);
    Rig rig(patch);
    rig.engine.setTable(0, TableData::build(sineTable()));
    rig.run(4800, {MidiEvent::noteOn(0, 60, 1.0f)});
    for (int i = 0; i < 2000; ++i)
        rig.run(48, {MidiEvent::pitchBend(0, static_cast<float>(i) / 2000.0f)});
    const double steepest = std::sqrt(0.5) * 2.0 * kPi * noteToHz(84) / kRate;
    CHECK(zedtest::largestJump(rig.left, 100, rig.left.size()) <= steepest * 1.02);

    // With a harmonic-rich wave the loudness stays put while harmonics leave at the top.
    Patch square = basePatch();
    square.set("osc1.position", 1.0 / 3.0); // the square of the built-in table
    square.set("global.bend_up", 24);
    Rig rich(square);
    rich.run(4800, {MidiEvent::noteOn(0, 48, 1.0f)});
    for (int i = 0; i < 2000; ++i)
        rich.run(48, {MidiEvent::pitchBend(0, static_cast<float>(i) / 2000.0f)});
    const double first = zedtest::rmsOf(rich.left, 4800, 14400);
    for (std::size_t from = 14400; from + 9600 <= rich.left.size(); from += 9600)
        CHECK(zedtest::rmsOf(rich.left, from, from + 9600) == Approx(first).epsilon(0.03));
    bool finite = true;
    for (const float s : rich.left)
        finite = finite && std::isfinite(s);
    CHECK(finite);
}

// --- Unison ------------------------------------------------------------------------------------

TEST_CASE("unison: loudness stays within 1.5 dB whatever the voice count", "[osc][unison]") {
    Patch patch = basePatch();
    patch.set("osc1.phase_random", 1);
    double reference = 0.0, lowest = 1e9, highest = -1e9;
    for (int voices = 1; voices <= 16; ++voices) {
        patch.set("osc1.unison.voices", voices);
        const RenderResult r = renderWith(patch, 57, 4.0);
        const auto from = static_cast<std::size_t>(0.5 * kRate), to = static_cast<std::size_t>(3.9 * kRate);
        const double rms = std::sqrt(0.5 * (std::pow(zedtest::rmsOf(r.left, from, to), 2) + std::pow(zedtest::rmsOf(r.right, from, to), 2)));
        if (voices == 1)
            reference = rms;
        const double db = 20.0 * std::log10(rms / reference);
        INFO(voices << " voices: " << db << " dB");
        CHECK(std::abs(db) <= 1.5);
        lowest = std::min(lowest, db);
        highest = std::max(highest, db);
    }
    char line[120];
    std::snprintf(line, sizeof line, "unison level compensation, 1 to 16 voices: %.2f to %+.2f dB against one voice", lowest, highest);
    WARN(line);
}

TEST_CASE("unison: detune, range and spread laws place the voices", "[osc][unison]") {
    const auto table = TableData::build(sineTable());
    Patch patch = basePatch();
    patch.set("osc1.unison.voices", 5);
    patch.set("osc1.unison.detune", 1.0);
    patch.set("osc1.unison.range", 2.0); // outer voices one semitone below and above
    patch.set("osc1.unison.blend", 0.5);
    patch.set("osc1.unison.width", 0.0);
    const auto peaks = [&](const Patch& p) {
        const RenderResult r = renderWith(p, 69, 3.0, table);
        const auto s = zedtest::spectrumOf(r.left, 9600, 131072, kRate);
        std::vector<double> found;
        for (std::size_t k = 2; k + 2 < s.magnitude.size(); ++k)
            if (s.magnitude[k] > 0.02 && s.magnitude[k] > s.magnitude[k - 1] && s.magnitude[k] >= s.magnitude[k + 1])
                found.push_back(1200.0 * std::log2(k * s.binHz / 440.0)); // cents from the note
        return found;
    };
    auto cents = peaks(patch);
    REQUIRE(cents.size() == 5);
    CHECK(cents.front() == Approx(-100.0).margin(1.5));
    CHECK(cents[1] == Approx(-50.0).margin(1.5)); // linear: evenly spaced
    CHECK(cents[2] == Approx(0.0).margin(1.5));
    CHECK(cents.back() == Approx(100.0).margin(1.5));

    patch.set("osc1.unison.law", choiceOf("osc1.unison.law", "ensemble"));
    cents = peaks(patch);
    REQUIRE(cents.size() == 5);
    CHECK(cents[1] == Approx(-25.0).margin(1.5)); // denser near the centre
    CHECK(cents.back() == Approx(100.0).margin(1.5));

    patch.set("osc1.unison.law", choiceOf("osc1.unison.law", "inverse"));
    cents = peaks(patch);
    REQUIRE(cents.size() == 5);
    CHECK(cents[1] == Approx(-100.0 * std::sqrt(0.5)).margin(1.5)); // denser near the edges

    patch.set("osc1.unison.law", choiceOf("osc1.unison.law", "linear"));
    patch.set("osc1.unison.detune", 0.5);
    patch.set("osc1.unison.range", 12.0); // half of twelve semitones: three either way
    cents = peaks(patch);
    REQUIRE(cents.size() == 5);
    CHECK(cents.front() == Approx(-300.0).margin(2.0));

    // Stack: alternate voices an octave up.
    patch.set("osc1.unison.detune", 0.0);
    patch.set("osc1.unison.stack", choiceOf("osc1.unison.stack", "octave"));
    cents = peaks(patch);
    REQUIRE(cents.size() == 2);
    CHECK(cents[0] == Approx(0.0).margin(1.0));
    CHECK(cents[1] == Approx(1200.0).margin(1.0));
    patch.set("osc1.unison.stack", choiceOf("osc1.unison.stack", "center_down_12"));
    cents = peaks(patch);
    REQUIRE(cents.size() == 2);
    CHECK(cents[0] == Approx(-1200.0).margin(1.0));
}

TEST_CASE("unison: width spreads the voices and blend balances centre against sides", "[osc][unison]") {
    const auto table = TableData::build(sineTable());
    Patch patch = basePatch();
    patch.set("osc1.unison.voices", 7);
    patch.set("osc1.unison.detune", 0.5);
    patch.set("osc1.phase_random", 1);

    patch.set("osc1.unison.width", 0.0);
    RenderResult r = renderWith(patch, 60, 1.0, table);
    double difference = 0.0;
    for (std::size_t i = 0; i < r.left.size(); ++i)
        difference = std::max<double>(difference, std::abs(r.left[i] - r.right[i]));
    CHECK(difference < 1e-5); // mono

    patch.set("osc1.unison.width", 1.0);
    r = renderWith(patch, 60, 2.0, table);
    double side = 0.0, mid = 0.0;
    for (std::size_t i = 4800; i < r.left.size(); ++i) {
        side += std::pow(r.left[i] - r.right[i], 2);
        mid += std::pow(r.left[i] + r.right[i], 2);
    }
    CHECK(side / mid > 0.15); // clearly wide
    // Both sides carry the same energy.
    CHECK(zedtest::rmsOf(r.left, 4800, r.left.size()) == Approx(zedtest::rmsOf(r.right, 4800, r.right.size())).epsilon(0.1));

    // Blend at zero leaves only the centre voice: a single spectral line.
    patch.set("osc1.unison.blend", 0.0);
    patch.set("osc1.unison.detune", 1.0);
    patch.set("osc1.unison.range", 4.0);
    patch.set("osc1.unison.width", 0.0);
    r = renderWith(patch, 69, 3.0, table);
    auto s = zedtest::spectrumOf(r.left, 9600, 131072, kRate);
    int lines = 0;
    for (std::size_t k = 2; k + 2 < s.magnitude.size(); ++k)
        lines += s.magnitude[k] > 0.005 && s.magnitude[k] > s.magnitude[k - 1] && s.magnitude[k] >= s.magnitude[k + 1];
    CHECK(lines == 1);
    patch.set("osc1.unison.blend", 1.0); // and at 100 % the centre voice is gone
    r = renderWith(patch, 69, 3.0, table);
    s = zedtest::spectrumOf(r.left, 9600, 131072, kRate);
    const int centreBin = static_cast<int>(std::lround(440.0 / s.binHz));
    CHECK(s.magnitude[static_cast<std::size_t>(centreBin)] < 1e-4);
}

TEST_CASE("unison: position and warp spread differ per voice", "[osc][unison]") {
    const auto table = TableData::build(harmonicLadder(5));
    Patch patch = basePatch();
    patch.set("osc1.unison.voices", 3);
    patch.set("osc1.unison.detune", 0.0);
    patch.set("osc1.unison.blend", 0.5);
    patch.set("osc1.unison.width", 0.0);
    patch.set("osc1.position", 0.5); // frame 3: harmonic 3
    const auto level = [&](const Patch& p, int harmonic) {
        const RenderResult r = renderWith(p, 57, 0.6, table);
        const auto s = zedtest::spectrumOf(r.left, 4800, 16384, kRate);
        return zedtest::lineLevel(s, harmonic * 220.0);
    };
    CHECK(level(patch, 1) < 1e-4);
    CHECK(level(patch, 5) < 1e-4);
    patch.set("osc1.unison.position_spread", 0.5); // the outer voices move to the ends of the table
    CHECK(level(patch, 1) > 0.05);
    CHECK(level(patch, 3) > 0.05);
    CHECK(level(patch, 5) > 0.05);
}

TEST_CASE("unison: start phases are free, random or consistent", "[osc][unison]") {
    Patch patch = basePatch();
    patch.set("osc1.unison.voices", 7);
    const auto twoNotes = [&](const char* mode) {
        patch.set("osc1.unison.phase", choiceOf("osc1.unison.phase", mode));
        Rig rig(patch);
        rig.run(4800, {MidiEvent::noteOn(0, 60, 1.0f)});
        rig.run(4800, {MidiEvent::noteOff(0, 60)});
        rig.run(4800, {MidiEvent::noteOn(0, 60, 1.0f)});
        return rig.left;
    };
    const auto same = [](const std::vector<float>& x) {
        for (std::size_t i = 0; i < 2000; ++i)
            if (x[i] != x[9600 + i])
                return false;
        return true;
    };
    CHECK(same(twoNotes("consistent")));  // every note attacks the same way
    CHECK_FALSE(same(twoNotes("random"))); // every note differs
    CHECK_FALSE(same(twoNotes("free")));   // the voices kept running in between
}

// --- SIMD against scalar -----------------------------------------------------------------------

TEST_CASE("kernel: four and eight lanes give exactly what one lane gives", "[osc][simd]") {
    const auto table = TableData::build(harmonicLadder(6));
    struct Case {
        const char* name;
        Patch patch;
    };
    std::vector<Case> cases;
    {
        Patch p = basePatch();
        p.set("osc1.phase_random", 1);
        p.set("osc1.unison.voices", 7);
        p.set("osc1.position", 0.37);
        cases.push_back({"unison 7", p});
        p.set("osc1.unison.voices", 16);
        p.set("osc1.unison.position_spread", 0.4);
        p.set("osc1.unison.warp_spread", -0.3);
        p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "bend_both"));
        p.set("osc1.warp_a.amount", 0.8);
        p.set("osc1.warp_b.mode", choiceOf("osc1.warp_b.mode", "fold"));
        p.set("osc1.warp_b.amount", 0.6);
        cases.push_back({"unison 16, warped", p});
        p.set("osc1.unison.voices", 3);
        p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "soft_sync"));
        p.set("osc1.warp_b.mode", choiceOf("osc1.warp_b.mode", "curve_half"));
        cases.push_back({"unison 3, sync and curve", p});
        p.set("osc1.unison.voices", 11);
        p.set("osc1.engine", choiceOf("osc1.engine", "classic"));
        p.set("osc1.classic.shape", 0.45);
        p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "drive"));
        cases.push_back({"classic, unison 11", p});
        Patch q = basePatch();
        q.set("osc1.unison.voices", 9);
        q.set("osc2.on", 1);
        q.set("osc2.coarse", 7.02);
        q.set("osc2.unison.voices", 2);
        q.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "pm"));
        q.set("osc1.warp_a.amount", 0.5);
        q.set("osc1.warp_b.mode", choiceOf("osc1.warp_b.mode", "quantize"));
        q.set("osc1.warp_b.amount", 0.3);
        cases.push_back({"phase modulation", q});
    }
    for (const auto& c : cases) {
        RenderResult reference;
        {
            const ForcedKernel scalar(1);
            reference = renderWith(c.patch, 50, 0.5, table);
        }
        REQUIRE(reference.peak() > 0.01f);
        for (const int width : {4, 8, 0}) {
            const ForcedKernel forced(width);
            const RenderResult r = renderWith(c.patch, 50, 0.5, table);
            INFO(c.name << ", kernel width " << width);
            REQUIRE(r.left.size() == reference.left.size());
            CHECK(std::memcmp(r.left.data(), reference.left.data(), r.left.size() * sizeof(float)) == 0);
            CHECK(std::memcmp(r.right.data(), reference.right.data(), r.right.size() * sizeof(float)) == 0);
        }
    }
}

TEST_CASE("kernel: the reference read of a table matches what the oscillator plays", "[osc]") {
    // One lane, no smoothing in the way: the kernel's output is the table read at each phase.
    const auto table = TableData::build(TableData::builtinWavetable());
    osc::Lanes lanes{};
    lanes.count = 1;
    const float inc = 0.01234f;
    const int level = TableData::levelFor(inc);
    lanes.inc[0] = inc;
    lanes.fpos[0] = 0.6f;
    lanes.gL[0] = 1.0f;
    lanes.tabOff[0] = lanes.tabOff2[0] = static_cast<float>(table->level(level).offset);
    lanes.tabStride[0] = lanes.tabStride2[0] = static_cast<float>(table->level(level).stride);
    lanes.tabSize[0] = lanes.tabSize2[0] = static_cast<float>(table->level(level).size);
    float out[64];
    osc::Block block;
    block.samples = 64;
    block.table = table->data();
    block.maxFrame = static_cast<float>(table->frames() - 1);
    block.outL = out;
    osc::renderScalar(lanes, block);
    std::uint32_t phase = 0;
    for (int s = 0; s < 64; ++s) {
        const double expected = table->read(level, 0.6, (phase >> 8) / 16777216.0);
        CHECK(out[s] == Approx(expected).margin(2e-5));
        phase += static_cast<std::uint32_t>(inc * 4294967296.0f);
    }
}

// --- Phase -------------------------------------------------------------------------------------

TEST_CASE("osc: start phase, phase random and continue", "[osc][phase]") {
    const auto table = TableData::build(sineTable());
    Patch patch = basePatch();
    const auto first = [&](const Patch& p) { return renderWith(p, 57, 0.05, table).left; };

    // Phase 0 starts a sine at zero going up; 90 degrees starts it at its peak; 180 inverts it.
    // (The first 16 samples are the rise of the amp envelope, so the wave is checked just after.)
    const double level = std::sqrt(0.5), step = 2.0 * kPi * 220.0 / kRate;
    auto x = first(patch);
    for (std::size_t n = 20; n < 400; n += 9)
        REQUIRE(x[n] == Approx(level * std::sin(step * static_cast<double>(n))).margin(2e-4));
    patch.set("osc1.phase", 90);
    auto quarter = first(patch);
    for (std::size_t n = 20; n < 400; n += 9)
        REQUIRE(quarter[n] == Approx(level * std::cos(step * static_cast<double>(n))).margin(2e-4));
    patch.set("osc1.phase", 180);
    auto y = first(patch);
    for (std::size_t i = 0; i < 400; ++i)
        REQUIRE(y[i] == Approx(-x[i]).margin(2e-4));

    // Phase random: 0 % repeats exactly, 100 % does not.
    patch.set("osc1.phase", 0);
    const auto notes = [&](double random) {
        patch.set("osc1.phase_random", random);
        Rig rig(patch);
        rig.engine.setTable(0, table);
        std::vector<float> starts;
        for (int n = 0; n < 6; ++n) {
            const std::size_t at = rig.left.size();
            rig.run(2400, {MidiEvent::noteOn(0, 57, 1.0f)});
            rig.run(2400, {MidiEvent::noteOff(0, 57)});
            starts.push_back(rig.left[at + 20]);
        }
        return starts;
    };
    auto starts = notes(0.0);
    for (const float s : starts)
        CHECK(s == starts[0]);
    starts = notes(1.0);
    int different = 0;
    for (const float s : starts)
        different += std::abs(s - starts[0]) > 1e-3f;
    CHECK(different >= 4);

    // Continue: a new note carries on from where the last one is, so two overlapping notes of
    // the same pitch stay exactly in phase.
    patch.set("osc1.phase_random", 1.0);
    patch.set("osc1.phase_mode", choiceOf("osc1.phase_mode", "continue"));
    patch.set("osc1.keytrack", 0); // fixed pitch: both voices play the same frequency
    Rig rig(patch);
    rig.engine.setTable(0, table);
    const double fixedHz = noteToHz(60);
    for (const int gap : {4800, 5000, 5333}) {
        Rig both(patch);
        both.engine.setTable(0, table);
        both.run(gap, {MidiEvent::noteOn(0, 57, 1.0f)});
        both.run(24000, {MidiEvent::noteOn(0, 64, 1.0f)});
        const auto s = zedtest::spectrumOf(both.left, static_cast<std::size_t>(gap) + 2400, 16384, kRate);
        INFO("second note after " << gap << " samples");
        CHECK(zedtest::lineLevel(s, fixedHz) == Approx(2.0 * level).epsilon(0.002)); // in phase: amplitudes add
    }
    // With the phase reset per note (and fully random) the two voices do not line up.
    patch.set("osc1.phase_mode", choiceOf("osc1.phase_mode", "reset"));
    int outOfPhase = 0;
    for (const int gap : {4800, 4837, 4873, 4910, 4947}) { // the first voice is a fifth of a cycle further on each time
        Rig both(patch);
        both.engine.setTable(0, table);
        both.run(gap, {MidiEvent::noteOn(0, 57, 1.0f)});
        both.run(24000, {MidiEvent::noteOn(0, 64, 1.0f)});
        const auto s = zedtest::spectrumOf(both.left, static_cast<std::size_t>(gap) + 2400, 16384, kRate);
        outOfPhase += zedtest::lineLevel(s, fixedHz) < 2.0 * level * 0.97;
    }
    CHECK(outOfPhase >= 3);
}

// --- Warp modes --------------------------------------------------------------------------------

TEST_CASE("warp: every mode is neutral where the plan says so and audible elsewhere", "[osc][warp]") {
    const auto table = TableData::build(TableData::builtinWavetable());
    Patch plain = basePatch();
    plain.set("global.quality_offline", choiceOf("global.quality_offline", "draft")); // same rate with and without a warp
    plain.set("osc1.position", 1.0); // the sine: stays inside -1..1 everywhere
    plain.set("osc2.on", 1);         // a modulator for pm, am and rm
    plain.set("osc2.level", kSilenceDb);
    plain.set("osc2.coarse", 12.0);
    const RenderResult reference = renderWith(plain, 57, 0.25, table);
    const auto& modes = Registry::get().find("osc1.warp_a.mode")->choices;
    REQUIRE(modes.size() == static_cast<std::size_t>(osc::kNumWarpModes));

    for (const char* slot : {"warp_a", "warp_b"}) {
        for (std::size_t m = 1; m < modes.size(); ++m) {
            const std::string name = modes[m];
            const std::string modeId = std::string("osc1.") + slot + ".mode", amountId = std::string("osc1.") + slot + ".amount";
            Patch p = plain;
            p.set(modeId, static_cast<double>(m));
            const bool twoSided = name == "bend_both" || name == "skew_both";
            const bool neverNeutral = name == "quantize";
            INFO(slot << " " << name);

            p.set(amountId, twoSided ? 0.5 : 0.0);
            const RenderResult neutral = renderWith(p, 57, 0.25, table);
            double difference = 0.0;
            for (std::size_t i = 0; i < neutral.left.size(); ++i)
                difference = std::max<double>(difference, std::abs(neutral.left[i] - reference.left[i]));
            if (!neverNeutral)
                CHECK(difference < 2e-4);

            p.set(amountId, 0.8);
            const RenderResult active = renderWith(p, 57, 0.25, table);
            difference = 0.0;
            bool finite = true;
            for (std::size_t i = 0; i < active.left.size(); ++i) {
                difference = std::max<double>(difference, std::abs(active.left[i] - reference.left[i]));
                finite = finite && std::isfinite(active.left[i]);
            }
            CHECK(finite);
            CHECK(difference > 0.01);
            CHECK(active.peak() < 4.0f);
        }
    }
}

TEST_CASE("warp: each mode does what its description says", "[osc][warp]") {
    const auto sine = TableData::build(sineTable());
    Patch patch = basePatch();
    patch.set("global.quality_offline", choiceOf("global.quality_offline", "draft"));
    patch.set("global.quality", choiceOf("global.quality", "draft"));
    const int note = 45; // 110 Hz: 436.36 samples per cycle
    const double period = kRate / 110.0;
    const auto render = [&](const char* mode, double amount) {
        Patch p = patch;
        p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", mode));
        p.set("osc1.warp_a.amount", amount);
        return renderWith(p, note, 0.5, sine).left;
    };
    const double level = std::sqrt(0.5);

    SECTION("sync plays a faster inner cycle that restarts every period") {
        // Amount 25 % is a ratio of 2: the sine simply runs twice per period.
        const auto x = render("sync", 0.25);
        CHECK(zedtest::cents(zedtest::measureFrequency(x, 2400, 21600, kRate), 220.0) == Approx(0.0).margin(1.0));
        // A ratio that is not whole restarts mid-cycle: the output has an edge once per period.
        const auto y = render("sync", 0.35);
        CHECK(zedtest::largestJump(y, 2400, 21600) > 0.3f);
    }
    SECTION("soft sync removes the restart edge with a window") {
        const auto hard = render("sync", 0.35);
        const auto soft = render("soft_sync", 0.35);
        CHECK(zedtest::largestJump(soft, 2400, 21600) < zedtest::largestJump(hard, 2400, 21600) * 0.3f);
    }
    SECTION("pulse width squeezes the cycle into part of the period") {
        const auto x = render("pulse_width", 0.5); // the cycle takes 52 % of the period
        int flat = 0;
        for (std::size_t i = 4800; i < 4800 + static_cast<std::size_t>(period * 20); ++i)
            flat += std::abs(x[i]) < 2e-3f;
        CHECK(flat / (period * 20) == Approx(0.48).margin(0.03));
        CHECK(zedtest::peakOf(x, 4800, 9600) == Approx(level).epsilon(0.01));
    }
    SECTION("mirror plays the cycle forward then backward") {
        const auto x = render("mirror", 1.0);
        // The first half period holds the whole sine, the second half the same in reverse.
        const std::size_t start = static_cast<std::size_t>(std::lround(period * 12));
        for (int i = 1; i < 200; ++i) {
            const double a = x[start + static_cast<std::size_t>(i)];
            const double tb = period * 25 - static_cast<double>(start + static_cast<std::size_t>(i));
            const std::size_t ib = static_cast<std::size_t>(tb);
            const double b = x[ib] + (x[ib + 1] - x[ib]) * (tb - static_cast<double>(ib));
            REQUIRE(a == Approx(b).margin(0.02));
        }
    }
    SECTION("flip inverts polarity after a movable point") {
        const auto x = render("flip", 0.5);
        const auto plain = render("off", 0.0);
        const std::size_t start = static_cast<std::size_t>(std::lround(period * 12));
        const auto quarter = static_cast<std::size_t>(period / 4), three = static_cast<std::size_t>(period * 3 / 4);
        CHECK(x[start + quarter] == Approx(plain[start + quarter]).margin(1e-4));
        CHECK(x[start + three] == Approx(-plain[start + three]).margin(1e-4));
    }
    SECTION("quantize steps the phase") {
        const auto x = render("quantize", 8.0 / 9.0); // four steps per cycle: the sine becomes 0, 1, 0, -1
        std::vector<float> values;
        for (std::size_t i = 4800; i < 9600; ++i) {
            bool known = false;
            for (const float v : values)
                known = known || std::abs(v - x[i]) < 1e-3f;
            if (!known)
                values.push_back(x[i]);
        }
        CHECK(values.size() == 3);
    }
    SECTION("bend and skew move the wave without changing its pitch or peak") {
        for (const char* mode : {"bend_plus", "bend_minus", "bend_both", "skew_plus", "skew_minus", "skew_both"}) {
            const auto x = render(mode, 0.9);
            INFO(mode);
            CHECK(zedtest::peakOf(x, 4800, 21600) == Approx(level).epsilon(0.02));
            const auto s = zedtest::spectrumOf(x, 2400, 16384, kRate);
            const int bin = static_cast<int>(std::lround(110.0 / s.binHz));
            CHECK(s.magnitude[static_cast<std::size_t>(bin)] > 0.05);
            // Skew leans the cycle to one side: its zero crossing leaves the middle.
        }
        const auto skewed = render("skew_plus", 0.9);
        const std::size_t start = static_cast<std::size_t>(std::lround(period * 12));
        const std::size_t middle = start + static_cast<std::size_t>(period / 2);
        CHECK(std::abs(skewed[middle]) > 0.2f); // a plain sine is at zero there
    }
    SECTION("the curve variants reshape the phase by the drawn curve") {
        for (const char* mode : {"curve_full", "curve_half", "curve_sine", "curve_quad"}) {
            const auto a = render(mode, 1.0);
            INFO(mode);
            CHECK(zedtest::peakOf(a, 4800, 21600) == Approx(level).epsilon(0.03));
        }
        // A drawn curve replaces the default: a straight line makes the full-cycle variant neutral.
        Patch p = patch;
        p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "curve_full"));
        p.set("osc1.warp_a.amount", 1.0);
        Rig rig(p);
        rig.engine.setTable(0, sine);
        const float line[] = {0.0f, 1.0f};
        rig.engine.setWarpCurve(0, 0, line);
        rig.run(9600, {MidiEvent::noteOn(0, note, 1.0f)});
        Patch off = patch;
        Rig plain(off);
        plain.engine.setTable(0, sine);
        plain.run(9600, {MidiEvent::noteOn(0, note, 1.0f)});
        for (std::size_t i = 0; i < 9600; i += 7)
            REQUIRE(rig.left[i] == Approx(plain.left[i]).margin(2e-3));
        // And a different curve is audible.
        const float bent[] = {0.0f, 0.9f, 1.0f};
        rig.engine.setWarpCurve(0, 0, bent);
        rig.run(9600);
        double difference = 0.0;
        plain.run(9600);
        for (std::size_t i = 9600; i < 19200; ++i)
            difference = std::max<double>(difference, std::abs(rig.left[i] - plain.left[i]));
        CHECK(difference > 0.1);
    }
    SECTION("fold and drive act on the amplitude") {
        const auto folded = render("fold", 1.0);
        CHECK(zedtest::peakOf(folded, 4800, 21600) <= static_cast<float>(level) * 1.001f);
        CHECK(zedtest::measureFrequency(folded, 2400, 21600, kRate) > 500.0); // many more zero crossings
        const auto driven = render("drive", 1.0);
        const auto plain = render("off", 0.0);
        CHECK(zedtest::peakOf(driven, 4800, 21600) == Approx(level).epsilon(0.01));              // peak unchanged
        CHECK(zedtest::rmsOf(driven, 4800, 21600) > zedtest::rmsOf(plain, 4800, 21600) * 1.25); // but much fuller
    }
}

TEST_CASE("warp: the reference functions are pure and stay in range", "[osc][warp]") {
    // Displays use these functions directly (plan 5.7): the same input gives the same output,
    // and a phase always comes back inside the cycle.
    for (int m = 1; m < osc::kNumWarpModes; ++m) {
        const auto mode = static_cast<osc::WarpMode>(m);
        for (const float amount : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
            const float d = osc::warpDerive(mode, amount);
            CHECK(osc::warpSlope(mode, d) >= 1.0f);
            for (int i = 0; i < 200; ++i) {
                const float p = static_cast<float>(i) / 200.0f;
                float gain = 1.0f, gainAgain = 1.0f;
                const float out = osc::warpPhase(mode, p, d, 0.3f, nullptr, gain);
                CHECK(out == osc::warpPhase(mode, p, d, 0.3f, nullptr, gainAgain));
                CHECK(out >= 0.0f);
                CHECK(out <= 1.0f);
                CHECK(std::abs(gain) <= 1.0f);
                CHECK(std::isfinite(osc::warpAmplitude(mode, 2.0f * p - 1.0f, d, -0.7f)));
            }
        }
    }
    // Two-sided modes are neutral in the middle.
    CHECK(osc::warpDerive(osc::WarpMode::BendBoth, 0.5f) == 0.0f);
    CHECK(osc::warpDerive(osc::WarpMode::SkewBoth, 0.5f) == 0.0f);
    CHECK(osc::warpDerive(osc::WarpMode::SkewBoth, 0.2f) < 0.0f);
}

TEST_CASE("warp: two slots work in series", "[osc][warp]") {
    const auto sine = TableData::build(sineTable());
    Patch patch = basePatch();
    patch.set("global.quality_offline", choiceOf("global.quality_offline", "draft"));
    patch.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "sync"));
    patch.set("osc1.warp_a.amount", 0.25); // twice per period
    patch.set("osc1.warp_b.mode", choiceOf("osc1.warp_b.mode", "sync"));
    patch.set("osc1.warp_b.amount", 0.25); // and twice again
    const RenderResult r = renderWith(patch, 45, 0.5, sine);
    CHECK(zedtest::cents(zedtest::measureFrequency(r.left, 2400, 21600, kRate), 440.0) == Approx(0.0).margin(1.0));
}

// --- Cross-modulation --------------------------------------------------------------------------

TEST_CASE("cross-modulation: ring, amplitude and phase modulation from another source", "[osc][crossmod]") {
    const auto sine = TableData::build(sineTable());
    Patch patch = basePatch();
    patch.set("global.quality_offline", choiceOf("global.quality_offline", "draft"));
    patch.set("osc2.on", 1);
    patch.set("osc2.phase_random", 0);
    patch.set("osc2.level", kSilenceDb); // the modulator is silent in the mix
    patch.set("osc2.coarse", 7.0);       // a free ratio: a fifth above
    patch.set("osc1.warp_a.source", choiceOf("osc1.warp_a.source", "osc2"));
    const double f1 = noteToHz(57), f2 = noteToHz(64);
    const auto lineAt = [&](const std::vector<float>& x, double hz) {
        const auto s = zedtest::spectrumOf(x, 4800, 32768, kRate);
        return zedtest::lineLevel(s, hz);
    };
    const double level = std::sqrt(0.5);

    SECTION("ring modulation leaves the sum and the difference") {
        patch.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "rm"));
        patch.set("osc1.warp_a.amount", 1.0);
        const auto x = renderWith(patch, 57, 1.0, sine).left;
        CHECK(lineAt(x, f2 - f1) == Approx(level * 0.5).epsilon(0.01));
        CHECK(lineAt(x, f2 + f1) == Approx(level * 0.5).epsilon(0.01));
        CHECK(lineAt(x, f1) < 1e-4);
        // Sample for sample it is the product of the two sines: the modulator is evaluated
        // first and used without delay.
        for (std::size_t n = 20; n < 2000; n += 13) {
            const double expected = level * std::sin(2.0 * kPi * f1 * n / kRate) * std::sin(2.0 * kPi * f2 * n / kRate);
            REQUIRE(x[n] == Approx(expected).margin(2e-3));
        }
    }
    SECTION("amplitude modulation keeps the carrier and adds side bands") {
        patch.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "am"));
        patch.set("osc1.warp_a.amount", 0.5);
        const auto x = renderWith(patch, 57, 1.0, sine).left;
        CHECK(lineAt(x, f1) == Approx(level).epsilon(0.01));
        CHECK(lineAt(x, f2 - f1) == Approx(level * 0.25).epsilon(0.02));
        CHECK(lineAt(x, f2 + f1) == Approx(level * 0.25).epsilon(0.02));
    }
    SECTION("phase modulation follows the Bessel functions") {
        patch.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "pm"));
        patch.set("osc1.warp_a.amount", 0.25); // peak deviation 4 * 0.25^2 = 0.25 cycles: index pi/2
        const auto x = renderWith(patch, 57, 1.0, sine).left;
        // J0(pi/2) = 0.4720, J1(pi/2) = 0.5668
        CHECK(lineAt(x, f1) == Approx(level * 0.4720).epsilon(0.01));
        CHECK(lineAt(x, f1 + f2) == Approx(level * 0.5668).epsilon(0.01));
        CHECK(lineAt(x, f2 - f1) == Approx(level * 0.5668).epsilon(0.01));
    }
    SECTION("Sub and Texture can be the modulator, at any ratio") {
        patch.set("osc2.on", 0);
        patch.set("sub.on", 1);
        patch.set("sub.level", kSilenceDb);
        patch.set("sub.octave", 0);
        patch.set("sub.coarse", 3.86); // 5/4 of the carrier
        patch.set("osc1.warp_a.source", choiceOf("osc1.warp_a.source", "sub"));
        patch.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "rm"));
        patch.set("osc1.warp_a.amount", 1.0);
        const auto x = renderWith(patch, 57, 1.0, sine).left;
        const double fs = noteToHz(57 + 3.86);
        CHECK(lineAt(x, fs + f1) == Approx(level * 0.5).epsilon(0.01));
        CHECK(lineAt(x, f1) < 1e-3);

        patch.set("sub.on", 0);
        patch.set("texture.on", 1);
        patch.set("texture.level", kSilenceDb);
        patch.set("osc1.warp_a.source", choiceOf("osc1.warp_a.source", "texture"));
        const auto noisy = renderWith(patch, 57, 1.0, sine).left;
        CHECK(zedtest::rmsOf(noisy, 4800, 48000) > 0.05); // a sine times noise
        CHECK(lineAt(noisy, f1) < 0.05);                  // no carrier left
    }
    SECTION("a source that is off modulates nothing") {
        patch.set("osc2.on", 0);
        patch.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "pm"));
        patch.set("osc1.warp_a.amount", 1.0);
        const auto x = renderWith(patch, 57, 1.0, sine).left;
        CHECK(lineAt(x, f1) == Approx(level).epsilon(0.005));
    }
}

TEST_CASE("cross-modulation: loops and self-modulation use a one-sample delay", "[osc][crossmod]") {
    const auto sine = TableData::build(sineTable());
    Patch patch = basePatch();
    patch.set("global.quality_offline", choiceOf("global.quality_offline", "draft"));
    patch.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "rm"));
    patch.set("osc1.warp_a.amount", 0.5);
    patch.set("osc1.warp_a.source", choiceOf("osc1.warp_a.source", "osc1")); // itself

    Patch plain = basePatch();
    const auto x = renderWith(plain, 57, 0.2, sine).left; // the unmodulated sine
    const auto y = renderWith(patch, 57, 0.2, sine).left;
    // y[n] = x[n] * (0.5 + 0.5 * m[n - 1]) where m is the oscillator's own output before level and pan.
    const double level = std::sqrt(0.5);
    for (std::size_t n = 20; n < 4000; ++n) { // past the first 16 samples, where the amp envelope rises
        const double expected = x[n] * (0.5 + 0.5 * y[n - 1] / level);
        REQUIRE(y[n] == Approx(expected).margin(1e-4));
    }

    // A loop of two: oscillator 1 is evaluated first and hears oscillator 2 one sample late;
    // oscillator 2 hears oscillator 1 without delay.
    Patch loop = basePatch();
    loop.set("global.quality_offline", choiceOf("global.quality_offline", "draft"));
    loop.set("osc2.on", 1);
    loop.set("osc2.phase_random", 0);
    loop.set("osc2.level", 0);
    loop.set("osc2.pan", 1.0); // oscillator 2 on the right only ...
    loop.set("osc1.pan", -1.0); // ... oscillator 1 on the left only
    loop.set("osc2.coarse", 4.0);
    for (const char* id : {"osc1.warp_a", "osc2.warp_a"}) {
        loop.set(std::string(id) + ".mode", choiceOf("osc1.warp_a.mode", "rm"));
        loop.set(std::string(id) + ".amount", 0.5);
    }
    loop.set("osc1.warp_a.source", choiceOf("osc1.warp_a.source", "osc2"));
    loop.set("osc2.warp_a.source", choiceOf("osc2.warp_a.source", "osc1"));
    const RenderResult r = renderWith(loop, 57, 0.2, sine);
    Patch second = basePatch();
    second.set("osc1.coarse", 4.0);
    const auto x2 = renderWith(second, 57, 0.2, sine).left;
    for (std::size_t n = 20; n < 4000; ++n) {
        const double a = r.left[n], b = r.right[n]; // hard-panned: the whole signal of each
        const double expectedA = x[n] * std::sqrt(2.0) * (0.5 + 0.5 * r.right[n - 1]);
        const double expectedB = x2[n] * std::sqrt(2.0) * (0.5 + 0.5 * a);
        REQUIRE(a == Approx(expectedA).margin(2e-4));
        REQUIRE(b == Approx(expectedB).margin(2e-4));
    }

    // Feedback phase modulation at full depth stays finite in every quality mode.
    for (const char* quality : {"draft", "standard", "high", "ultra"}) {
        Patch fm = basePatch();
        fm.set("global.quality_offline", choiceOf("global.quality_offline", quality));
        fm.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "pm"));
        fm.set("osc1.warp_a.amount", 1.0);
        fm.set("osc1.warp_a.source", choiceOf("osc1.warp_a.source", "osc1"));
        const RenderResult out = renderWith(fm, 60, 0.3, sine);
        bool finite = true;
        for (const float s : out.left)
            finite = finite && std::isfinite(s);
        INFO(quality);
        CHECK(finite);
        CHECK(out.peak() <= 1.5f); // band-limiting a wave this rough overshoots a little
        CHECK(out.peak() > 0.3f);
    }
}
