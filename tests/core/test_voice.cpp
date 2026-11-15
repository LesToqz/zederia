// The voice around its sources (plan 3.5, 3.6, 5.7, 6.1): the two filter slots and their
// routing, the response the display is given, glide, tuning, the Character controls, smoothing
// of every continuous control, and stability of every filter type under audio-rate modulation.
#include "../support/Analysis.h"
#include "../support/EngineRig.h"
#include "zederia/engine/Filter.h"
#include "zederia/engine/SampleData.h"
#include "zederia/tuning/Tuning.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>

using namespace zed;
using Catch::Approx;
using zedtest::addRoute;
using zedtest::choiceOf;
using zedtest::EngineRig;
using zedtest::idx;

namespace {

constexpr double kRate = 48000.0;

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

// A pure tone at the played key: the Sub oscillator as a sine, at its own octave.
Patch tonePatch(double levelDb = 0.0) {
    Patch p = zedtest::flatPatch();
    p.set("sub.on", 1);
    p.set("sub.octave", 0);
    p.set("sub.level", levelDb);
    return p;
}

// Left and right level of a held note once it has settled.
struct Levels {
    double left, right;
};
Levels levelsOf(const Patch& patch, int note, double seconds = 0.5) {
    EngineRig rig(patch);
    const int frames = static_cast<int>(seconds * kRate);
    rig.run(frames, {MidiEvent::noteOn(0, note, 1.0f)});
    const auto from = static_cast<std::size_t>(frames / 2);
    return {zedtest::rmsOf(rig.left, from, rig.left.size()), zedtest::rmsOf(rig.right, from, rig.right.size())};
}

flt::Settings settingsOf(const char* type, double cutoff, double resonance = 0.0, double character = 0.5) {
    flt::Settings s;
    s.type = choiceOf("filter1.type", type);
    s.cutoffHz[0] = s.cutoffHz[1] = static_cast<float>(cutoff);
    s.resonance = static_cast<float>(resonance);
    s.character = static_cast<float>(character);
    return s;
}

} // namespace

TEST_CASE("voice filter: the slot does what the response it reports says", "[voice][filter]") {
    struct Case {
        const char* type;
        double cutoff, resonance, character, levelDb;
        int note;
    };
    const Case cases[] = {
        {"svf_lp12", 300, 0.0, 0.5, 0, 69},   {"svf_lp24", 800, 0.4, 0.5, 0, 81},   {"svf_hp24", 1000, 0.0, 0.5, 0, 57},
        {"svf_bp12", 440, 0.6, 0.5, 0, 81},   {"svf_notch12", 500, 0.3, 0.5, 0, 69}, {"low_shelf", 2000, 0.0, 0.9, -12, 57},
        {"bell", 440, 0.5, 0.1, 0, 69},       {"ladder_lp24", 400, 0.2, 0.0, -40, 69}, {"morph_lbh12", 700, 0.2, 0.8, 0, 69},
        {"dual_lp_hp", 600, 0.3, 0.7, 0, 60},
    };
    double worst = 0.0;
    for (const Case& c : cases) {
        Patch p = tonePatch(c.levelDb);
        const Levels dry = levelsOf(p, c.note);
        p.set("filter1.on", 1);
        p.set("filter1.type", choiceOf("filter1.type", c.type));
        p.set("filter1.cutoff", c.cutoff);
        p.set("filter1.resonance", c.resonance);
        p.set("filter1.character", c.character);
        EngineRig rig(p);
        rig.run(48000, {MidiEvent::noteOn(0, c.note, 1.0f)});
        const double measured = db(zedtest::rmsOf(rig.left, 24000, 48000) / dry.left);
        const double hz[1] = {noteToHz(c.note)};
        float magnitude[1] = {0.0f};
        rig.engine.filterResponse(0, hz, magnitude, {});
        INFO(c.type << ": measured " << measured << " dB, reported " << magnitude[0] << " dB");
        CHECK(measured == Approx(magnitude[0]).margin(0.5));
        worst = std::max(worst, std::abs(measured - magnitude[0]));
        const FilterView view = rig.engine.filterView(0);
        CHECK(view.on);
        CHECK(view.settings.type == choiceOf("filter1.type", c.type));
        CHECK(view.settings.cutoffHz[0] == Approx(c.cutoff).epsilon(1e-4));
    }
    WARN("voice filters against the response the engine reports: worst difference " << worst << " dB");

    // Magnitude and phase over a list of frequencies; a slot that is off is flat.
    Patch p = tonePatch();
    p.set("filter1.on", 1);
    p.set("filter1.cutoff", 1000);
    EngineRig rig(p);
    rig.run(512);
    const double hz[4] = {100.0, 1000.0, 4000.0, 16000.0};
    float magnitude[4], phase[4];
    rig.engine.filterResponse(0, hz, magnitude, phase);
    CHECK(magnitude[0] == Approx(0.0).margin(0.1));
    CHECK(magnitude[1] > -7.0f); // around the corner
    CHECK(magnitude[1] < 1.0f);
    CHECK(magnitude[2] < -20.0f);
    CHECK(magnitude[3] < magnitude[2]);
    CHECK(phase[1] == Approx(-kPi / 2.0).margin(0.05)); // a two-pole low-pass is a quarter turn late at its cutoff
    rig.engine.filterResponse(1, hz, magnitude, phase);
    for (int i = 0; i < 4; ++i) {
        CHECK(magnitude[i] == 0.0f);
        CHECK(phase[i] == 0.0f);
    }
    CHECK_FALSE(rig.engine.filterView(1).on);
}

TEST_CASE("voice filter: key tracking, stereo offset and modulation reach the cutoff", "[voice][filter]") {
    Patch p = tonePatch();
    p.set("filter1.on", 1);
    p.set("filter1.cutoff", 500);
    const auto viewAt = [](const Patch& patch, int note) {
        EngineRig rig(patch);
        rig.run(4800, {MidiEvent::noteOn(0, note, 1.0f)});
        return rig.engine.filterView(0);
    };
    CHECK(viewAt(p, 84).settings.cutoffHz[0] == Approx(500.0).epsilon(1e-4)); // no tracking by default
    p.set("filter1.keytrack", 1.0);
    CHECK(viewAt(p, 60).settings.cutoffHz[0] == Approx(500.0).epsilon(1e-4)); // middle C is the pivot
    CHECK(viewAt(p, 72).settings.cutoffHz[0] == Approx(1000.0).epsilon(1e-4));
    CHECK(viewAt(p, 36).settings.cutoffHz[1] == Approx(125.0).epsilon(1e-4));
    p.set("filter1.keytrack", 0.5);
    CHECK(viewAt(p, 84).settings.cutoffHz[0] == Approx(1000.0).epsilon(1e-4));
    // The pitch wheel moves the tracked cutoff with the note.
    p.set("filter1.keytrack", 1.0);
    p.set("global.bend_up", 12);
    {
        EngineRig rig(p);
        rig.run(9600, {MidiEvent::noteOn(0, 60, 1.0f), MidiEvent::pitchBend(0, 1.0f)});
        CHECK(rig.engine.filterView(0).settings.cutoffHz[0] == Approx(1000.0).epsilon(1e-3));
    }

    // Stereo offset moves the two sides apart, an octave each way at 100 %.
    Patch wide = tonePatch();
    wide.set("filter1.on", 1);
    wide.set("filter1.cutoff", 440);
    wide.set("filter1.stereo_offset", 0.5);
    const FilterView view = viewAt(wide, 69);
    CHECK(view.settings.cutoffHz[0] == Approx(440.0 / std::sqrt(2.0)).epsilon(1e-4));
    CHECK(view.settings.cutoffHz[1] == Approx(440.0 * std::sqrt(2.0)).epsilon(1e-4));
    const Levels dry = levelsOf(tonePatch(), 69);
    const Levels wet = levelsOf(wide, 69);
    CHECK(db(wet.left / dry.left) == Approx(db(std::abs(flt::response(view.settings, 0, kRate, 440.0)))).margin(0.3));
    CHECK(db(wet.right / dry.right) == Approx(db(std::abs(flt::response(view.settings, 1, kRate, 440.0)))).margin(0.3));
    CHECK(wet.right > wet.left * 1.5);

    // A route onto the cutoff shows in the view; with no note the view is the patch's own setting.
    Patch moved = tonePatch();
    moved.set("filter1.on", 1);
    moved.set("filter1.cutoff", 500);
    moved.set("macro1.value", 1.0);
    addRoute(moved, "macro1", "filter1.cutoff", 0.25);
    const ParamDef& cutoff = *Registry::get().find("filter1.cutoff");
    const double expected = cutoff.fromNormalized(cutoff.toNormalized(500.0) + 0.25);
    CHECK(viewAt(moved, 60).settings.cutoffHz[0] == Approx(expected).epsilon(1e-3));
    EngineRig idle(moved);
    idle.run(512);
    CHECK(idle.engine.filterView(0).settings.cutoffHz[0] == Approx(expected).epsilon(1e-3));
    // An envelope per voice: the view follows the most recent note.
    Patch swept = tonePatch();
    swept.set("filter1.on", 1);
    swept.set("filter1.cutoff", 200);
    swept.set("env2.attack", 0);
    swept.set("env2.decay", 100);
    swept.set("env2.decay_curve", 0);
    swept.set("env2.sustain", 0);
    addRoute(swept, "env2", "filter1.cutoff", 0.5);
    EngineRig rig(swept);
    rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
    rig.runTo(50.0);
    CHECK(rig.engine.filterView(0).settings.cutoffHz[0] == Approx(cutoff.fromNormalized(cutoff.toNormalized(200.0) + 0.25)).epsilon(0.01));
    rig.runTo(150.0);
    CHECK(rig.engine.filterView(0).settings.cutoffHz[0] == Approx(200.0).epsilon(1e-3));
}

TEST_CASE("voice filter: series, parallel and everything between; mix and level", "[voice][filter][routing]") {
    // A tone at 440 Hz; filter 1 a low-pass below it, filter 2 a high-pass above it.
    const flt::Settings one = settingsOf("svf_lp12", 220.0), two = settingsOf("svf_hp12", 880.0);
    const std::complex<double> h1 = flt::response(one, 0, kRate, 440.0), h2 = flt::response(two, 0, kRate, 440.0);
    Patch p = tonePatch();
    const double dry = levelsOf(p, 69).left;
    p.set("filter1.on", 1);
    p.set("filter1.type", one.type);
    p.set("filter1.cutoff", 220);
    p.set("filter2.on", 1);
    p.set("filter2.type", two.type);
    p.set("filter2.cutoff", 880);
    const auto gain = [&](double routing, double balance) {
        Patch q = p;
        q.set("voice.filter_routing", routing);
        q.set("sub.sends.filter_balance", balance);
        return levelsOf(q, 69).left / dry;
    };
    // Series: everything goes through both.
    CHECK(db(gain(0.0, 0.0)) == Approx(db(std::abs(h1 * h2))).margin(0.3));
    // Series, sent straight to filter 2: filter 1 is skipped.
    CHECK(db(gain(0.0, 1.0)) == Approx(db(std::abs(h2))).margin(0.3));
    // Parallel, split evenly: half of each, added with their phases.
    CHECK(db(gain(1.0, 0.5)) == Approx(db(std::abs(0.5 * h1 + 0.5 * h2))).margin(0.3));
    CHECK(db(gain(1.0, 0.0)) == Approx(db(std::abs(h1))).margin(0.3));
    // Halfway: filter 1 feeds the output and filter 2 in equal parts.
    CHECK(db(gain(0.5, 0.0)) == Approx(db(std::abs(h1 * (0.5 + 0.5 * h2)))).margin(0.3));
    // The direct bus passes both by.
    Patch direct = p;
    direct.set("sub.direct", 1);
    CHECK(db(levelsOf(direct, 69).left / dry) == Approx(0.0).margin(0.05));

    // Mix blends dry and filtered; Level trims the filtered signal alone.
    Patch mixed = tonePatch();
    mixed.set("filter1.on", 1);
    mixed.set("filter1.type", one.type);
    mixed.set("filter1.cutoff", 220);
    mixed.set("filter1.mix", 0.5);
    CHECK(db(levelsOf(mixed, 69).left / dry) == Approx(db(std::abs(0.5 + 0.5 * h1))).margin(0.3));
    mixed.set("filter1.mix_mode", choiceOf("filter1.mix_mode", "level"));
    CHECK(db(levelsOf(mixed, 69).left / dry) == Approx(db(std::abs(0.5 * h1))).margin(0.3));
    mixed.set("filter1.mix", 0.0);
    CHECK(levelsOf(mixed, 69).left == 0.0);

    // Switching a filter on while a note sounds, and changing its type, never leaves the range.
    EngineRig rig(tonePatch());
    rig.run(4800, {MidiEvent::noteOn(0, 69, 1.0f)});
    rig.set("filter1.on", 1);
    rig.run(4800);
    for (const char* type : {"ladder_lp24", "comb_pos_lp", "phase_neg_bp", "formant_a", "diffuser", "svf_lp12"}) {
        rig.set("filter1.type", choiceOf("filter1.type", type));
        rig.run(4800);
    }
    rig.set("filter1.on", 0);
    rig.run(4800);
    bool finite = true;
    for (const float s : rig.left)
        finite = finite && std::isfinite(s) && std::abs(s) < 8.0f;
    CHECK(finite);
    CHECK(zedtest::rmsOf(rig.left, rig.left.size() - 2400, rig.left.size()) == Approx(dry).epsilon(0.01)); // off again: unchanged
}

TEST_CASE("voice filter: every type stays finite with audio-rate cutoff modulation at full resonance", "[voice][filter][stability]") {
    // The whole voice, as a player could set it up: a bright source, both slots on the same type
    // at full resonance and drive, filter 1's cutoff shaken by an oscillator and filter 2's by
    // an audio-rate LFO, while an envelope sweeps both across the range.
    const auto& types = Registry::get().find("filter1.type")->choices;
    double worst = 0.0;
    std::string worstType;
    for (std::size_t type = 0; type < types.size(); ++type) {
        Patch p;
        p.set("osc1.level", 0);
        p.set("osc1.unison.voices", 3);
        p.set("osc2.on", 1);
        p.set("osc2.level", -12);
        p.set("osc2.coarse", 19.02);
        p.set("lfo1.rate", 1700);
        p.set("lfo1.audio_rate", 1);
        p.set("env2.attack", 400);
        p.set("env2.decay", 400);
        p.set("env2.sustain", 0);
        for (const char* f : {"filter1", "filter2"}) {
            const std::string id = f;
            p.set(id + ".on", 1);
            p.set(id + ".type", static_cast<double>(type));
            p.set(id + ".cutoff", 300);
            p.set(id + ".resonance", 1.0);
            p.set(id + ".drive", 1.0);
        }
        p.set("voice.filter_routing", 0.5);
        addRoute(p, "osc2_audio", "filter1.cutoff", 0.3, Polarity::Bi);
        addRoute(p, "lfo1", "filter2.cutoff", 0.3, Polarity::Bi);
        addRoute(p, "env2", "filter1.cutoff", 0.6);
        addRoute(p, "env2", "filter2.cutoff", 0.6);
        EngineRig rig(p);
        rig.run(48000, {MidiEvent::noteOn(0, 45, 1.0f), MidiEvent::noteOn(0, 64, 1.0f)});
        bool finite = true;
        float peak = 0.0f;
        for (std::size_t i = 0; i < rig.left.size(); ++i) {
            finite = finite && std::isfinite(rig.left[i]) && std::isfinite(rig.right[i]);
            peak = std::max({peak, std::abs(rig.left[i]), std::abs(rig.right[i])});
        }
        INFO(types[type] << ": peak " << peak);
        REQUIRE(finite);
        CHECK(peak < 200.0f);
        if (peak > worst) {
            worst = peak;
            worstType = types[type];
        }
    }
    WARN("voice stability sweep, " << types.size() << " types at full resonance and drive with audio-rate modulation: largest peak "
                                   << worst << " (" << worstType << ")");
}

TEST_CASE("glide: time, curve, legato-only and scaled", "[voice][glide]") {
    Patch p = tonePatch();
    p.set("global.glide", 100);
    // Where the pitch is `ms` after a second note that follows the first (held or released).
    const auto keyAt = [](const Patch& patch, int first, int second, bool overlap, double ms) {
        EngineRig rig(patch);
        rig.run(16, {MidiEvent::noteOn(0, first, 1.0f)});
        rig.runUntil(200.0);
        if (overlap)
            rig.run(16, {MidiEvent::noteOn(0, second, 1.0f)});
        else
            rig.run(16, {MidiEvent::noteOff(0, first), MidiEvent::noteOn(0, second, 1.0f)});
        rig.runTo(200.0 + ms);
        return 60.0 + 60.0 * static_cast<double>(rig.source("note"));
    };
    // Straight in pitch: halfway there at half the time, arrived at the end.
    CHECK(keyAt(p, 48, 60, false, 25.0) == Approx(51.0).margin(0.05));
    CHECK(keyAt(p, 48, 60, false, 50.0) == Approx(54.0).margin(0.05));
    CHECK(keyAt(p, 48, 60, false, 99.0) == Approx(59.88).margin(0.05));
    CHECK(keyAt(p, 48, 60, false, 101.0) == Approx(60.0).margin(1e-6));
    CHECK(keyAt(p, 72, 60, false, 50.0) == Approx(66.0).margin(0.05)); // downward too

    Patch curved = p;
    curved.set("global.glide_curve", 0.6);
    CHECK(keyAt(curved, 48, 60, false, 50.0) == Approx(48.0 + 12.0 * curveValue(0.5, 0.6)).margin(0.05));
    curved.set("global.glide_curve", -0.6);
    CHECK(keyAt(curved, 48, 60, false, 50.0) == Approx(48.0 + 12.0 * curveValue(0.5, -0.6)).margin(0.05));

    Patch none = p;
    none.set("global.glide", 0);
    CHECK(keyAt(none, 48, 60, false, 1.0) == Approx(60.0).margin(1e-6));

    // Legato-only: a detached note starts on its pitch; an overlapping one slides.
    Patch legato = p;
    legato.set("global.glide_mode", choiceOf("global.glide_mode", "legato"));
    CHECK(keyAt(legato, 48, 60, false, 50.0) == Approx(60.0).margin(1e-6));
    CHECK(keyAt(legato, 48, 60, true, 50.0) == Approx(54.0).margin(0.05));
    CHECK(keyAt(p, 48, 60, true, 50.0) == Approx(54.0).margin(0.05)); // "always" slides either way

    // Scaled: the time is per octave.
    Patch scaled = p;
    scaled.set("global.glide_scaled", 1);
    CHECK(keyAt(scaled, 48, 60, false, 50.0) == Approx(54.0).margin(0.05));
    CHECK(keyAt(scaled, 48, 72, false, 100.0) == Approx(60.0).margin(0.05));
    CHECK(keyAt(scaled, 48, 72, false, 201.0) == Approx(72.0).margin(1e-6));
    CHECK(keyAt(scaled, 48, 54, false, 25.0) == Approx(51.0).margin(0.05));

    // Mono and legato modes slide the one voice from wherever it is, even in the middle of a slide.
    for (const char* mode : {"mono", "legato"}) {
        Patch mono = p;
        mono.set("global.mode", choiceOf("global.mode", mode));
        INFO(mode);
        CHECK(keyAt(mono, 48, 60, true, 50.0) == Approx(54.0).margin(0.05));
        EngineRig rig(mono);
        rig.run(16, {MidiEvent::noteOn(0, 48, 1.0f)});
        rig.runUntil(100.0);
        rig.run(16, {MidiEvent::noteOn(0, 60, 1.0f)});
        rig.runUntil(150.0); // halfway: at 54
        rig.run(16, {MidiEvent::noteOn(0, 66, 1.0f)});
        rig.runTo(200.0);
        CHECK(60.0 + 60.0 * rig.source("note") == Approx(60.0).margin(0.1)); // halfway from 54 to 66
        // Letting the top key go slides back to the key still held.
        rig.runUntil(400.0);
        rig.run(16, {MidiEvent::noteOff(0, 66)});
        rig.runTo(450.0);
        CHECK(60.0 + 60.0 * rig.source("note") == Approx(63.0).margin(0.1));
        CHECK(rig.engine.activeVoices() == 1);
    }

    // What is heard: the pitch passes through the middle and arrives exactly.
    EngineRig rig(p);
    rig.run(9600, {MidiEvent::noteOn(0, 48, 1.0f)});
    rig.run(28800, {MidiEvent::noteOff(0, 48), MidiEvent::noteOn(0, 60, 1.0f)});
    const double middle = zedtest::measureFrequency(rig.left, 9600 + 2000, 9600 + 2800, kRate);
    CHECK(zedtest::cents(middle, noteToHz(54.0)) == Approx(0.0).margin(40.0));
    CHECK(zedtest::cents(zedtest::measureFrequency(rig.left, 9600 + 7200, 9600 + 28800, kRate), noteToHz(60.0)) == Approx(0.0).margin(0.1));
}

TEST_CASE("tuning: scales, the patch's tuning, the lock, master tune and reference", "[voice][tuning]") {
    const auto centsOff = [](EngineRig& rig, int key, double expectedHz) {
        rig.left.clear();
        rig.right.clear();
        rig.run(48000, {MidiEvent::noteOn(0, key, 1.0f)});
        const double hz = zedtest::measureFrequency(rig.left, 9600, 48000, kRate);
        rig.run(2400, {MidiEvent::noteOff(0, key)});
        rig.run(2400);
        return zedtest::cents(hz, expectedHz);
    };
    ScaleDef scale;
    std::string error;
    REQUIRE(parseScl("just\n7\n9/8\n5/4\n4/3\n3/2\n5/3\n15/8\n2/1\n", scale, error));
    const auto just = std::make_shared<Tuning>(tuningFromScale(scale, KeyMap{}, "Just"));

    EngineRig rig(tonePatch());
    CHECK(rig.engine.tuning()->isEqual());
    CHECK(std::abs(centsOff(rig, 64, noteToHz(64.0))) < 0.1);
    rig.engine.setTuning(just);
    REQUIRE(rig.engine.tuning() != nullptr);
    CHECK(rig.engine.tuning()->name == "Just");
    double worst = 0.0;
    for (const int key : {36, 53, 60, 61, 64, 66, 67, 74, 96}) {
        const double off = centsOff(rig, key, noteToHz(just->note[static_cast<std::size_t>(key)]));
        INFO("key " << key << ": " << off << " cents");
        CHECK(std::abs(off) < 0.1);
        worst = std::max(worst, std::abs(off));
    }
    WARN("microtuned pitch (just scale), worst error " << worst << " cents");
    CHECK(std::abs(centsOff(rig, 64, 261.6255653 * 1.5)) < 0.1); // the fourth key above middle C is a pure fifth

    // Master tune and the reference pitch sit on top of the tuning.
    rig.set("global.master_tune", 50);
    rig.set("global.reference", 432);
    rig.runBlocks(9600);
    CHECK(std::abs(centsOff(rig, 64, 261.6255653 * 1.5 * (432.0 / 440.0) * std::exp2(50.0 / 1200.0))) < 0.1);
    rig.set("global.master_tune", 0);
    rig.set("global.reference", 440);
    rig.runBlocks(9600);

    // A patch carries its tuning; loading a patch without one returns to equal temperament.
    Patch tuned = tonePatch();
    tuned.tuning.name = "Stretched";
    for (int key = 0; key < 128; ++key)
        tuned.tuning.keys.push_back(60.0 + (key - 60) * 1.01);
    rig.engine.loadPatch(tuned);
    CHECK(rig.engine.tuning()->name == "Stretched");
    CHECK(std::abs(centsOff(rig, 84, noteToHz(60.0 + 24.0 * 1.01))) < 0.1);
    rig.engine.loadPatch(tonePatch());
    CHECK(rig.engine.tuning()->isEqual());
    CHECK(std::abs(centsOff(rig, 84, noteToHz(84.0))) < 0.1);

    // Locked: preset changes leave the tuning alone.
    rig.engine.setTuning(just);
    rig.engine.setTuningLocked(true);
    CHECK(rig.engine.tuningLocked());
    rig.engine.loadPatch(tuned);
    rig.engine.loadPatch(tonePatch());
    CHECK(rig.engine.tuning()->name == "Just");
    CHECK(std::abs(centsOff(rig, 67, noteToHz(just->note[67]))) < 0.1);
    rig.engine.setTuningLocked(false);
    rig.engine.loadPatch(tonePatch());
    CHECK(rig.engine.tuning()->isEqual());
    rig.engine.setTuning(nullptr);
    CHECK(rig.engine.tuning()->isEqual());

    // The tuning can change under a held note, and a glide runs through the tuned pitches.
    EngineRig held(tonePatch());
    held.run(24000, {MidiEvent::noteOn(0, 64, 1.0f)});
    held.engine.setTuning(just);
    held.run(48000);
    CHECK(zedtest::cents(zedtest::measureFrequency(held.left, 4800, 24000, kRate), noteToHz(64.0)) == Approx(0.0).margin(0.1));
    CHECK(zedtest::cents(zedtest::measureFrequency(held.left, 28800, 72000, kRate), noteToHz(just->note[64])) == Approx(0.0).margin(0.1));
    Patch gliding = tonePatch();
    gliding.set("global.glide", 100);
    EngineRig slide(gliding);
    slide.engine.setTuning(just);
    slide.run(9600, {MidiEvent::noteOn(0, 60, 1.0f)});
    slide.run(28800, {MidiEvent::noteOff(0, 60), MidiEvent::noteOn(0, 67, 1.0f)});
    CHECK(zedtest::cents(zedtest::measureFrequency(slide.left, 9600 + 7200, 9600 + 28800, kRate), noteToHz(just->note[67])) == Approx(0.0).margin(0.1));
}

TEST_CASE("character: drift, variance, warmth and noise floor", "[voice][character]") {
    SECTION("drift wanders slowly, differently for every note, within its range") {
        const auto wander = [](double drift, int repeat) {
            Patch p = tonePatch();
            p.set("voice.drift", drift);
            EngineRig rig(p);
            for (int i = 0; i < repeat; ++i) { // earlier notes use up random numbers: this one drifts its own way
                rig.run(480, {MidiEvent::noteOn(0, 40, 1.0f)});
                rig.run(480, {MidiEvent::noteOff(0, 40)});
            }
            rig.runBlocks(2400, 480);
            const std::size_t start = rig.left.size();
            rig.run(48000 * 8, {MidiEvent::noteOn(0, 69, 1.0f)});
            std::vector<double> cents;
            for (std::size_t at = start + 9600; at + 9600 <= rig.left.size(); at += 9600)
                cents.push_back(zedtest::cents(zedtest::measureFrequency(rig.left, at, at + 9600, kRate), 440.0));
            return cents;
        };
        const auto still = wander(0.0, 0), a = wander(1.0, 0), b = wander(1.0, 1), half = wander(0.5, 0);
        double stillSpread = 0.0, low = 1e9, high = -1e9, difference = 0.0, halfHigh = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            stillSpread = std::max(stillSpread, std::abs(still[i]));
            low = std::min(low, a[i]);
            high = std::max(high, a[i]);
            difference = std::max(difference, std::abs(a[i] - b[i]));
            halfHigh = std::max(halfHigh, std::abs(half[i]));
        }
        CHECK(stillSpread < 0.05);
        CHECK(high - low > 3.0);          // it moves
        CHECK(std::max(high, -low) < 26.0); // at most a quarter tone at 100 %
        CHECK(difference > 2.0);          // another note, another path
        CHECK(halfHigh < 13.5);
        WARN("drift at 100 %: pitch between " << low << " and " << high << " cents over 8 s");
    }
    SECTION("variance gives every note its own level, pan, cutoff and envelope speed") {
        Patch p = tonePatch();
        p.set("filter1.on", 1);
        p.set("filter1.cutoff", 1000);
        p.set("env1.release", 100);
        p.set("env1.release_curve", 0);
        const auto notes = [](const Patch& patch) {
            struct Note {
                double level, balance, cutoff, release;
            };
            std::vector<Note> out;
            EngineRig rig(patch);
            for (int n = 0; n < 16; ++n) {
                const std::size_t start = rig.left.size();
                rig.run(4800, {MidiEvent::noteOn(0, 57, 1.0f)});
                Note note;
                const double l = zedtest::rmsOf(rig.left, start + 2400, start + 4800), r = zedtest::rmsOf(rig.right, start + 2400, start + 4800);
                note.level = db(std::sqrt(0.5 * (l * l + r * r)));
                note.balance = l / r;
                note.cutoff = rig.engine.filterView(0).settings.cutoffHz[0];
                rig.run(16, {MidiEvent::noteOff(0, 57)});
                const double off = (rig.left.size() - 16) * 1000.0 / kRate;
                note.release = -1.0;
                while (rig.engine.activeVoices() > 0 && rig.left.size() < start + 48000) {
                    rig.run(16);
                    if (note.release < 0.0 && rig.source("env1") <= 0.5f)
                        note.release = rig.left.size() * 1000.0 / kRate - off;
                }
                out.push_back(note);
                rig.runBlocks(480);
            }
            return out;
        };
        const auto spread = [](const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end()); };
        const auto column = [](const auto& list, auto member) {
            std::vector<double> v;
            for (const auto& item : list)
                v.push_back(item.*member);
            return v;
        };
        const auto plain = notes(p);
        using NoteT = std::decay_t<decltype(plain[0])>;
        CHECK(spread(column(plain, &NoteT::level)) < 0.01);
        CHECK(spread(column(plain, &NoteT::balance)) < 0.001);
        CHECK(spread(column(plain, &NoteT::cutoff)) < 0.01);
        CHECK(spread(column(plain, &NoteT::release)) < 0.7);
        CHECK(plain[0].release == Approx(50.0).margin(1.0)); // halfway down a straight 100 ms release

        Patch varied = p;
        varied.set("voice.variance.level", 1.0);
        varied.set("voice.variance.pan", 1.0);
        varied.set("voice.variance.cutoff", 1.0);
        varied.set("voice.variance.env", 1.0);
        const auto wild = notes(varied);
        const auto levels = column(wild, &NoteT::level), cutoffs = column(wild, &NoteT::cutoff), releases = column(wild, &NoteT::release);
        const auto balances = column(wild, &NoteT::balance);
        CHECK(spread(levels) > 3.0);
        CHECK(spread(releases) > 20.0);
        CHECK(*std::max_element(balances.begin(), balances.end()) > 1.5);
        CHECK(*std::min_element(balances.begin(), balances.end()) < 0.67);
        for (std::size_t i = 0; i < wild.size(); ++i) {
            CHECK(cutoffs[i] >= 499.9);  // an octave either way at 100 %
            CHECK(cutoffs[i] <= 2000.1);
            CHECK(releases[i] >= 24.0);  // half to twice the time
            CHECK(releases[i] <= 101.0);
        }
        CHECK(*std::max_element(cutoffs.begin(), cutoffs.end()) / *std::min_element(cutoffs.begin(), cutoffs.end()) > 1.5);
        // Half the amount, half the spread (the same random numbers are drawn).
        varied.set("voice.variance.level", 0.5);
        CHECK(spread(column(notes(varied), &NoteT::level)) == Approx(spread(levels) * 0.5).epsilon(0.25));
    }
    SECTION("warmth adds a little third harmonic and tilts the top down") {
        Patch p = tonePatch(-6.0);
        const auto harmonic = [](const Patch& patch, int note, int number) {
            EngineRig rig(patch);
            rig.run(48000, {MidiEvent::noteOn(0, note, 1.0f)});
            const auto spectrum = zedtest::spectrumOf(rig.left, 9600, 32768, kRate);
            return zedtest::lineLevel(spectrum, noteToHz(note) * number);
        };
        const double clean1 = harmonic(p, 57, 1), clean3 = harmonic(p, 57, 3);
        CHECK(db(clean3 / clean1) < -90.0);
        Patch warm = p;
        warm.set("voice.warmth", 1.0);
        const double warm1 = harmonic(warm, 57, 1), warm3 = harmonic(warm, 57, 3), warm2 = harmonic(warm, 57, 2);
        CHECK(db(warm3 / warm1) > -40.0); // audible colour
        CHECK(db(warm3 / warm1) < -12.0); // but gentle
        CHECK(db(warm2 / warm1) < -80.0); // odd harmonics only: the curve is symmetric
        CHECK(db(warm1 / clean1) == Approx(0.0).margin(3.0)); // about the same loudness
        // A quiet high tone is hardly bent, but it is turned down a little.
        Patch quiet = tonePatch(-40.0);
        const double high = harmonic(quiet, 117, 1);
        quiet.set("voice.warmth", 1.0);
        const double tilted = harmonic(quiet, 117, 1);
        CHECK(db(tilted / high) < -1.0);
        CHECK(db(tilted / high) > -5.0);
        const double low = harmonic(tonePatch(-40.0), 45, 1);
        Patch quietLow = tonePatch(-40.0);
        quietLow.set("voice.warmth", 1.0);
        CHECK(db(harmonic(quietLow, 45, 1) / low) == Approx(0.0).margin(0.2));
    }
    SECTION("noise floor is a low bed of noise that follows the voice") {
        Patch p = tonePatch(kSilenceDb); // a voice with nothing in it
        EngineRig silent(p);
        silent.run(48000, {MidiEvent::noteOn(0, 60, 1.0f)});
        CHECK(zedtest::peakOf(silent.left, 0, 48000) == 0.0f);
        p.set("voice.noise_floor", 1.0);
        EngineRig rig(p);
        rig.run(48000, {MidiEvent::noteOn(0, 60, 1.0f)});
        const double level = zedtest::rmsOf(rig.left, 4800, 48000);
        CHECK(db(level) == Approx(db(0.01 / std::sqrt(3.0))).margin(0.5)); // -40 dB peak, evenly spread
        CHECK(zedtest::maxDifference(rig.left, rig.right) > 0.005);         // a different noise on each side
        rig.run(16, {MidiEvent::noteOff(0, 60)});
        rig.runBlocks(4800);
        const std::size_t end = rig.left.size();
        rig.run(4800);
        CHECK(zedtest::peakOf(rig.left, end, end + 4800) == 0.0f); // gone with the note
        // Half the amount is a quarter of the level, and it passes through the filter.
        p.set("voice.noise_floor", 0.5);
        EngineRig half(p);
        half.run(48000, {MidiEvent::noteOn(0, 60, 1.0f)});
        CHECK(db(zedtest::rmsOf(half.left, 4800, 48000) / level) == Approx(-12.04).margin(0.5));
        p.set("filter1.on", 1);
        p.set("filter1.cutoff", 200);
        EngineRig filtered(p);
        filtered.run(48000, {MidiEvent::noteOn(0, 60, 1.0f)});
        CHECK(db(zedtest::rmsOf(filtered.left, 4800, 48000) / zedtest::rmsOf(half.left, 4800, 48000)) < -12.0);
    }
}

TEST_CASE("smoothing: no continuous control steps when it crosses its range in 50 ms", "[voice][zipper]") {
    // Pure tones from an oscillator, the Sub and the Texture player through both filters. A step
    // in any control shows as a jump from one sample to the next that the steady sound, at any
    // setting of that control, never makes. Every continuous parameter of the voice is swept from
    // one end of its range to the other in 50 ms and back, in host blocks of 512 samples, and
    // the largest jump is compared with the largest one found at five fixed settings.
    const auto sine = zedtest::sineTableData();
    std::vector<float> wave(48000);
    for (std::size_t i = 0; i < wave.size(); ++i)
        wave[i] = static_cast<float>(std::sin(2.0 * kPi * 330.0 * static_cast<double>(i) / kRate));
    const auto tone = SampleData::fromStereo(wave, wave, kRate, "tone");

    Patch base = zedtest::flatPatch();
    base.set("osc1.on", 1);
    base.set("osc1.level", -12);
    base.set("osc1.unison.voices", 3);
    base.set("osc1.unison.detune", 0); // no beating: at any one setting the sound repeats exactly
    base.set("osc1.unison.phase", choiceOf("osc1.unison.phase", "consistent"));
    base.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", "bend_plus"));
    base.set("osc2.on", 1);
    base.set("osc2.engine", choiceOf("osc2.engine", "classic"));
    base.set("osc2.classic.shape", 1.0); // a sine
    base.set("osc2.level", -12);
    base.set("sub.on", 1);
    base.set("sub.level", -12);
    base.set("texture.on", 1);
    base.set("texture.level", -12);
    base.set("filter1.on", 1);
    base.set("filter1.cutoff", 3000);
    base.set("filter1.resonance", 0.3);
    base.set("filter2.on", 1);
    base.set("filter2.type", choiceOf("filter2.type", "svf_hp12"));
    base.set("filter2.cutoff", 40);
    base.set("voice.filter_routing", 0.3);
    base.set("osc1.sends.filter_balance", 0.2);

    const auto prepare = [&](EngineRig& rig) {
        rig.engine.setTable(0, sine);
        rig.engine.setTexture(tone);
    };
    const auto largestJump = [](const EngineRig& rig, std::size_t from) {
        return std::max(zedtest::largestJump(rig.left, from, rig.left.size()), zedtest::largestJump(rig.right, from, rig.right.size()));
    };

    // The measure does find steps: the same sound with its level stepped once per host block.
    {
        EngineRig rig(base);
        prepare(rig);
        rig.run(24000, {MidiEvent::noteOn(0, 57, 1.0f)});
        const float steady = largestJump(rig, 4800);
        std::vector<float> stepped(rig.left.begin() + 4800, rig.left.end());
        for (std::size_t i = 0; i < stepped.size(); ++i)
            stepped[i] *= 1.0f - 0.2f * static_cast<float>((i / 512) % 5);
        CHECK(zedtest::largestJump(stepped, 0, stepped.size()) > steady * 1.5f);
    }

    const auto& reg = Registry::get();
    int checked = 0;
    double worstRatio = 0.0;
    std::string worstId;
    const auto check = [&](const Patch& patch, const ParamDef& def) {
        // What the sound does at every setting of the control: a sweep across the range so slow
        // (1.5 s, in 4500 steps) that it is as good as standing still everywhere on the way.
        float bound = 0.0f;
        {
            Patch p = patch;
            p.set(def.index, def.min);
            EngineRig rig(p);
            prepare(rig);
            rig.run(4800, {MidiEvent::noteOn(0, 57, 1.0f)});
            for (int step = 1; step <= 4500; ++step) {
                rig.engine.params().set(def.index, def.fromNormalized(step / 4500.0));
                rig.run(16);
            }
            rig.run(4800);
            bound = largestJump(rig, 2400);
        }
        // The fast sweep: up in 50 ms, hold, down in 50 ms, hold.
        Patch p = patch;
        p.set(def.index, def.min);
        EngineRig rig(p);
        prepare(rig);
        rig.run(4608, {MidiEvent::noteOn(0, 57, 1.0f)});
        const std::size_t from = rig.left.size() - 512;
        for (int pass = 0; pass < 2; ++pass) {
            for (int step = 1; step <= 5; ++step) {
                const double position = pass == 0 ? step / 5.0 : 1.0 - step / 5.0;
                rig.engine.params().set(def.index, def.fromNormalized(position));
                rig.run(512);
            }
            for (int hold = 0; hold < 8; ++hold)
                rig.run(512);
        }
        const float swept = largestJump(rig, from);
        INFO(def.id << ": largest jump " << swept << " in the fast sweep, " << bound << " in the slow one");
        CHECK(swept <= bound * 1.5f + 2.0e-4f);
        const double ratio = swept / std::max(bound, 1.0e-4f);
        if (ratio > worstRatio) {
            worstRatio = ratio;
            worstId = def.id;
        }
        ++checked;
    };

    // Every continuous parameter of the modules a voice is made of.
    for (const ParamDef& def : reg.params()) {
        const bool voiceModule = def.module == "global" || def.module == "voice" || def.module == "osc1" || def.module == "osc2" ||
                                 def.module == "sub" || def.module == "texture" || def.module == "filter1" ||
                                 def.module == "filter2" || def.module == "env1";
        if (def.type == ParamType::Float && voiceModule)
            check(base, def);
    }
    // Macros, route amounts and modulator controls act through routes.
    Patch routed = base;
    routed.set("macro1.value", 0.5);
    routed.set("lfo1.rate", 50); // fast enough that the slow sweep meets every LFO phase at every setting
    routed.set("env2.sustain", 0.5);
    routed.set("env2.decay", 1); // at its sustain level before the sweeps begin: the sound itself must not be changing
    addRoute(routed, "macro1", "filter1.cutoff", 0.5, Polarity::Bi);
    addRoute(routed, "lfo1", "osc1.pan", 0.5, Polarity::Bi);
    // (Kept shallow: one partial much louder than the rest makes the filter ring audibly when the
    // cutoff is thrown across it, which is the filter answering, not a control stepping.)
    addRoute(routed, "env2", "osc2.level", 0.1);
    addRoute(routed, "macro2", "mod.2.amount", 0.5);
    for (const char* id : {"macro1.value", "macro2.value", "mod.1.amount", "mod.2.amount", "mod.3.amount", "lfo1.rate", "lfo1.phase",
                           "lfo1.smooth", "env2.sustain"})
        check(routed, *reg.find(id));

    CHECK(checked >= 90);
    WARN("zipper check: " << checked << " continuous controls swept over their range in 50 ms; largest jump relative to the steady sound "
                          << worstRatio << " (" << worstId << "), limit 1.5");
}
