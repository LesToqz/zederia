#include "zederia/bench/Bench.h"

#include "zederia/engine/Engine.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/table/TableData.h"
#include "zederia/table/Wavetable.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>

namespace zed {
namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;

struct RouteSetup {
    const char* source;
    const char* dest;
    double amount;
};

struct FxSetup {
    int lane;
    const char* id;
    std::vector<std::pair<const char*, double>> values; // the effect's own parameters, by key
    bool on = true;
};

enum class Kind {
    Hold,       // the notes are held for the whole run
    AfterTails, // the notes play and stop; the run is timed once every tail has died away
    Tail,       // a long reverb tail is timed early and late in its decay
    PresetLoad, // milliseconds to read, check and apply a patch with a wavetable
};

struct Setup {
    BenchScenario info;
    std::vector<int> notes;
    std::vector<std::pair<const char*, double>> values; // parameters that differ from the default patch
    std::vector<RouteSetup> routes;
    std::vector<FxSetup> fx;
    Kind kind = Kind::Hold;
};

const std::vector<int> kChord = {36, 43, 48, 52, 55, 60, 64, 67};

// Three oscillators at unison 16 and both filters: the heaviest voice the plan budgets for.
std::vector<std::pair<const char*, double>> fullVoice(double quality) {
    return {{"osc1.unison.voices", 16}, {"osc2.on", 1},          {"osc2.unison.voices", 16}, {"osc2.coarse", 7},
            {"osc3.on", 1},             {"osc3.unison.voices", 16}, {"osc3.coarse", -12},    {"filter1.on", 1},
            {"filter2.on", 1},          {"filter2.type", 5},        {"filter2.cutoff", 120}, {"global.quality", quality}};
}

// The plan's six effects: a typical chain in lane A and a master bus.
std::vector<FxSetup> sixEffects() {
    return {{0, "drive", {{"amount", 0.3}}},
            {0, "chorus", {}},
            {0, "delay", {{"feedback", 0.4}}},
            {0, "reverb", {{"decay", 2500.0}}},
            {2, "eq", {{"mid1_gain", 3.0}, {"high_gain", -2.0}}},
            {2, "compressor", {{"threshold", -20.0}}}};
}

std::vector<FxSetup> fullRack(bool on = true);

// The same rack with every slot switched off: it must cost nothing.
std::vector<FxSetup> rackOff() { return fullRack(false); }

// Every slot of every lane holds an effect: the idle budget must hold even then.
std::vector<FxSetup> fullRack(bool on) {
    const char* lanes[3][8] = {{"drive", "compressor", "eq", "filter", "chorus", "flanger", "phaser", "delay"},
                               {"reverb", "swarm", "widener", "utility", "limiter", "reverb", "delay", "chorus"},
                               {"eq", "compressor", "limiter", "utility", "widener", "filter", "phaser", "drive"}};
    std::vector<FxSetup> out;
    for (int lane = 0; lane < 3; ++lane)
        for (const char* id : lanes[lane])
            out.push_back({lane, id, {}, on});
    return out;
}

const std::vector<Setup>& setups() {
    static const std::vector<Setup> all = {
        {{"idle", "Idle instance: no voices, tails finished", 0.2}, {}, {}, {}, {}},
        {{"idle_rack", "Idle instance with all 24 rack slots filled, after its tails have died away", 0.2},
         kChord, {}, {}, fullRack(), Kind::AfterTails},
        {{"one_voice", "One voice, one wavetable oscillator", 0.5}, {48}, {}, {}, {}},
        {{"one_voice_rack_off", "One voice with all 24 rack slots filled and switched off: as one_voice", 0.5}, {48}, {}, {}, rackOff()},
        {{"chord8", "8-voice chord, one wavetable oscillator at unison 7, one filter", 4.0},
         kChord,
         {{"osc1.unison.voices", 7}, {"filter1.on", 1}},
         {},
         {}},
        {{"chord8_mod", "The same chord with a per-voice LFO, an envelope and velocity on the cutoff and a macro on position", 4.0},
         kChord,
         {{"osc1.unison.voices", 7}, {"filter1.on", 1}, {"macro1.value", 0.5}, {"env2.decay", 2000}},
         {{"lfo1", "filter1.cutoff", 0.1}, {"env2", "filter1.cutoff", 0.2}, {"velocity", "filter1.cutoff", 0.1}, {"macro1", "osc1.position", 0.5}},
         {}},
        {{"full8", "8 voices x 3 oscillators x unison 16, two filters, six effects", 35.0}, kChord, fullVoice(1), {}, sixEffects()},
        {{"full8_draft", "The same in Draft quality", 20.0}, kChord, fullVoice(0), {}, sixEffects()},
        {{"fx_tail", "A 30-second reverb tail after a chord, timed early and 20 s later: the late part may not cost more (no denormal slowdown); one reverb at most 2 %", 2.0, "%", 1.5},
         kChord, {}, {}, {{2, "reverb", {{"decay", 30000.0}}}}, Kind::Tail},
        {{"preset_load", "Read, check and apply a patch with six effects, routes and a 64-frame wavetable", 100.0, "ms"},
         {}, fullVoice(1), {{"lfo1", "filter1.cutoff", 0.2}, {"macro1", "fx.a1.p2", 0.3}}, sixEffects(), Kind::PresetLoad},
    };
    return all;
}

Patch patchFor(const Setup& s) {
    const auto& reg = Registry::get();
    Patch patch;
    for (const auto& [id, value] : s.values)
        patch.set(id, value);
    for (const FxSetup& fx : s.fx) {
        const int type = reg.effectIndex(fx.id);
        const int slot = patch.addFxSlot(fx.lane, type);
        if (slot < 0)
            continue;
        patch.set(reg.fxSlotOn(fx.lane, slot), fx.on ? 1.0 : 0.0);
        const auto& params = reg.effectTypes()[static_cast<std::size_t>(type)].params;
        for (const auto& [key, value] : fx.values)
            for (const auto& p : params)
                if (p.id == std::string(fx.id) + "." + key)
                    patch.set(reg.fxSlotParam(fx.lane, slot, p.index), value);
    }
    for (const RouteSetup& r : s.routes) {
        ModRoute route;
        route.source = r.source;
        route.dest = r.dest;
        patch.routes.push_back(route);
        patch.set(reg.modAmount(static_cast<int>(patch.routes.size()) - 1), r.amount);
    }
    return patch;
}

double seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// A wavetable that no earlier run has built (builds are cached by content).
Wavetable freshTable(std::uint64_t seed) {
    Wavetable table;
    Rng rng(seed);
    table.frames.resize(64);
    for (std::size_t f = 0; f < table.frames.size(); ++f) {
        const double brightness = 1.0 + 30.0 * rng.uniform();
        for (int n = 0; n < kFrameSize; ++n) {
            double x = 0.0;
            for (int h = 1; h <= 24; ++h)
                x += std::sin(2.0 * kPi * h * n / kFrameSize) / std::pow(h, 1.0 + 2.0 / brightness);
            table.frames[f][static_cast<std::size_t>(n)] = static_cast<float>(0.4 * x);
        }
    }
    return table;
}

} // namespace

std::span<const BenchScenario> benchScenarios() {
    static const std::vector<BenchScenario> list = [] {
        std::vector<BenchScenario> v;
        for (const auto& s : setups())
            v.push_back(s.info);
        return v;
    }();
    return list;
}

bool runBench(std::string_view name, double audioSeconds, int runs, BenchResult& out, bool profile) {
    const auto& all = setups();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Setup& s) { return s.info.name == name; });
    if (it == all.end())
        return false;
    const Setup& setup = *it;
    const Patch patch = patchFor(setup);
    out = BenchResult{};
    out.runs = std::max(runs, 1);

    if (setup.kind == Kind::PresetLoad) {
        const std::string text = savePatch(patch);
        double best = 1e300;
        std::array<float, kBlock> left{}, right{};
        double steps[4] = {};
        for (int run = 0; run < out.runs; ++run) {
            Engine engine;
            engine.prepare(kSampleRate);
            engine.process({left.data(), right.data(), kBlock, {}, {}});
            const Wavetable table = freshTable(1000 + static_cast<std::uint64_t>(run));
            const auto start = std::chrono::steady_clock::now();
            const LoadResult loaded = loadPatch(text);
            if (!loaded.ok())
                return false;
            const double read = seconds(start);
            engine.loadPatch(loaded.patch);
            const double applied = seconds(start);
            engine.setTable(0, TableData::build(table));
            const double built = seconds(start);
            engine.process({left.data(), right.data(), kBlock, {}, {}}); // the audio thread takes it all
            const double total = seconds(start);
            if (total < best) {
                best = total;
                steps[0] = read;
                steps[1] = applied - read;
                steps[2] = built - applied;
                steps[3] = total - built;
            }
        }
        out.percent = best * 1000.0;
        const char* names[] = {"read and check the JSON (ms)", "hand it to the engine, effects made (ms)",
                               "build the 64-frame wavetable (ms)", "first block: everything taken over (ms)"};
        for (int i = 0; i < 4; ++i)
            out.profile.emplace_back(names[i], steps[i] * 1000.0);
        return true;
    }

    const int blocks = std::max(1, static_cast<int>(audioSeconds * kSampleRate / kBlock));
    std::array<float, kBlock> left{}, right{};
    std::vector<MidiEvent> noteOns, noteOffs;
    for (const int note : setup.notes) {
        noteOns.push_back(MidiEvent::noteOn(0, note, 0.8f));
        noteOffs.push_back(MidiEvent::noteOff(0, note));
    }
    const auto timeBlocks = [&](Engine& engine, int count) {
        const auto start = std::chrono::steady_clock::now();
        for (int b = 0; b < count; ++b)
            engine.process({left.data(), right.data(), kBlock, {}, {}});
        return seconds(start);
    };

    double best = 1e300, bestRatio = 1e300;
    for (int run = 0; run < out.runs; ++run) {
        Engine engine;
        engine.prepare(kSampleRate);
        engine.loadPatch(patch);
        // The first block starts the notes and settles the parameters; it is not timed.
        engine.process({left.data(), right.data(), kBlock, noteOns, {}});
        const bool last = run == out.runs - 1;
        if (setup.kind == Kind::AfterTails || setup.kind == Kind::Tail) {
            for (int b = 0; b < static_cast<int>(0.5 * kSampleRate / kBlock); ++b)
                engine.process({left.data(), right.data(), kBlock, {}, {}});
            engine.process({left.data(), right.data(), kBlock, noteOffs, {}});
        }
        if (setup.kind == Kind::AfterTails) { // wait, untimed, until nothing runs any more
            for (int b = 0; b < static_cast<int>(30.0 * kSampleRate / kBlock) && (engine.fxSlotsRunning() > 0 || engine.activeVoices() > 0); ++b)
                engine.process({left.data(), right.data(), kBlock, {}, {}});
        }
        if (profile && last)
            engine.setProfiling(true);
        if (setup.kind == Kind::Tail) {
            // Early in the tail, then twenty seconds later when the levels are a hundred times lower
            // (values that would be denormal without flush-to-zero appear there first).
            const int window = std::max(1, static_cast<int>(std::min(audioSeconds, 2.0) * kSampleRate / kBlock));
            const double early = timeBlocks(engine, window);
            for (int b = 0; b < static_cast<int>(20.0 * kSampleRate / kBlock); ++b)
                engine.process({left.data(), right.data(), kBlock, {}, {}});
            const double late = timeBlocks(engine, window);
            const double audio = window * kBlock / kSampleRate;
            best = std::min(best, late / audio * 100.0);
            bestRatio = std::min(bestRatio, late / std::max(early, 1e-9));
            out.audioSeconds = audio;
        } else {
            const double elapsed = timeBlocks(engine, blocks);
            best = std::min(best, elapsed);
            out.audioSeconds = blocks * kBlock / kSampleRate;
        }
        if (profile && last) {
            double total = 0.0;
            for (const auto& entry : engine.profile())
                total += entry.seconds;
            for (const auto& entry : engine.profile())
                if (entry.seconds > 0.0)
                    out.profile.emplace_back(entry.section, entry.seconds / out.audioSeconds * 100.0);
            out.profile.emplace_back("(all timed sections)", total / out.audioSeconds * 100.0);
        }
    }
    if (setup.kind == Kind::Tail) {
        out.percent = best;
        out.ratio = bestRatio;
    } else {
        out.percent = best / out.audioSeconds * 100.0;
    }
    return true;
}

} // namespace zed
