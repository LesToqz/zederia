// The modulation matrix inside the engine (plan 3.7, 6.1): every route's arithmetic end to end,
// per-voice and shared routes, routes onto other routes and onto modulators, and the switches.
#include "../support/Analysis.h"
#include "../support/EngineRig.h"
#include "zederia/engine/ModMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;
using zedtest::addRoute;
using zedtest::EngineRig;
using zedtest::idx;

namespace {

constexpr double kRate = 48000.0;

// The value a destination should show for a sum of route contributions (in normalized units).
double shifted(const char* id, double base, double offset) {
    const ParamDef& def = *Registry::get().find(id);
    return def.fromNormalized(std::clamp(def.toNormalized(base) + offset, 0.0, 1.0));
}

} // namespace

TEST_CASE("matrix: amount, polarity, curve and via reach the destination as the arithmetic says", "[matrix][modulation]") {
    struct Case {
        double macro, amount;
        Polarity polarity;
        ModCurve curve;
        bool via, invert;
        double viaValue;
    };
    const Case cases[] = {
        {0.6, 0.5, Polarity::Uni, ModCurve::Linear, false, false, 0.0},
        {0.6, 0.5, Polarity::Bi, ModCurve::Linear, false, false, 0.0},
        {0.2, 0.5, Polarity::Bi, ModCurve::Linear, false, false, 0.0}, // below the centre: pulls down
        {0.6, -0.25, Polarity::Uni, ModCurve::Linear, false, false, 0.0},
        {0.5, 0.4, Polarity::Uni, ModCurve::Exp, false, false, 0.0},
        {0.5, 0.4, Polarity::Uni, ModCurve::Log, false, false, 0.0},
        {0.3, 0.4, Polarity::Uni, ModCurve::S, false, false, 0.0},
        {0.2, 0.4, Polarity::Bi, ModCurve::Exp, false, false, 0.0}, // the curve keeps the sign
        {0.8, 0.5, Polarity::Uni, ModCurve::Linear, true, false, 0.25},
        {0.8, 0.5, Polarity::Uni, ModCurve::Linear, true, true, 0.25},
        {1.0, 1.0, Polarity::Uni, ModCurve::Linear, false, false, 0.0},  // runs into the top of the range
        {1.0, -1.0, Polarity::Uni, ModCurve::Linear, false, false, 0.0}, // and the bottom
    };
    int n = 0;
    for (const Case& c : cases) {
        Patch p;
        p.set("macro1.value", c.macro);
        p.set("macro2.value", c.viaValue);
        ModRoute& r = addRoute(p, "macro1", "filter1.cutoff", c.amount, c.polarity, c.curve);
        if (c.via) {
            r.via = "macro2";
            r.viaInvert = c.invert;
        }
        EngineRig rig(p);
        rig.run(512);
        float signal = modShape(static_cast<float>(c.macro), false, c.polarity, c.curve, nullptr);
        if (c.via)
            signal *= modVia(static_cast<float>(c.viaValue), false, c.invert);
        INFO("case " << n++);
        CHECK(rig.live("filter1.cutoff") == Approx(shifted("filter1.cutoff", 2000.0, c.amount * signal)).epsilon(1e-4));
    }
}

TEST_CASE("matrix: bipolar macros, drawn curves and several routes on one destination", "[matrix][modulation]") {
    // A bipolar macro reads -1..1 with 50 % as zero, and "bi" leaves it alone.
    Patch p;
    p.set("macro1.bipolar", 1);
    p.set("macro1.value", 0.25);
    addRoute(p, "macro1", "osc1.pan", 0.5, Polarity::Bi);
    {
        EngineRig rig(p);
        rig.run(256);
        CHECK(rig.source("macro1") == Approx(-0.5));
        CHECK(rig.live("osc1.pan") == Approx(shifted("osc1.pan", 0.0, 0.5 * -0.5)).margin(1e-5));
    }

    // A drawn curve: a triangle that peaks in the middle.
    Patch drawn;
    drawn.set("macro1.value", 0.25);
    ModRoute& r = addRoute(drawn, "macro1", "osc1.position", 1.0, Polarity::Uni, ModCurve::Drawn);
    r.curveShape.points = {{0.0, 0.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, 0.0, 0.0}};
    {
        EngineRig rig(drawn);
        rig.run(256);
        CHECK(rig.live("osc1.position") == Approx(0.5).margin(1e-3));
        rig.set("macro1.value", 0.5);
        rig.runBlocks(4800);
        CHECK(rig.live("osc1.position") == Approx(1.0).margin(1e-3));
        rig.set("macro1.value", 0.875);
        rig.runBlocks(4800);
        CHECK(rig.live("osc1.position") == Approx(0.25).margin(1e-3));
    }
    // Drawn without a shape is a straight line.
    Patch undrawn;
    undrawn.set("macro1.value", 0.3);
    addRoute(undrawn, "macro1", "osc1.position", 1.0, Polarity::Uni, ModCurve::Drawn);
    {
        EngineRig rig(undrawn);
        rig.run(256);
        CHECK(rig.live("osc1.position") == Approx(0.3).margin(1e-4));
    }

    // Contributions add up, and the sum is clamped to the parameter's range.
    Patch sum;
    sum.set("macro1.value", 0.5);
    sum.set("macro2.value", 1.0);
    sum.set("macro3.value", 1.0);
    sum.set("osc1.position", 0.1);
    addRoute(sum, "macro1", "osc1.position", 0.4);
    addRoute(sum, "macro2", "osc1.position", 0.3);
    addRoute(sum, "macro3", "osc1.position", -0.2);
    {
        EngineRig rig(sum);
        rig.run(256);
        CHECK(rig.live("osc1.position") == Approx(0.1 + 0.2 + 0.3 - 0.2).margin(1e-5));
        rig.set("macro1.value", 1.0); // 0.1 + 0.4 + 0.3 - 0.2 = 0.6
        rig.set("macro3.value", 0.0); // 0.1 + 0.4 + 0.3 = 0.8
        rig.runBlocks(4800);
        CHECK(rig.live("osc1.position") == Approx(0.8).margin(1e-4));
        rig.set("osc1.position", 0.9); // would be 1.6
        rig.runBlocks(4800);
        CHECK(rig.live("osc1.position") == Approx(1.0).margin(1e-6));
    }
}

TEST_CASE("matrix: enable, mute and solo choose which routes act", "[matrix][modulation]") {
    const auto positionWith = [](bool enabled0, bool mute0, bool solo0, bool solo1) {
        Patch p;
        p.set("macro1.value", 1.0);
        ModRoute& a = addRoute(p, "macro1", "osc1.position", 0.5);
        a.enabled = enabled0;
        a.mute = mute0;
        a.solo = solo0;
        ModRoute& b = addRoute(p, "macro1", "osc1.position", 0.25);
        b.solo = solo1;
        EngineRig rig(p);
        rig.run(256);
        return rig.live("osc1.position");
    };
    CHECK(positionWith(true, false, false, false) == Approx(0.75).margin(1e-5));
    CHECK(positionWith(false, false, false, false) == Approx(0.25).margin(1e-5)); // disabled
    CHECK(positionWith(true, true, false, false) == Approx(0.25).margin(1e-5));   // muted
    CHECK(positionWith(true, false, true, false) == Approx(0.5).margin(1e-5));    // only the solo route
    CHECK(positionWith(true, false, false, true) == Approx(0.25).margin(1e-5));
    CHECK(positionWith(true, false, true, true) == Approx(0.75).margin(1e-5));    // both solo
    CHECK(positionWith(true, true, true, false) == Approx(0.0).margin(1e-5));     // a muted solo is still muted

    // Routes can change while the engine runs; unknown sources and destinations are ignored.
    Patch p;
    p.set("macro1.value", 1.0);
    EngineRig rig(p);
    rig.run(256);
    CHECK(rig.live("osc1.position") == 0.0);
    std::vector<ModRoute> routes(3);
    routes[0].source = "macro1";
    routes[0].dest = "osc1.position";
    routes[0].polarity = Polarity::Uni;
    routes[1].source = "no_such_source";
    routes[1].dest = "osc1.position";
    routes[2].source = "macro1";
    routes[2].dest = "osc1.octave"; // not a continuous parameter
    rig.engine.setRoutes(routes);
    for (int i = 0; i < 3; ++i)
        rig.engine.params().set(Registry::get().modAmount(i), 0.4);
    rig.runBlocks(4800);
    CHECK(rig.live("osc1.position") == Approx(0.4).margin(1e-4));
    CHECK(rig.live("osc1.octave") == 0.0);
    rig.engine.setRoutes({});
    rig.runBlocks(256);
    CHECK(rig.live("osc1.position") == 0.0);
}

TEST_CASE("matrix: routes reach other routes' amounts and the modulators themselves", "[matrix][modulation]") {
    // Macro 2 opens route 1's amount: 0.2 + 0.5 * 0.4 of the amount's two-unit range = 0.6.
    Patch p;
    p.set("macro1.value", 0.5);
    p.set("macro2.value", 0.4);
    addRoute(p, "macro1", "osc1.position", 0.2);
    addRoute(p, "macro2", "mod.1.amount", 0.5);
    EngineRig rig(p);
    rig.runBlocks(960);
    CHECK(rig.live("mod.1.amount") == Approx(0.6).margin(1e-5));
    CHECK(rig.live("osc1.position") == Approx(0.6 * 0.5).margin(1e-4));

    // A macro on an LFO's rate, and an LFO's rate following an envelope per voice.
    Patch rates;
    rates.set("macro1.value", 0.5);
    addRoute(rates, "macro1", "lfo1.rate", 0.3);
    addRoute(rates, "lfo1", "osc1.pan", 0.0);
    rates.set("lfo1.scope", zedtest::choiceOf("lfo1.scope", "global"));
    rates.lfoShapes[0] = zedtest::lineShape(-1.0, 1.0);
    EngineRig lfo(rates);
    lfo.runBlocks(960);
    const double hz = shifted("lfo1.rate", 2.0, 0.15);
    CHECK(lfo.live("lfo1.rate") == Approx(hz).epsilon(1e-4));
    // The LFO really runs at that rate: a ramp that started with the engine, read a little later.
    lfo.runTo(300.0);
    const double phase = 0.3 * hz;
    CHECK(lfo.source("lfo1") == Approx(-1.0 + 2.0 * (phase - std::floor(phase))).margin(0.02));

    // Depth of one modulator set by another: an envelope as Via.
    Patch via;
    via.set("env2.attack", 100);
    via.set("env2.attack_curve", 0);
    via.set("env2.sustain", 1);
    ModRoute& r = addRoute(via, "constant", "osc1.position", 0.8);
    r.via = "env2";
    EngineRig depth(via);
    depth.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
    depth.runTo(50.0);
    CHECK(depth.live("osc1.position") == Approx(0.4).margin(0.01));
    depth.runTo(150.0);
    CHECK(depth.live("osc1.position") == Approx(0.8).margin(1e-3));
}

TEST_CASE("matrix: a route with a per-voice source and destination is polyphonic", "[matrix][modulation]") {
    // Velocity transposes the oscillator: an octave at full velocity.
    Patch p = zedtest::flatPatch();
    p.set("osc1.on", 1);
    addRoute(p, "velocity", "osc1.coarse", 12.0 / 128.0);
    const auto play = [&](std::vector<MidiEvent> events) {
        EngineRig rig(p);
        rig.run(24000, events);
        return rig.left;
    };
    const auto hard = play({MidiEvent::noteOn(0, 48, 1.0f)});
    const auto soft = play({MidiEvent::noteOn(0, 55, 0.5f)});
    CHECK(zedtest::cents(zedtest::measureFrequency(hard, 4800, 24000, kRate), noteToHz(60.0)) == Approx(0.0).margin(0.2));
    CHECK(zedtest::cents(zedtest::measureFrequency(soft, 4800, 24000, kRate), noteToHz(61.0)) == Approx(0.0).margin(0.2));
    // Played together, each voice keeps its own transposition: the chord is the sum of the notes.
    const auto both = play({MidiEvent::noteOn(0, 48, 1.0f), MidiEvent::noteOn(0, 55, 0.5f)});
    std::vector<float> sum(hard.size());
    for (std::size_t i = 0; i < sum.size(); ++i)
        sum[i] = hard[i] + soft[i];
    CHECK(zedtest::maxDifference(both, sum) < 1e-4);

    // A source every voice shares moves them all alike, and the display shows one value.
    Patch wheel = zedtest::flatPatch();
    wheel.set("osc1.on", 1);
    addRoute(wheel, "modwheel", "osc1.coarse", 12.0 / 128.0);
    EngineRig shared(wheel);
    shared.run(24000, {MidiEvent::controller(0, 1, 1.0f), MidiEvent::noteOn(0, 48, 1.0f)});
    CHECK(zedtest::cents(zedtest::measureFrequency(shared.left, 4800, 24000, kRate), noteToHz(60.0)) == Approx(0.0).margin(0.2));
    CHECK(shared.live("osc1.coarse") == Approx(12.0).margin(1e-3));

    // A per-voice source on a destination that exists once follows the most recent note.
    Patch master = zedtest::flatPatch();
    master.set("osc1.on", 1);
    master.set("global.master", -24);
    addRoute(master, "velocity", "global.master", 0.25);
    EngineRig lead(master);
    lead.run(4800, {MidiEvent::noteOn(0, 48, 1.0f)});
    CHECK(lead.live("global.master") == Approx(shifted("global.master", -24.0, 0.25)).margin(0.01));
    lead.run(4800, {MidiEvent::noteOn(0, 55, 0.2f)});
    CHECK(lead.live("global.master") == Approx(shifted("global.master", -24.0, 0.05)).margin(0.01));
}

TEST_CASE("matrix: the unison index spreads an oscillator's lanes", "[matrix][modulation][unison]") {
    // Three unison voices with no detune and no width: mono. A route from the unison index to
    // pan puts the outer voices on opposite sides.
    Patch p = zedtest::flatPatch();
    p.set("osc1.on", 1);
    p.set("osc1.unison.voices", 3);
    p.set("osc1.unison.detune", 0);
    p.set("osc1.unison.width", 0);
    p.set("osc1.unison.phase", zedtest::choiceOf("osc1.unison.phase", "consistent"));
    const auto render = [](const Patch& patch) {
        EngineRig rig(patch);
        rig.run(96000, {MidiEvent::noteOn(0, 57, 1.0f)});
        return zedtest::Stereo{rig.left, rig.right};
    };
    const zedtest::Stereo mono = render(p);
    CHECK(zedtest::maxDifference(mono.left, mono.right) < 1e-6);
    Patch spread = p;
    addRoute(spread, "unison_index", "osc1.pan", 0.5, Polarity::Bi);
    const zedtest::Stereo wide = render(spread);
    CHECK(zedtest::maxDifference(wide.left, wide.right) > 0.05);

    // To pitch: the outer voices move a semitone away from the centre, one up and one down.
    Patch detuned = p;
    addRoute(detuned, "unison_index", "osc1.coarse", 1.0 / 128.0, Polarity::Bi);
    const zedtest::Stereo beating = render(detuned);
    // Each lane is now a tone of its own: the centre on the key, the outer two a semitone off.
    const auto spectrum = zedtest::spectrumOf(beating.left, 4800, 65536, kRate); // fine enough to tell a semitone apart
    const auto plain = zedtest::spectrumOf(mono.left, 4800, 65536, kRate);
    const double centre = zedtest::lineLevel(spectrum, noteToHz(57.0));
    CHECK(zedtest::lineLevel(spectrum, noteToHz(58.0)) > centre * 0.25);
    CHECK(zedtest::lineLevel(spectrum, noteToHz(56.0)) > centre * 0.25);
    CHECK(zedtest::lineLevel(plain, noteToHz(58.0)) < zedtest::lineLevel(plain, noteToHz(57.0)) * 1.0e-4);
    CHECK(zedtest::lineLevel(plain, noteToHz(56.0)) < zedtest::lineLevel(plain, noteToHz(57.0)) * 1.0e-4);
}

TEST_CASE("matrix: a full matrix stays finite, deterministic and independent of the block size", "[matrix][determinism]") {
    // Sixty-four routes from every kind of source onto a spread of destinations.
    const auto& reg = Registry::get();
    Patch p;
    p.set("osc2.on", 1);
    p.set("sub.on", 1);
    p.set("filter1.on", 1);
    p.set("filter2.on", 1);
    p.set("lfo2.scope", zedtest::choiceOf("lfo2.scope", "global"));
    p.set("lfo3.audio_rate", 1);
    p.set("lfo3.rate", 700);
    p.set("flux2.scope", zedtest::choiceOf("flux2.scope", "voice"));
    p.set("env3.mode", zedtest::choiceOf("env3.mode", "multi"));
    p.envShapes[2].points = {{20.0, "", 1.0, 0.3}, {60.0, "", 0.4, -0.3}, {0.0, "1/16", 0.8, 0.0}, {200.0, "", 0.0, 0.5}};
    p.envShapes[2].loopStart = 0;
    p.envShapes[2].loopEnd = 2;
    std::vector<std::string> destinations;
    for (const ParamDef& d : reg.params())
        if (d.modulatable && (d.module == "osc1" || d.module == "osc2" || d.module == "sub" || d.module == "filter1" ||
                              d.module == "filter2" || d.module == "voice" || d.module == "lfo1" || d.module == "env2"))
            destinations.push_back(d.id);
    std::vector<std::string> sources;
    for (const ModSourceDef& s : reg.modSources())
        sources.push_back(s.id);
    sources.emplace_back("cc74");
    Rng rng(404);
    for (int i = 0; i < kNumModRoutes; ++i) {
        ModRoute& r = addRoute(p, sources[static_cast<std::size_t>(i) % sources.size()].c_str(),
                               destinations[rng.next() % destinations.size()].c_str(), rng.uniform() * 0.4 - 0.2,
                               i % 2 == 0 ? Polarity::Bi : Polarity::Uni, static_cast<ModCurve>(i % 4));
        if (i % 5 == 0)
            r.via = sources[rng.next() % sources.size()];
    }
    REQUIRE(p.routes.size() == static_cast<std::size_t>(kNumModRoutes));
    p.routes[10].source = "osc2_audio"; // two that move a cutoff every sample
    p.routes[10].via.clear();
    p.routes[10].dest = "filter1.cutoff";
    p.routes[11].source = "lfo3";
    p.routes[11].dest = "filter2.cutoff";

    std::vector<SeqEvent> events;
    for (int i = 0; i < 10; ++i) {
        events.push_back({static_cast<std::int64_t>(0.02 * i * kRate), MidiEvent::noteOn(0, 40 + i * 5, 0.3f + 0.07f * static_cast<float>(i))});
        events.push_back({static_cast<std::int64_t>((0.25 + 0.02 * i) * kRate), MidiEvent::noteOff(0, 40 + i * 5, 0.5f)});
    }
    events.push_back({2000, MidiEvent::controller(0, 1, 0.7f)});
    events.push_back({3000, MidiEvent::controller(0, 74, 0.4f)});
    events.push_back({5000, MidiEvent::pitchBend(0, -0.3f)});
    std::stable_sort(events.begin(), events.end(), [](const SeqEvent& a, const SeqEvent& b) { return a.sample < b.sample; });

    std::uint64_t reference = 0;
    for (const int block : {512, 1, 16, 37, 4096}) {
        RenderSettings settings;
        settings.blockSize = block;
        const RenderResult r = renderOffline(p, events, settings);
        bool finite = true;
        for (const float s : r.left)
            finite = finite && std::isfinite(s);
        REQUIRE(finite);
        REQUIRE(r.peak() > 0.01f);
        REQUIRE(r.peak() < 50.0f);
        if (reference == 0)
            reference = r.hash();
        INFO("block size " << block);
        CHECK(r.hash() == reference);
    }
}
