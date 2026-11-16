// The modulation effects (plan 3.8): Chorus, Flanger, Phaser and Swarm.
//
// Chorus: up to six voices, each a delay line read at a moving position. Their base delays spread
// from Delay 1 to Delay 2, their sine modulators run at slightly different rates and phases, and
// they alternate between the left and the right line and side, so the ensemble is wide and never
// beats in step. Feedback is damped and soft-limited inside the loop; a low- or high-pass shapes
// the wet signal. Reads are cubic (linear in Draft).
//
// Flanger and Phaser give the classic comb or notch sound at full mix: their wet signal is the
// dry signal plus the delayed or all-passed one.
//
// Swarm: up to seven copies, each shifted by a few cents with a crossfaded pair of moving taps (a
// small pitch shifter), drifting slowly around their offsets.
#include "FxDsp.h"
#include "FxImpl.h"

#include <array>

namespace zed::fx {
namespace {

inline float lfoSin(double phase) noexcept { return static_cast<float>(std::sin(2.0 * kPiD * phase)); }

// --- Chorus -------------------------------------------------------------------------------------

class Chorus final : public Effect {
public:
    enum P { Voices, Rate, Sync, Division, Delay1, Delay2, Depth, Feedback, FilterType, FilterCutoff };
    static constexpr int kMaxVoices = 6;

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        for (auto& line : line_)
            line.prepare(static_cast<int>(sampleRate * 0.08) + 8);
        damp_[0].design(9000.0, sampleRate);
        damp_[1] = damp_[0];
        clear();
    }
    void clear() noexcept override {
        for (auto& line : line_)
            line.clear();
        for (int c = 0; c < 2; ++c) {
            feedbackState_[c] = 0.0f;
            damp_[c].s = 0.0f;
            wet_[c].reset();
        }
        for (int v = 0; v < kMaxVoices; ++v)
            phase_[v] = static_cast<double>(v) / kMaxVoices;
    }
    void set(const double* v, const Context& ctx, bool snap) noexcept override {
        voices_ = std::clamp(static_cast<int>(v[Voices]), 2, kMaxVoices);
        rateHz_ = static_cast<int>(v[Sync]) != 0 ? syncedHz(static_cast<int>(v[Division]), ctx.bpm) : v[Rate];
        delayTarget_[0] = static_cast<float>(v[Delay1]);
        delayTarget_[1] = static_cast<float>(v[Delay2]);
        if (snap) {
            delayNow_[0] = delayTarget_[0];
            delayNow_[1] = delayTarget_[1];
        }
        depth_.aim(static_cast<float>(v[Depth]), snap);
        feedback_.aim(static_cast<float>(std::min(v[Feedback], 0.95)), snap);
        highPass_ = static_cast<int>(v[FilterType]) == 1;
        for (auto& f : wet_)
            f.design(v[FilterCutoff], 0.70710678, sampleRate_);
        cubic_ = !ctx.draft;
    }
    void process(float* left, float* right, int frames) noexcept override {
        const int n = voices_;
        const float msToSamples = static_cast<float>(sampleRate_ * 0.001);
        // Each voice's delay at both ends of the call; straight lines in between.
        // The delay times glide slowly (about 60 ms), like tape: a new time bends the pitch, never clicks.
        const float d1a = delayNow_[0], d2a = delayNow_[1], deptha = depth_.now, fba = feedback_.now;
        const float glide = 1.0f - std::exp(-static_cast<float>(frames) / static_cast<float>(0.06 * sampleRate_));
        for (float& d : delayNow_)
            d += (delayTarget_[&d - delayNow_] - d) * glide;
        for (int s = 0; s < frames; ++s) {
            depth_.next();
            feedback_.next();
        }
        const float d1b = delayNow_[0], d2b = delayNow_[1], depthb = depth_.now, fbb = feedback_.now;
        float delayStart[kMaxVoices], delayStep[kMaxVoices], pan[kMaxVoices][2];
        int side[kMaxVoices];
        int perSide[3] = {0, 0, 0};
        for (int v = 0; v < n; ++v) {
            const float spread = n > 1 ? static_cast<float>(v) / static_cast<float>(n - 1) : 0.0f;
            const double rate = rateHz_ * (1.0 + 0.045 * (v - 0.5 * (n - 1)) / std::max(1.0, 0.5 * (n - 1)));
            const double inc = rate / sampleRate_;
            const float modA = lfoSin(phase_[v]);
            phase_[v] += inc * frames;
            phase_[v] -= std::floor(phase_[v]);
            const float modB = lfoSin(phase_[v]);
            const float baseA = d1a + (d2a - d1a) * spread, baseB = d1b + (d2b - d1b) * spread;
            const float a = std::max(baseA * (1.0f + 0.5f * deptha * modA) * msToSamples, 2.0f);
            const float b = std::max(baseB * (1.0f + 0.5f * depthb * modB) * msToSamples, 2.0f);
            delayStart[v] = a;
            delayStep[v] = (b - a) / static_cast<float>(frames);
            // Alternate sides; with an odd count the last voice sits in the middle.
            const float p = (n % 2 == 1 && v == n - 1) ? 0.0f : (v % 2 == 0 ? -0.8f : 0.8f);
            side[v] = p < 0.0f ? 0 : p > 0.0f ? 1 : 2;
            ++perSide[side[v]];
            const float angle = (p + 1.0f) * 0.25f * kPiF;
            pan[v][0] = std::cos(angle);
            pan[v][1] = std::sin(angle);
        }
        const float norm = std::sqrt(2.0f / static_cast<float>(n));
        const float fbNorm[2] = {1.0f / static_cast<float>(std::max(perSide[0] + perSide[2], 1)),
                                 1.0f / static_cast<float>(std::max(perSide[1] + perSide[2], 1))};
        float fb = fba;
        const float fbStep = (fbb - fba) / static_cast<float>(frames);
        for (int s = 0; s < frames; ++s) {
            float out[2] = {0.0f, 0.0f}, loop[2] = {0.0f, 0.0f};
            for (int v = 0; v < n; ++v) {
                const double d = delayStart[v] + delayStep[v] * static_cast<float>(s);
                float x;
                if (side[v] == 2)
                    x = 0.5f * (line_[0].read(d, cubic_) + line_[1].read(d, cubic_));
                else
                    x = line_[side[v]].read(d, cubic_);
                out[0] += pan[v][0] * x;
                out[1] += pan[v][1] * x;
                if (side[v] != 1)
                    loop[0] += x;
                if (side[v] != 0)
                    loop[1] += x;
            }
            line_[0].push(left[s] + feedbackState_[0]); // after the reads: the delays are exact
            line_[1].push(right[s] + feedbackState_[1]);
            for (int c = 0; c < 2; ++c)
                feedbackState_[c] = damp_[c].lowpass(softSat(loop[c] * fbNorm[c] * fb));
            fb += fbStep;
            float band, high;
            const float lowL = wet_[0].run(out[0] * norm, band, high);
            left[s] = highPass_ ? high : lowL;
            const float lowR = wet_[1].run(out[1] * norm, band, high);
            right[s] = highPass_ ? high : lowR;
        }
    }
    int holdSamples() const noexcept override { return static_cast<int>(sampleRate_ * 0.1); }

private:
    double sampleRate_ = 48000.0, rateHz_ = 0.6;
    int voices_ = 4;
    bool highPass_ = false, cubic_ = true;
    DelayLine line_[2];
    double phase_[kMaxVoices] = {};
    float delayNow_[2] = {6.0f, 14.0f}, delayTarget_[2] = {6.0f, 14.0f};
    Ramp depth_, feedback_;
    float feedbackState_[2] = {};
    OnePole damp_[2];
    Svf wet_[2];
};

// --- Flanger ------------------------------------------------------------------------------------

class Flanger final : public Effect {
public:
    enum P { Rate, Sync, Division, Depth, Feedback, StereoPhase };

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        for (auto& line : line_)
            line.prepare(static_cast<int>(sampleRate * 0.02) + 8);
        clear();
    }
    void clear() noexcept override {
        for (auto& line : line_)
            line.clear();
        phase_ = 0.0;
        last_[0] = last_[1] = 0.0f;
    }
    void set(const double* v, const Context& ctx, bool snap) noexcept override {
        rateHz_ = static_cast<int>(v[Sync]) != 0 ? syncedHz(static_cast<int>(v[Division]), ctx.bpm) : v[Rate];
        depth_.aim(static_cast<float>(v[Depth]), snap);
        feedback_.aim(static_cast<float>(std::clamp(v[Feedback], -0.97, 0.97)), snap);
        stereo_.aim(static_cast<float>(v[StereoPhase] / 360.0), snap);
        cubic_ = !ctx.draft;
    }
    void process(float* left, float* right, int frames) noexcept override {
        float* io[2] = {left, right};
        const float minDelay = static_cast<float>(sampleRate_ * 0.0002); // 0.2 ms
        float delayA[2], delayB[2];
        const float depthA = depth_.now, stereoA = stereo_.now;
        const double phaseA = phase_;
        phase_ += rateHz_ / sampleRate_ * frames;
        phase_ -= std::floor(phase_);
        float fbStart = feedback_.now;
        for (int s = 0; s < frames; ++s) {
            depth_.next();
            stereo_.next();
            feedback_.next();
        }
        // The delay sweeps exponentially, up to six octaves above 0.2 ms at full depth.
        for (int c = 0; c < 2; ++c) {
            const float offA = c == 1 ? stereoA : 0.0f, offB = c == 1 ? stereo_.now : 0.0f;
            delayA[c] = std::max(minDelay * std::exp2(depthA * 6.0f * (0.5f + 0.5f * lfoSin(phaseA + offA))), 2.0f);
            delayB[c] = std::max(minDelay * std::exp2(depth_.now * 6.0f * (0.5f + 0.5f * lfoSin(phase_ + offB))), 2.0f);
        }
        const float fbStep = (feedback_.now - fbStart) / static_cast<float>(frames);
        for (int c = 0; c < 2; ++c) {
            const float step = (delayB[c] - delayA[c]) / static_cast<float>(frames);
            float fb = fbStart;
            for (int s = 0; s < frames; ++s) {
                const float x = io[c][s];
                const float y = line_[c].read(delayA[c] + step * static_cast<float>(s), cubic_); // before the push: exact delay
                line_[c].push(x + fb * softSat(y));
                io[c][s] = 0.5f * (x + y);
                fb += fbStep;
            }
        }
    }
    int holdSamples() const noexcept override { return static_cast<int>(sampleRate_ * 0.2); }

private:
    double sampleRate_ = 48000.0, rateHz_ = 0.3, phase_ = 0.0;
    bool cubic_ = true;
    DelayLine line_[2];
    Ramp depth_, feedback_, stereo_;
    float last_[2] = {};
};

// --- Phaser -------------------------------------------------------------------------------------

class Phaser final : public Effect {
public:
    enum P { Stages, Rate, Sync, Division, Depth, Center, Feedback, StereoPhase };
    static constexpr int kMaxStages = 12;
    static constexpr int kStageFade = 256;

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        clear();
    }
    void clear() noexcept override {
        for (auto& ch : state_)
            std::fill(std::begin(ch), std::end(ch), 0.0f);
        last_[0] = last_[1] = 0.0f;
        phase_ = 0.0;
    }
    void set(const double* v, const Context& ctx, bool snap) noexcept override {
        const int stages = std::clamp(static_cast<int>(v[Stages]), 2, kMaxStages);
        if (snap) {
            stages_ = oldStages_ = stages;
            fade_ = 0;
        } else if (stages != stages_) {
            oldStages_ = stages_;
            stages_ = stages;
            fade_ = kStageFade;
        }
        rateHz_ = static_cast<int>(v[Sync]) != 0 ? syncedHz(static_cast<int>(v[Division]), ctx.bpm) : v[Rate];
        depth_.aim(static_cast<float>(v[Depth]), snap);
        center_.aim(static_cast<float>(std::log2(std::max(v[Center], 1.0))), snap);
        feedback_.aim(static_cast<float>(std::clamp(v[Feedback], -0.9, 0.9)), snap);
        stereo_.aim(static_cast<float>(v[StereoPhase] / 360.0), snap);
    }
    void process(float* left, float* right, int frames) noexcept override {
        float* io[2] = {left, right};
        const float depthA = depth_.now, centerA = center_.now, stereoA = stereo_.now, fbA = feedback_.now;
        const double phaseA = phase_;
        phase_ += rateHz_ / sampleRate_ * frames;
        phase_ -= std::floor(phase_);
        for (int s = 0; s < frames; ++s) {
            depth_.next();
            center_.next();
            stereo_.next();
            feedback_.next();
        }
        const float fbStep = (feedback_.now - fbA) / static_cast<float>(frames);
        const double top = sampleRate_ * 0.45;
        for (int c = 0; c < 2; ++c) {
            // The all-pass corner sweeps up to three octaves either side of the centre.
            const auto corner = [&](float centre, float depth, double phase) {
                const double hz = std::clamp(static_cast<double>(std::exp2(centre + depth * 3.0f * lfoSin(phase))), 20.0, top);
                const double t = std::tan(kPiD * hz / sampleRate_);
                return static_cast<float>(t / (1.0 + t));
            };
            const float gA = corner(centerA, depthA, phaseA + (c == 1 ? stereoA : 0.0f));
            const float gB = corner(center_.now, depth_.now, phase_ + (c == 1 ? stereo_.now : 0.0f));
            const float gStep = (gB - gA) / static_cast<float>(frames);
            float g = gA, fb = fbA;
            float* st = state_[c];
            for (int s = 0; s < frames; ++s) {
                const float x = io[c][s];
                float a = x + fb * last_[c];
                float taps[kMaxStages];
                for (int k = 0; k < kMaxStages; ++k) {
                    const float v = (a - st[k]) * g;
                    const float lp = v + st[k];
                    st[k] = lp + v;
                    a = 2.0f * lp - a;
                    taps[k] = a;
                }
                float out = taps[stages_ - 1];
                if (fade_ > 0) {
                    const float w = static_cast<float>(std::max(fade_ - s, 0)) / kStageFade;
                    out += w * (taps[oldStages_ - 1] - out);
                }
                last_[c] = softSat(out);
                io[c][s] = 0.5f * (x + out);
                g += gStep;
                fb += fbStep;
            }
        }
        fade_ = std::max(fade_ - frames, 0);
    }
    int holdSamples() const noexcept override { return static_cast<int>(sampleRate_ * 0.1); }

private:
    double sampleRate_ = 48000.0, rateHz_ = 0.4, phase_ = 0.0;
    int stages_ = 6, oldStages_ = 6, fade_ = 0;
    Ramp depth_, center_, feedback_, stereo_;
    float state_[2][kMaxStages] = {};
    float last_[2] = {};
};

// --- Swarm --------------------------------------------------------------------------------------

class Swarm final : public Effect {
public:
    enum P { Voices, Rate, Detune, Restart };
    static constexpr int kMaxVoices = 7;

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        window_ = static_cast<float>(sampleRate * 0.045);
        for (auto& line : line_)
            line.prepare(static_cast<int>(window_) + 16);
        clear();
    }
    void clear() noexcept override {
        for (auto& line : line_)
            line.clear();
        restartVoices();
    }
    void set(const double* v, const Context& ctx, bool snap) noexcept override {
        voices_ = std::clamp(static_cast<int>(v[Voices]), 1, kMaxVoices);
        rateHz_ = v[Rate];
        detune_.aim(static_cast<float>(v[Detune]), snap);
        restart_ = static_cast<int>(v[Restart]) != 0;
        cubic_ = !ctx.draft;
    }
    void noteOn() noexcept override {
        if (restart_)
            restartVoices();
    }
    void process(float* left, float* right, int frames) noexcept override {
        const int n = voices_;
        const float detuneA = detune_.now;
        for (int s = 0; s < frames; ++s)
            detune_.next();
        const float detune = 0.5f * (detuneA + detune_.now);
        float inc[kMaxVoices], gain[kMaxVoices][2];
        const auto cycles = static_cast<float>(rateHz_ / sampleRate_ * frames);
        for (int v = 0; v < n; ++v) {
            // Offsets spread evenly up to +/-25 cents at full detune, each wandering a third of that.
            const float spread = n == 1 ? 1.0f : 2.0f * static_cast<float>(v) / static_cast<float>(n - 1) - 1.0f;
            const float cents = detune * (25.0f * spread + 8.0f * wander_[v].advance(cycles));
            const float ratio = std::exp2(cents / 1200.0f);
            inc[v] = (1.0f - ratio) / window_; // the tap moves through the window at the shift's rate
            const float p = n == 1 ? 0.0f : 0.7f * spread;
            const float angle = (p + 1.0f) * 0.25f * kPiF;
            gain[v][0] = std::cos(angle);
            gain[v][1] = std::sin(angle);
        }
        const float norm = 1.0f / std::sqrt(static_cast<float>(n));
        for (int s = 0; s < frames; ++s) {
            line_[0].push(left[s]);
            line_[1].push(right[s]);
            float out[2] = {0.0f, 0.0f};
            for (int v = 0; v < n; ++v) {
                float p = phase_[v] + inc[v];
                p -= std::floor(p);
                phase_[v] = p;
                float q = p + 0.5f;
                if (q >= 1.0f)
                    q -= 1.0f;
                // Two taps half a window apart; each fades out where it jumps. w(p) = sin^2(pi p)
                // through a rational approximation; the two weights always sum to one.
                const float u = p * (1.0f - p);
                const float sine = 16.0f * u / (5.0f - 4.0f * u);
                const float wa = sine * sine;
                const float wb = 1.0f - wa;
                const double da = 2.0 + p * window_, db = 2.0 + q * window_;
                for (int c = 0; c < 2; ++c) {
                    const float y = wa * line_[c].read(da, cubic_) + wb * line_[c].read(db, cubic_);
                    out[c] += y * gain[v][c];
                }
            }
            // Each voice is a stereo copy panned with constant power; a centred voice keeps unit gain.
            left[s] = out[0] * norm * 1.41421356f;
            right[s] = out[1] * norm * 1.41421356f;
        }
    }
    int holdSamples() const noexcept override { return static_cast<int>(window_) + 32; }

private:
    void restartVoices() noexcept {
        for (int v = 0; v < kMaxVoices; ++v) {
            phase_[v] = std::fmod(0.37f * static_cast<float>(v), 1.0f);
            wander_[v].seed(0x9e3779b9u * static_cast<std::uint32_t>(v + 1));
        }
    }

    double sampleRate_ = 48000.0, rateHz_ = 0.5;
    float window_ = 2160.0f;
    int voices_ = 5;
    bool restart_ = false, cubic_ = true;
    DelayLine line_[2];
    Ramp detune_;
    float phase_[kMaxVoices] = {};
    Wander wander_[kMaxVoices];
};

} // namespace

std::unique_ptr<Effect> makeChorus() { return std::make_unique<Chorus>(); }
std::unique_ptr<Effect> makeFlanger() { return std::make_unique<Flanger>(); }
std::unique_ptr<Effect> makePhaser() { return std::make_unique<Phaser>(); }
std::unique_ptr<Effect> makeSwarm() { return std::make_unique<Swarm>(); }

const char* const kChorusParams[] = {"voices", "rate",   "sync",     "division",    "delay1",
                                     "delay2", "depth",  "feedback", "filter_type", "filter_cutoff"};
const std::size_t kChorusParamCount = std::size(kChorusParams);
const char* const kFlangerParams[] = {"rate", "sync", "division", "depth", "feedback", "stereo_phase"};
const std::size_t kFlangerParamCount = std::size(kFlangerParams);
const char* const kPhaserParams[] = {"stages", "rate", "sync", "division", "depth", "center", "feedback", "stereo_phase"};
const std::size_t kPhaserParamCount = std::size(kPhaserParams);
const char* const kSwarmParams[] = {"voices", "rate", "detune", "restart"};
const std::size_t kSwarmParamCount = std::size(kSwarmParams);

} // namespace zed::fx
