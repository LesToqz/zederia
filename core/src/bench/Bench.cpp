#include "zederia/bench/Bench.h"

#include "zederia/engine/Engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

namespace zed {
namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;

struct Setup {
    BenchScenario info;
    std::vector<int> notes; // held for the whole run
    std::vector<std::pair<const char*, double>> values; // parameters that differ from the default patch
};

const std::vector<Setup>& setups() {
    static const std::vector<Setup> all = {
        {{"idle", "Idle instance: no voices, tails finished", 0.2}, {}, {}},
        {{"one_voice", "One voice, one wavetable oscillator", 0.5}, {48}, {}},
        {{"chord8", "8-voice chord, one wavetable oscillator at unison 7 (the filter joins in milestone 3)", 4.0},
         {36, 43, 48, 52, 55, 60, 64, 67},
         {{"osc1.unison.voices", 7}}},
    };
    return all;
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

bool runBench(std::string_view name, double audioSeconds, int runs, BenchResult& out) {
    const auto& all = setups();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Setup& s) { return s.info.name == name; });
    if (it == all.end())
        return false;

    const int blocks = std::max(1, static_cast<int>(audioSeconds * kSampleRate / kBlock));
    std::array<float, kBlock> left{}, right{};
    std::vector<MidiEvent> noteOns;
    for (const int note : it->notes)
        noteOns.push_back(MidiEvent::noteOn(0, note, 0.8f));

    double best = 1e300;
    for (int run = 0; run < std::max(runs, 1); ++run) {
        Engine engine;
        engine.prepare(kSampleRate);
        Patch patch;
        for (const auto& [id, value] : it->values)
            patch.set(id, value);
        engine.loadPatch(patch);
        // The first block starts the notes and settles the parameters; it is not timed.
        engine.process({left.data(), right.data(), kBlock, noteOns, {}});
        const auto start = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
            engine.process({left.data(), right.data(), kBlock, {}, {}});
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        best = std::min(best, elapsed.count());
    }
    out.audioSeconds = blocks * kBlock / kSampleRate;
    out.percent = best / out.audioSeconds * 100.0;
    out.runs = std::max(runs, 1);
    return true;
}

} // namespace zed
