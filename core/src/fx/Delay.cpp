// Delay (plan 3.8): Dual (two independent sides), Ping-pong (echoes cross from side to side) and
// Pre-tap (one early tap at the left time ahead of the repeating echoes at the right time). Times
// in milliseconds or as note lengths with a fine offset whose ends are the triplet (x 2/3) and
// dotted (x 3/2) lengths. Every echo passes the feedback band and the saturation; the time glides
// like tape when it changes, so a change never clicks.
#include "FxDsp.h"
#include "FxImpl.h"

namespace zed::fx {
namespace {

class Delay final : public Effect {
public:
    enum P { Mode, Sync, TimeL, TimeR, DivL, DivR, OffL, OffR, Link, Feedback, BandCenter, BandWidth, Saturation,
             Modulation, Ducking };
    static constexpr double kMaxMs = 4000.0;

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        for (auto& line : line_)
            line.prepare(static_cast<int>(sampleRate * (kMaxMs + 60.0) / 1000.0));
        glide_ = 1.0 - std::exp(-1.0 / (0.06 * sampleRate));
        duckAttack_ = smoothingCoef(5.0, sampleRate);
        duckRelease_ = smoothingCoef(250.0, sampleRate);
        clear();
    }
    void clear() noexcept override {
        for (auto& line : line_)
            line.clear();
        for (int c = 0; c < 2; ++c) {
            low_[c].reset();
            high_[c].reset();
            time_[c] = target_[c];
        }
        modPhase_ = 0.0;
        duckEnv_ = 0.0f;
    }
    void set(const double* v, const Context& ctx, bool snap) noexcept override {
        mode_ = std::clamp(static_cast<int>(v[Mode]), 0, 2);
        const bool sync = static_cast<int>(v[Sync]) != 0;
        const bool link = static_cast<int>(v[Link]) != 0;
        const auto timeMs = [&](int side) {
            const int t = side == 0 || link ? TimeL : TimeR;
            const int d = side == 0 || link ? DivL : DivR;
            const int o = side == 0 || link ? OffL : OffR;
            const double ms = sync ? divisionBeats(static_cast<int>(v[d])) * 60000.0 / std::max(ctx.bpm, 1.0) *
                                         std::pow(1.5, std::clamp(v[o], -1.0, 1.0))
                                   : v[t];
            return std::clamp(ms, 1.0, kMaxMs);
        };
        for (int c = 0; c < 2; ++c) {
            target_[c] = timeMs(c) * 0.001 * sampleRate_;
            if (snap)
                time_[c] = target_[c];
        }
        feedback_.aim(static_cast<float>(v[Feedback]), snap);
        const double halfWidth = 0.3 + 5.0 * std::clamp(v[BandWidth], 0.0, 1.0); // octaves either side
        const double lo = v[BandCenter] / std::exp2(halfWidth), hi = v[BandCenter] * std::exp2(halfWidth);
        useHigh_ = lo > 15.0;
        useLow_ = hi < sampleRate_ * 0.45;
        for (int c = 0; c < 2; ++c) {
            high_[c].design(lo, 0.70710678, sampleRate_);
            low_[c].design(hi, 0.70710678, sampleRate_);
        }
        drive_ = static_cast<float>(1.0 + 4.0 * v[Saturation]);
        modDepth_ = v[Modulation] * 0.002 * sampleRate_; // up to 2 ms of wow
        ducking_ = static_cast<float>(v[Ducking]);
        cubic_ = !ctx.draft;
    }
    void process(float* left, float* right, int frames) noexcept override {
        const double modInc = 0.55 / sampleRate_;
        for (int s = 0; s < frames; ++s) {
            const float fb = feedback_.next();
            double d[2];
            for (int c = 0; c < 2; ++c) {
                time_[c] += (target_[c] - time_[c]) * glide_;
                const double wobble = modDepth_ > 0.0 ? modDepth_ * (0.5 + 0.5 * std::sin(2.0 * kPiD * (modPhase_ + 0.31 * c))) : 0.0;
                d[c] = std::clamp(time_[c] + wobble, 2.0, static_cast<double>(line_[c].capacity() - 4));
            }
            modPhase_ += modInc;
            if (modPhase_ >= 1.0)
                modPhase_ -= 1.0;

            const float in[2] = {left[s], right[s]};
            float wet[2];
            if (mode_ == 2) { // pre-tap: an early tap at the left time, echoes at the right time
                for (int c = 0; c < 2; ++c) {
                    const float echo = band(c, line_[c].read(d[1], cubic_));
                    const float pre = line_[c].read(std::min(d[0], d[1]), cubic_);
                    line_[c].push(in[c] + fb * saturate(echo));
                    wet[c] = echo + 0.7f * pre;
                }
            } else if (mode_ == 1) { // ping-pong: the input enters on the left, echoes cross over
                const float yl = band(0, line_[0].read(d[0], cubic_));
                const float yr = band(1, line_[1].read(d[1], cubic_));
                line_[0].push(0.5f * (in[0] + in[1]) + fb * saturate(yr));
                line_[1].push(fb * saturate(yl));
                wet[0] = yl;
                wet[1] = yr;
            } else {
                for (int c = 0; c < 2; ++c) {
                    const float y = band(c, line_[c].read(d[c], cubic_));
                    line_[c].push(in[c] + fb * saturate(y));
                    wet[c] = y;
                }
            }
            if (ducking_ > 0.0f) {
                const float x = std::max(std::abs(in[0]), std::abs(in[1]));
                duckEnv_ += (x - duckEnv_) * (x > duckEnv_ ? duckAttack_ : duckRelease_);
                const float db = 20.0f * std::log10(std::max(duckEnv_, 1.0e-6f));
                const float cut = ducking_ * std::clamp((db + 40.0f) / 40.0f, 0.0f, 1.0f) * 30.0f;
                const float g = std::pow(10.0f, -cut * 0.05f);
                wet[0] *= g;
                wet[1] *= g;
            }
            left[s] = wet[0];
            right[s] = wet[1];
        }
    }
    int holdSamples() const noexcept override {
        return static_cast<int>(std::max(time_[0], time_[1]) + modDepth_) + 256;
    }

private:
    float band(int c, float x) noexcept {
        if (useHigh_)
            x = high_[c].highpass(x);
        if (useLow_)
            x = low_[c].lowpass(x);
        return x;
    }
    float saturate(float x) const noexcept { return drive_ > 1.0f ? std::tanh(drive_ * x) / drive_ : x; }

    double sampleRate_ = 48000.0, glide_ = 0.0003;
    int mode_ = 0;
    bool cubic_ = true, useHigh_ = false, useLow_ = false;
    DelayLine line_[2];
    double time_[2] = {18000.0, 18000.0}, target_[2] = {18000.0, 18000.0};
    Ramp feedback_;
    Svf high_[2], low_[2];
    float drive_ = 1.0f, ducking_ = 0.0f, duckEnv_ = 0.0f, duckAttack_ = 0.1f, duckRelease_ = 0.001f;
    double modDepth_ = 0.0, modPhase_ = 0.0;
};

} // namespace

std::unique_ptr<Effect> makeDelay() { return std::make_unique<Delay>(); }

const char* const kDelayParams[] = {"mode",     "sync",        "time_l",     "time_r",     "division_l",
                                    "division_r", "offset_l",  "offset_r",   "link",       "feedback",
                                    "band_center", "band_width", "saturation", "modulation", "ducking"};
const std::size_t kDelayParamCount = std::size(kDelayParams);

} // namespace zed::fx
