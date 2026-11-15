#include "zederia/engine/Lfo.h"

#include <algorithm>
#include <cmath>

namespace zed {

void buildLfoTable(const Shape& shape, LfoTable& out) noexcept {
    if (shape.empty()) {
        for (int i = 0; i <= kLfoTableSize; ++i)
            out.values[i] = static_cast<float>(std::sin(2.0 * kPi * i / kLfoTableSize));
        out.values[kLfoTableSize] = 0.0f;
        out.loopStart = 0.0f;
        return;
    }
    compileShape(shape, out.values, kLfoTableSize);
    for (float& v : out.values)
        v = std::clamp(v, -1.0f, 1.0f);
    out.loopStart = static_cast<float>(std::clamp(shape.loopStart, 0.0, 0.999));
}

float lfoTableValue(const LfoTable& table, double phase) noexcept {
    phase -= std::floor(phase);
    const double position = phase * kLfoTableSize;
    const int i = std::min(static_cast<int>(position), kLfoTableSize - 1);
    const auto t = static_cast<float>(position - i);
    return table.values[i] + t * (table.values[i + 1] - table.values[i]);
}

void lfoTrigger(LfoState& state, double startPhase) noexcept {
    state.phase = startPhase;
    state.age = 0.0;
    state.smoothed = 0.0;
    state.smoothedValid = false;
}

namespace {

// One step of `samples`; `smoothing` is the share of the way the smoothed value moves.
float lfoStep(LfoState& state, const LfoParams& p, const LfoTable& table, double samples, double smoothing) noexcept {
    const double before = state.age;
    state.age += samples;
    const double running = std::max(state.age - p.delaySamples, 0.0) - std::max(before - p.delaySamples, 0.0);
    state.phase += running * p.cyclesPerSample;
    double raw = 0.0;
    switch (p.mode) {
    case LfoMode::OneShot: {
        // Plays through once from the start phase and holds the last value.
        const double at = std::min(state.phase + p.phaseOffset, 1.0);
        state.phase = std::min(state.phase, 1.0);
        raw = at >= 1.0 ? table.values[kLfoTableSize] : lfoTableValue(table, at);
        break;
    }
    case LfoMode::LoopPoint: {
        const double start = table.loopStart, length = 1.0 - start;
        if (state.phase >= 1.0)
            state.phase = start + std::fmod(state.phase - 1.0, length);
        raw = lfoTableValue(table, std::min(state.phase + (state.phase < start ? p.phaseOffset : 0.0), 0.999999));
        break;
    }
    default:
        state.phase -= std::floor(state.phase);
        raw = lfoTableValue(table, state.phase + p.phaseOffset);
        break;
    }
    if (state.age < p.delaySamples)
        raw = 0.0;
    if (smoothing < 1.0) {
        if (!state.smoothedValid) {
            state.smoothed = raw;
            state.smoothedValid = true;
        }
        state.smoothed += (raw - state.smoothed) * smoothing;
        raw = state.smoothed;
    }
    const double rise = p.riseSamples > 0.0 ? std::clamp((state.age - p.delaySamples) / p.riseSamples, 0.0, 1.0) : 1.0;
    return static_cast<float>(p.unipolar ? rise * (raw + 1.0) * 0.5 : rise * raw);
}

// Smooth = 100 % rounds the shape over about a quarter of its cycle.
double smoothingFor(const LfoParams& p, double samples) noexcept {
    if (p.smooth <= 0.0 || p.cyclesPerSample <= 0.0)
        return 1.0;
    const double tau = p.smooth * p.smooth * 0.25 / p.cyclesPerSample;
    return tau < 1.0e-9 ? 1.0 : 1.0 - std::exp(-samples / tau);
}

} // namespace

float lfoAdvance(LfoState& state, const LfoParams& params, const LfoTable& table, double samples) noexcept {
    return lfoStep(state, params, table, samples, smoothingFor(params, samples));
}

void lfoRender(LfoState& state, const LfoParams& params, const LfoTable& table, float* out, int samples) noexcept {
    const double smoothing = smoothingFor(params, 1.0);
    for (int i = 0; i < samples; ++i)
        out[i] = lfoStep(state, params, table, 1.0, smoothing);
}

// --- Flux ----------------------------------------------------------------------------------------

void fluxReset(FluxState& state, std::uint64_t seed) noexcept {
    state = FluxState{};
    state.rng.seed(seed);
    state.previous = state.rng.uniform() * 2.0 - 1.0;
    state.next = state.rng.uniform() * 2.0 - 1.0;
    for (int i = 0; i < 3; ++i) {
        state.orbitPhase[i] = state.rng.uniform();
        state.orbitRate[i] = state.orbitTarget[i] = 0.85 + 0.3 * state.rng.uniform();
    }
    state.burstWait = state.rng.uniform();
    state.value = state.previous;
}

float fluxAdvance(FluxState& s, FluxType type, double cycles) noexcept {
    cycles = std::clamp(cycles, 0.0, 64.0);
    switch (type) {
    case FluxType::Stepped:
    case FluxType::Smooth: {
        s.phase += cycles;
        while (s.phase >= 1.0) {
            s.phase -= 1.0;
            s.previous = s.next;
            s.next = s.rng.uniform() * 2.0 - 1.0;
        }
        if (type == FluxType::Stepped) {
            s.value = s.previous;
        } else {
            const double t = s.phase * s.phase * (3.0 - 2.0 * s.phase);
            s.value = s.previous + (s.next - s.previous) * t;
        }
        break;
    }
    case FluxType::Orbit: {
        // Three circles at ratios that never line up, each drifting a little in speed: smooth,
        // nearly periodic, never the same twice.
        static const double ratio[3] = {1.0, 1.6180339887, 0.4142135624};
        static const double weight[3] = {0.5, 0.3, 0.2};
        s.phase += cycles;
        while (s.phase >= 1.0) {
            s.phase -= 1.0;
            for (double& target : s.orbitTarget)
                target = 0.8 + 0.4 * s.rng.uniform();
        }
        double sum = 0.0;
        const double glide = 1.0 - std::exp(-cycles * 2.0);
        for (int i = 0; i < 3; ++i) {
            s.orbitRate[i] += (s.orbitTarget[i] - s.orbitRate[i]) * glide;
            s.orbitPhase[i] += cycles * ratio[i] * s.orbitRate[i];
            s.orbitPhase[i] -= std::floor(s.orbitPhase[i]);
            sum += weight[i] * std::sin(2.0 * kPi * s.orbitPhase[i]);
        }
        s.value = sum;
        break;
    }
    case FluxType::Burst: {
        // A level that jumps at irregular moments and dies away until the next one.
        s.burstWait -= cycles;
        s.burstLevel *= std::exp(-cycles * 4.0);
        int guard = 0;
        while (s.burstWait <= 0.0 && guard++ < 64) {
            s.burstWait += 0.3 + 1.4 * s.rng.uniform();
            s.burstPeak = (0.4 + 0.6 * s.rng.uniform()) * (s.rng.uniform() < 0.5 ? -1.0 : 1.0);
            s.burstLevel = s.burstPeak;
        }
        s.value = s.burstLevel;
        break;
    }
    }
    return static_cast<float>(std::clamp(s.value, -1.0, 1.0));
}

} // namespace zed
