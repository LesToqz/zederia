// Compressor and Limiter (plan 3.8). The compressor works in one band, or in three bands split by
// Linkwitz-Riley crossovers that sum flat, each band pressing down above its threshold and lifting
// up below it. The limiter has no look-ahead: its gain falls at once, so nothing passes above the
// ceiling.
#include "FxDsp.h"
#include "FxImpl.h"

namespace zed::fx {
namespace {

inline float toDb(float x) noexcept { return 20.0f * std::log10(std::max(x, 1.0e-9f)); }
inline float fromDb(float db) noexcept { return std::pow(10.0f, db * 0.05f); }

// Gain change in dB for a level `over` dB above the threshold, with a 6 dB soft knee.
inline float downward(float over, float ratio) noexcept {
    constexpr float knee = 6.0f;
    const float slope = ratio >= 19.99f ? 1.0f : 1.0f - 1.0f / ratio;
    if (over <= -0.5f * knee)
        return 0.0f;
    if (over >= 0.5f * knee)
        return -over * slope;
    const float x = over + 0.5f * knee;
    return -slope * x * x / (2.0f * knee);
}

// A level detector: follows the peak with the attack time and falls with the release time.
struct Detector {
    float env = 0.0f;
    float step(float x, float attack, float release) noexcept {
        env += (x - env) * (x > env ? attack : release);
        return env;
    }
};

class Compressor final : public Effect {
public:
    enum P { Mode, Threshold, Ratio, Attack, Release, Makeup, Upward, LowThr, LowAmt, MidThr, MidAmt, HighThr, HighAmt,
             XLow, XHigh, kParams };

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        clear();
    }
    void clear() noexcept override {
        for (auto& d : det_)
            d = Detector{};
        for (int c = 0; c < 2; ++c) {
            for (auto& f : xl_[c])
                f.reset();
            for (auto& f : xh_[c])
                f.reset();
            ap_[c].reset();
        }
    }
    void set(const double* v, const Context&, bool snap) noexcept override {
        multiband_ = static_cast<int>(v[Mode]) == 1;
        threshold_ = static_cast<float>(v[Threshold]);
        ratio_ = static_cast<float>(v[Ratio]);
        attack_ = smoothingCoef(v[Attack], sampleRate_);
        release_ = smoothingCoef(v[Release], sampleRate_);
        makeup_.aim(fromDb(static_cast<float>(v[Makeup])), snap);
        upward_ = static_cast<float>(v[Upward]);
        bandThr_[0] = static_cast<float>(v[LowThr]);
        bandThr_[1] = static_cast<float>(v[MidThr]);
        bandThr_[2] = static_cast<float>(v[HighThr]);
        bandAmt_[0] = static_cast<float>(v[LowAmt]);
        bandAmt_[1] = static_cast<float>(v[MidAmt]);
        bandAmt_[2] = static_cast<float>(v[HighAmt]);
        const double low = std::min(v[XLow], v[XHigh] * 0.8);
        for (int c = 0; c < 2; ++c) {
            for (auto& f : xl_[c])
                f.design(low, 0.70710678, sampleRate_);
            for (auto& f : xh_[c])
                f.design(v[XHigh], 0.70710678, sampleRate_);
            ap_[c].design(v[XHigh], 0.70710678, sampleRate_);
        }
    }

    void process(float* left, float* right, int frames) noexcept override {
        if (!multiband_) {
            for (int s = 0; s < frames; ++s) {
                const float level = det_[0].step(std::max(std::abs(left[s]), std::abs(right[s])), attack_, release_);
                const float gr = level > kSilent ? downward(toDb(level) - threshold_, ratio_) : 0.0f;
                const float g = (gr != 0.0f ? fromDb(gr) : 1.0f) * makeup_.next();
                left[s] *= g;
                right[s] *= g;
            }
            return;
        }
        float* io[2] = {left, right};
        for (int s = 0; s < frames; ++s) {
            float band[3][2];
            for (int c = 0; c < 2; ++c) {
                // Linkwitz-Riley: two Butterworth sections per side of each crossover. The low band
                // passes the upper crossover's all-pass so the three bands sum to a flat response.
                const float x = io[c][s];
                const float low = xl_[c][1].lowpass(xl_[c][0].lowpass(x));
                const float rest = xl_[c][3].highpass(xl_[c][2].highpass(x));
                band[1][c] = xh_[c][1].lowpass(xh_[c][0].lowpass(rest));
                band[2][c] = xh_[c][3].highpass(xh_[c][2].highpass(rest));
                float bp, hp;
                const float lp = ap_[c].run(low, bp, hp);
                band[0][c] = lp - ap_[c].k * bp + hp;
            }
            float sum[2] = {0.0f, 0.0f};
            for (int b = 0; b < 3; ++b) {
                const float level =
                    det_[b].step(std::max(std::abs(band[b][0]), std::abs(band[b][1])), attack_, release_);
                float gr = 0.0f;
                if (level > kSilent) {
                    const float db = toDb(level);
                    const float over = db - bandThr_[b];
                    if (over > -3.0f)
                        gr = downward(over, 1.0f + 9.0f * bandAmt_[b]);
                    else {
                        // Upward: lift quiet material toward the threshold, but never noise or silence.
                        const float under = -over;
                        const float floorFade = std::clamp((db + 70.0f) / 10.0f, 0.0f, 1.0f);
                        gr = std::min((under - 3.0f) * 0.75f * upward_ * bandAmt_[b], 18.0f) * floorFade;
                    }
                }
                const float g = gr != 0.0f ? fromDb(gr) : 1.0f;
                sum[0] += band[b][0] * g;
                sum[1] += band[b][1] * g;
            }
            const float m = makeup_.next();
            left[s] = sum[0] * m;
            right[s] = sum[1] * m;
        }
    }

private:
    double sampleRate_ = 48000.0;
    bool multiband_ = false;
    float threshold_ = -18.0f, ratio_ = 4.0f, attack_ = 0.01f, release_ = 0.001f, upward_ = 0.5f;
    float bandThr_[3] = {-24.0f, -24.0f, -24.0f}, bandAmt_[3] = {0.5f, 0.5f, 0.5f};
    Ramp makeup_;
    Detector det_[3];
    Svf xl_[2][4], xh_[2][4], ap_[2];
};

class Limiter final : public Effect {
public:
    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        clear();
    }
    void clear() noexcept override { limiter_.reset(); }
    void set(const double* v, const Context&, bool snap) noexcept override {
        ceiling_.aim(static_cast<float>(std::pow(10.0, v[0] / 20.0)), snap);
        limiter_.setRelease(v[1], sampleRate_);
    }
    void process(float* left, float* right, int frames) noexcept override {
        // The ceiling moves per sample; the limiter takes it per call, at the lower of the two ends
        // so that no sample passes above either.
        const float start = ceiling_.now;
        for (int s = 0; s < frames; ++s)
            ceiling_.next();
        limiter_.process(left, right, frames, std::min(start, ceiling_.now));
    }

private:
    double sampleRate_ = 48000.0;
    Ramp ceiling_;
    PeakLimiter limiter_;
};

} // namespace

void PeakLimiter::setRelease(double ms, double sampleRate) {
    releaseCoef = static_cast<float>(std::exp(-1.0 / std::max(ms * 0.001 * sampleRate, 1.0)));
}

void PeakLimiter::process(float* left, float* right, int frames, float ceiling) noexcept {
    float e = env;
    for (int s = 0; s < frames; ++s) {
        const float peak = std::max(std::abs(left[s]), std::abs(right[s]));
        e = std::max(peak, e * releaseCoef);
        if (e > ceiling) {
            const float g = ceiling / e;
            left[s] = std::clamp(left[s] * g, -ceiling, ceiling);
            right[s] = std::clamp(right[s] * g, -ceiling, ceiling);
        }
    }
    env = e < 1.0e-20f ? 0.0f : e;
}

std::unique_ptr<Effect> makeCompressor() { return std::make_unique<Compressor>(); }
std::unique_ptr<Effect> makeLimiter() { return std::make_unique<Limiter>(); }

const char* const kCompressorParams[] = {"mode", "threshold", "ratio", "attack", "release", "makeup", "upward",
                                         "low_threshold", "low_amount", "mid_threshold", "mid_amount",
                                         "high_threshold", "high_amount", "crossover_low", "crossover_high"};
const std::size_t kCompressorParamCount = std::size(kCompressorParams);
const char* const kLimiterParams[] = {"ceiling", "release"};
const std::size_t kLimiterParamCount = std::size(kLimiterParams);

} // namespace zed::fx
