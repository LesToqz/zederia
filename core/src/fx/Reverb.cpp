// Reverb (plan 3.8, 5.7): a sixteen-line feedback delay network.
//
//  input -> pre-delay -> low cut, high cut -> four all-pass diffusers per side -> FDN
//                     \-> early reflections (Room, Hall)
//  FDN: sixteen lines of incommensurate lengths, each read at a slowly moving position (cubic
//  reads; linear in Draft), each followed by a one-pole absorption filter that sets its loss at
//  low and at high frequencies from Decay and Damping, mixed by a 16 x 16 Hadamard matrix.
//
// The diffusers turn a click into a dense burst before it enters the network, and the moving
// lines keep the network's modes from standing still, so no metallic ringing builds up. The two
// outputs are taken through two orthogonal rows of the Hadamard matrix: they are uncorrelated,
// and their sum (mono) keeps the full energy of the tail. Freeze makes the network lossless and
// closes the input.
#include "FxDsp.h"
#include "FxImpl.h"

#include <array>

namespace zed::fx {
namespace {

constexpr int kLines = 16;
constexpr int kDiffusers = 4;
constexpr int kTaps = 6;
constexpr int kModeFade = 512;

struct ModeShape {
    float shortestMs, longestMs;   // line lengths at Size 50 %
    float diffuserMs[2][kDiffusers];
    float diffusion;
    float earlyLevel;
    float earlyMs[2][kTaps];
};

const ModeShape kModes[3] = {
    // Plate: short dense lines, strong diffusion, no early reflections.
    {6.0f, 31.0f, {{1.7f, 2.9f, 4.3f, 6.1f}, {1.9f, 3.1f, 4.1f, 5.7f}}, 0.72f, 0.0f, {{}, {}}},
    // Hall: long lines, a gentler build-up, early reflections spread over 60 ms.
    {24.0f, 104.0f, {{3.1f, 4.7f, 7.3f, 10.3f}, {3.3f, 5.1f, 6.9f, 9.7f}}, 0.65f, 0.35f,
     {{11.0f, 19.0f, 27.0f, 36.0f, 47.0f, 58.0f}, {13.0f, 21.0f, 31.0f, 39.0f, 51.0f, 61.0f}}},
    // Room: short lines and prominent early reflections.
    {8.0f, 37.0f, {{2.3f, 3.7f, 5.3f, 7.9f}, {2.1f, 3.9f, 5.9f, 7.3f}}, 0.6f, 0.55f,
     {{3.1f, 6.7f, 9.9f, 13.3f, 17.9f, 23.0f}, {4.3f, 7.1f, 11.3f, 14.9f, 19.7f, 24.1f}}},
};
constexpr float kEarlyGain[kTaps] = {0.8f, -0.62f, 0.5f, -0.4f, 0.31f, -0.24f};

void hadamard16(float* x) noexcept {
    for (int h = 1; h < kLines; h *= 2)
        for (int i = 0; i < kLines; i += 2 * h)
            for (int j = i; j < i + h; ++j) {
                const float a = x[j], b = x[j + h];
                x[j] = a + b;
                x[j + h] = a - b;
            }
    for (int i = 0; i < kLines; ++i)
        x[i] *= 0.25f;
}

// Sign of row r, column c of the 16 x 16 Sylvester Hadamard matrix.
float hadamardSign(int r, int c) noexcept {
    int bits = r & c, parity = 0;
    while (bits != 0) {
        parity ^= bits & 1;
        bits >>= 1;
    }
    return parity != 0 ? -1.0f : 1.0f;
}

class Reverb final : public Effect {
public:
    enum P { Mode, Size, Decay, Predelay, LowCut, HighCut, Damping, Modulation, Width, Freeze };

    Reverb() {
        for (int i = 0; i < kLines; ++i) {
            outL_[i] = hadamardSign(1, i) * 0.25f;
            outR_[i] = hadamardSign(2, i) * 0.25f;
            inSign_[i] = hadamardSign(3, i);
            const double golden = 0.6180339887 * (i + 1);
            lfoRate_[i] = 0.22 + 0.85 * (golden - std::floor(golden));
            const double phase = 0.3819660113 * (i + 1);
            lfoPhase_[i] = phase - std::floor(phase);
        }
    }

    void prepare(double sampleRate) override {
        sampleRate_ = sampleRate;
        const double ms = sampleRate / 1000.0;
        for (auto& line : lines_)
            line.prepare(static_cast<int>(104.0 * 2.0 * 1.1 * ms) + 64);
        for (auto& side : diffusers_)
            for (auto& d : side)
                d.prepare(static_cast<int>(12.0 * ms) + 8);
        for (auto& line : predelay_)
            line.prepare(static_cast<int>((500.0 + 90.0) * ms) + 8); // pre-delay plus the latest early reflection
        clear();
    }

    void clear() noexcept override {
        for (auto& line : lines_)
            line.clear();
        for (auto& side : diffusers_)
            for (auto& d : side)
                d.clear();
        for (auto& line : predelay_)
            line.clear();
        for (int c = 0; c < 2; ++c) {
            lowCut_[c].reset();
            highCut_[c].reset();
        }
        std::fill(std::begin(absorb_), std::end(absorb_), 0.0f);
        for (int i = 0; i < kLines; ++i)
            length_[i] = lengthTarget_[i];
    }

    void set(const double* v, const Context& ctx, bool snap) noexcept override {
        const int mode = std::clamp(static_cast<int>(v[Mode]), 0, 2);
        if (snap) {
            mode_ = pendingMode_ = mode;
            modeFade_ = 0;
        } else if (mode != pendingMode_) {
            pendingMode_ = mode;
            modeFade_ = kModeFade; // fade the tail out, switch, and let the new space build
        }
        size_ = v[Size];
        decayMs_ = v[Decay];
        damping_ = v[Damping];
        frozen_ = static_cast<int>(v[Freeze]) != 0;
        predelay_ms_ = v[Predelay];
        for (int c = 0; c < 2; ++c) {
            lowCut_[c].design(v[LowCut], 0.70710678, sampleRate_);
            highCut_[c].design(v[HighCut], 0.70710678, sampleRate_);
        }
        modDepth_ = v[Modulation] * 0.0004 * sampleRate_;
        width_.aim(static_cast<float>(v[Width]), snap);
        // Longer decays fill up more: a gentle input trim keeps steady material at a similar level.
        const double trim = std::clamp(std::pow(2000.0 / std::max(decayMs_, 1.0), 0.25), 0.35, 1.0);
        input_.aim(frozen_ ? 0.0f : static_cast<float>(trim * 0.35), snap);
        predelaySamples_.aim(static_cast<float>(predelay_ms_ * 0.001 * sampleRate_), snap);
        cubic_ = !ctx.draft;
        updateShape(snap);
    }

    void process(float* left, float* right, int frames) noexcept override {
        const float erStart = erScale_;
        float lengthStart[kLines];
        std::copy(std::begin(length_), std::end(length_), lengthStart);
        // Line lengths glide toward their targets (a change of Size bends the tail, never clicks).
        if (lengthsMoving_) {
            bool moving = false;
            const float k = std::min(1.0f, static_cast<float>(frames) * lengthGlide_);
            erScale_ += (erTarget_ - erScale_) * k; // early reflections move with the room
            if (std::abs(erTarget_ - erScale_) > 1.0e-4f)
                moving = true;
            else
                erScale_ = erTarget_;
            for (int i = 0; i < kLines; ++i) {
                length_[i] += (lengthTarget_[i] - length_[i]) * k;
                if (std::abs(lengthTarget_[i] - length_[i]) > 0.01f)
                    moving = true;
                else
                    length_[i] = lengthTarget_[i];
            }
            lengthsMoving_ = moving;
            updateLoss();
        }
        // Each line's read position at both ends of the call.
        float readA[kLines], readStep[kLines];
        for (int i = 0; i < kLines; ++i) {
            const float a = modDepth_ > 0.0 ? static_cast<float>(modDepth_ * std::sin(2.0 * kPiD * lfoPhase_[i])) : 0.0f;
            lfoPhase_[i] += lfoRate_[i] / sampleRate_ * frames;
            lfoPhase_[i] -= std::floor(lfoPhase_[i]);
            const float b = modDepth_ > 0.0 ? static_cast<float>(modDepth_ * std::sin(2.0 * kPiD * lfoPhase_[i])) : 0.0f;
            readA[i] = lengthStart[i] + a; // both the glide of the length and the modulation move per sample
            readStep[i] = (length_[i] + b - readA[i]) / static_cast<float>(frames);
        }
        const ModeShape& shape = kModes[mode_];
        const float msToSamples = static_cast<float>(sampleRate_ * 0.001);
        for (int s = 0; s < frames; ++s) {
            const float gainIn = input_.next();
            const float pre = predelaySamples_.next();
            const float er = erStart + (erScale_ - erStart) * static_cast<float>(s) / static_cast<float>(frames);
            float x[2] = {left[s], right[s]};
            float early[2] = {0.0f, 0.0f};
            for (int c = 0; c < 2; ++c) {
                float y = pre >= 1.0f ? predelay_[c].read(pre, false) : x[c]; // read before the push: exact delay
                y = highCut_[c].lowpass(lowCut_[c].highpass(y));
                if (shape.earlyLevel > 0.0f)
                    for (int t = 0; t < kTaps; ++t)
                        early[c] += kEarlyGain[t] * predelay_[c].read(std::max(pre, 1.0f) + shape.earlyMs[c][t] * er * msToSamples, false);
                predelay_[c].push(x[c]);
                // Input diffusion: Schroeder all-passes.
                for (int k = 0; k < kDiffusers; ++k) {
                    DelayLine& d = diffusers_[c][k];
                    const float delayed = d.at(diffLength_[c][k]);
                    const float w = y + diffusion_ * delayed;
                    y = delayed - diffusion_ * w;
                    d.push(w);
                }
                x[c] = y * gainIn;
            }
            float z[kLines];
            for (int i = 0; i < kLines; ++i) {
                const float y = lines_[i].read(readA[i] + readStep[i] * static_cast<float>(s), cubic_);
                absorb_[i] = lossB_[i] * y + lossP_[i] * absorb_[i];
                z[i] = absorb_[i];
            }
            float outL = 0.0f, outR = 0.0f;
            for (int i = 0; i < kLines; ++i) {
                outL += outL_[i] * z[i];
                outR += outR_[i] * z[i];
            }
            hadamard16(z);
            for (int i = 0; i < kLines; ++i)
                lines_[i].push(z[i] + inSign_[i] * x[i & 1]);
            outL += early[0] * shape.earlyLevel * 0.3f;
            outR += early[1] * shape.earlyLevel * 0.3f;
            // Width on the mid and side of the wet signal.
            const float w = width_.next();
            const float mid = 0.5f * (outL + outR), side = 0.5f * (outL - outR) * w;
            float gainOut = 1.0f;
            if (modeFade_ > 0) {
                gainOut = std::abs(static_cast<float>(modeFade_ - kModeFade / 2)) / (kModeFade / 2);
                if (--modeFade_ == kModeFade / 2) { // silent: switch the space now
                    mode_ = pendingMode_;
                    clearTank();
                    updateShape(true);
                }
            }
            left[s] = (mid + side) * gainOut;
            right[s] = (mid - side) * gainOut;
        }
    }

    int holdSamples() const noexcept override { return static_cast<int>((predelay_ms_ + 120.0) * 0.001 * sampleRate_); }
    bool mayIdle() const noexcept override { return !frozen_; }

private:
    void clearTank() noexcept {
        for (auto& line : lines_)
            line.clear();
        for (auto& side : diffusers_)
            for (auto& d : side)
                d.clear();
        std::fill(std::begin(absorb_), std::end(absorb_), 0.0f);
    }

    // Line lengths and the diffusers for the mode and size; the losses for the decay and damping.
    void updateShape(bool snap) noexcept {
        const ModeShape& shape = kModes[mode_];
        const double scale = std::exp2((std::clamp(size_, 0.0, 1.0) - 0.5) * 2.0); // 0.5x .. 2x
        erTarget_ = static_cast<float>(std::sqrt(scale));
        if (snap)
            erScale_ = erTarget_;
        const double ms = sampleRate_ * 0.001;
        for (int i = 0; i < kLines; ++i) {
            const double t = static_cast<double>(i) / (kLines - 1);
            const double jitter = 1.0 + 0.06 * std::sin(2.4 * (i + 1));
            const double lineMs = shape.shortestMs * std::pow(shape.longestMs / shape.shortestMs, t) * jitter * scale;
            lengthTarget_[i] = static_cast<float>(std::max(lineMs * ms, 4.0));
            if (snap)
                length_[i] = lengthTarget_[i];
        }
        lengthsMoving_ = !snap;
        lengthGlide_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.15 * sampleRate_)));
        diffusion_ = shape.diffusion;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < kDiffusers; ++k)
                diffLength_[c][k] = std::max(static_cast<int>(shape.diffuserMs[c][k] * ms), 1);
        updateLoss();
    }

    void updateLoss() noexcept {
        if (frozen_) {
            for (int i = 0; i < kLines; ++i) {
                lossB_[i] = 1.0f;
                lossP_[i] = 0.0f;
            }
            return;
        }
        const double t60 = std::max(decayMs_, 10.0) * 0.001 * sampleRate_;
        const double t60High = t60 * (1.0 - 0.9 * std::clamp(damping_, 0.0, 1.0));
        for (int i = 0; i < kLines; ++i) {
            const double gDc = std::pow(10.0, -3.0 * length_[i] / t60);
            const double gNy = std::pow(10.0, -3.0 * length_[i] / t60High);
            const double p = (gDc - gNy) / (gDc + gNy);
            lossP_[i] = static_cast<float>(p);
            lossB_[i] = static_cast<float>(gDc * (1.0 - p));
        }
    }

    double sampleRate_ = 48000.0;
    int mode_ = 1, pendingMode_ = 1, modeFade_ = 0;
    double size_ = 0.5, decayMs_ = 2000.0, damping_ = 0.4, predelay_ms_ = 10.0, modDepth_ = 0.0;
    bool frozen_ = false, cubic_ = true, lengthsMoving_ = false;
    DelayLine lines_[kLines];
    DelayLine diffusers_[2][kDiffusers];
    DelayLine predelay_[2];
    int diffLength_[2][kDiffusers] = {};
    float diffusion_ = 0.65f, erScale_ = 1.0f, erTarget_ = 1.0f;
    float length_[kLines] = {}, lengthTarget_[kLines] = {}, lengthGlide_ = 0.0001f;
    float lossB_[kLines] = {}, lossP_[kLines] = {}, absorb_[kLines] = {};
    float outL_[kLines] = {}, outR_[kLines] = {}, inSign_[kLines] = {};
    double lfoRate_[kLines] = {}, lfoPhase_[kLines] = {};
    Svf lowCut_[2], highCut_[2];
    Ramp width_, input_, predelaySamples_;
};

} // namespace

std::unique_ptr<Effect> makeReverb() { return std::make_unique<Reverb>(); }

const char* const kReverbParams[] = {"mode",     "size",    "decay",   "predelay",   "low_cut",
                                     "high_cut", "damping", "modulation", "width",   "freeze"};
const std::size_t kReverbParamCount = std::size(kReverbParams);

} // namespace zed::fx
