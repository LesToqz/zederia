// Low-latency 2x decimation (plan 5.7): a half-band low-pass built from two chains of first-order
// all-pass sections (a polyphase IIR design). It adds no reportable latency, costs a handful of
// multiplies per output sample, and its stop band is set by the number of sections.
//
// The coefficients come from the closed-form elliptic half-band design (Valenzuela and
// Constantinides, 1983). tests/core/test_quality.cpp measures the resulting responses.
#pragma once

#include <array>

namespace zed::dsp {

struct HalfBandCoefs {
    static constexpr int kMax = 12;
    int count = 0;
    std::array<float, kMax> c{};
};

// `attenuationDb`: wanted stop-band rejection. `transition`: width of the transition band as a
// fraction of the input (higher) sample rate, 0 < transition < 0.5; the pass band ends at
// 0.25 - transition / 2 and the stop band begins at 0.25 + transition / 2.
HalfBandCoefs designHalfBand(double attenuationDb, double transition);

// Magnitude response of the decimating filter at `frequency` (a fraction of the input rate).
double halfBandMagnitude(const HalfBandCoefs& coefs, double frequency);

struct HalfBandState {
    std::array<float, HalfBandCoefs::kMax> x{}, y{};
    void reset() { *this = HalfBandState{}; }
};

// Consumes 2 * frames input samples and writes `frames` output samples. In and out may overlap
// when out <= in.
inline void halfBandDecimate(const HalfBandCoefs& k, HalfBandState& s, const float* in, float* out, int frames) {
    for (int n = 0; n < frames; ++n) {
        float a = in[2 * n + 1]; // chain 0: sections 0, 2, 4, ...
        float b = in[2 * n];     // chain 1: sections 1, 3, 5, ...
        int i = 0;
        for (; i + 1 < k.count; i += 2) {
            const float ya = (a - s.y[static_cast<std::size_t>(i)]) * k.c[static_cast<std::size_t>(i)] +
                             s.x[static_cast<std::size_t>(i)];
            const float yb = (b - s.y[static_cast<std::size_t>(i + 1)]) * k.c[static_cast<std::size_t>(i + 1)] +
                             s.x[static_cast<std::size_t>(i + 1)];
            s.x[static_cast<std::size_t>(i)] = a;
            s.x[static_cast<std::size_t>(i + 1)] = b;
            s.y[static_cast<std::size_t>(i)] = ya;
            s.y[static_cast<std::size_t>(i + 1)] = yb;
            a = ya;
            b = yb;
        }
        if (i < k.count) {
            const float ya = (a - s.y[static_cast<std::size_t>(i)]) * k.c[static_cast<std::size_t>(i)] +
                             s.x[static_cast<std::size_t>(i)];
            s.x[static_cast<std::size_t>(i)] = a;
            s.y[static_cast<std::size_t>(i)] = ya;
            a = ya;
        }
        out[n] = 0.5f * (a + b);
    }
}

// A decimator by 2, 4 or 8: a chain of half-band stages. Only the last stage (to the base rate)
// needs a narrow transition band; the earlier ones are cheap.
struct DecimatorDesign {
    std::array<HalfBandCoefs, 3> stage; // [0] is the last stage (2x -> 1x), [1] 4x -> 2x, [2] 8x -> 4x
};
// `steep`: the stricter set used by the High and Ultra quality modes.
const DecimatorDesign& decimatorDesign(bool steep);

struct DecimatorState {
    std::array<HalfBandState, 3> stage;
    void reset() { *this = DecimatorState{}; }
};

// Reduces `frames * factor` samples to `frames`, in place (the result is at the start of `data`).
inline void decimate(const DecimatorDesign& d, DecimatorState& s, float* data, int frames, int factor) {
    if (factor >= 8)
        halfBandDecimate(d.stage[2], s.stage[2], data, data, frames * 4);
    if (factor >= 4)
        halfBandDecimate(d.stage[1], s.stage[1], data, data, frames * 2);
    if (factor >= 2)
        halfBandDecimate(d.stage[0], s.stage[0], data, data, frames);
}

} // namespace zed::dsp
