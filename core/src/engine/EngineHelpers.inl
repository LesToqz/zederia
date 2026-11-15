using osc::kMaxLanes;
using osc::kMaxRun;
using osc::WarpMode;

constexpr double kStealFadeSeconds = 0.004;
constexpr int kFixedPitchNote = 60;
constexpr int kOscCount = kNumOscs;
constexpr int kSub = 3, kTexture = 4;
constexpr int kHistory = osc::kMaxOversample; // modulator samples kept from the previous run, for loops
constexpr float kSmoothingSeconds = 0.005f;   // control-rate smoothing of position, warp and unison controls
constexpr int kInterval = Engine::kControlInterval;
constexpr int kFade = Engine::kSwapFadeSamples;

enum class UnisonLaw { Linear, Ensemble, Exponential, Inverse, Random };
enum class UnisonStack { Off, Octave, TwoOctaves, Fifths, FifthsOctaves, CenterDown12, CenterDown24 };
enum class UnisonPhase { Free, Random, Consistent };
enum class SubShape { Sine, Triangle, Saw, Square, Pulse25, RoundedSquare };

struct WarpCurve {
    std::array<float, osc::kCurvePoints + 2> values{};
};

// --- Small helpers -----------------------------------------------------------------------------

// Moves `current` a fraction of the way to `target`; returns true while it is still moving.
bool approach(float& current, float target, float coefficient) {
    if (current == target)
        return false;
    const float next = current + (target - current) * coefficient;
    current = std::abs(target - next) < 1.0e-5f ? target : next;
    return true;
}

// Constant-power pan for a single lane: -3 dB on each side at the centre.
void lanePan(float pan, float gain, float& left, float& right) {
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * static_cast<float>(kPi * 0.25);
    left = gain * std::cos(angle);
    right = gain * std::sin(angle);
}

// Balance for an already stereo source: unity on both sides at the centre.
void balance(double pan, double gain, float& left, float& right) {
    const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * kPi * 0.25;
    left = static_cast<float>(gain * std::sqrt(2.0) * std::cos(angle));
    right = static_cast<float>(gain * std::sqrt(2.0) * std::sin(angle));
}

std::uint32_t toPhase(double cycles) {
    cycles -= std::floor(cycles);
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(cycles * 4294967296.0) & 0xffffffffu);
}

// sin(2 pi t) for t in 0..1 (the Sub oscillator; error below 1e-6).
float sineTurn(float t) {
    float u = t - std::floor(t + 0.5f);
    const float sign = u < 0.0f ? -1.0f : 1.0f;
    u = std::abs(u);
    if (u > 0.25f)
        u = 0.5f - u;
    const float z = u * 6.2831853f, z2 = z * z;
    return sign * z * (1.0f + z2 * (-1.6666667e-1f + z2 * (8.3333333e-3f + z2 * (-1.9841270e-4f + z2 * 2.7557319e-6f))));
}

// Corrections for a step of +2 and for a corner at phase 0 (see OscKernel.inl).
float stepFix(float t, float dt) {
    if (t < dt) {
        const float x = t / dt;
        return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - dt) {
        const float x = (t - 1.0f) / dt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}
float cornerFix(float t, float dt) {
    if (t < dt) {
        const float a = 1.0f - t / dt;
        return a * a * a / 6.0f;
    }
    if (t > 1.0f - dt) {
        const float b = (t - 1.0f) / dt + 1.0f;
        return b * b * b / 6.0f;
    }
    return 0.0f;
}

float subWave(SubShape shape, float t, float dt) {
    dt = std::clamp(dt, 1.0e-7f, 0.25f);
    const auto pulse = [&](float width) {
        float fall = t - width;
        if (fall < 0.0f)
            fall += 1.0f;
        return (t < width ? 1.0f : -1.0f) + stepFix(t, dt) - stepFix(fall, dt);
    };
    switch (shape) {
    case SubShape::Sine:
        return sineTurn(t);
    case SubShape::Triangle: {
        float q = t + 0.25f;
        if (q >= 1.0f)
            q -= 1.0f;
        float h = q + 0.5f;
        if (h >= 1.0f)
            h -= 1.0f;
        return 1.0f - 4.0f * std::abs(q - 0.5f) + 8.0f * dt * (cornerFix(q, dt) - cornerFix(h, dt));
    }
    case SubShape::Saw:
        return 2.0f * t - 1.0f - stepFix(t, dt);
    case SubShape::Square:
        return pulse(0.5f);
    case SubShape::Pulse25:
        return pulse(0.25f);
    case SubShape::RoundedSquare: { // a sine pushed toward a square, with soft shoulders
        const float s = sineTurn(t);
        return s * 4.0f / (1.0f + 3.0f * std::abs(s));
    }
    }
    return 0.0f;
}

const float kZeros[kHistory + kMaxRun] = {};

// Lane positions across the unison spread, centre first, then alternating sides outward.
void layoutLanes(int count, float x[kMaxLanes]) {
    std::fill(x, x + kMaxLanes, 0.0f);
    if (count <= 1)
        return;
    const float unit = 1.0f / static_cast<float>(count - 1);
    if (count % 2 == 1) {
        for (int m = 1; 2 * m - 1 < count; ++m) {
            x[2 * m - 1] = 2.0f * static_cast<float>(m) * unit;
            x[2 * m] = -2.0f * static_cast<float>(m) * unit;
        }
    } else {
        for (int m = 0; 2 * m < count; ++m) {
            x[2 * m] = static_cast<float>(2 * m + 1) * unit;
            x[2 * m + 1] = -static_cast<float>(2 * m + 1) * unit;
        }
    }
}

bool isCentreLane(int lane, int count) { return count % 2 == 1 ? lane == 0 : lane < 2; }

float detuneLaw(UnisonLaw law, float x) {
    const float a = std::abs(x), sign = x < 0.0f ? -1.0f : 1.0f;
    switch (law) {
    case UnisonLaw::Ensemble: return sign * a * a;
    case UnisonLaw::Exponential: return sign * (std::exp2(3.0f * a) - 1.0f) / 7.0f;
    case UnisonLaw::Inverse: return sign * std::sqrt(a);
    default: return x;
    }
}

float stackOffset(UnisonStack stack, int lane, int count) {
    switch (stack) {
    case UnisonStack::Octave: return lane % 2 == 1 ? 12.0f : 0.0f;
    case UnisonStack::TwoOctaves: return 12.0f * static_cast<float>(lane % 3);
    case UnisonStack::Fifths: return lane % 2 == 1 ? 7.0f : 0.0f;
    case UnisonStack::FifthsOctaves: return lane % 3 == 1 ? 7.0f : lane % 3 == 2 ? 12.0f : 0.0f;
    case UnisonStack::CenterDown12: return isCentreLane(lane, count) ? -12.0f : 0.0f;
    case UnisonStack::CenterDown24: return isCentreLane(lane, count) ? -24.0f : 0.0f;
    default: return 0.0f;
    }
}

// Reads a sample with four-point interpolation. Positions past the loop wrap back into it;
// one-shot playback holds the edges.
inline float sampleAt(const std::vector<float>& data, int index, int frames, bool loop, int loopStart, int loopEnd) {
    if (loop) {
        if (index >= loopEnd)
            index -= loopEnd - loopStart;
        else if (index < 0)
            index = 0;
    }
    return data[static_cast<std::size_t>(std::clamp(index, 0, frames - 1))];
}
