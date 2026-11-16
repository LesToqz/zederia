// Benchmark scenarios (plan 5.8). Each one runs the engine on one core at 48 kHz with 512-sample
// blocks and reports the realtime factor: processing time as a percentage of the audio's length.
// Preset loading reports milliseconds instead.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zed {

struct BenchScenario {
    std::string name;
    std::string description;
    double budgetPercent = 0.0; // the most the scenario may cost (plan 5.8), in `unit`
    std::string unit = "%";     // "%" realtime factor, or "ms" per load
    double maxRatio = 0.0;      // fx_tail: late tail against early tail at most this (0 = not checked)
};

struct BenchResult {
    double percent = 0.0;      // best run: processing time / audio time * 100 (or milliseconds)
    double audioSeconds = 0.0; // length of audio processed per run
    int runs = 0;
    double ratio = 0.0;        // fx_tail: cost late in a decaying tail against early in it
    std::vector<std::pair<std::string, double>> profile; // section, share of the processing time (percent)
};

std::span<const BenchScenario> benchScenarios();

// Runs `runs` passes of `audioSeconds` each and keeps the fastest. With `profile`, the engine
// times its sections in the last run. Returns false for an unknown name.
bool runBench(std::string_view name, double audioSeconds, int runs, BenchResult& out, bool profile = false);

} // namespace zed
