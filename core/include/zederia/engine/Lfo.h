// LFOs and Flux generators (plan 3.7). Both are small state machines the engine steps from one
// control tick to the next (or one sample at a time in audio-rate mode); the values they take are
// already modulated and converted by the engine.
#pragma once

#include "zederia/patch/Shape.h"
#include "zederia/util/Math.h"

#include <cstdint>

namespace zed {

inline constexpr int kLfoTableSize = 1024;

// A shape as the LFO reads it: kLfoTableSize + 1 values from -1 to 1 across one cycle.
struct LfoTable {
    float values[kLfoTableSize + 1];
    float loopStart = 0.0f;
};
// From a drawn shape; an empty shape gives one cycle of a sine.
void buildLfoTable(const Shape& shape, LfoTable& out) noexcept;

enum class LfoMode : std::uint8_t { Free, Retrigger, OneShot, LoopPoint };

struct LfoParams {
    LfoMode mode = LfoMode::Retrigger;
    double cyclesPerSample = 0.0;
    double phaseOffset = 0.0; // 0..1
    double delaySamples = 0.0, riseSamples = 0.0;
    double smooth = 0.0;      // 0..1
    bool unipolar = false;
};

struct LfoState {
    double phase = 0.0; // cycles since the start (wrapped for the cyclic modes)
    double age = 0.0;   // samples since the trigger
    double smoothed = 0.0;
    bool smoothedValid = false;
};

// Restarts at a note. Free-running LFOs pass the phase they should carry on from.
void lfoTrigger(LfoState& state, double startPhase = 0.0) noexcept;
// Moves `samples` forward and returns the output there: -1..1, or 0..1 when unipolar.
float lfoAdvance(LfoState& state, const LfoParams& params, const LfoTable& table, double samples) noexcept;
// Audio-rate mode: one output value per sample.
void lfoRender(LfoState& state, const LfoParams& params, const LfoTable& table, float* out, int samples) noexcept;
// The shape's value at a phase (cyclic).
float lfoTableValue(const LfoTable& table, double phase) noexcept;

// --- Flux: random and chaos generators ------------------------------------------------------------

enum class FluxType : std::uint8_t { Orbit, Burst, Stepped, Smooth };

struct FluxState {
    Rng rng{1};
    double phase = 0.0;
    double previous = 0.0, next = 0.0;                 // stepped, smooth
    double orbitPhase[3] = {0.0, 0.0, 0.0};            // orbit
    double orbitRate[3] = {1.0, 1.0, 1.0}, orbitTarget[3] = {1.0, 1.0, 1.0};
    double burstLevel = 0.0, burstPeak = 0.0, burstWait = 0.0; // burst
    double value = 0.0;
};

void fluxReset(FluxState& state, std::uint64_t seed) noexcept;
// Moves forward by `cycles` (rate times time) and returns the output there, -1..1.
float fluxAdvance(FluxState& state, FluxType type, double cycles) noexcept;

} // namespace zed
