// The modulators inside the engine (plan 3.7, 6.1): envelope timing, LFO modes and sync phase,
// Flux, the MIDI and per-note sources, MPE routing, the envelope follower and bar phase.
//
// The engine reports each source once per block as it will be at the next control tick. The
// tests run in blocks of one control interval, so a reading taken after `n` samples is the
// source's value at sample `n`.
#include "../support/Analysis.h"
#include "../support/EngineRig.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;
using zedtest::addRoute;
using zedtest::choiceOf;
using zedtest::EngineRig;
using zedtest::idx;

namespace {

constexpr double kRate = 48000.0;

// A patch in which the named sources are in use (a route with no depth is enough).
Patch listening(std::initializer_list<const char*> sources) {
    Patch p;
    for (const char* source : sources)
        addRoute(p, source, "osc1.pan", 0.0);
    return p;
}

// First time (ms) at which `source` crosses `level` going up (or down), read every control tick.
double crossing(EngineRig& rig, const char* source, double level, bool rising, double untilMs) {
    float before = rig.source(source);
    while (rig.left.size() < static_cast<std::size_t>(untilMs * kRate / 1000.0)) {
        rig.run(16);
        const float now = rig.source(source);
        if (rising ? (before < level && now >= level) : (before > level && now <= level)) {
            const double fraction = (level - before) / (static_cast<double>(now) - before);
            return (static_cast<double>(rig.left.size()) + 16.0 * fraction) * 1000.0 / kRate;
        }
        before = now;
    }
    return -1.0;
}

} // namespace

TEST_CASE("envelopes: segment times are within a millisecond, with curves", "[modulation][envelope]") {
    Patch p = listening({"env2"});
    p.set("env2.delay", 20);
    p.set("env2.attack", 40);
    p.set("env2.hold", 30);
    p.set("env2.decay", 80);
    p.set("env2.sustain", 0.25);
    p.set("env2.release", 120);
    p.set("env2.attack_curve", 0);
    p.set("env2.decay_curve", 0);
    p.set("env2.release_curve", 0);
    p.set("env1.release", 500); // keeps the voice alive while envelope 2 releases
    SECTION("straight segments") {
        EngineRig rig(p);
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        CHECK(crossing(rig, "env2", 0.001, true, 100) == Approx(20.0).margin(1.0));  // end of the delay
        CHECK(crossing(rig, "env2", 0.5, true, 100) == Approx(40.0).margin(1.0));    // halfway up
        CHECK(crossing(rig, "env2", 0.999, true, 100) == Approx(60.0).margin(1.0));  // attack done
        CHECK(crossing(rig, "env2", 0.999, false, 200) == Approx(90.0).margin(1.0)); // hold over
        CHECK(crossing(rig, "env2", 0.625, false, 200) == Approx(130.0).margin(1.0)); // halfway down the decay
        rig.runTo(171.0);
        CHECK(rig.source("env2") == Approx(0.25).margin(1e-4)); // sustain from 170 ms
        rig.runTo(400.0);
        CHECK(rig.source("env2") == Approx(0.25).margin(1e-6));
        rig.run(16, {MidiEvent::noteOff(0, 60)});
        const double off = rig.left.size() * 1000.0 / kRate - 16000.0 / kRate;
        CHECK(crossing(rig, "env2", 0.125, false, 700) - off == Approx(60.0).margin(1.0)); // halfway through the release
        CHECK(crossing(rig, "env2", 0.0005, false, 700) - off == Approx(120.0).margin(1.0));
    }
    SECTION("curved segments pass through the curve's value") {
        p.set("env2.delay", 0);
        p.set("env2.hold", 0);
        p.set("env2.attack_curve", 0.6);
        p.set("env2.decay_curve", -0.4);
        p.set("env2.release_curve", 0.8);
        EngineRig rig(p);
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        rig.runTo(10.0);
        CHECK(rig.source("env2") == Approx(curveValue(0.25, 0.6)).margin(0.01));
        rig.runTo(20.0);
        CHECK(rig.source("env2") == Approx(curveValue(0.5, 0.6)).margin(0.01));
        CHECK(curveValue(0.5, 0.6) > 0.7); // fast at first
        rig.runTo(40.0 + 40.0);
        CHECK(rig.source("env2") == Approx(1.0 - 0.75 * curveValue(0.5, -0.4)).margin(0.01));
        rig.runUntil(300.0);
        rig.run(16, {MidiEvent::noteOff(0, 60)});
        rig.runTo(300.0 + 30.0);
        CHECK(rig.source("env2") == Approx(0.25 * (1.0 - curveValue(0.25, 0.8))).margin(0.005));
    }
    SECTION("the amp envelope's curves shape the sound") {
        Patch amp = zedtest::flatPatch();
        amp.set("sub.on", 1);
        amp.set("sub.octave", 0);
        amp.set("env1.attack", 200);
        amp.set("env1.attack_curve", 0.7);
        EngineRig rig(amp);
        rig.engine.setTable(0, zedtest::sineTableData());
        rig.run(24000, {MidiEvent::noteOn(0, 84, 1.0f)}); // about 1 kHz: many cycles per reading
        const double full = zedtest::peakOf(rig.left, 12000, 24000);
        const double half = zedtest::peakOf(rig.left, 4800 - 48, 4800 + 48); // 100 ms: half the attack time
        CHECK(half / full == Approx(curveValue(0.5, 0.7)).margin(0.02));
    }
}

TEST_CASE("envelopes: times given as note lengths follow the tempo", "[modulation][envelope]") {
    Patch p = listening({"env2"});
    p.set("env2.attack", 5);
    p.set("env2.attack_curve", 0);
    p.envSync[1][1] = "1/8"; // attack: an eighth note
    p.set("env1.release", 2000);
    for (const double bpm : {120.0, 60.0, 150.0}) {
        EngineRig rig(p);
        rig.transport.bpm = bpm;
        rig.transport.playing = true;
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        const double eighthMs = 0.5 * 60000.0 / bpm;
        INFO(bpm << " bpm");
        CHECK(crossing(rig, "env2", 0.5, true, 1200) == Approx(eighthMs * 0.5).epsilon(0.01));
        CHECK(crossing(rig, "env2", 0.999, true, 1200) == Approx(eighthMs).epsilon(0.01));
    }
    // Dotted, triplet and any fraction; an empty entry returns the time to milliseconds.
    EngineRig rig(p);
    rig.transport.bpm = 120.0;
    rig.engine.setEnvSync(1, 1, divisionInBeats("3/16"));
    rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
    CHECK(crossing(rig, "env2", 0.999, true, 1200) == Approx(375.0).epsilon(0.01));
    rig.engine.setEnvSync(1, 1, 0.0);
    rig.run(16, {MidiEvent::noteOff(0, 60), MidiEvent::noteOn(8, 62, 1.0f)});
    const double start = (rig.left.size() - 8.0) * 1000.0 / kRate;
    CHECK(crossing(rig, "env2", 0.999, true, 1200) - start == Approx(5.0).margin(1.0));
    // The amp envelope's tail follows a tempo-linked release.
    Patch tail;
    tail.envSync[0][4] = "1/2";
    EngineRig linked(tail);
    linked.transport.bpm = 120.0;
    linked.run(512);
    CHECK(linked.engine.tailSeconds() == Approx(1.0 + 0.3).margin(0.05)); // plus the DC filter's settling
}

TEST_CASE("envelopes: multi-segment mode with a sustain point and a loop", "[modulation][envelope]") {
    Patch p = listening({"env3"});
    p.set("env3.mode", choiceOf("env3.mode", "multi"));
    p.set("env1.release", 2000);
    SECTION("sustain point") {
        p.envShapes[2].points = {{10.0, "", 1.0, 0.0}, {30.0, "", 0.3, 0.0}, {50.0, "", 0.6, 0.0}, {100.0, "", 0.0, 0.0}};
        p.envShapes[2].sustainPoint = 2;
        EngineRig rig(p);
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        CHECK(crossing(rig, "env3", 0.999, true, 300) == Approx(10.0).margin(1.0));
        CHECK(crossing(rig, "env3", 0.65, false, 300) == Approx(25.0).margin(1.0));
        CHECK(crossing(rig, "env3", 0.45, true, 300) == Approx(65.0).margin(1.0));
        rig.runTo(92.0);
        CHECK(rig.source("env3") == Approx(0.6).margin(1e-3)); // holds at the sustain point
        rig.runTo(400.0);
        CHECK(rig.source("env3") == Approx(0.6).margin(1e-6));
        rig.run(16, {MidiEvent::noteOff(0, 60)});
        const double off = (rig.left.size() - 16.0) * 1000.0 / kRate;
        CHECK(crossing(rig, "env3", 0.3, false, 700) - off == Approx(50.0).margin(1.0));
        rig.runTo(off + 102.0);
        CHECK(rig.source("env3") == 0.0f);
    }
    SECTION("loop region, left when the key goes up") {
        p.envShapes[2].points = {{10.0, "", 1.0, 0.0}, {20.0, "", 0.2, 0.0}, {20.0, "", 1.0, 0.0}, {40.0, "", 0.0, 0.0}};
        p.envShapes[2].loopStart = 0;
        p.envShapes[2].loopEnd = 2;
        EngineRig rig(p);
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        // 10 ms up, then 20 down and 20 up, again and again: low points at 30, 70, 110 ms.
        for (const double low : {30.0, 70.0, 110.0, 150.0}) {
            rig.runTo(low);
            CHECK(rig.source("env3") == Approx(0.2).margin(0.03));
            rig.runTo(low + 20.0);
            CHECK(rig.source("env3") == Approx(1.0).margin(0.03));
        }
        rig.run(16, {MidiEvent::noteOff(0, 60)});
        rig.runTo(170.0 + 60.0);
        CHECK(rig.source("env3") == 0.0f);
    }
    SECTION("tempo-linked points and a shape changed while running") {
        p.envShapes[2].points = {{0.0, "1/16", 1.0, 0.0}, {0.0, "1/8", 0.0, 0.0}};
        EngineRig rig(p);
        rig.transport.bpm = 100.0;
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        CHECK(crossing(rig, "env3", 0.999, true, 800) == Approx(150.0).epsilon(0.01));
        CHECK(crossing(rig, "env3", 0.5, false, 800) == Approx(150.0 + 150.0).epsilon(0.01));
        EnvShape other;
        other.points = {{50.0, "", 0.5, 0.0}};
        other.sustainPoint = 0;
        rig.engine.setEnvShape(2, other);
        rig.run(16, {MidiEvent::noteOff(0, 60), MidiEvent::noteOn(8, 64, 1.0f)});
        rig.runBlocks(4800);
        CHECK(rig.source("env3") == Approx(0.5).margin(1e-3));
        // A multi-segment envelope with no points stays at zero.
        rig.engine.setEnvShape(2, EnvShape{});
        rig.run(16, {MidiEvent::noteOff(0, 64), MidiEvent::noteOn(8, 65, 1.0f)});
        rig.runBlocks(4800);
        CHECK(rig.source("env3") == 0.0f);
    }
}

TEST_CASE("lfo: shape, rate, phase, polarity, delay, rise and the four modes", "[modulation][lfo]") {
    Patch base = listening({"lfo1"});
    base.lfoShapes[0] = zedtest::lineShape(-1.0, 1.0); // a rising ramp: the value tells the phase
    base.set("lfo1.rate", 10);
    const auto at = [](const Patch& p, double ms, double noteMs = 0.0) {
        EngineRig rig(p);
        rig.runUntil(noteMs);
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        rig.runTo(noteMs + ms);
        return static_cast<double>(rig.source("lfo1"));
    };
    const auto ramp = [](double phase) { return -1.0 + 2.0 * (phase - std::floor(phase)); };

    CHECK(at(base, 40.0) == Approx(ramp(0.4)).margin(0.005));
    CHECK(at(base, 130.0) == Approx(ramp(1.3)).margin(0.005));
    CHECK(at(base, 40.0, 77.0) == Approx(ramp(0.4)).margin(0.005)); // restarts with the note

    Patch sine = base;
    sine.lfoShapes[0] = Shape{}; // the built-in sine
    CHECK(at(sine, 25.0) == Approx(1.0).margin(0.005));
    CHECK(at(sine, 50.0) == Approx(0.0).margin(0.005));

    Patch phase = base;
    phase.set("lfo1.phase", 90);
    CHECK(at(phase, 40.0) == Approx(ramp(0.65)).margin(0.005));

    Patch unipolar = base;
    unipolar.set("lfo1.polarity", choiceOf("lfo1.polarity", "unipolar"));
    CHECK(at(unipolar, 40.0) == Approx(0.4).margin(0.005));

    Patch delayed = base;
    delayed.set("lfo1.delay", 30);
    CHECK(at(delayed, 20.0) == Approx(0.0).margin(1e-6));
    CHECK(at(delayed, 70.0) == Approx(ramp(0.4)).margin(0.005)); // the cycle starts when the delay ends

    Patch rising = base;
    rising.set("lfo1.rise", 100);
    rising.set("lfo1.rate", 0.01);
    rising.set("lfo1.phase", 270); // three quarters up the ramp: the full value is 0.5
    CHECK(at(rising, 25.0) == Approx(0.125).margin(0.01));
    CHECK(at(rising, 50.0) == Approx(0.25).margin(0.01));
    CHECK(at(rising, 150.0) == Approx(0.5).margin(0.01));

    Patch free = base;
    free.set("lfo1.mode", choiceOf("lfo1.mode", "free"));
    CHECK(at(free, 40.0, 77.0) == Approx(ramp(1.17)).margin(0.005)); // counts from the start, not the note

    Patch once = base;
    once.set("lfo1.mode", choiceOf("lfo1.mode", "one_shot"));
    CHECK(at(once, 40.0) == Approx(ramp(0.4)).margin(0.005));
    CHECK(at(once, 99.0) == Approx(0.98).margin(0.01));
    CHECK(at(once, 250.0) == Approx(1.0).margin(1e-6)); // holds the last value

    Patch looped = base;
    looped.set("lfo1.mode", choiceOf("lfo1.mode", "loop_point"));
    looped.lfoShapes[0].loopStart = 0.5;
    CHECK(at(looped, 40.0) == Approx(ramp(0.4)).margin(0.005));
    CHECK(at(looped, 125.0) == Approx(ramp(0.75)).margin(0.005)); // second pass: only the half after the marker
    CHECK(at(looped, 190.0) == Approx(ramp(0.9)).margin(0.005));

    // Smooth rounds the ramp's jump: just after the wrap the value has not fallen all the way.
    Patch smooth = base;
    smooth.set("lfo1.smooth", 0.5);
    CHECK(at(smooth, 102.0) > at(base, 102.0) + 0.3);
    CHECK(std::abs(at(smooth, 95.0)) <= 1.0);

    // Without audio-rate mode the rate stops at 100 Hz; with it the LFO runs up to 2 kHz.
    Patch fast = base;
    fast.set("lfo1.rate", 400);
    CHECK(at(fast, 12.0) == Approx(ramp(1.2)).margin(0.02));
    fast.set("lfo1.audio_rate", 1);
    CHECK(at(fast, 12.0) == Approx(ramp(4.8)).margin(0.05));
}

TEST_CASE("lfo: per voice or shared, and a rate that follows the keyboard", "[modulation][lfo]") {
    Patch p = listening({"lfo1"});
    p.lfoShapes[0] = zedtest::lineShape(-1.0, 1.0);
    p.set("lfo1.rate", 5);
    const auto ramp = [](double phase) { return -1.0 + 2.0 * (phase - std::floor(phase)); };
    {
        // Per voice: the second note's LFO starts when that note does.
        EngineRig rig(p);
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        rig.runUntil(60.0);
        rig.run(16, {MidiEvent::noteOn(0, 64, 1.0f)});
        rig.runTo(100.0);
        CHECK(rig.source("lfo1") == Approx(ramp(0.2)).margin(0.01)); // the most recent voice: 40 ms old
    }
    {
        // Shared and free: one LFO, whatever the notes do.
        Patch shared = p;
        shared.set("lfo1.scope", choiceOf("lfo1.scope", "global"));
        shared.set("lfo1.mode", choiceOf("lfo1.mode", "free"));
        EngineRig rig(shared);
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        rig.runUntil(60.0);
        rig.run(16, {MidiEvent::noteOn(0, 64, 1.0f)});
        rig.runTo(100.0);
        CHECK(rig.source("lfo1") == Approx(ramp(0.5)).margin(0.01));
        // Shared and retriggered: every note restarts the one LFO.
        shared.set("lfo1.mode", choiceOf("lfo1.mode", "retrigger"));
        EngineRig again(shared);
        again.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        again.runUntil(60.0);
        again.run(16, {MidiEvent::noteOn(0, 64, 1.0f)});
        again.runTo(100.0);
        CHECK(again.source("lfo1") == Approx(ramp(0.2)).margin(0.01));
    }
    {
        // Key tracking at 100 %: an octave up doubles the rate.
        Patch tracked = p;
        tracked.set("lfo1.keytrack", 1.0);
        EngineRig low(tracked), high(tracked);
        low.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        high.run(16, {MidiEvent::noteOn(0, 72, 1.0f)});
        low.runTo(60.0);
        high.runTo(60.0);
        CHECK(low.source("lfo1") == Approx(ramp(0.3)).margin(0.01));
        CHECK(high.source("lfo1") == Approx(ramp(0.6)).margin(0.01));
    }
    {
        // An LFO moves what it is routed to: pitch, a semitone either way, twice a second.
        Patch vibrato = zedtest::flatPatch();
        vibrato.set("osc1.on", 1);
        vibrato.set("lfo1.rate", 2);
        vibrato.lfoShapes[0].points = {{0.0, 1.0, 0.0}, {0.5, 1.0, 0.0}, {0.5, -1.0, 0.0}, {1.0, -1.0, 0.0}}; // a square
        addRoute(vibrato, "lfo1", "osc1.coarse", 1.0 / 128.0, Polarity::Bi);
        EngineRig rig(vibrato);
        rig.run(48000, {MidiEvent::noteOn(0, 69, 1.0f)});
        CHECK(zedtest::cents(zedtest::measureFrequency(rig.left, 2400, 11000, kRate), noteToHz(70.0)) == Approx(0.0).margin(0.5));
        CHECK(zedtest::cents(zedtest::measureFrequency(rig.left, 13000, 23000, kRate), noteToHz(68.0)) == Approx(0.0).margin(0.5));
        CHECK(zedtest::cents(zedtest::measureFrequency(rig.left, 25000, 35000, kRate), noteToHz(70.0)) == Approx(0.0).margin(0.5));
    }
}

TEST_CASE("lfo: a synced LFO is in phase at the bar lines, and Anchor keeps it there", "[modulation][lfo][sync]") {
    Patch p = listening({"lfo1"});
    p.lfoShapes[0] = zedtest::lineShape(-1.0, 1.0);
    p.set("lfo1.sync", 1);
    p.set("lfo1.scope", choiceOf("lfo1.scope", "global"));
    p.set("lfo1.mode", choiceOf("lfo1.mode", "free"));
    const auto ramp = [](double phase) { return -1.0 + 2.0 * (phase - std::floor(phase)); };
    const auto samplesAtBeat = [](double beat, double bpm) { return beat * 60.0 / bpm * kRate; };

    for (const char* division : {"1/1", "1/4", "1/8T", "1/2D", "2/1"}) {
        Patch synced = p;
        synced.set("lfo1.division", choiceOf("lfo1.division", division));
        const double beats = divisionInBeats(division);
        EngineRig rig(synced);
        rig.transport.bpm = 120.0;
        rig.transport.playing = true;
        // Read a sixteenth after each of the first three bar lines (4/4): the phase there is known.
        for (int bar = 0; bar < 3; ++bar) {
            const double beat = bar * 4.0 + 0.25;
            rig.runTo(samplesAtBeat(beat, 120.0) * 1000.0 / kRate);
            const double actualBeat = (static_cast<double>(rig.left.size()) + 16.0) / samplesAtBeat(1.0, 120.0);
            INFO(division << ", bar " << bar);
            CHECK(rig.source("lfo1") == Approx(ramp(actualBeat / beats)).margin(0.003));
        }
        // Exactly on a bar line a cycle that divides the bar starts over.
        if (std::fmod(4.0, beats) == 0.0) {
            rig.runTo(samplesAtBeat(12.0, 120.0) * 1000.0 / kRate - 16000.0 / kRate);
            const float justBefore = rig.source("lfo1");
            rig.run(32);
            const float justAfter = rig.source("lfo1");
            CHECK(justBefore > 0.99f);
            CHECK(justAfter < -0.99f);
        }
    }

    // The tempo changes halfway through a cycle. Without Anchor the LFO carries on from where it
    // was; with Anchor it is where the song position says.
    for (const bool anchor : {false, true}) {
        Patch synced = p;
        synced.set("lfo1.division", choiceOf("lfo1.division", "1/1"));
        synced.set("lfo1.anchor", anchor ? 1 : 0);
        EngineRig rig(synced);
        rig.transport.bpm = 120.0;
        rig.transport.playing = true;
        rig.runUntil(1000.0); // two beats
        rig.transport.bpm = 60.0;
        rig.runTo(2000.0); // one more beat: the song is at beat 3
        const double ahead = 16.0 * 60.0 / (60.0 * kRate); // the report is one control interval ahead
        const double songPhase = (rig.transport.ppqPosition + ahead) / 4.0;
        INFO((anchor ? "anchored" : "free-running"));
        CHECK(rig.source("lfo1") == Approx(ramp(songPhase)).margin(0.005));
        // The host jumps back to the start of bar 2.
        rig.transport.ppqPosition = 4.0;
        rig.runBlocks(4800);
        const double expected = anchor ? (rig.transport.ppqPosition + ahead) / 4.0 : songPhase + 0.1 / 4.0;
        CHECK(rig.source("lfo1") == Approx(ramp(expected)).margin(0.005));
    }

    // Bar phase: where the song is inside its bar, in any time signature.
    Patch bar = listening({"bar_phase"});
    EngineRig rig(bar);
    rig.transport.bpm = 120.0;
    rig.transport.playing = true;
    rig.runTo(750.0); // beat 1.5
    CHECK(rig.source("bar_phase") == Approx(1.5 / 4.0).margin(0.001));
    rig.transport.timeSigNumerator = 3;
    rig.transport.barStartPpq = 1.0; // the bar started at beat 1
    rig.runTo(1000.0);               // beat 2: one beat into a 3/4 bar
    CHECK(rig.source("bar_phase") == Approx(1.0 / 3.0).margin(0.001));
}

TEST_CASE("lfo: an audio-rate LFO moves a filter's cutoff every sample", "[modulation][lfo][filter]") {
    // A pure tone through a low-pass whose cutoff an LFO shakes at 1 kHz: the shaking puts
    // sidebands 1 kHz either side of the tone. At control rate they cannot be made cleanly.
    Patch p = zedtest::flatPatch();
    p.set("sub.on", 1);
    p.set("sub.octave", 0);
    p.set("filter1.on", 1);
    p.set("filter1.cutoff", 600);
    p.set("lfo1.rate", 1000);
    p.set("lfo1.audio_rate", 1);
    addRoute(p, "lfo1", "filter1.cutoff", 0.15, Polarity::Bi);
    const double tone = noteToHz(69.0);
    const auto sideband = [&](const Patch& patch) {
        EngineRig rig(patch);
        rig.run(48000, {MidiEvent::noteOn(0, 69, 1.0f)});
        const auto spectrum = zedtest::spectrumOf(rig.left, 9600, 32768, kRate);
        return 20.0 * std::log10(std::max(zedtest::lineLevel(spectrum, 1000.0 - tone), zedtest::lineLevel(spectrum, 1000.0 + tone)) /
                                 zedtest::lineLevel(spectrum, tone));
    };
    const double withAudioRate = sideband(p);
    CHECK(withAudioRate > -30.0);
    Patch still = p;
    still.set(Registry::get().modAmount(0), 0.0);
    CHECK(sideband(still) < -90.0);

    // The same from an oscillator's audio output, and with Via closing the route.
    Patch fm = zedtest::flatPatch();
    fm.set("sub.on", 1);
    fm.set("sub.octave", 0);
    fm.set("filter1.on", 1);
    fm.set("filter1.cutoff", 600);
    fm.set("osc2.on", 1);
    fm.set("osc2.level", kSilenceDb); // heard only as a modulator
    fm.set("osc2.keytrack", 0);
    fm.set("osc2.coarse", 12.0 * std::log2(1000.0 / noteToHz(60.0)));
    ModRoute& r = addRoute(fm, "osc2_audio", "filter1.cutoff", 0.15, Polarity::Bi);
    {
        EngineRig rig(fm);
        rig.engine.setTable(1, zedtest::sineTableData());
        rig.run(48000, {MidiEvent::noteOn(0, 69, 1.0f)});
        const auto spectrum = zedtest::spectrumOf(rig.left, 9600, 32768, kRate);
        CHECK(20.0 * std::log10(zedtest::lineLevel(spectrum, 1000.0 + tone) / zedtest::lineLevel(spectrum, tone)) > -30.0);
    }
    r.via = "modwheel"; // the wheel is down: nothing gets through
    {
        EngineRig rig(fm);
        rig.engine.setTable(1, zedtest::sineTableData());
        rig.run(48000, {MidiEvent::noteOn(0, 69, 1.0f)});
        const auto spectrum = zedtest::spectrumOf(rig.left, 9600, 32768, kRate);
        CHECK(20.0 * std::log10(zedtest::lineLevel(spectrum, 1000.0 + tone) / zedtest::lineLevel(spectrum, tone)) < -90.0);
    }
}

TEST_CASE("flux: four generators, free or synced, shared or per voice", "[modulation][flux]") {
    const auto record = [](const char* type, double hz, int samples, std::uint64_t seed = 1) {
        Patch p = listening({"flux1"});
        p.set("flux1.type", choiceOf("flux1.type", type));
        p.set("flux1.rate", hz);
        EngineRig rig;
        rig.engine.setSeed(seed);
        rig.engine.reset();
        rig.engine.loadPatch(p);
        std::vector<float> values;
        for (int n = 0; n < samples; n += 16) {
            rig.run(16);
            values.push_back(rig.source("flux1"));
        }
        return values;
    };
    for (const char* type : {"orbit", "burst", "stepped", "smooth"}) {
        const auto a = record(type, 20.0, 96000), b = record(type, 20.0, 96000), c = record(type, 20.0, 96000, 7);
        INFO(type);
        float low = 1.0f, high = -1.0f, step = 0.0f;
        for (std::size_t i = 0; i < a.size(); ++i) {
            low = std::min(low, a[i]);
            high = std::max(high, a[i]);
            if (i > 0)
                step = std::max(step, std::abs(a[i] - a[i - 1]));
        }
        CHECK(low >= -1.0f);
        CHECK(high <= 1.0f);
        CHECK(high - low > 0.5f); // it moves
        CHECK(a == b);            // the same seed gives the same sequence
        CHECK(a != c);            // another seed another one
        if (std::string(type) == "smooth" || std::string(type) == "orbit")
            CHECK(step < 0.05f);  // no jumps
        if (std::string(type) == "stepped" || std::string(type) == "burst")
            CHECK(step > 0.2f);
    }
    // Stepped holds each value for one period of the rate.
    const auto stepped = record("stepped", 10.0, 48000);
    int changes = 0;
    for (std::size_t i = 1; i < stepped.size(); ++i)
        changes += stepped[i] != stepped[i - 1] ? 1 : 0;
    CHECK(changes >= 8);
    CHECK(changes <= 10);

    // Synced: one step per division.
    Patch synced = listening({"flux1"});
    synced.set("flux1.type", choiceOf("flux1.type", "stepped"));
    synced.set("flux1.sync", 1);
    synced.set("flux1.division", choiceOf("flux1.division", "1/8"));
    EngineRig rig(synced);
    rig.transport.bpm = 120.0;
    rig.transport.playing = true;
    changes = 0;
    float before = 0.0f;
    for (int n = 0; n < 96000; n += 16) {
        rig.run(16);
        const float now = rig.source("flux1");
        changes += n > 0 && now != before ? 1 : 0;
        before = now;
    }
    CHECK(changes >= 7); // eight eighth notes in two seconds
    CHECK(changes <= 8);

    // Per voice: every note gets its own generator; shared: every note reads the same one.
    for (const char* scope : {"voice", "global"}) {
        Patch pan = zedtest::flatPatch();
        pan.set("osc1.on", 1);
        pan.set("flux1.scope", choiceOf("flux1.scope", scope));
        pan.set("flux1.type", choiceOf("flux1.type", "stepped"));
        pan.set("flux1.rate", 0.01); // holds its first value
        addRoute(pan, "flux1", "osc1.pan", 0.5, Polarity::Bi);
        const auto balance = [&](int note) {
            EngineRig one(pan);
            one.run(480, {MidiEvent::noteOn(0, 40, 1.0f)});
            one.run(480, {MidiEvent::noteOff(0, 40)});
            one.runBlocks(4800);
            const std::size_t from = one.left.size();
            one.run(9600, {MidiEvent::noteOn(0, note, 1.0f)});
            return zedtest::rmsOf(one.left, from + 960, from + 9600) / zedtest::rmsOf(one.right, from + 960, from + 9600);
        };
        EngineRig first(pan);
        first.run(9600, {MidiEvent::noteOn(0, 40, 1.0f)});
        const double firstBalance = zedtest::rmsOf(first.left, 960, 9600) / zedtest::rmsOf(first.right, 960, 9600);
        INFO(scope);
        if (std::string(scope) == "global")
            CHECK(balance(40) == Approx(firstBalance).epsilon(0.01)); // the second note hears the same value
        else
            CHECK(std::abs(balance(40) / firstBalance - 1.0) > 0.02); // a new generator, a new value
    }
}

TEST_CASE("sources: MIDI, per-note and constant sources report what they should", "[modulation][sources]") {
    Patch p = listening({"cc20", "cc74"});
    p.set("global.polyphony", 8);
    EngineRig rig(p);
    rig.run(16, {MidiEvent::noteOn(0, 72, 0.6f)});
    CHECK(rig.source("velocity") == Approx(0.6f));
    CHECK(rig.source("note") == Approx(0.2f)); // an octave above middle C, on a scale of five octaves
    CHECK(rig.source("constant") == 1.0f);
    CHECK(rig.source("release_velocity") == 0.0f);
    CHECK(rig.source("voice_index") == 0.0f);
    CHECK(rig.source("env1") > 0.0f);
    const float random1 = rig.source("random1"), random2 = rig.source("random2"), alternate = rig.source("alternate");
    CHECK(random1 >= 0.0f);
    CHECK(random1 < 1.0f);
    CHECK(random1 != random2);
    CHECK(std::abs(alternate) == 1.0f);

    // Controllers glide to their new values within a few milliseconds.
    rig.run(16, {MidiEvent::controller(0, 1, 0.5f), MidiEvent::controller(0, 2, 0.25f), MidiEvent::controller(0, 11, 0.75f),
                 MidiEvent::controller(0, 20, 0.9f), MidiEvent::controller(0, 64, 1.0f), MidiEvent::pitchBend(0, -0.5f),
                 {0, MidiEvent::Type::ChannelPressure, 0, 0, 0.4f}, {0, MidiEvent::Type::PolyPressure, 0, 72, 0.3f}});
    CHECK(rig.source("modwheel") < 0.2f); // on its way
    rig.runBlocks(4800);
    CHECK(rig.source("modwheel") == Approx(0.5f).margin(1e-4));
    CHECK(rig.source("breath") == Approx(0.25f).margin(1e-4));
    CHECK(rig.source("expression") == Approx(0.75f).margin(1e-4));
    CHECK(rig.source("cc20") == Approx(0.9f).margin(1e-4));
    CHECK(rig.source("sustain") == Approx(1.0f).margin(1e-4));
    CHECK(rig.source("pitchbend") == Approx(-0.5f).margin(1e-4));
    CHECK(rig.source("aftertouch") == Approx(0.4f).margin(1e-4));
    CHECK(rig.source("poly_aftertouch") == Approx(0.3f).margin(1e-4));
    CHECK(rig.source("mpe_pressure") == 0.0f); // not in MPE mode
    CHECK(Engine::sourceIndex("cc127") == Engine::sourceIndex("cc0") + 127);
    CHECK(Engine::sourceIndex("cc128") == -1);
    CHECK(Engine::sourceIndex("nothing") == -1);

    // A second note: new random values, the other side of Alternate, the next voice index; poly
    // pressure belongs to its key.
    rig.run(16, {MidiEvent::controller(0, 64, 0.0f), MidiEvent::noteOn(0, 48, 1.0f)});
    rig.runBlocks(960);
    CHECK(rig.source("random1") != random1);
    CHECK(rig.source("alternate") == -alternate);
    CHECK(rig.source("voice_index") == Approx(1.0f / 7.0f));
    CHECK(rig.source("note") == Approx(-0.2f));
    CHECK(rig.source("poly_aftertouch") == 0.0f);
    rig.run(16, {MidiEvent::noteOff(0, 48, 0.35f)});
    rig.runBlocks(64);
    CHECK(rig.source("release_velocity") == Approx(0.35f));

    // Per-voice use: poly pressure opens only the key it is pressed on.
    Patch pressure = zedtest::flatPatch();
    pressure.set("osc1.on", 1);
    pressure.set("osc1.level", kSilenceDb);
    addRoute(pressure, "poly_aftertouch", "osc1.level", 1.0);
    EngineRig keys(pressure);
    keys.run(4800, {MidiEvent::noteOn(0, 48, 1.0f), MidiEvent::noteOn(0, 72, 1.0f)});
    CHECK(zedtest::peakOf(keys.left, 0, 4800) == 0.0f);
    keys.run(24000, {{0, MidiEvent::Type::PolyPressure, 0, 72, 1.0f}});
    CHECK(zedtest::cents(zedtest::measureFrequency(keys.left, 9600, 28800, kRate), noteToHz(72.0)) == Approx(0.0).margin(0.5));
}

TEST_CASE("mpe: bend, pressure and slide belong to the note on their channel", "[modulation][mpe]") {
    Patch p = zedtest::flatPatch();
    p.set("osc1.on", 1);
    p.set("global.mpe", 1);
    p.set("global.mpe_bend_range", 48);
    p.set("global.bend_up", 2);
    const auto play = [&](std::vector<MidiEvent> events, const Patch& patch) {
        EngineRig rig(patch);
        rig.run(28800, events);
        return zedtest::Stereo{rig.left, rig.right};
    };
    // Channel 2 bends a quarter of the range: one octave, for its own note only.
    const std::vector<MidiEvent> bend = {MidiEvent::pitchBend(0, 0.25f, 1)};
    const MidiEvent a = MidiEvent::noteOn(0, 48, 1.0f, 1), b = MidiEvent::noteOn(0, 67, 1.0f, 2);
    const zedtest::Stereo bent = play({bend[0], a}, p);
    const zedtest::Stereo plain = play({bend[0], b}, p);
    CHECK(zedtest::cents(zedtest::measureFrequency(bent.left, 4800, 28800, kRate), noteToHz(60.0)) == Approx(0.0).margin(0.2));
    CHECK(zedtest::cents(zedtest::measureFrequency(plain.left, 4800, 28800, kRate), noteToHz(67.0)) == Approx(0.0).margin(0.2));
    const zedtest::Stereo both = play({bend[0], a, b}, p);
    std::vector<float> sum(both.left.size());
    for (std::size_t i = 0; i < sum.size(); ++i)
        sum[i] = bent.left[i] + plain.left[i];
    CHECK(zedtest::maxDifference(both.left, sum) < 1e-4);

    // The master channel bends every note by the ordinary range, on top.
    const zedtest::Stereo master = play({MidiEvent::pitchBend(0, 1.0f, 0), a}, p);
    CHECK(zedtest::cents(zedtest::measureFrequency(master.left, 4800, 28800, kRate), noteToHz(50.0)) == Approx(0.0).margin(0.2));

    // A bend that arrives while the note sounds glides in; the range is a parameter.
    Patch narrow = p;
    narrow.set("global.mpe_bend_range", 12);
    EngineRig moving(narrow);
    moving.run(9600, {a});
    moving.run(28800, {MidiEvent::pitchBend(0, -1.0f, 1)});
    CHECK(zedtest::cents(zedtest::measureFrequency(moving.left, 14400, 38400, kRate), noteToHz(36.0)) == Approx(0.0).margin(0.2));
    CHECK(zedtest::largestJump(moving.left, 9000, 12000) < zedtest::largestJump(moving.left, 4800, 9000) * 1.2f);

    // Pressure and slide: per note, read from the note's channel.
    EngineRig rig(p);
    rig.run(16, {a, b});
    rig.run(16, {{0, MidiEvent::Type::ChannelPressure, 2, 0, 0.8f}, MidiEvent::controller(0, 74, 0.6f, 2),
                 {0, MidiEvent::Type::ChannelPressure, 1, 0, 0.1f}, MidiEvent::controller(0, 74, 0.2f, 1)});
    rig.runBlocks(4800);
    CHECK(rig.source("mpe_pressure") == Approx(0.8f).margin(1e-4)); // the most recent note is on channel 3
    CHECK(rig.source("mpe_slide") == Approx(0.6f).margin(1e-4));
    CHECK(rig.source("aftertouch") == 0.0f); // member channels do not touch the shared sources
    CHECK(rig.source("pitchbend") == 0.0f);
    // Routed per voice: slide opens the level of its own note only.
    Patch slide = p;
    slide.set("osc1.level", kSilenceDb);
    addRoute(slide, "mpe_slide", "osc1.level", 1.0);
    EngineRig sliding(slide);
    sliding.run(4800, {a, b});
    sliding.run(28800, {MidiEvent::controller(0, 74, 1.0f, 2)});
    CHECK(zedtest::cents(zedtest::measureFrequency(sliding.left, 9600, 33600, kRate), noteToHz(67.0)) == Approx(0.0).margin(0.5));

    // With MPE off, a bend on any channel is the ordinary pitch wheel for every note.
    Patch off = p;
    off.set("global.mpe", 0);
    const zedtest::Stereo wheel = play({MidiEvent::pitchBend(0, 1.0f, 1), b}, off);
    CHECK(zedtest::cents(zedtest::measureFrequency(wheel.left, 4800, 28800, kRate), noteToHz(69.0)) == Approx(0.0).margin(0.2));
}

TEST_CASE("follower: tracks the level of the voices or of the side chain", "[modulation][follower]") {
    Patch p = zedtest::flatPatch();
    p.set("osc1.on", 1);
    p.set("global.follower.attack", 5);
    p.set("global.follower.release", 50);
    addRoute(p, "follower", "osc2.pan", 0.0);
    EngineRig rig(p);
    rig.engine.setTable(0, zedtest::sineTableData());
    rig.runBlocks(960);
    CHECK(rig.source("follower") == 0.0f);
    rig.run(16, {MidiEvent::noteOn(0, 69, 1.0f)});
    rig.runBlocks(9600);
    const double peak = zedtest::peakOf(rig.left, 4800, 9600);
    CHECK(rig.source("follower") > 0.5 * peak); // a peak follower with a short attack sits near the peaks
    CHECK(rig.source("follower") <= peak * 1.001);
    rig.run(16, {MidiEvent::noteOff(0, 69)});
    rig.runBlocks(24000);
    CHECK(rig.source("follower") < 0.01f); // released

    // Side chain: the follower hears the input instead.
    p.set("global.follower.source", choiceOf("global.follower.source", "sidechain"));
    Engine engine;
    engine.prepare(kRate);
    engine.loadPatch(p);
    std::vector<float> left(512), right(512), side(512, 0.5f);
    for (int block = 0; block < 40; ++block) {
        ProcessArgs args{left.data(), right.data(), 512, {}, {}};
        args.sidechainLeft = side.data();
        args.sidechainRight = side.data();
        engine.process(args);
    }
    CHECK(engine.sourceValue(Engine::sourceIndex("follower")) == Approx(0.5f).margin(0.01));
    for (int block = 0; block < 40; ++block)
        engine.process({left.data(), right.data(), 512, {}, {}}); // no input connected: silence
    CHECK(engine.sourceValue(Engine::sourceIndex("follower")) < 0.01f);
}
