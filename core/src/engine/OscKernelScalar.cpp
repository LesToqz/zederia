// The one-lane copy of the oscillator kernel (the reference path), the reference warp functions
// built from the same code, and the choice of kernel at run time.
#include "zederia/dsp/Simd.h"
#include "zederia/engine/OscKernel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#define ZED_KERNEL_SIMD zed::dsp::SimdScalar
#include "OscKernel.inl"
#undef ZED_KERNEL_SIMD

namespace zed::osc {

void renderScalar(Lanes& lanes, const Block& block) { renderImpl(lanes, block); }

float warpPhase(WarpMode mode, float phase, float derived, float modulator, const float* curve, float& gain) noexcept {
    return warpPhaseV(mode, phase, derived, modulator, curve != nullptr ? curve : defaultWarpCurve(), gain);
}

float warpAmplitude(WarpMode mode, float sample, float derived, float modulator) noexcept {
    return warpAmplitudeV(mode, sample, derived, modulator);
}

bool warpIsPhaseShaper(WarpMode mode) noexcept {
    return mode >= WarpMode::Sync && mode <= WarpMode::CurveQuad;
}
bool warpUsesModulator(WarpMode mode) noexcept {
    return mode == WarpMode::Pm || mode == WarpMode::Am || mode == WarpMode::Rm;
}
bool warpUsesCurve(WarpMode mode) noexcept {
    return mode >= WarpMode::CurveFull && mode <= WarpMode::CurveQuad;
}

float warpDerive(WarpMode mode, float amount) noexcept {
    const float a = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
    switch (mode) {
    case WarpMode::Sync:
    case WarpMode::SoftSync:
        return std::exp2(4.0f * a); // inner cycle 1 to 16 times as fast
    case WarpMode::BendPlus:
    case WarpMode::BendMinus:
    case WarpMode::SkewPlus:
    case WarpMode::SkewMinus:
        return std::exp2(4.0f * a) - 1.0f; // curve constant 0 to 15
    case WarpMode::BendBoth:
    case WarpMode::SkewBoth: { // neutral at 50 %: negative values curve the other way
        const float c = 2.0f * a - 1.0f;
        const float magnitude = std::exp2(4.0f * std::abs(c)) - 1.0f;
        return c < 0.0f ? -magnitude : magnitude;
    }
    case WarpMode::PulseWidth:
        return 1.0f / (1.0f - 0.96f * a); // the cycle plays in 100 % down to 4 % of the period
    case WarpMode::Mirror:
        return 1.0f + a; // the turning point moves from the end of the cycle to its middle
    case WarpMode::Flip:
        return 1.0f - a; // the point after which polarity is inverted
    case WarpMode::Quantize:
        return std::exp2(10.0f - 9.0f * a); // 1024 down to 2 steps per cycle
    case WarpMode::Fold:
        return std::exp2(3.0f * a); // gain 1 to 8 into the folder
    case WarpMode::Drive:
        return std::exp2(5.0f * a) - 1.0f; // curve constant 0 to 31
    case WarpMode::Pm:
        return 4.0f * a * a; // peak phase deviation in cycles
    default:
        return a; // curves, am, rm: the amount itself
    }
}

float warpSlope(WarpMode mode, float derived) noexcept {
    switch (mode) {
    case WarpMode::Sync:
    case WarpMode::SoftSync:
    case WarpMode::PulseWidth:
    case WarpMode::Mirror:
        return derived;
    case WarpMode::BendPlus:
    case WarpMode::BendMinus:
    case WarpMode::BendBoth:
    case WarpMode::SkewPlus:
    case WarpMode::SkewMinus:
    case WarpMode::SkewBoth:
        return std::sqrt(1.0f + std::abs(derived)); // steepest for a part of the cycle only
    case WarpMode::CurveFull:
    case WarpMode::CurveHalf:
    case WarpMode::CurveSine:
    case WarpMode::CurveQuad:
        return 1.0f + derived;
    default:
        return 1.0f;
    }
}

const float* defaultWarpCurve() noexcept {
    static const std::array<float, kCurvePoints + 2> curve = [] {
        std::array<float, kCurvePoints + 2> c{};
        for (int i = 0; i < kCurvePoints + 2; ++i) {
            const float x = i >= kCurvePoints ? 1.0f : static_cast<float>(i) / kCurvePoints;
            c[static_cast<std::size_t>(i)] = x * x * (3.0f - 2.0f * x);
        }
        return c;
    }();
    return curve.data();
}

void fillWarpCurve(std::span<const float> values, float* table) noexcept {
    if (values.empty()) {
        std::copy(defaultWarpCurve(), defaultWarpCurve() + kCurvePoints + 2, table);
        return;
    }
    const auto last = static_cast<double>(values.size() - 1);
    for (int i = 0; i < kCurvePoints + 2; ++i) {
        const double at = std::min(static_cast<double>(i) / kCurvePoints, 1.0) * last;
        const auto index = static_cast<std::size_t>(at);
        const std::size_t next = std::min(index + 1, values.size() - 1);
        const double v = values[index] + (values[next] - values[index]) * (at - static_cast<double>(index));
        table[i] = static_cast<float>(std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0);
    }
}

namespace {
std::atomic<int> forcedWidth{0};
}

void forceKernelWidth(int width) noexcept { forcedWidth.store(width, std::memory_order_relaxed); }

RenderFn pickRender(int lanes) noexcept {
    const int forced = forcedWidth.load(std::memory_order_relaxed);
    const bool avx2 = dsp::cpuHasAvx2();
    if (forced == 1)
        return renderScalar;
    if (forced == 4)
        return renderSse2;
    if (forced == 8)
        return avx2 ? renderAvx2 : renderSse2;
    if (lanes <= 1)
        return renderScalar;
    if (lanes <= 4 || !avx2)
        return renderSse2;
    return renderAvx2;
}

} // namespace zed::osc
