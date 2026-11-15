#include "../support/TestUtil.h"
#include "zederia/engine/Envelope.h"
#include "zederia/engine/VoiceAllocator.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;
using zedtest::idx;

namespace {

constexpr double kRate = 48000.0;

// Drives an engine block by block and keeps everything it produced.
struct Rig {
    Engine engine;
    std::vector<float> left, right;
    explicit Rig(const Patch& patch = Patch(), double sampleRate = kRate) {
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

std::vector<SeqEvent> busySequence() {
    // Overlapping notes, a repeated key, pitch bend, the sustain pedal and enough notes to steal.
    std::vector<SeqEvent> e;
    const auto at = [&](double seconds, MidiEvent event) {
        e.push_back({static_cast<std::int64_t>(seconds * kRate), event});
    };
    for (int i = 0; i < 12; ++i) {
        at(0.013 * i, MidiEvent::noteOn(0, 48 + i * 3, 0.5f + 0.04f * static_cast<float>(i)));
        at(0.30 + 0.011 * i, MidiEvent::noteOff(0, 48 + i * 3));
    }
    at(0.05, MidiEvent::pitchBend(0, 0.5f));
    at(0.09, MidiEvent::controller(0, 64, 1.0f));
    at(0.11, MidiEvent::noteOn(0, 48, 0.9f));
    at(0.20, MidiEvent::pitchBend(0, -1.0f));
    at(0.33, MidiEvent::controller(0, 64, 0.0f));
    at(0.34, MidiEvent::pitchBend(0, 0.0f));
    at(0.36, MidiEvent::noteOff(0, 48));
    std::stable_sort(e.begin(), e.end(), [](const SeqEvent& a, const SeqEvent& b) { return a.sample < b.sample; });
    return e;
}

} // namespace

TEST_CASE("envelope: segment times are exact", "[engine][envelope]") {
    EnvelopeTimes t;
    t.delay = 100;
    t.attack = 480;
    t.hold = 240;
    t.decay = 960;
    t.sustain = 0.25;
    t.release = 1920;
    Envelope env;
    env.trigger(0.0);
    CHECK(env.advance(100, t) == 0.0);                    // end of the delay
    CHECK(env.advance(240, t) == Approx(0.5));            // halfway up the attack
    CHECK(env.advance(240, t) == Approx(1.0));            // attack done
    CHECK(env.advance(240, t) == Approx(1.0));            // hold
    CHECK(env.advance(480, t) == Approx(0.625));          // halfway down the decay
    CHECK(env.advance(480, t) == Approx(0.25));           // sustain reached
    CHECK(env.advance(100000, t) == Approx(0.25));        // and held
    CHECK(env.stage() == Envelope::Stage::Sustain);
    env.release(0.25);
    CHECK(env.advance(960, t) == Approx(0.125));
    CHECK_FALSE(env.finished());
    CHECK(env.advance(960, t) == 0.0);
    CHECK(env.finished());

    // One large step lands where many small ones do.
    Envelope a, b;
    a.trigger(0.0);
    b.trigger(0.0);
    double la = 0.0;
    for (int i = 0; i < 1300; ++i)
        la = a.advance(1, t);
    CHECK(b.advance(1300, t) == Approx(la).margin(1e-9));

    // A retrigger rises from the current level; a kill ignores the release time.
    env.trigger(0.4);
    t.delay = 0;
    CHECK(env.advance(240, t) == Approx(0.7));
    env.kill(0.7, 100);
    CHECK(env.advance(50, t) == Approx(0.35));
    CHECK(env.advance(50, t) == 0.0);
    CHECK(env.finished());
}

TEST_CASE("engine: silent when idle, sounds on a note, falls silent after the release", "[engine]") {
    Rig rig;
    rig.run(512);
    CHECK(zedtest::peakOf(rig.left, 0, 512) == 0.0f);
    CHECK(rig.engine.activeVoices() == 0);

    rig.run(4800, {MidiEvent::noteOn(0, 60, 1.0f)});
    CHECK(rig.engine.activeVoices() == 1);
    CHECK(zedtest::peakOf(rig.left, 512, 5312) > 0.1f);
    CHECK(zedtest::peakOf(rig.right, 512, 5312) > 0.1f);

    rig.run(512, {MidiEvent::noteOff(0, 60)});
    const double tail = rig.engine.tailSeconds();
    CHECK(tail >= 0.05); // the default release
    rig.run(static_cast<int>(tail * kRate) + 64);
    CHECK(rig.engine.activeVoices() == 0);
    const std::size_t end = rig.left.size();
    rig.run(1024);
    CHECK(zedtest::peakOf(rig.left, end, end + 1024) == 0.0f);
    CHECK(zedtest::peakOf(rig.right, end, end + 1024) == 0.0f);
}

TEST_CASE("engine: MIDI is sample-accurate", "[engine]") {
    for (const int offset : {0, 1, 15, 16, 17, 137, 500, 511}) {
        Rig rig;
        rig.run(512, {MidiEvent::noteOn(offset, 69, 1.0f)});
        INFO("offset " << offset);
        // Nothing before the event; sound from the very next samples on.
        CHECK(zedtest::peakOf(rig.left, 0, static_cast<std::size_t>(offset) + 1) == 0.0f);
        rig.run(64);
        CHECK(zedtest::peakOf(rig.left, static_cast<std::size_t>(offset) + 1, static_cast<std::size_t>(offset) + 48) > 0.0f);
    }
    // The note sounds the same wherever in a block it starts: only the position moves.
    Rig early, late;
    early.run(1024, {MidiEvent::noteOn(16, 57, 1.0f)});
    late.run(1024, {MidiEvent::noteOn(16 + 320, 57, 1.0f)});
    for (std::size_t i = 0; i < 600; ++i)
        REQUIRE(early.left[16 + i] == late.left[16 + 320 + i]);
}

TEST_CASE("engine: output does not depend on the block size", "[engine][determinism]") {
    const auto events = busySequence();
    Patch patch;
    patch.set("global.polyphony", 6); // forces stealing
    patch.set("env1.release", 120);
    std::uint64_t reference = 0;
    for (const int block : {512, 1, 7, 16, 33, 64, 480, 4096}) {
        RenderSettings settings;
        settings.blockSize = block;
        const RenderResult r = renderOffline(patch, events, settings);
        REQUIRE(r.peak() > 0.05f);
        if (reference == 0)
            reference = r.hash();
        INFO("block size " << block);
        CHECK(r.hash() == reference);
    }
}

TEST_CASE("engine: polyphony limits the voices and stolen voices fade out", "[engine]") {
    Patch patch;
    patch.set("global.polyphony", 2);
    Rig rig(patch);
    rig.run(96, {MidiEvent::noteOn(0, 60, 1.0f), MidiEvent::noteOn(10, 64, 1.0f), MidiEvent::noteOn(20, 67, 1.0f)});
    CHECK(rig.engine.activeVoices() == 3); // two sounding, one fading
    rig.run(480);                          // 10 ms: the fade is over
    CHECK(rig.engine.activeVoices() == 2);
    // No click when a voice is stolen: the largest jump stays near what the saws alone produce.
    float jump = 0.0f;
    for (std::size_t i = 1; i < rig.left.size(); ++i)
        jump = std::max(jump, std::abs(rig.left[i] - rig.left[i - 1]));
    CHECK(jump < 1.0f);
}

TEST_CASE("engine: mono retriggers the envelope, legato does not", "[engine]") {
    for (const char* mode : {"mono", "legato"}) {
        Patch patch;
        patch.set("global.mode", Registry::get().find("global.mode")->choiceIndex(mode));
        patch.set("env1.attack", 0);
        patch.set("env1.decay", 100);
        patch.set("env1.sustain", 0.1);
        Rig rig(patch);
        rig.run(14400, {MidiEvent::noteOn(0, 60, 1.0f)}); // 300 ms: settled at the sustain level
        const float settled = zedtest::peakOf(rig.left, 12000, 14400);
        rig.run(2400, {MidiEvent::noteOn(0, 67, 1.0f)}); // second key while the first is held
        const float after = zedtest::peakOf(rig.left, 14400, 15400);
        INFO(mode);
        CHECK(rig.engine.activeVoices() == 1);
        if (std::string(mode) == "mono")
            CHECK(after > settled * 5.0f); // the envelope went back to the top
        else
            CHECK(after < settled * 1.3f); // only the pitch changed
        // Either way the pitch is now the new key.
        const double f = zedtest::measureFrequency(rig.left, 15400, 16800, kRate);
        CHECK(zedtest::cents(f, noteToHz(67)) == Approx(0.0).margin(0.5));
    }
}

TEST_CASE("engine: amp envelope timing is within a millisecond", "[engine][envelope]") {
    Patch patch;
    patch.set("env1.attack", 50);
    patch.set("env1.decay", 100);
    patch.set("env1.sustain", 0.5);
    patch.set("env1.release", 80);
    patch.set("env1.decay_curve", 0); // straight segments, so halfway in time is halfway in level
    patch.set("env1.release_curve", 0);
    patch.set("osc1.phase_random", 0);
    // A period of exactly 24 samples: every cycle is sampled at the same points, so the peak of
    // each cycle is the envelope times a constant.
    const int note = 96;
    patch.set("osc1.coarse", 12.0 * std::log2(2000.0 / noteToHz(note)));
    Rig rig(patch);
    rig.run(24000, {MidiEvent::noteOn(0, note, 1.0f)});
    const auto level = [&](double ms) {
        const auto centre = static_cast<std::size_t>(ms * kRate / 1000.0);
        return zedtest::peakOf(rig.left, centre - 24, centre + 24);
    };
    const float full = level(50.5);
    CHECK(level(25.0) == Approx(full * 0.5f).epsilon(0.03)); // halfway up at 25 ms
    CHECK(level(49.0) > full * 0.96f);                       // attack complete at 50 ms, not before
    CHECK(level(100.0) == Approx(full * 0.75f).epsilon(0.03)); // halfway down the decay
    CHECK(level(151.0) == Approx(full * 0.5f).epsilon(0.02));  // sustain from 150 ms
    CHECK(level(400.0) == Approx(full * 0.5f).epsilon(0.02));

    rig.run(9600, {MidiEvent::noteOff(0, note)});
    const std::size_t off = 24000;
    const auto after = [&](double ms) {
        const std::size_t centre = off + static_cast<std::size_t>(ms * kRate / 1000.0);
        return zedtest::peakOf(rig.left, centre - 24, centre + 24);
    };
    CHECK(after(40.0) == Approx(full * 0.25f).epsilon(0.04)); // halfway through the release
    CHECK(after(79.0) > 0.0f);
    CHECK(zedtest::peakOf(rig.left, off + static_cast<std::size_t>(81.0 * kRate / 1000.0), rig.left.size()) == 0.0f);
}

TEST_CASE("engine: pitch bend, level, pan and velocity act as specified", "[engine]") {
    Patch patch;
    patch.set("osc1.phase_random", 0);
    patch.set("global.bend_up", 7);
    patch.set("global.bend_down", 12);
    SECTION("pitch bend uses the up and down ranges") {
        Rig rig(patch);
        rig.run(9600, {MidiEvent::noteOn(0, 60, 1.0f), MidiEvent::pitchBend(0, 1.0f)});
        CHECK(zedtest::cents(zedtest::measureFrequency(rig.left, 2400, 9600, kRate), noteToHz(67)) == Approx(0.0).margin(0.1));
        rig.run(9600, {MidiEvent::pitchBend(0, -1.0f)});
        CHECK(zedtest::cents(zedtest::measureFrequency(rig.left, 12000, 19200, kRate), noteToHz(48)) == Approx(0.0).margin(0.1));
    }
    SECTION("master volume and oscillator level are in decibels") {
        const float base = zedtest::renderNote(patch, 60, 0.3).peak();
        patch.set("global.master", patch.get("global.master") - 6.0);
        const float quieter = zedtest::renderNote(patch, 60, 0.3).peak();
        CHECK(quieter / base == Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.001));
        patch.set("osc1.level", patch.get("osc1.level") - 6.0);
        CHECK(zedtest::renderNote(patch, 60, 0.3).peak() / quieter == Approx(std::pow(10.0, -6.0 / 20.0)).epsilon(0.001));
        patch.set("osc1.level", kSilenceDb);
        CHECK(zedtest::renderNote(patch, 60, 0.3).peak() == 0.0f);
        patch.set("osc1.level", 0.0);
        patch.set("osc1.on", 0);
        CHECK(zedtest::renderNote(patch, 60, 0.3).peak() == 0.0f);
    }
    SECTION("pan moves the sound with constant power") {
        const RenderResult centre = zedtest::renderNote(patch, 60, 0.3);
        CHECK(zedtest::peakOf(centre.left, 0, centre.left.size()) == Approx(zedtest::peakOf(centre.right, 0, centre.right.size())));
        patch.set("osc1.pan", -1.0);
        const RenderResult hardLeft = zedtest::renderNote(patch, 60, 0.3);
        CHECK(zedtest::peakOf(hardLeft.right, 0, hardLeft.right.size()) < 1e-6f);
        CHECK(zedtest::peakOf(hardLeft.left, 0, hardLeft.left.size()) ==
              Approx(zedtest::peakOf(centre.left, 0, centre.left.size()) * std::sqrt(2.0f)).epsilon(0.001));
    }
    SECTION("velocity sensitivity scales the level") {
        patch.set("voice.velocity_sens", 1.0);
        const float loud = zedtest::renderNote(patch, 60, 0.3, kRate, 1.0).peak();
        const float soft = zedtest::renderNote(patch, 60, 0.3, kRate, 0.25).peak();
        CHECK(soft / loud == Approx(0.25).epsilon(0.01));
        patch.set("voice.velocity_sens", 0.0);
        CHECK(zedtest::renderNote(patch, 60, 0.3, kRate, 0.25).peak() == Approx(loud).epsilon(0.001));
    }
}

TEST_CASE("engine: parameter changes while playing are smooth", "[engine]") {
    Rig rig;
    rig.engine.params().set(idx("osc1.phase_random"), 0.0);
    rig.run(4800, {MidiEvent::noteOn(0, 36, 1.0f)});
    // A 60 dB jump of the master volume, from another "thread", between two blocks.
    rig.engine.params().set(idx("global.master"), -66.0);
    rig.run(1024);
    float jump = 0.0f;
    for (std::size_t i = 4800 - 8; i < rig.left.size(); ++i)
        jump = std::max(jump, std::abs(rig.left[i] - rig.left[i - 1]));
    CHECK(jump < 0.05f); // glides there over a few milliseconds, not stepped
    CHECK(zedtest::peakOf(rig.left, 4800, 4800 + 16) > 0.01f);              // still on its way at first
    CHECK(zedtest::peakOf(rig.left, 4800 + 720, rig.left.size()) < 0.001f); // and there within 15 ms
}

TEST_CASE("engine: survives a storm of events with finite output", "[engine][fuzz]") {
    Rng rng(99);
    Patch patch;
    patch.set("global.polyphony", 64);
    patch.set("env1.release", 400);
    Rig rig(patch);
    const auto& reg = Registry::get();
    for (int block = 0; block < 600; ++block) {
        std::vector<MidiEvent> events;
        const int count = static_cast<int>(rng.next() % 12);
        for (int i = 0; i < count; ++i) {
            const int offset = static_cast<int>(rng.next() % 256);
            switch (rng.next() % 6) {
            case 0:
            case 1: events.push_back(MidiEvent::noteOn(offset, static_cast<int>(rng.next() % 128), static_cast<float>(rng.uniform()))); break;
            case 2: events.push_back(MidiEvent::noteOff(offset, static_cast<int>(rng.next() % 128))); break;
            case 3: events.push_back(MidiEvent::pitchBend(offset, static_cast<float>(rng.uniform() * 2.0 - 1.0))); break;
            case 4: events.push_back(MidiEvent::controller(offset, rng.next() % 2 ? 64 : 123, static_cast<float>(rng.uniform()))); break;
            default: events.push_back(MidiEvent::controller(offset, static_cast<int>(rng.next() % 128), static_cast<float>(rng.uniform()))); break;
            }
        }
        std::stable_sort(events.begin(), events.end(), [](const MidiEvent& a, const MidiEvent& b) { return a.offset < b.offset; });
        if (block % 37 == 0) { // parameters move too, including the voice mode
            for (const char* id : {"global.mode", "global.polyphony", "voice.steal", "voice.priority", "env1.attack",
                                   "env1.release", "osc1.pan", "osc1.coarse", "global.master"}) {
                const ParamDef& d = *reg.find(id);
                rig.engine.params().set(d.index, d.fromNormalized(rng.uniform()));
            }
        }
        rig.run(256, events);
        REQUIRE(rig.engine.activeVoices() <= kVoiceSlots);
    }
    bool finite = true;
    for (std::size_t i = 0; i < rig.left.size(); ++i)
        finite = finite && std::isfinite(rig.left[i]) && std::isfinite(rig.right[i]);
    CHECK(finite);
    CHECK(zedtest::peakOf(rig.left, 0, rig.left.size()) < 64.0f); // at most the sum of all voices
    CHECK(zedtest::peakOf(rig.left, 0, rig.left.size()) > 0.01f);
}
