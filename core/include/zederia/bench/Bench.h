// Benchmark scenarios (plan 5.8). Each one runs the engine on one core at 48 kHz with 512-sample
// blocks and reports the realtime factor: processing time as a percentage of the audio's length.
#pragma once

#include <span>
#include <string>
#include <string_view>

namespace zed {

struct BenchScenario {
    std::string name;
    std::string description;
    double budgetPercent = 0.0; // the most the scenario may cost (plan 5.8)
};

struct BenchResult {
    double percent = 0.0;      // best run: processing time / audio time * 100
    double audioSeconds = 0.0; // length of audio processed per run
    int runs = 0;
};

std::span<const BenchScenario> benchScenarios();

// Runs `runs` passes of `audioSeconds` each and keeps the fastest. Returns false for an unknown name.
bool runBench(std::string_view name, double audioSeconds, int runs, BenchResult& out);

} // namespace zed
