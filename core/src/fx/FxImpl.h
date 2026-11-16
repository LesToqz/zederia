// The effect implementations, one factory per type. Private to the core.
#pragma once

#include "zederia/fx/Effect.h"

#include <cstddef>

namespace zed::fx {

#define ZED_DECLARE_EFFECT(Name)                                                                                       \
    std::unique_ptr<Effect> make##Name();                                                                              \
    extern const char* const k##Name##Params[];                                                                        \
    extern const std::size_t k##Name##ParamCount;

ZED_DECLARE_EFFECT(Drive)
ZED_DECLARE_EFFECT(Compressor)
ZED_DECLARE_EFFECT(Eq)
ZED_DECLARE_EFFECT(FilterFx)
ZED_DECLARE_EFFECT(Chorus)
ZED_DECLARE_EFFECT(Flanger)
ZED_DECLARE_EFFECT(Phaser)
ZED_DECLARE_EFFECT(Delay)
ZED_DECLARE_EFFECT(Reverb)
ZED_DECLARE_EFFECT(Swarm)
ZED_DECLARE_EFFECT(Widener)
ZED_DECLARE_EFFECT(Utility)
ZED_DECLARE_EFFECT(Limiter)

#undef ZED_DECLARE_EFFECT

bool eqResponse(const double* values, double sampleRate, double hz, std::complex<double>& out);
bool filterFxResponse(const double* values, double sampleRate, double hz, std::complex<double>& out);

// A look-ahead-free peak limiter, shared by the Limiter effect and the output protection. The
// gain falls at once to keep every sample at or below the ceiling and recovers with the release.
struct PeakLimiter {
    float env = 0.0f, releaseCoef = 0.999f;
    void setRelease(double ms, double sampleRate);
    void reset() { env = 0.0f; }
    // In place; `ceiling` is linear.
    void process(float* left, float* right, int frames, float ceiling) noexcept;
};

} // namespace zed::fx
