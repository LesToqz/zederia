// Widener and Utility (plan 3.8).
//
// Widener: the side signal gains four delayed, alternating-sign taps of the mid signal. Only the
// side changes, so the mono sum of the result is exactly the mono sum of the input: the effect
// is mono-compatible by construction. It reports the correlation of its output for the
// mono-compatibility indicator.
//
// Utility: gain, constant-power balance, width, mono below a frequency (the side signal through a
// Linkwitz-Riley high-pass), polarity and a DC filter. Every switch moves through a short ramp.
#include "FxDsp.h"
#include "FxImpl.h"

#include <atomic>

namespace zed::fx {
namespace {

class Widener final : public Effect {
public:
    static constexpr int kTaps = 4;

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        line_.prepare(static_cast<int>(sampleRate * 0.025) + 8);
        corrCoef_ = smoothingCoef(150.0, sampleRate);
        clear();
    }
    void clear() noexcept override {
        line_.clear();
        lr_ = ll_ = rr_ = 0.0f;
        correlation_.store(1.0f, std::memory_order_relaxed);
    }
    void set(const double* v, const Context&, bool snap) noexcept override { size_.aim(static_cast<float>(v[0]), snap); }
    void process(float* left, float* right, int frames) noexcept override {
        static constexpr float tapMs[kTaps] = {3.1f, 7.3f, 11.9f, 17.3f};
        static constexpr float tapGain[kTaps] = {0.5f, -0.42f, 0.35f, -0.28f};
        const float ms = static_cast<float>(sampleRate_ * 0.001);
        for (int s = 0; s < frames; ++s) {
            const float size = size_.next();
            const float mid = 0.5f * (left[s] + right[s]);
            float side = 0.5f * (left[s] - right[s]);
            line_.push(mid);
            for (int k = 0; k < kTaps; ++k)
                side += tapGain[k] * line_.read(std::max(0.3f + size * tapMs[k], 0.05f) * ms + 1.0f, false);
            left[s] = mid + side;
            right[s] = mid - side;
            lr_ += (left[s] * right[s] - lr_) * corrCoef_;
            ll_ += (left[s] * left[s] - ll_) * corrCoef_;
            rr_ += (right[s] * right[s] - rr_) * corrCoef_;
        }
        const float energy = std::sqrt(ll_ * rr_);
        correlation_.store(energy > 1.0e-12f ? std::clamp(lr_ / energy, -1.0f, 1.0f) : 1.0f, std::memory_order_relaxed);
    }
    int holdSamples() const noexcept override { return static_cast<int>(sampleRate_ * 0.03); }
    float meter() const noexcept override { return correlation_.load(std::memory_order_relaxed); }

private:
    double sampleRate_ = 48000.0;
    DelayLine line_;
    Ramp size_;
    float lr_ = 0.0f, ll_ = 0.0f, rr_ = 0.0f, corrCoef_ = 0.001f;
    std::atomic<float> correlation_{1.0f};
};

class Utility final : public Effect {
public:
    enum P { Gain, Pan, Width, MonoBelow, Polarity, DcFilter };

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        dcCoef_ = static_cast<float>(1.0 - 2.0 * kPiD * 5.0 / sampleRate);
        clear();
    }
    void clear() noexcept override {
        for (auto& f : monoHp_)
            f.reset();
        for (int c = 0; c < 2; ++c)
            dcX_[c] = dcY_[c] = 0.0f;
    }
    void set(const double* v, const Context&, bool snap) noexcept override {
        const double gain = std::pow(10.0, v[Gain] / 20.0);
        const double angle = (std::clamp(v[Pan], -1.0, 1.0) + 1.0) * 0.25 * kPiD;
        // Constant-power balance, unity in the centre.
        const double balL = std::cos(angle) * 1.41421356, balR = std::sin(angle) * 1.41421356;
        const int polarity = static_cast<int>(v[Polarity]);
        const double signL = polarity == 1 || polarity == 3 ? -1.0 : 1.0;
        const double signR = polarity == 2 || polarity == 3 ? -1.0 : 1.0;
        gainL_.aim(static_cast<float>(gain * std::min(balL, 1.0) * signL), snap);
        gainR_.aim(static_cast<float>(gain * std::min(balR, 1.0) * signR), snap);
        width_.aim(static_cast<float>(v[Width]), snap);
        mono_ = v[MonoBelow] > 0.0;
        if (mono_)
            for (auto& f : monoHp_)
                f.design(v[MonoBelow], 0.70710678, sampleRate_);
        dc_ = static_cast<int>(v[DcFilter]) != 0;
    }
    void process(float* left, float* right, int frames) noexcept override {
        for (int s = 0; s < frames; ++s) {
            const float mid = 0.5f * (left[s] + right[s]);
            float side = 0.5f * (left[s] - right[s]) * width_.next();
            if (mono_)
                side = monoHp_[1].highpass(monoHp_[0].highpass(side));
            float l = (mid + side) * gainL_.next(), r = (mid - side) * gainR_.next();
            if (dc_) {
                const float yl = l - dcX_[0] + dcCoef_ * dcY_[0];
                dcX_[0] = l;
                dcY_[0] = yl;
                const float yr = r - dcX_[1] + dcCoef_ * dcY_[1];
                dcX_[1] = r;
                dcY_[1] = yr;
                l = yl;
                r = yr;
            }
            left[s] = l;
            right[s] = r;
        }
    }

private:
    double sampleRate_ = 48000.0;
    Ramp gainL_, gainR_, width_;
    bool mono_ = false, dc_ = false;
    Svf monoHp_[2];
    float dcCoef_ = 0.999f, dcX_[2] = {}, dcY_[2] = {};
};

} // namespace

std::unique_ptr<Effect> makeWidener() { return std::make_unique<Widener>(); }
std::unique_ptr<Effect> makeUtility() { return std::make_unique<Utility>(); }

const char* const kWidenerParams[] = {"size"};
const std::size_t kWidenerParamCount = std::size(kWidenerParams);
const char* const kUtilityParams[] = {"gain", "pan", "width", "mono_below", "polarity", "dc_filter"};
const std::size_t kUtilityParamCount = std::size(kUtilityParams);

} // namespace zed::fx
