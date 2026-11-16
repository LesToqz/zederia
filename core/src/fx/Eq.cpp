// EQ (plan 3.8): four bands of zero-delay-feedback state-variable sections. Low: shelf, bell or
// high-pass; two bells; high: shelf, bell or low-pass. The coefficients glide per sample, and the
// response the display draws is computed from the same coefficients the audio uses.
//
// Filter (plan 3.8): the voice filter module (every type, all controls) in stereo.
#include "FxDsp.h"
#include "FxImpl.h"
#include "zederia/engine/Filter.h"

#include <vector>

namespace zed::fx {
namespace {

enum class Kind { Bell, LowShelf, HighShelf, HighPass, LowPass };

struct Coefs {
    double g = 0.1, k = 1.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;
};

double qFromAmount(double a) { return 0.3 * std::exp2(5.0 * std::clamp(a, 0.0, 1.0)); }

Coefs design(Kind kind, double hz, double db, double q, double sampleRate) {
    Coefs c;
    const double t = std::tan(kPiD * std::clamp(hz, 5.0, sampleRate * 0.49) / sampleRate);
    const double a = std::pow(10.0, db / 40.0);
    q = std::max(q, 0.05);
    switch (kind) {
    case Kind::Bell:
        c.g = t;
        c.k = 1.0 / (q * a);
        c.m1 = c.k * (a * a - 1.0);
        break;
    case Kind::LowShelf:
        c.g = t / std::sqrt(a);
        c.k = 1.0 / q;
        c.m1 = c.k * (a - 1.0);
        c.m2 = a * a - 1.0;
        break;
    case Kind::HighShelf:
        c.g = t * std::sqrt(a);
        c.k = 1.0 / q;
        c.m0 = a * a;
        c.m1 = c.k * (1.0 - a) * a;
        c.m2 = 1.0 - a * a;
        break;
    case Kind::HighPass:
        c.g = t;
        c.k = 1.0 / q;
        c.m1 = -c.k;
        c.m2 = -1.0;
        break;
    case Kind::LowPass:
        c.g = t;
        c.k = 1.0 / q;
        c.m0 = 0.0;
        c.m2 = 1.0;
        break;
    }
    return c;
}

std::complex<double> evaluate(const Coefs& c, double hz, double sampleRate) {
    const std::complex<double> s(0.0, std::tan(kPiD * std::min(hz, sampleRate * 0.4999) / sampleRate) / c.g);
    const std::complex<double> d = s * s + c.k * s + 1.0;
    return c.m0 + (c.m1 * s + c.m2) / d;
}

enum P { LowType, LowFreq, LowGain, LowQ, M1Freq, M1Gain, M1Q, M2Freq, M2Gain, M2Q, HighType, HighFreq, HighGain, HighQ };

void designAll(const double* v, double sampleRate, Coefs (&out)[4]) {
    static constexpr Kind lowKinds[] = {Kind::LowShelf, Kind::Bell, Kind::HighPass};
    static constexpr Kind highKinds[] = {Kind::HighShelf, Kind::Bell, Kind::LowPass};
    out[0] = design(lowKinds[std::clamp(static_cast<int>(v[LowType]), 0, 2)], v[LowFreq], v[LowGain], qFromAmount(v[LowQ]),
                    sampleRate);
    out[1] = design(Kind::Bell, v[M1Freq], v[M1Gain], qFromAmount(v[M1Q]), sampleRate);
    out[2] = design(Kind::Bell, v[M2Freq], v[M2Gain], qFromAmount(v[M2Q]), sampleRate);
    out[3] = design(highKinds[std::clamp(static_cast<int>(v[HighType]), 0, 2)], v[HighFreq], v[HighGain],
                    qFromAmount(v[HighQ]), sampleRate);
}

class Eq final : public Effect {
public:
    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        clear();
    }
    void clear() noexcept override {
        for (auto& b : bands_)
            for (int c = 0; c < 2; ++c)
                b.ic1[c] = b.ic2[c] = 0.0f;
    }
    void set(const double* v, const Context&, bool snap) noexcept override {
        Coefs coefs[4];
        designAll(v, sampleRate_, coefs);
        for (int i = 0; i < 4; ++i) {
            Band& b = bands_[i];
            b.g.aim(static_cast<float>(coefs[i].g), snap);
            b.k.aim(static_cast<float>(coefs[i].k), snap);
            b.m0.aim(static_cast<float>(coefs[i].m0), snap);
            b.m1.aim(static_cast<float>(coefs[i].m1), snap);
            b.m2.aim(static_cast<float>(coefs[i].m2), snap);
            b.update();
        }
    }
    void process(float* left, float* right, int frames) noexcept override {
        for (auto& b : bands_) {
            const bool moving = b.g.moving() || b.k.moving() || b.m0.moving() || b.m1.moving() || b.m2.moving();
            if (!moving && b.m0.now == 1.0f && b.m1.now == 0.0f && b.m2.now == 0.0f) {
                b.ic1[0] = b.ic1[1] = b.ic2[0] = b.ic2[1] = 0.0f; // a flat bell or shelf: nothing to do
                continue;
            }
            for (int s = 0; s < frames; ++s) {
                if (moving) {
                    b.g.next();
                    b.k.next();
                    b.m0.next();
                    b.m1.next();
                    b.m2.next();
                    b.update();
                }
                left[s] = b.run(0, left[s]);
                right[s] = b.run(1, right[s]);
            }
        }
    }

private:
    struct Band {
        Ramp g, k, m0, m1, m2;
        float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        float ic1[2] = {}, ic2[2] = {};
        void update() noexcept {
            a1 = 1.0f / (1.0f + g.now * (g.now + k.now));
            a2 = g.now * a1;
            a3 = g.now * a2;
        }
        float run(int c, float x) noexcept {
            const float v3 = x - ic2[c];
            const float v1 = a1 * ic1[c] + a2 * v3;
            const float v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
            ic1[c] = 2.0f * v1 - ic1[c];
            ic2[c] = 2.0f * v2 - ic2[c];
            return m0.now * x + m1.now * v1 + m2.now * v2;
        }
    };
    double sampleRate_ = 48000.0;
    Band bands_[4];
};

// --- Filter -------------------------------------------------------------------------------------

enum F { Type, Cutoff, Resonance, Drive, Character, StereoOffset };

flt::Settings filterSettings(const double* v) {
    flt::Settings s;
    s.type = std::clamp(static_cast<int>(v[Type]), 0, flt::typeCount() - 1);
    const double spread = std::exp2(v[StereoOffset]);
    s.cutoffHz[0] = static_cast<float>(v[Cutoff] / spread);
    s.cutoffHz[1] = static_cast<float>(v[Cutoff] * spread);
    s.resonance = static_cast<float>(v[Resonance]);
    s.drive = static_cast<float>(v[Drive]);
    s.character = static_cast<float>(v[Character]);
    s.mix = 1.0f;
    return s;
}

// A change of filter type or of quality makes the voice filter start afresh; the effect keeps a
// second filter, starts it with the new settings, lets it settle and crossfades to it.
class FilterFx final : public Effect {
public:
    static constexpr int kWarmUp = 64, kFade = 256;

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        const int length = flt::delayLengthFor(sampleRate);
        for (int i = 0; i < 2; ++i) {
            memory_[i].assign(static_cast<std::size_t>(2 * length), 0.0f);
            filter_[i].prepare(sampleRate, memory_[i].data(), length);
        }
        clear();
    }
    void clear() noexcept override {
        for (auto& f : filter_)
            f.reset();
        sinceAim_ = 0;
        aimed_ = false;
        fade_ = 0;
    }
    void set(const double* v, const Context& ctx, bool snap) noexcept override {
        halfBands_ = ctx.halfBands;
        const flt::Settings s = filterSettings(v);
        const int oversample = std::clamp(ctx.oversample, 1, 8);
        if (aimed_)
            for (auto& f : filter_)
                f.settle();
        const bool restart = s.type != type_ || oversample != oversample_;
        if (snap || !started_) {
            filter_[active_].aim(s, kMaxFrames, oversample, true);
            fade_ = 0;
        } else if (restart && fade_ == 0) {
            const int next = 1 - active_;
            filter_[next].reset();
            filter_[next].aim(s, kMaxFrames, oversample, true);
            flt::Settings old = s; // the outgoing filter keeps its type and rate while it fades
            old.type = type_;
            filter_[active_].aim(old, kMaxFrames, oversample_, false);
            fade_ = kWarmUp + kFade;
        } else {
            if (fade_ > 0) {
                flt::Settings old = s;
                old.type = outgoingType_;
                filter_[active_].aim(old, kMaxFrames, outgoingRate_, false);
                filter_[1 - active_].aim(s, kMaxFrames, oversample, false);
            } else {
                filter_[active_].aim(s, kMaxFrames, oversample, false);
            }
        }
        if (fade_ > 0 && restart) {
            outgoingType_ = type_;
            outgoingRate_ = oversample_;
        }
        type_ = s.type;
        oversample_ = oversample;
        started_ = true;
        aimed_ = true;
        sinceAim_ = 0;
    }
    void process(float* left, float* right, int frames) noexcept override {
        if (halfBands_ == nullptr)
            return;
        if (fade_ > 0) {
            float l2[kMaxFrames], r2[kMaxFrames];
            std::copy(left, left + frames, l2);
            std::copy(right, right + frames, r2);
            filter_[1 - active_].process(l2, r2, frames, nullptr, *halfBands_);
            filter_[active_].process(left, right, frames, nullptr, *halfBands_);
            for (int s = 0; s < frames; ++s) {
                const int remaining = fade_ - s;
                const float w = remaining > kFade ? 0.0f : 1.0f - static_cast<float>(std::max(remaining, 0)) / kFade;
                left[s] += w * (l2[s] - left[s]);
                right[s] += w * (r2[s] - right[s]);
            }
            fade_ = std::max(fade_ - frames, 0);
            if (fade_ == 0)
                active_ = 1 - active_;
        } else {
            filter_[active_].process(left, right, frames, nullptr, *halfBands_);
        }
        sinceAim_ += frames;
        if (aimed_ && sinceAim_ >= kMaxFrames) {
            for (auto& f : filter_)
                f.settle(); // the ramps have arrived: hold them there until the next change
            aimed_ = false;
        }
    }
    int holdSamples() const noexcept override { return static_cast<int>(sampleRate_ * 0.05); }

private:
    double sampleRate_ = 48000.0;
    std::vector<float> memory_[2];
    flt::VoiceFilter filter_[2];
    const dsp::DecimatorDesign* halfBands_ = nullptr;
    int active_ = 0, fade_ = 0, sinceAim_ = 0;
    int type_ = -1, oversample_ = 0, outgoingType_ = -1, outgoingRate_ = 1;
    bool aimed_ = false, started_ = false;
};

} // namespace

bool eqResponse(const double* values, double sampleRate, double hz, std::complex<double>& out) {
    Coefs coefs[4];
    designAll(values, sampleRate, coefs);
    out = 1.0;
    for (const auto& c : coefs)
        out *= evaluate(c, hz, sampleRate);
    return true;
}

bool filterFxResponse(const double* values, double sampleRate, double hz, std::complex<double>& out) {
    out = flt::response(filterSettings(values), 0, sampleRate, hz, 1);
    return true;
}

std::unique_ptr<Effect> makeEq() { return std::make_unique<Eq>(); }
std::unique_ptr<Effect> makeFilterFx() { return std::make_unique<FilterFx>(); }

const char* const kEqParams[] = {"low_type",  "low_freq",  "low_gain",  "low_q",     "mid1_freq",
                                 "mid1_gain", "mid1_q",    "mid2_freq", "mid2_gain", "mid2_q",
                                 "high_type", "high_freq", "high_gain", "high_q"};
const std::size_t kEqParamCount = std::size(kEqParams);
const char* const kFilterFxParams[] = {"filter_type", "cutoff", "resonance", "drive", "character", "stereo_offset"};
const std::size_t kFilterFxParamCount = std::size(kFilterFxParams);

} // namespace zed::fx
