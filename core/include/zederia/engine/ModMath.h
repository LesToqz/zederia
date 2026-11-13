// The arithmetic of one modulation route (plan 3.7), kept apart from the engine so it can be
// tested on its own and so the interface can draw what the audio does.
//
//   signal = curve(polarity(source)) * via
//   destination (normalized 0..1) = base + sum over routes of amount * signal
//
// Polarity: "uni" reads the source in its own range. "bi" recentres a source that runs 0..1
// (an envelope, a macro, velocity) to -1..1; a source that already runs -1..1 (an LFO, pitch
// bend) is left as it is. The curve is applied to the size of the value and keeps its sign.
// Via scales the route by a second source read as 0..1 (a -1..1 source is first moved to 0..1);
// inverted, it scales by one minus that.
#pragma once

#include "zederia/patch/Patch.h"
#include "zederia/patch/Shape.h"

#include <algorithm>
#include <cmath>

namespace zed {

inline constexpr int kModCurveSize = 256; // a drawn curve is a table of kModCurveSize + 1 values, 0..1 -> 0..1

// x in 0..1 -> 0..1. `drawn` is used only for ModCurve::Drawn (null falls back to linear).
inline float modCurve(ModCurve curve, float x, const float* drawn) noexcept {
    x = std::clamp(x, 0.0f, 1.0f);
    switch (curve) {
    case ModCurve::Exp: return static_cast<float>(curveValue(x, -2.0 / 3.0)); // slow start
    case ModCurve::Log: return static_cast<float>(curveValue(x, 2.0 / 3.0));  // fast start
    case ModCurve::S: return x * x * (3.0f - 2.0f * x);
    case ModCurve::Drawn: {
        if (drawn == nullptr)
            return x;
        const float position = x * static_cast<float>(kModCurveSize);
        const int i = std::min(static_cast<int>(position), kModCurveSize - 1);
        return drawn[i] + (position - static_cast<float>(i)) * (drawn[i + 1] - drawn[i]);
    }
    case ModCurve::Linear: break;
    }
    return x;
}

// A source's value after the route's polarity and curve: -1..1.
inline float modShape(float source, bool sourceIsBipolar, Polarity polarity, ModCurve curve, const float* drawn) noexcept {
    float v = polarity == Polarity::Bi && !sourceIsBipolar ? 2.0f * source - 1.0f : source;
    v = std::clamp(v, -1.0f, 1.0f);
    if (curve == ModCurve::Linear)
        return v;
    return v < 0.0f ? -modCurve(curve, -v, drawn) : modCurve(curve, v, drawn);
}

// The factor a Via source contributes: 0..1.
inline float modVia(float via, bool viaIsBipolar, bool invert) noexcept {
    const float unit = std::clamp(viaIsBipolar ? 0.5f * (via + 1.0f) : via, 0.0f, 1.0f);
    return invert ? 1.0f - unit : unit;
}

} // namespace zed
