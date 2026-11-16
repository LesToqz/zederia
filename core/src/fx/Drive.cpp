// Drive (plan 3.8): fifteen distortion modes with an optional pre or post filter. The shaping
// modes run oversampled by the quality mode; Bit crush and Downsample are digital effects whose
// aliasing is the point and run at the base rate. A change of mode or of quality crossfades.
#include "FxDsp.h"
#include "FxImpl.h"

#include <array>

namespace zed::fx {
namespace {

enum P { Mode, Amount, FilterPos, FilterMorph, FilterFreq, FilterQ, CurveMorph, CurveSym, Level, kParams };
enum M {
    Tube, SoftClip, HardClip, DiodeA, DiodeB, LinearFold, SineFold, Rectify, Asymmetric, BitCrush, Downsample,
    SineShaper, Fuzz, CustomCurve, DualCurve, kModes
};

constexpr int kFade = 256;    // samples of a mode or quality crossfade
constexpr int kWarmUp = 64;   // a new oversampling path settles this long before it is heard
constexpr int kMaxUp = kMaxFrames * 8;

float readCurve(const float* table, float x) noexcept {
    const float pos = std::clamp(x, 0.0f, 1.0f) * kCurveSize;
    const int i = std::min(static_cast<int>(pos), kCurveSize - 1);
    const float t = pos - i;
    return table[i] + t * (table[i + 1] - table[i]);
}

class Drive final : public Effect {
public:
    Drive() {
        for (int i = 0; i <= kCurveSize; ++i) {
            const float x = static_cast<float>(i) / kCurveSize;
            defaultA_[static_cast<std::size_t>(i)] = std::tanh(2.0f * x) / std::tanh(2.0f);
            defaultB_[static_cast<std::size_t>(i)] = std::min(1.6f * x, 1.0f);
        }
        curveA_ = defaultA_;
        curveB_ = defaultB_;
    }

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        dcCoef_ = static_cast<float>(1.0 - 2.0 * kPiD * 10.0 / sampleRate); // 10 Hz: settles within a quarter second
        clear();
    }

    void clear() noexcept override {
        for (auto& p : path_) {
            const int factor = p.factor;
            const auto* design = p.design;
            p = Path{};
            p.factor = factor;
            p.design = design;
        }
        for (auto& f : filter_)
            f.reset();
        for (int c = 0; c < 2; ++c)
            dcX_[c] = dcY_[c] = 0.0f;
        fade_ = 0;
        pathFade_ = 0;
    }

    void setCurves(const float* a, const float* b) noexcept override {
        if (a != nullptr)
            std::copy(a, a + kCurveSize + 1, curveA_.begin());
        else
            curveA_ = defaultA_;
        if (b != nullptr)
            std::copy(b, b + kCurveSize + 1, curveB_.begin());
        else
            curveB_ = defaultB_;
    }

    void set(const double* v, const Context& ctx, bool snap) noexcept override {
        halfBands_ = ctx.halfBands;
        const int mode = std::clamp(static_cast<int>(v[Mode]), 0, kModes - 1);
        if (snap) {
            mode_ = oldMode_ = mode;
            fade_ = 0;
        } else if (mode != mode_) {
            oldMode_ = mode_;
            mode_ = mode;
            fade_ = kFade;
            path_[2] = path_[active_]; // the old mode carries on from the same filter states
        }
        const int factor = std::clamp(ctx.oversample, 1, 8);
        if (snap) {
            path_[active_].factor = factor;
            path_[active_].design = ctx.halfBands;
            pathFade_ = 0;
        } else if ((factor != path_[active_].factor || ctx.halfBands != path_[active_].design) && pathFade_ == 0) {
            const int next = 1 - active_;
            path_[next] = Path{};
            path_[next].factor = factor;
            path_[next].design = ctx.halfBands;
            pathFade_ = kWarmUp + kFade;
        }
        amount_.aim(static_cast<float>(v[Amount]), snap);
        drive_.aim(static_cast<float>(std::pow(10.0, v[Amount] * 36.0 / 20.0)), snap);
        level_.aim(static_cast<float>(std::pow(10.0, v[Level] / 20.0)), snap);
        morph_.aim(static_cast<float>(v[CurveMorph]), snap);
        asymmetric_ = static_cast<int>(v[CurveSym]) == 1;
        filterPos_ = static_cast<int>(v[FilterPos]);
        filterMorph_ = static_cast<float>(v[FilterMorph]);
        const double q = 0.5 + 12.0 * v[FilterQ] * v[FilterQ];
        for (auto& f : filter_)
            f.design(v[FilterFreq], q, sampleRate_);
    }

    void process(float* left, float* right, int frames) noexcept override {
        float* io[2] = {left, right};
        if (filterPos_ == 1)
            runFilter(io, frames);
        // Gain before the shaper (it commutes with the oversampling filters).
        float gainBuf[kMaxFrames], amountBuf[kMaxFrames], morphBuf[kMaxFrames];
        for (int s = 0; s < frames; ++s) {
            gainBuf[s] = drive_.next();
            amountBuf[s] = amount_.next();
            morphBuf[s] = morph_.next();
        }
        float dry[2][kMaxFrames];
        for (int c = 0; c < 2; ++c)
            std::copy(io[c], io[c] + frames, dry[c]);

        float out[2][kMaxFrames];
        renderMode(mode_, dry, out, frames, gainBuf, amountBuf, morphBuf, active_);
        if (pathFade_ > 0) {
            // The other oversampling path runs alongside: silent while it settles, then crossfaded in.
            float other[2][kMaxFrames];
            renderMode(mode_, dry, other, frames, gainBuf, amountBuf, morphBuf, 1 - active_);
            for (int s = 0; s < frames; ++s) {
                const int remaining = pathFade_ - s;
                const float w = remaining > kFade ? 0.0f : 1.0f - static_cast<float>(std::max(remaining, 0)) / kFade;
                for (int c = 0; c < 2; ++c)
                    out[c][s] += w * (other[c][s] - out[c][s]);
            }
            pathFade_ = std::max(pathFade_ - frames, 0);
            if (pathFade_ == 0)
                active_ = 1 - active_;
        }
        if (fade_ > 0) {
            float old[2][kMaxFrames];
            renderMode(oldMode_, dry, old, frames, gainBuf, amountBuf, morphBuf, 2);
            for (int s = 0; s < frames; ++s) {
                const float w = static_cast<float>(std::max(fade_ - s, 0)) / kFade; // weight of the old mode
                for (int c = 0; c < 2; ++c)
                    out[c][s] += w * (old[c][s] - out[c][s]);
            }
            fade_ = std::max(fade_ - frames, 0);
        }
        // DC blocker (asymmetric modes leave an offset), then the output level.
        for (int c = 0; c < 2; ++c) {
            float x1 = dcX_[c], y1 = dcY_[c];
            for (int s = 0; s < frames; ++s) {
                const float x = out[c][s];
                const float y = x - x1 + dcCoef_ * y1;
                x1 = x;
                y1 = y;
                io[c][s] = y;
            }
            dcX_[c] = x1;
            dcY_[c] = y1;
        }
        for (int s = 0; s < frames; ++s) {
            const float g = level_.next();
            left[s] *= g;
            right[s] *= g;
        }
        if (filterPos_ == 2)
            runFilter(io, frames);
    }

private:
    struct Channel {
        dsp::DecimatorState up, down;
    };
    struct Path {
        int factor = 1;
        const dsp::DecimatorDesign* design = nullptr; // the half-band set its states belong to
        Channel ch[2];
        float holdPhase = 1.0f, held[2] = {};
    };

    // Shapes one sample of an oversampled mode. Every shaper passes small signals at unit slope
    // except the ones whose character is gain (Fuzz).
    float shape(int mode, float x, float morph) const noexcept {
        switch (mode) {
        case Tube: {
            constexpr float bias = 0.2f, t0 = 0.19737532f, norm = 1.0f / (1.0f - t0 * t0);
            return (std::tanh(x + bias) - t0) * norm;
        }
        case SoftClip: return std::tanh(x);
        case HardClip: return std::clamp(x, -1.0f, 1.0f);
        case DiodeA: return x >= 0.0f ? 1.0f - std::exp(-x) : std::exp(x) - 1.0f;
        case DiodeB: return x >= 0.0f ? 1.0f - std::exp(-x) : (std::exp(1.8f * x) - 1.0f) / 1.8f;
        case LinearFold: {
            // Reflect into -1..1: a triangle of period 4.
            float u = (x + 1.0f) * 0.25f;
            u -= std::floor(u);
            return 4.0f * std::abs(u - 0.5f) - 1.0f;
        }
        case SineFold: return std::sin(x);
        case Rectify: return std::tanh(std::abs(x)); // full-wave, bounded
        case Asymmetric: return x >= 0.0f ? std::tanh(x) : 2.0f * std::tanh(0.5f * x);
        case SineShaper: return std::sin(std::clamp(x, -1.5707963f, 1.5707963f));
        case Fuzz: {
            constexpr float t0 = 0.29131261f;
            const float y = (std::tanh(6.0f * x + 0.3f) - t0) * 0.7f;
            return std::clamp(y, -1.0f, 1.0f);
        }
        case CustomCurve: return curve(curveA_.data(), x);
        case DualCurve: {
            const float a = curve(curveA_.data(), x), b = curve(curveB_.data(), x);
            return a + morph * (b - a);
        }
        default: return x;
        }
    }

    float curve(const float* table, float x) const noexcept {
        if (asymmetric_) // the curve spans -1..1: its middle is silence
            return 2.0f * (readCurve(table, 0.5f * (x + 1.0f)) - readCurve(table, 0.5f));
        const float y = readCurve(table, std::abs(x));
        return x >= 0.0f ? y : -y;
    }

    void renderMode(int mode, const float (&in)[2][kMaxFrames], float (&out)[2][kMaxFrames], int frames, const float* gain,
                    const float* amount, const float* morph, int pathIndex) noexcept {
        Path& path = path_[pathIndex];
        if (mode == BitCrush) {
            for (int c = 0; c < 2; ++c)
                for (int s = 0; s < frames; ++s) {
                    const float bits = 16.0f - 14.0f * amount[s];
                    const float q = std::exp2(-bits + 1.0f);
                    out[c][s] = std::round(in[c][s] / q) * q;
                }
            return;
        }
        if (mode == Downsample) {
            float phase = path.holdPhase, held[2] = {path.held[0], path.held[1]};
            for (int s = 0; s < frames; ++s) {
                const float hold = 1.0f + 63.0f * amount[s] * amount[s];
                phase += 1.0f / hold;
                if (phase >= 1.0f) {
                    phase -= std::floor(phase);
                    held[0] = in[0][s];
                    held[1] = in[1][s];
                }
                out[0][s] = held[0];
                out[1][s] = held[1];
            }
            path.holdPhase = phase;
            path.held[0] = held[0];
            path.held[1] = held[1];
            return;
        }
        const int factor = path.factor;
        for (int c = 0; c < 2; ++c) {
            float driven[kMaxFrames];
            for (int s = 0; s < frames; ++s)
                driven[s] = in[c][s] * gain[s];
            if (factor <= 1 || path.design == nullptr) {
                for (int s = 0; s < frames; ++s)
                    out[c][s] = shape(mode, driven[s], morph[s]);
                continue;
            }
            float up[kMaxUp], scratch[kMaxUp];
            dsp::interpolate(*path.design, path.ch[c].up, driven, up, scratch, frames, factor);
            for (int s = 0; s < frames; ++s)
                for (int k = 0; k < factor; ++k)
                    up[s * factor + k] = shape(mode, up[s * factor + k], morph[s]);
            dsp::decimate(*path.design, path.ch[c].down, up, frames, factor);
            std::copy(up, up + frames, out[c]);
        }
    }

    void runFilter(float* io[2], int frames) noexcept {
        const float m = filterMorph_;
        for (int c = 0; c < 2; ++c) {
            Svf& f = filter_[c];
            for (int s = 0; s < frames; ++s) {
                float band, high;
                const float low = f.run(io[c][s], band, high);
                const float bp = band * f.k;
                io[c][s] = m < 0.5f ? low + 2.0f * m * (bp - low) : bp + (2.0f * m - 1.0f) * (high - bp);
            }
        }
    }

    double sampleRate_ = 48000.0;
    const dsp::DecimatorDesign* halfBands_ = nullptr;
    int mode_ = Tube, oldMode_ = Tube, fade_ = 0;
    Path path_[3]; // two for a change of quality, one for the old mode during a change of mode
    int active_ = 0, pathFade_ = 0;
    Ramp amount_, drive_, level_, morph_;
    bool asymmetric_ = false;
    int filterPos_ = 0;
    float filterMorph_ = 0.0f;
    Svf filter_[2];
    float dcCoef_ = 0.999f, dcX_[2] = {}, dcY_[2] = {};
    std::array<float, kCurveSize + 1> curveA_{}, curveB_{}, defaultA_{}, defaultB_{};
};

} // namespace

std::unique_ptr<Effect> makeDrive() { return std::make_unique<Drive>(); }
const char* const kDriveParams[] = {"mode", "amount", "filter_position", "filter_morph", "filter_freq",
                                    "filter_q", "curve_morph", "curve_symmetry", "level"};
const std::size_t kDriveParamCount = std::size(kDriveParams);

} // namespace zed::fx
