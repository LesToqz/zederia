// Small building blocks shared by the effects: ramps, delay lines with fractional reads,
// zero-delay-feedback filter sections, envelope helpers. Private to the core.
#pragma once

#include "zederia/fx/Effect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace zed::fx {

inline constexpr float kPiF = 3.14159265358979f;
inline constexpr double kPiD = 3.14159265358979323846;
inline constexpr float kSilent = 1.0e-7f; // -140 dB: what the rack treats as silence

// Moves to a target in a straight line over one control interval (kMaxFrames samples), then
// holds it exactly.
struct Ramp {
    float now = 0.0f, target = 0.0f, step = 0.0f;
    int left = 0;
    void aim(float value, bool snap) noexcept {
        target = value;
        if (snap || value == now) {
            now = value;
            left = 0;
            step = 0.0f;
        } else {
            left = kMaxFrames;
            step = (value - now) / static_cast<float>(kMaxFrames);
        }
    }
    float next() noexcept {
        if (left > 0) {
            now += step;
            if (--left == 0)
                now = target;
        }
        return now;
    }
    bool moving() const noexcept { return left > 0; }
};

// A circular delay line. Reads look back from the next sample to be written: a delay of 1 is the
// sample written last. Linear reads need a delay of at least 1, cubic reads at least 2.
class DelayLine {
public:
    void prepare(int maxDelay) {
        int size = 4;
        while (size < maxDelay + 4)
            size <<= 1;
        buffer_.assign(static_cast<std::size_t>(size), 0.0f);
        mask_ = size - 1;
        write_ = 0;
    }
    void clear() noexcept { std::fill(buffer_.begin(), buffer_.end(), 0.0f); }
    int capacity() const noexcept { return mask_ - 3; }
    void push(float x) noexcept {
        buffer_[static_cast<std::size_t>(write_)] = x;
        write_ = (write_ + 1) & mask_;
    }
    float at(int delay) const noexcept { return buffer_[static_cast<std::size_t>((write_ - delay) & mask_)]; }
    // Fractional reads: whole and fractional parts given apart so long lines keep their precision.
    float linear(int whole, float frac) const noexcept {
        const int i = write_ - whole - 1;
        const float a = buffer_[static_cast<std::size_t>(i & mask_)];
        const float b = buffer_[static_cast<std::size_t>((i + 1) & mask_)];
        return b + frac * (a - b);
    }
    float cubic(int whole, float frac) const noexcept {
        const int i = write_ - whole - 1;
        const float t = 1.0f - frac;
        const float xm1 = buffer_[static_cast<std::size_t>((i - 1) & mask_)];
        const float x0 = buffer_[static_cast<std::size_t>(i & mask_)];
        const float x1 = buffer_[static_cast<std::size_t>((i + 1) & mask_)];
        const float x2 = buffer_[static_cast<std::size_t>((i + 2) & mask_)];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }
    float read(double delay, bool cubicRead) const noexcept {
        const int whole = static_cast<int>(delay);
        const auto frac = static_cast<float>(delay - whole);
        return cubicRead ? cubic(whole, frac) : linear(whole, frac);
    }

private:
    std::vector<float> buffer_;
    int mask_ = 0, write_ = 0;
};

// One channel of a zero-delay-feedback state-variable filter (two poles). Outputs per sample:
// low, band (unit peak gain when multiplied by k) and high.
struct Svf {
    float ic1 = 0.0f, ic2 = 0.0f;
    float g = 0.1f, k = 1.4142f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    void design(double hz, double q, double sampleRate) noexcept {
        const double f = std::clamp(hz, 1.0, sampleRate * 0.49);
        setG(static_cast<float>(std::tan(kPiD * f / sampleRate)), static_cast<float>(1.0 / std::max(q, 0.025)));
    }
    void setG(float gain, float damping) noexcept {
        g = gain;
        k = damping;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }
    // Returns low; band and high through the references.
    float run(float x, float& band, float& high) noexcept {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        band = v1;
        high = x - k * v1 - v2;
        return v2;
    }
    float lowpass(float x) noexcept {
        float b, h;
        return run(x, b, h);
    }
    float highpass(float x) noexcept {
        float b, h;
        run(x, b, h);
        return h;
    }
};

// A trapezoidal one-pole: low and high outputs.
struct OnePole {
    float g = 0.5f, s = 0.0f;
    void design(double hz, double sampleRate) noexcept {
        const double t = std::tan(kPiD * std::clamp(hz, 0.1, sampleRate * 0.49) / sampleRate);
        g = static_cast<float>(t / (1.0 + t));
    }
    float lowpass(float x) noexcept {
        const float v = (x - s) * g;
        const float y = v + s;
        s = y + v;
        return y;
    }
    float highpass(float x) noexcept { return x - lowpass(x); }
    // First-order all-pass with the same corner.
    float allpass(float x) noexcept { return 2.0f * lowpass(x) - x; }
};

// Coefficient for a one-pole smoother with time constant `ms`.
inline float smoothingCoef(double ms, double sampleRate) noexcept {
    return static_cast<float>(1.0 - std::exp(-1.0 / std::max(ms * 0.001 * sampleRate, 1.0)));
}

inline float softSat(float x) noexcept { return std::tanh(x); }

// Smooth random wander in -1..1 (for drift): a value that glides to a new random target every
// period. Deterministic for a given seed.
struct Wander {
    std::uint32_t state = 1;
    float from = 0.0f, to = 0.0f, phase = 1.0f;
    float nextTarget() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
    void seed(std::uint32_t s) noexcept {
        state = s != 0 ? s : 1;
        from = 0.0f;
        to = nextTarget();
        phase = 0.0f;
    }
    // Advances by `cycles` periods and returns the value (raised-cosine glide between targets).
    float advance(float cycles) noexcept {
        phase += cycles;
        while (phase >= 1.0f) {
            phase -= 1.0f;
            from = to;
            to = nextTarget();
        }
        const float w = 0.5f - 0.5f * std::cos(kPiF * phase);
        return from + (to - from) * w;
    }
};

// The tempo-synced rate of a modulation in Hz.
inline double syncedHz(int division, double bpm) noexcept { return bpm / (60.0 * divisionBeats(division)); }

} // namespace zed::fx
