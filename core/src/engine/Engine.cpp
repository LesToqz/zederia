#include "zederia/engine/Engine.h"

#include "zederia/dsp/HalfBand.h"
#include "zederia/dsp/Simd.h"
#include "zederia/engine/Envelope.h"
#include "zederia/engine/OscKernel.h"
#include "zederia/engine/VoiceAllocator.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/rt/Denormals.h"
#include "zederia/rt/Handoff.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(disable : 4324) // structures padded because of alignas: intended
#endif

namespace zed {
namespace {

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

// Placeholder for milestone 3: a filter slot that passes its input through unchanged.
struct FilterSlot {
    void process(float*, float*, int) noexcept {}
};

// --- Per-voice state ---------------------------------------------------------------------------

// One oscillator's unison lanes plus the values they move toward at the next control tick.
struct LaneSet {
    osc::Lanes lanes;
    alignas(32) float incT[kMaxLanes], fposT[kMaxLanes], dAT[kMaxLanes], dBT[kMaxLanes];
    alignas(32) float gLT[kMaxLanes], gRT[kMaxLanes], gMT[kMaxLanes];
    int level[kMaxLanes] = {};     // band-limited level each lane reads
    int levelNext[kMaxLanes] = {}; // level it is fading to (equal to level when not fading)
    bool levelFade = false;
};

struct SourceGain { // a source's level and pan as left and right gains, ramped per sample
    float now[2] = {0.0f, 0.0f}, step[2] = {0.0f, 0.0f}, target[2] = {0.0f, 0.0f};
    void aim(float left, float right, int samples, bool snap) {
        target[0] = left;
        target[1] = right;
        for (int c = 0; c < 2; ++c) {
            if (snap)
                now[c] = target[c];
            step[c] = (target[c] - now[c]) / static_cast<float>(samples);
        }
    }
    void settle() {
        for (int c = 0; c < 2; ++c) {
            now[c] = target[c];
            step[c] = 0.0f;
        }
    }
    bool silent() const { return now[0] == 0.0f && now[1] == 0.0f && target[0] == 0.0f && target[1] == 0.0f; }
};

struct OscVoice {
    LaneSet main;
    LaneSet fade;     // keeps playing the previous table while a new one fades in
    int fadeLeft = 0; // samples of that crossfade still to go
    float ratio[kMaxLanes] = {}; // pitch ratio per lane when the detune is random per note
    float randomDetune[kMaxLanes] = {};
    std::uint32_t ratioEpoch = 0;
    SourceGain gain;
    dsp::DecimatorState decimatorL, decimatorR;
};

struct SubVoice {
    double phase = 0.0, inc = 0.0, incStep = 0.0, incTarget = 0.0; // inc in cycles per rendered sample
    SourceGain gain;
    dsp::DecimatorState decimator;
};

struct TexturePlay {
    double position = 0.0; // in frames of the sample
    bool finished = false;
};

struct TextureVoice {
    TexturePlay main, fade;
    int fadeLeft = 0;
    double rate = 0.0, rateStep = 0.0, rateTarget = 0.0; // frames per rendered sample
    SourceGain gain;
    dsp::DecimatorState decimatorL, decimatorR;
};

struct Voice {
    bool active = false;
    bool done = false; // envelope finished; freed at the next tick, once the last ramp has reached zero
    int note = kFixedPitchNote;
    double velocity = 0.0;
    double envTarget = 0.0, envStep = 0.0; // envelope level at the next tick and its per-sample slope
    float amp = 0.0f, ampStep = 0.0f;      // envelope times velocity
    Envelope env;
    OscVoice osc[kOscCount];
    SubVoice sub;
    TextureVoice texture;
    // What each source last produced as a modulator: kHistory samples from the previous run, then
    // the current run.
    float mod[kNumSources][kHistory + kMaxRun] = {};
};

// --- Engine-wide state of one oscillator -------------------------------------------------------

struct OscShared {
    // Targets, straight from the parameters.
    bool on = false, classic = false, keytrack = true, continuePhase = false;
    bool turnedOn = false; // switched on by the last parameter update: voices need its lanes set up
    int count = 1;
    UnisonLaw law = UnisonLaw::Linear;
    UnisonStack stack = UnisonStack::Off;
    UnisonPhase unisonPhase = UnisonPhase::Random;
    WarpMode mode[2] = {WarpMode::Off, WarpMode::Off}; // effective: off when the engine cannot do it
    int modSource[2] = {0, 0};
    double tuneSemis = 0.0, levelGain = 0.0, pan = 0.0;
    float startPhase = 0.0f, phaseRandom = 0.0f;
    float positionT = 0, amountT[2] = {0, 0}, spreadSemisT = 0, blendT = 0, widthT = 0, posSpreadT = 0, warpSpreadT = 0;
    float shapeT = 0, pulseWidthT = 0.5f, sendT[3] = {1, 0, 0};
    // The same after control-rate smoothing.
    float position = 0, amount[2] = {0, 0}, spreadSemis = 0, blend = 0, width = 0, posSpread = 0, warpSpread = 0;
    float shape = 0, pulseWidth = 0.5f, send[3] = {1, 0, 0};
    // Per-lane values shared by every voice, recomputed when something above moves.
    bool lanesDirty = true;
    float x[kMaxLanes] = {};         // lane position, -1..1, centre first
    float lanePos[kMaxLanes] = {};   // table position 0..1
    float laneD[2][kMaxLanes] = {};  // derived warp values
    float laneSlope[kMaxLanes] = {}; // how much the warps steepen the phase
    float laneGL[kMaxLanes] = {}, laneGR[kMaxLanes] = {}, laneGM[kMaxLanes] = {};
    float laneRatio[kMaxLanes] = {}; // detune and stack as a frequency ratio
    float stackSemis[kMaxLanes] = {};
    std::uint32_t ratioEpoch = 1;
    float consistentPhase[kMaxLanes] = {};
    // Rendering.
    int oversample = 1;
    int padded = 1; // lanes the chosen kernel touches
    osc::RenderFn render = osc::renderScalar;
    const TableData* table = nullptr;
    const TableData* oldTable = nullptr; // during a swap
    int fadeLeft = 0;
    const float* curve[2] = {nullptr, nullptr};
};

struct SubShared {
    bool on = false, direct = false, turnedOn = false;
    SubShape shape = SubShape::Sine;
    double tuneSemis = 0.0, levelGain = 0.0, pan = 0.0;
    float balanceT = 0.0f, balance = 0.0f;
    int oversample = 1;
};

struct TextureShared {
    bool on = false, direct = false, loop = true, keytrack = false, turnedOn = false;
    double tuneSemis = 0.0, levelGain = 0.0, pan = 0.0;
    float start = 0.0f, randomStart = 0.0f;
    float balanceT = 0.0f, balance = 0.0f;
    int oversample = 1;
    const SampleData* sample = nullptr;
    const SampleData* oldSample = nullptr;
    int fadeLeft = 0;
};

// Which source modulates which, and the order that follows from it.
struct Routing {
    int order[kNumSources] = {kSub, kTexture, 0, 1, 2};
    bool usedAsModulator[kNumSources] = {};
    bool delayed[kOscCount][2] = {}; // the modulator is evaluated later (or is the oscillator itself)
    bool loop = false;               // some oscillator needs a delayed modulator: render sample by sample
};

// Registry positions of the parameters the engine listens to.
struct Ids {
    int mode, polyphony, steal, priority, bendUp, bendDown, master, masterTune, reference, velocitySens;
    int quality, qualityOffline, filterRouting;
    int envDelay, envAttack, envHold, envDecay, envSustain, envRelease;
    struct Osc {
        int on, engine, position, octave, semi, fine, coarse, level, pan, phase, phaseMode, phaseRandom, keytrack;
        int shape, pulseWidth, warpMode[2], warpAmount[2], warpSource[2];
        int voices, detune, range, blend, width, positionSpread, warpSpread, law, stack, unisonPhase;
        int balance, direct;
    } osc[kOscCount];
    struct {
        int on, shape, octave, coarse, fine, level, pan, direct, balance;
    } sub;
    struct {
        int on, mode, start, randomStart, keytrack, pitch, fine, level, pan, direct, balance;
    } texture;

    Ids() {
        const auto& r = Registry::get();
        const auto at = [&](const std::string& id) { return r.indexOf(id); };
        mode = at("global.mode");
        polyphony = at("global.polyphony");
        steal = at("voice.steal");
        priority = at("voice.priority");
        bendUp = at("global.bend_up");
        bendDown = at("global.bend_down");
        master = at("global.master");
        masterTune = at("global.master_tune");
        reference = at("global.reference");
        velocitySens = at("voice.velocity_sens");
        quality = at("global.quality");
        qualityOffline = at("global.quality_offline");
        filterRouting = at("voice.filter_routing");
        envDelay = at("env1.delay");
        envAttack = at("env1.attack");
        envHold = at("env1.hold");
        envDecay = at("env1.decay");
        envSustain = at("env1.sustain");
        envRelease = at("env1.release");
        for (int i = 0; i < kOscCount; ++i) {
            const std::string p = "osc" + std::to_string(i + 1) + ".";
            Osc& o = osc[i];
            o.on = at(p + "on");
            o.engine = at(p + "engine");
            o.position = at(p + "position");
            o.octave = at(p + "octave");
            o.semi = at(p + "semi");
            o.fine = at(p + "fine");
            o.coarse = at(p + "coarse");
            o.level = at(p + "level");
            o.pan = at(p + "pan");
            o.phase = at(p + "phase");
            o.phaseMode = at(p + "phase_mode");
            o.phaseRandom = at(p + "phase_random");
            o.keytrack = at(p + "keytrack");
            o.shape = at(p + "classic.shape");
            o.pulseWidth = at(p + "classic.pulse_width");
            for (int s = 0; s < 2; ++s) {
                const std::string w = p + (s == 0 ? "warp_a." : "warp_b.");
                o.warpMode[s] = at(w + "mode");
                o.warpAmount[s] = at(w + "amount");
                o.warpSource[s] = at(w + "source");
            }
            o.voices = at(p + "unison.voices");
            o.detune = at(p + "unison.detune");
            o.range = at(p + "unison.range");
            o.blend = at(p + "unison.blend");
            o.width = at(p + "unison.width");
            o.positionSpread = at(p + "unison.position_spread");
            o.warpSpread = at(p + "unison.warp_spread");
            o.law = at(p + "unison.law");
            o.stack = at(p + "unison.stack");
            o.unisonPhase = at(p + "unison.phase");
            o.balance = at(p + "sends.filter_balance");
            o.direct = at(p + "sends.direct");
        }
        sub.on = at("sub.on");
        sub.shape = at("sub.shape");
        sub.octave = at("sub.octave");
        sub.coarse = at("sub.coarse");
        sub.fine = at("sub.fine");
        sub.level = at("sub.level");
        sub.pan = at("sub.pan");
        sub.direct = at("sub.direct");
        sub.balance = at("sub.sends.filter_balance");
        texture.on = at("texture.on");
        texture.mode = at("texture.mode");
        texture.start = at("texture.start");
        texture.randomStart = at("texture.random_start");
        texture.keytrack = at("texture.keytrack");
        texture.pitch = at("texture.pitch");
        texture.fine = at("texture.fine");
        texture.level = at("texture.level");
        texture.pan = at("texture.pan");
        texture.direct = at("texture.direct");
        texture.balance = at("texture.sends.filter_balance");
    }
};

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

void setLaneLevel(LaneSet& s, int lane, const TableData& table, int level, bool second) {
    const TableData::Level& l = table.level(level);
    if (second) {
        s.lanes.tabOff2[lane] = static_cast<float>(l.offset);
        s.lanes.tabStride2[lane] = static_cast<float>(l.stride);
        s.lanes.tabSize2[lane] = static_cast<float>(l.size);
    } else {
        s.lanes.tabOff[lane] = static_cast<float>(l.offset);
        s.lanes.tabStride[lane] = static_cast<float>(l.stride);
        s.lanes.tabSize[lane] = static_cast<float>(l.size);
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

} // namespace

struct Engine::Impl {
    ParamStore store{Registry::get().size()};
    std::vector<double> p = std::vector<double>(static_cast<std::size_t>(Registry::get().size())); // audio thread's copy
    Ids ids;
    double sampleRate = 48000.0;
    std::atomic<bool> offline{false};
    bool offlineSeen = false;

    VoiceAllocator allocator;
    VoiceAllocator::Actions actions;
    std::array<Voice, kVoiceSlots> voices;
    int activeCount = 0;
    int lastStarted = -1; // voice slot of the most recent note, for "continue" phase
    std::atomic<int> activeForOthers{0};

    int samplesToTick = kInterval;
    bool dirty = true;      // a parameter changed since derived values were last computed
    bool snapMaster = true; // no ramp for the first block after a reset
    bool snapSmoothing = true;
    float smoothing = 0.1f; // control-rate one-pole coefficient

    // Derived from parameters.
    EnvelopeTimes envTimes;
    double velocitySens = 0.5;
    double referenceHz = 440.0, bendUp = 2.0, bendDown = 2.0;
    double master = 0.0, masterTarget = 0.0, masterStep = 0.0;
    float filterRoutingT = 0.0f, filterRouting = 0.0f;
    int oversample = 2;
    const dsp::DecimatorDesign* decimator = nullptr;

    OscShared osc[kOscCount];
    SubShared sub;
    TextureShared texture;
    Routing routing;
    FilterSlot filter1, filter2;

    rt::Handoff<TableData> tables[kOscCount];
    rt::Handoff<SampleData> sampleSlot;
    rt::Handoff<WarpCurve> curves[kOscCount][2];

    double bend = 0.0; // pitch wheel, -1..1
    double modWheel = 0.0;
    Rng rng;
    std::uint64_t seed = 1;

    void updateSmoothing() {
        smoothing = static_cast<float>(1.0 - std::exp(-static_cast<double>(kInterval) / (kSmoothingSeconds * sampleRate)));
    }

    Impl() {
        updateSmoothing();
        // Everything with lazy one-time setup is touched here, off the audio thread.
        decimator = &dsp::decimatorDesign(false);
        (void)dsp::decimatorDesign(true);
        (void)osc::defaultWarpCurve();
        (void)dsp::cpuHasAvx2();
        for (int i = 0; i < kOscCount; ++i) {
            tables[i].set(TableData::builtin());
            tables[i].take();
            osc[i].table = tables[i].current();
            osc[i].curve[0] = osc[i].curve[1] = osc::defaultWarpCurve();
            Rng fixed(0x5eed0000u + static_cast<std::uint64_t>(i)); // "consistent" start phases: the same for every note
            for (float& phase : osc[i].consistentPhase)
                phase = static_cast<float>(fixed.uniform());
        }
        sampleSlot.set(SampleData::builtin());
        sampleSlot.take();
        texture.sample = sampleSlot.current();
    }

    double value(int index) const { return p[static_cast<std::size_t>(index)]; }
    bool flag(int index) const { return value(index) >= 0.5; }
    int choice(int index) const { return static_cast<int>(value(index)); }

    // --- Parameters ---------------------------------------------------------------------------

    void refresh() {
        const double msToSamples = sampleRate / 1000.0;
        envTimes.delay = value(ids.envDelay) * msToSamples;
        envTimes.attack = value(ids.envAttack) * msToSamples;
        envTimes.hold = value(ids.envHold) * msToSamples;
        envTimes.decay = value(ids.envDecay) * msToSamples;
        envTimes.sustain = value(ids.envSustain);
        envTimes.release = value(ids.envRelease) * msToSamples;
        velocitySens = value(ids.velocitySens);
        referenceHz = value(ids.reference);
        bendUp = value(ids.bendUp);
        bendDown = value(ids.bendDown);
        filterRoutingT = static_cast<float>(value(ids.filterRouting));
        const double masterTune = value(ids.masterTune) / 100.0;

        const int quality = std::clamp(choice(offlineSeen ? ids.qualityOffline : ids.quality), 0, 3);
        const int newOversample = 1 << quality; // Draft 1x, Standard 2x, High 4x, Ultra 8x
        decimator = &dsp::decimatorDesign(quality >= 2);
        const bool qualityChanged = newOversample != oversample;
        oversample = newOversample;

        bool relayout[kOscCount] = {};
        for (int i = 0; i < kOscCount; ++i) {
            const Ids::Osc& id = ids.osc[i];
            OscShared& o = osc[i];
            o.turnedOn = flag(id.on) && !o.on;
            o.on = flag(id.on);
            o.classic = choice(id.engine) == 1;
            o.keytrack = flag(id.keytrack);
            o.continuePhase = choice(id.phaseMode) == 1;
            const int count = std::clamp(choice(id.voices), 1, kMaxLanes);
            const auto law = static_cast<UnisonLaw>(choice(id.law));
            const auto stack = static_cast<UnisonStack>(choice(id.stack));
            const osc::RenderFn render = osc::pickRender(count);
            const int width = render == osc::renderScalar ? 1 : render == osc::renderSse2 ? 4 : 8;
            const int padded = std::min((count + width - 1) / width * width, kMaxLanes);
            relayout[i] = count != o.count || law != o.law || padded != o.padded;
            o.render = render;
            o.padded = padded;
            o.lanesDirty = o.lanesDirty || relayout[i] || stack != o.stack;
            o.count = count;
            o.law = law;
            o.stack = stack;
            o.unisonPhase = static_cast<UnisonPhase>(choice(id.unisonPhase));
            for (int s = 0; s < 2; ++s) {
                auto mode = static_cast<WarpMode>(std::clamp(choice(id.warpMode[s]), 0, osc::kNumWarpModes - 1));
                if (o.classic && osc::warpIsPhaseShaper(mode))
                    mode = WarpMode::Off; // phase shaping needs a wavetable
                o.lanesDirty = o.lanesDirty || mode != o.mode[s];
                o.mode[s] = mode;
                o.modSource[s] = std::clamp(choice(id.warpSource[s]), 0, kNumSources - 1);
                o.amountT[s] = static_cast<float>(value(id.warpAmount[s]));
            }
            o.tuneSemis = value(id.octave) * 12.0 + value(id.semi) + value(id.coarse) + value(id.fine) / 100.0 + masterTune;
            o.levelGain = dbToGain(value(id.level));
            o.pan = value(id.pan);
            o.startPhase = static_cast<float>(value(id.phase) / 360.0);
            o.phaseRandom = static_cast<float>(value(id.phaseRandom));
            o.positionT = static_cast<float>(value(id.position));
            o.spreadSemisT = static_cast<float>(value(id.detune) * value(id.range));
            o.blendT = static_cast<float>(value(id.blend));
            o.widthT = static_cast<float>(value(id.width));
            o.posSpreadT = static_cast<float>(value(id.positionSpread));
            o.warpSpreadT = static_cast<float>(value(id.warpSpread));
            o.shapeT = static_cast<float>(value(id.shape));
            o.pulseWidthT = static_cast<float>(std::clamp(value(id.pulseWidth), 0.02, 0.98));
            const auto direct = static_cast<float>(value(id.direct)), toSecond = static_cast<float>(value(id.balance));
            o.sendT[0] = (1.0f - direct) * (1.0f - toSecond);
            o.sendT[1] = (1.0f - direct) * toSecond;
            o.sendT[2] = direct;
        }
        sub.turnedOn = flag(ids.sub.on) && !sub.on;
        sub.on = flag(ids.sub.on);
        sub.direct = flag(ids.sub.direct);
        sub.shape = static_cast<SubShape>(choice(ids.sub.shape));
        sub.tuneSemis = value(ids.sub.octave) * 12.0 + value(ids.sub.coarse) + value(ids.sub.fine) / 100.0 + masterTune;
        sub.levelGain = dbToGain(value(ids.sub.level));
        sub.pan = value(ids.sub.pan);
        sub.balanceT = static_cast<float>(value(ids.sub.balance));
        texture.turnedOn = flag(ids.texture.on) && !texture.on;
        texture.on = flag(ids.texture.on);
        texture.direct = flag(ids.texture.direct);
        texture.loop = choice(ids.texture.mode) == 1;
        texture.keytrack = flag(ids.texture.keytrack);
        texture.tuneSemis = value(ids.texture.pitch) + value(ids.texture.fine) / 100.0 + masterTune;
        texture.levelGain = dbToGain(value(ids.texture.level));
        texture.pan = value(ids.texture.pan);
        texture.start = static_cast<float>(value(ids.texture.start));
        texture.randomStart = static_cast<float>(value(ids.texture.randomStart));
        texture.balanceT = static_cast<float>(value(ids.texture.balance));

        const bool rateChanged = updateRouting() || qualityChanged;

        if (snapSmoothing)
            snapSmoothed();
        for (int i = 0; i < kOscCount; ++i)
            if (osc[i].lanesDirty)
                updateLaneTables(i);

        masterTarget = dbToGain(value(ids.master));
        if (snapMaster) {
            master = masterTarget;
            masterStep = 0.0;
            snapMaster = false;
        } else {
            masterStep = (masterTarget - master) / samplesToTick;
        }

        actions.count = 0;
        allocator.configure(static_cast<VoiceMode>(choice(ids.mode)), static_cast<int>(value(ids.polyphony)),
                            static_cast<StealMode>(choice(ids.steal)), static_cast<NotePriority>(choice(ids.priority)),
                            actions);
        applyActions();
        for (auto& v : voices) {
            if (!v.active)
                continue;
            for (int i = 0; i < kOscCount; ++i) {
                if (relayout[i] && osc[i].on)
                    relayoutVoice(v, i);
                else if (osc[i].turnedOn)
                    aimOsc(v, i, true); // its lanes may hold values from another table or none at all
            }
            if (sub.turnedOn)
                aimSub(v, true);
            if (texture.turnedOn)
                aimTexture(v, true);
            if (rateChanged)
                resetRates(v);
            aimSources(v, rateChanged);
            aimAmp(v);
        }
    }

    // Works out the evaluation order and the oversampling of every source. Returns true when a
    // source's rate changed.
    bool updateRouting() {
        Routing r;
        bool edge[kOscCount][kOscCount] = {}; // edge[j][i]: j modulates i
        for (int i = 0; i < kOscCount; ++i) {
            if (!osc[i].on)
                continue;
            for (int s = 0; s < 2; ++s) {
                if (!osc::warpUsesModulator(osc[i].mode[s]))
                    continue;
                const int j = osc[i].modSource[s];
                r.usedAsModulator[j] = true;
                if (j < kOscCount && j != i)
                    edge[j][i] = true;
            }
        }
        // Modulators first. Sub and Texture never depend on anything; oscillators that form a loop
        // keep their numeric order.
        int placed = 0;
        bool done[kOscCount] = {};
        r.order[placed++] = kSub;
        r.order[placed++] = kTexture;
        for (int round = 0; round < kOscCount; ++round) {
            int pick = -1;
            for (int i = 0; i < kOscCount && pick < 0; ++i) {
                if (done[i])
                    continue;
                bool waiting = false;
                for (int j = 0; j < kOscCount; ++j)
                    waiting = waiting || (edge[j][i] && !done[j]);
                if (!waiting)
                    pick = i;
            }
            if (pick < 0) // a loop: take the lowest oscillator that is left
                for (int i = 0; i < kOscCount && pick < 0; ++i)
                    if (!done[i])
                        pick = i;
            done[pick] = true;
            r.order[placed++] = pick;
        }
        int position[kNumSources] = {};
        for (int k = 0; k < kNumSources; ++k)
            position[r.order[k]] = k;
        for (int i = 0; i < kOscCount; ++i) {
            if (!osc[i].on)
                continue;
            for (int s = 0; s < 2; ++s) {
                if (!osc::warpUsesModulator(osc[i].mode[s]))
                    continue;
                r.delayed[i][s] = position[osc[i].modSource[s]] >= position[i];
                r.loop = r.loop || r.delayed[i][s];
            }
        }
        routing = r;

        // Warped and modulated paths, and the modulators that feed them, run oversampled. So
        // does the Classic engine: its step correction is cheap but leaves more aliasing than
        // the wavetable levels do, and oversampling is what pushes that down.
        bool changed = false;
        for (int i = 0; i < kOscCount; ++i) {
            const bool warped = osc[i].mode[0] != WarpMode::Off || osc[i].mode[1] != WarpMode::Off;
            const int rate = warped || r.usedAsModulator[i] || osc[i].classic ? oversample : 1;
            changed = changed || rate != osc[i].oversample;
            osc[i].oversample = rate;
        }
        const int subRate = r.usedAsModulator[kSub] ? oversample : 1;
        const int textureRate = r.usedAsModulator[kTexture] ? oversample : 1;
        changed = changed || subRate != sub.oversample || textureRate != texture.oversample;
        sub.oversample = subRate;
        texture.oversample = textureRate;
        return changed;
    }

    void snapSmoothed() {
        for (auto& o : osc) {
            o.position = o.positionT;
            o.amount[0] = o.amountT[0];
            o.amount[1] = o.amountT[1];
            o.spreadSemis = o.spreadSemisT;
            o.blend = o.blendT;
            o.width = o.widthT;
            o.posSpread = o.posSpreadT;
            o.warpSpread = o.warpSpreadT;
            o.shape = o.shapeT;
            o.pulseWidth = o.pulseWidthT;
            std::copy(o.sendT, o.sendT + 3, o.send);
            o.lanesDirty = true;
        }
        sub.balance = sub.balanceT;
        texture.balance = texture.balanceT;
        filterRouting = filterRoutingT;
        snapSmoothing = false;
    }

    // One control-rate step of the smoothed controls.
    void smoothControls() {
        for (auto& o : osc) {
            bool moved = approach(o.position, o.positionT, smoothing);
            moved = approach(o.amount[0], o.amountT[0], smoothing) || moved;
            moved = approach(o.amount[1], o.amountT[1], smoothing) || moved;
            moved = approach(o.spreadSemis, o.spreadSemisT, smoothing) || moved;
            moved = approach(o.blend, o.blendT, smoothing) || moved;
            moved = approach(o.width, o.widthT, smoothing) || moved;
            moved = approach(o.posSpread, o.posSpreadT, smoothing) || moved;
            moved = approach(o.warpSpread, o.warpSpreadT, smoothing) || moved;
            approach(o.shape, o.shapeT, smoothing);
            approach(o.pulseWidth, o.pulseWidthT, smoothing);
            for (int k = 0; k < 3; ++k)
                approach(o.send[k], o.sendT[k], smoothing);
            o.lanesDirty = o.lanesDirty || moved;
        }
        approach(sub.balance, sub.balanceT, smoothing);
        approach(texture.balance, texture.balanceT, smoothing);
        approach(filterRouting, filterRoutingT, smoothing);
    }

    // The per-lane values every voice of an oscillator shares (plan 3.4).
    void updateLaneTables(int i) {
        OscShared& o = osc[i];
        o.lanesDirty = false;
        const int count = o.count;
        layoutLanes(count, o.x);

        // Centre against side voices, then scaled so the summed power does not depend on the count.
        const float angle = std::clamp(o.blend, 0.0f, 1.0f) * static_cast<float>(kPi * 0.5);
        const float centre = count <= 2 ? 1.0f : std::cos(angle), side = std::sin(angle);
        float weight[kMaxLanes], power = 0.0f;
        for (int l = 0; l < count; ++l) {
            weight[l] = isCentreLane(l, count) ? centre : side;
            power += weight[l] * weight[l];
        }
        const float norm = power > 1.0e-9f ? 1.0f / std::sqrt(power) : 0.0f;
        for (int l = 0; l < kMaxLanes; ++l) {
            if (l >= count) {
                o.laneGL[l] = o.laneGR[l] = o.laneGM[l] = 0.0f;
                continue;
            }
            const float gain = power > 1.0e-9f ? weight[l] * norm : 1.0f / std::sqrt(static_cast<float>(count));
            // Neighbouring detunes go to opposite sides, so neither side is flat or sharp as a whole.
            const int rank = count % 2 == 1 ? (l + 1) / 2 : l / 2 + 1;
            const float sideSign = (o.x[l] < 0.0f ? -1.0f : 1.0f) * (rank % 2 == 1 ? -1.0f : 1.0f);
            lanePan(o.width * std::abs(o.x[l]) * sideSign, gain, o.laneGL[l], o.laneGR[l]);
            o.laneGM[l] = gain;
        }
        for (int l = 0; l < kMaxLanes; ++l) {
            const int src = l < count ? l : 0;
            o.lanePos[l] = std::clamp(o.position + o.posSpread * o.x[src], 0.0f, 1.0f);
            float slope = 1.0f;
            for (int s = 0; s < 2; ++s) {
                const float amount = std::clamp(o.amount[s] + o.warpSpread * o.x[src], 0.0f, 1.0f);
                o.laneD[s][l] = osc::warpDerive(o.mode[s], amount);
                slope *= osc::warpSlope(o.mode[s], o.laneD[s][l]);
            }
            o.laneSlope[l] = std::min(slope, 64.0f);
            o.stackSemis[l] = stackOffset(o.stack, src, count);
            const float detune = detuneLaw(o.law, o.x[src]) * o.spreadSemis * 0.5f;
            o.laneRatio[l] = std::exp2((detune + o.stackSemis[l]) / 12.0f);
        }
        ++o.ratioEpoch;
    }

    // --- Voices -------------------------------------------------------------------------------

    double velocityGain(const Voice& v) const { return 1.0 - velocitySens + velocitySens * v.velocity; }
    double bendSemis() const { return bend >= 0.0 ? bend * bendUp : bend * bendDown; }

    // Cycles per sample at the base rate for a pitch in semitones.
    double pitchInc(double note) const { return std::min(noteToHz(note, referenceHz) / sampleRate, 0.49); }

    // Points every lane's ramps at the values wanted at the next control tick.
    void aimLanes(LaneSet& s, const OscShared& o, const TableData& table, const float* ratio, double baseInc, bool snap) {
        osc::Lanes& L = s.lanes;
        L.count = o.count;
        const float maxFrame = static_cast<float>(table.frames() - 1);
        const auto rate = static_cast<float>(o.oversample);
        const float perStep = 1.0f / (static_cast<float>(samplesToTick) * rate);
        const auto base = static_cast<float>(baseInc);
        const auto ramp = [&](float& now, float& step, float& target, float value) {
            target = value;
            if (snap)
                now = value;
            step = (value - now) * perStep;
        };
        for (int l = 0; l < o.padded; ++l) {
            ramp(L.inc[l], L.incStep[l], s.incT[l], std::min(base * ratio[l], 0.49f) / rate);
            float frame = o.lanePos[l] * maxFrame;
            if (table.stepped())
                frame = std::floor(frame + 0.5f);
            if (table.stepped() && !snap) { // a staircase on purpose: no glide between frames
                s.fposT[l] = L.fpos[l] = frame;
                L.fposStep[l] = 0.0f;
            } else {
                ramp(L.fpos[l], L.fposStep[l], s.fposT[l], frame);
            }
            ramp(L.dA[l], L.dAStep[l], s.dAT[l], o.laneD[0][l]);
            ramp(L.dB[l], L.dBStep[l], s.dBT[l], o.laneD[1][l]);
            ramp(L.gL[l], L.gLStep[l], s.gLT[l], o.laneGL[l]);
            ramp(L.gR[l], L.gRStep[l], s.gRT[l], o.laneGR[l]);
            ramp(L.gM[l], L.gMStep[l], s.gMT[l], o.laneGM[l]);

            // The band-limited level for the highest pitch this lane reaches before the next tick.
            const float inc = std::max(L.inc[l], s.incT[l]) * rate * o.laneSlope[l];
            const int level = TableData::levelFor(inc);
            if (snap) {
                s.level[l] = s.levelNext[l] = level;
                setLaneLevel(s, l, table, level, false);
                setLaneLevel(s, l, table, level, true);
                L.xf[l] = L.xfStep[l] = 0.0f;
            } else if (level != s.level[l] && s.levelNext[l] == s.level[l]) {
                s.levelNext[l] = level;
                setLaneLevel(s, l, table, level, true);
                L.xf[l] = 0.0f;
                L.xfStep[l] = perStep;
                s.levelFade = true;
            }
        }
        if (snap)
            s.levelFade = false;
    }

    // At a control tick: every ramp has arrived; level fades are complete.
    static void settleLanes(LaneSet& s, int padded) {
        osc::Lanes& L = s.lanes;
        for (int l = 0; l < padded; ++l) {
            L.inc[l] = s.incT[l];
            L.fpos[l] = s.fposT[l];
            L.dA[l] = s.dAT[l];
            L.dB[l] = s.dBT[l];
            L.gL[l] = s.gLT[l];
            L.gR[l] = s.gRT[l];
            L.gM[l] = s.gMT[l];
            if (s.levelNext[l] != s.level[l]) {
                s.level[l] = s.levelNext[l];
                L.tabOff[l] = L.tabOff2[l];
                L.tabStride[l] = L.tabStride2[l];
                L.tabSize[l] = L.tabSize2[l];
            }
            L.xf[l] = L.xfStep[l] = 0.0f;
        }
        s.levelFade = false;
    }

    void aimOsc(Voice& v, int i, bool snap) {
        OscShared& o = osc[i];
        OscVoice& ov = v.osc[i];
        const double note = (o.keytrack ? v.note : kFixedPitchNote) + o.tuneSemis + bendSemis();
        const double base = pitchInc(note);
        const float* ratio = o.laneRatio;
        if (o.law == UnisonLaw::Random) {
            if (ov.ratioEpoch != o.ratioEpoch) {
                for (int l = 0; l < kMaxLanes; ++l)
                    ov.ratio[l] = std::exp2((ov.randomDetune[l < o.count ? l : 0] * o.spreadSemis * 0.5f + o.stackSemis[l]) / 12.0f);
                ov.ratioEpoch = o.ratioEpoch;
            }
            ratio = ov.ratio;
        }
        aimLanes(ov.main, o, *o.table, ratio, base, snap);
        if (ov.fadeLeft > 0 && o.oldTable != nullptr)
            aimLanes(ov.fade, o, *o.oldTable, ratio, base, snap);
        float left = 0.0f, right = 0.0f;
        balance(o.pan, o.levelGain, left, right);
        ov.gain.aim(left, right, samplesToTick, snap);
    }

    void aimSub(Voice& v, bool snap) {
        const double note = v.note + sub.tuneSemis + bendSemis();
        v.sub.incTarget = pitchInc(note) / sub.oversample;
        if (snap)
            v.sub.inc = v.sub.incTarget;
        v.sub.incStep = (v.sub.incTarget - v.sub.inc) / (samplesToTick * sub.oversample);
        float left = 0.0f, right = 0.0f;
        balance(sub.pan, sub.levelGain * std::sqrt(0.5), left, right); // a mono source: -3 dB per side at the centre
        v.sub.gain.aim(left, right, samplesToTick, snap);
    }

    void aimTexture(Voice& v, bool snap) {
        const SampleData& sample = *texture.sample;
        const double semis = texture.tuneSemis + bendSemis() + (texture.keytrack ? v.note - kFixedPitchNote : 0);
        const double rate = std::min(sample.sampleRate / sampleRate * std::exp2(semis / 12.0), 64.0) / texture.oversample;
        v.texture.rateTarget = rate;
        if (snap)
            v.texture.rate = rate;
        v.texture.rateStep = (rate - v.texture.rate) / (samplesToTick * texture.oversample);
        float left = 0.0f, right = 0.0f;
        balance(texture.pan, texture.levelGain, left, right);
        v.texture.gain.aim(left, right, samplesToTick, snap);
    }

    void aimSources(Voice& v, bool snap) {
        for (int i = 0; i < kOscCount; ++i)
            if (osc[i].on)
                aimOsc(v, i, snap);
        if (sub.on)
            aimSub(v, snap);
        if (texture.on)
            aimTexture(v, snap);
    }

    void aimAmp(Voice& v) {
        const double gain = v.envTarget * velocityGain(v);
        v.ampStep = static_cast<float>((gain - v.amp) / samplesToTick);
    }

    // A source's oversampling changed: its decimators start clean.
    void resetRates(Voice& v) {
        for (auto& o : v.osc) {
            o.decimatorL.reset();
            o.decimatorR.reset();
        }
        v.sub.decimator.reset();
        v.texture.decimatorL.reset();
        v.texture.decimatorR.reset();
    }

    // Envelope level at this exact sample: the ramp runs backwards from its target.
    double envNow(const Voice& v) const { return v.envTarget - v.envStep * samplesToTick; }

    // After an envelope stage change at the current sample: advance to the next tick and re-aim.
    void restage(Voice& v, double from) {
        v.envTarget = v.env.advance(samplesToTick, envTimes);
        v.envStep = (v.envTarget - from) / samplesToTick;
        v.done = false;
        aimAmp(v);
    }

    // The unison voice count or spread law changed while a note plays.
    void relayoutVoice(Voice& v, int i) {
        OscVoice& ov = v.osc[i];
        for (int l = 0; l < kMaxLanes; ++l) {
            if (osc[i].law == UnisonLaw::Random)
                ov.randomDetune[l] = l == 0 && osc[i].count % 2 == 1 ? 0.0f : static_cast<float>(rng.uniform() * 2.0 - 1.0);
            if (l >= ov.main.lanes.count) // lanes that are new start somewhere of their own
                ov.main.lanes.phase[l] = toPhase(rng.uniform());
        }
        ov.ratioEpoch = 0;
        aimOsc(v, i, true);
    }

    void startVoice(Voice& v, int slot, int note, float velocity) {
        v.active = true;
        v.done = false;
        v.note = note;
        v.velocity = velocity;
        v.amp = 0.0f;
        v.ampStep = 0.0f;
        v.envTarget = v.envStep = 0.0;
        v.env = Envelope{};
        std::memset(v.mod, 0, sizeof v.mod);
        const Voice* previous = lastStarted >= 0 && lastStarted != slot && voices[static_cast<std::size_t>(lastStarted)].active
                                    ? &voices[static_cast<std::size_t>(lastStarted)]
                                    : nullptr;
        for (int i = 0; i < kOscCount; ++i) {
            OscShared& o = osc[i];
            OscVoice& ov = v.osc[i];
            ov.fadeLeft = 0;
            ov.ratioEpoch = 0;
            ov.decimatorL.reset();
            ov.decimatorR.reset();
            if (!o.on)
                continue;
            if (o.law == UnisonLaw::Random)
                for (int l = 0; l < kMaxLanes; ++l)
                    ov.randomDetune[l] = l == 0 && o.count % 2 == 1 ? 0.0f : static_cast<float>(rng.uniform() * 2.0 - 1.0);
            std::uint32_t* phase = ov.main.lanes.phase;
            if (o.continuePhase) {
                // Carry on from where the previous note's oscillator is right now; with no
                // previous note the lanes simply keep running from where this voice left them.
                if (previous != nullptr)
                    std::copy(previous->osc[i].main.lanes.phase, previous->osc[i].main.lanes.phase + kMaxLanes, phase);
            } else if (o.count > 1 && o.unisonPhase == UnisonPhase::Free) {
                // Free: the lanes keep running across notes.
            } else {
                const double base = o.startPhase + (o.phaseRandom > 0.0f ? o.phaseRandom * rng.uniform() : 0.0);
                phase[0] = toPhase(base);
                for (int l = 1; l < kMaxLanes; ++l) {
                    const double offset = l >= o.count ? 0.0
                                          : o.unisonPhase == UnisonPhase::Consistent ? o.consistentPhase[l]
                                                                                     : rng.uniform();
                    phase[l] = toPhase(base + offset);
                }
            }
        }
        v.sub.phase = 0.0;
        v.sub.decimator.reset();
        v.texture.fadeLeft = 0;
        v.texture.decimatorL.reset();
        v.texture.decimatorR.reset();
        v.texture.main.finished = false;
        if (texture.on) {
            const double random = texture.randomStart > 0.0f ? texture.randomStart * rng.uniform() : 0.0;
            const double start = std::clamp(texture.start + random * (1.0 - texture.start), 0.0, 1.0);
            v.texture.main.position = start * std::max(texture.sample->frames - 1, 0);
        } else {
            v.texture.main.position = 0.0;
        }
        aimSources(v, true);
        lastStarted = slot;
    }

    void applyActions() {
        for (int i = 0; i < actions.count; ++i) {
            const VoiceAction& a = actions.items[static_cast<std::size_t>(i)];
            Voice& v = voices[static_cast<std::size_t>(a.slot)];
            switch (a.kind) {
            case VoiceAction::Kind::Start: {
                const bool wasActive = v.active;
                startVoice(v, a.slot, a.note, a.velocity);
                v.env.trigger(0.0);
                restage(v, 0.0);
                activeCount += wasActive ? 0 : 1;
                break;
            }
            case VoiceAction::Kind::Retrigger: {
                if (!v.active)
                    break;
                const double from = envNow(v);
                v.note = a.note;
                v.velocity = a.velocity;
                v.env.trigger(from);
                restage(v, from);
                aimSources(v, false);
                break;
            }
            case VoiceAction::Kind::Glide:
                if (v.active) {
                    v.note = a.note;
                    aimSources(v, false);
                }
                break;
            case VoiceAction::Kind::Release:
                if (v.active) {
                    const double from = envNow(v);
                    v.env.release(from);
                    restage(v, from);
                }
                break;
            case VoiceAction::Kind::Kill:
                if (v.active) {
                    const double from = envNow(v);
                    v.env.kill(from, kStealFadeSeconds * sampleRate);
                    restage(v, from);
                }
                break;
            }
        }
        actions.count = 0;
    }

    void handle(const MidiEvent& e) {
        actions.count = 0;
        switch (e.type) {
        case MidiEvent::Type::NoteOn:
            allocator.noteOn(e.number, e.value, actions);
            break;
        case MidiEvent::Type::NoteOff:
            allocator.noteOff(e.number, actions);
            break;
        case MidiEvent::Type::PitchBend:
            bend = std::clamp(static_cast<double>(e.value), -1.0, 1.0);
            for (auto& v : voices)
                if (v.active)
                    aimSources(v, false);
            break;
        case MidiEvent::Type::Controller:
            if (e.number == 64)
                allocator.sustainPedal(e.value >= 0.5f, actions);
            else if (e.number == 120)
                allocator.allSoundOff(actions);
            else if (e.number == 123)
                allocator.allNotesOff(actions);
            else if (e.number == 1)
                modWheel = e.value;
            break;
        case MidiEvent::Type::ChannelPressure:
        case MidiEvent::Type::PolyPressure:
            break; // modulation sources arrive in milestone 3
        }
        applyActions();
    }

    // The control-rate tick: every kControlInterval samples, on a grid fixed in absolute time.
    // Milestone 3 evaluates LFOs and the modulation matrix here.
    void tick() {
        samplesToTick = kInterval;
        master = masterTarget;
        masterStep = 0.0;
        smoothControls();
        for (int i = 0; i < kOscCount; ++i)
            if (osc[i].lanesDirty)
                updateLaneTables(i);
        for (int slot = 0; slot < kVoiceSlots; ++slot) {
            Voice& v = voices[static_cast<std::size_t>(slot)];
            if (!v.active)
                continue;
            if (v.done) {
                v.active = false;
                --activeCount;
                allocator.voiceFinished(slot);
                continue;
            }
            for (int i = 0; i < kOscCount; ++i) {
                if (!osc[i].on)
                    continue;
                settleLanes(v.osc[i].main, osc[i].padded);
                if (v.osc[i].fadeLeft > 0)
                    settleLanes(v.osc[i].fade, osc[i].padded);
                v.osc[i].gain.settle();
            }
            v.sub.inc = v.sub.incTarget;
            v.sub.gain.settle();
            v.texture.rate = v.texture.rateTarget;
            v.texture.gain.settle();
            aimSources(v, false);

            const double from = v.envTarget;
            v.envTarget = v.env.advance(kInterval, envTimes);
            v.envStep = (v.envTarget - from) / kInterval;
            aimAmp(v);
            v.done = v.env.finished();
            allocator.setLevel(slot, static_cast<float>(v.envTarget * velocityGain(v)));
        }
    }

    // --- Tables and samples -------------------------------------------------------------------

    // At the start of a block: switch to anything that was handed over since the last one.
    void takeHandoffs() {
        for (int i = 0; i < kOscCount; ++i) {
            OscShared& o = osc[i];
            if (o.fadeLeft <= 0) {
                o.oldTable = nullptr;
                tables[i].retirePrevious();
            }
            if (tables[i].previous() == nullptr && tables[i].take()) {
                o.oldTable = tables[i].previous();
                o.table = tables[i].current();
                o.fadeLeft = activeCount > 0 && o.on && !o.classic && o.oldTable != nullptr ? kFade : 0;
                for (auto& v : voices) {
                    if (!v.active || !o.on)
                        continue;
                    OscVoice& ov = v.osc[i];
                    ov.fade = ov.main; // keeps playing the old table from the same phases
                    ov.fadeLeft = o.fadeLeft;
                    aimOsc(v, i, true);
                }
            }
            for (int s = 0; s < 2; ++s) {
                if (curves[i][s].take())
                    curves[i][s].retirePrevious();
                else if (curves[i][s].previous() != nullptr)
                    curves[i][s].retirePrevious();
                const WarpCurve* curve = curves[i][s].current();
                o.curve[s] = curve != nullptr ? curve->values.data() : osc::defaultWarpCurve();
            }
        }
        if (texture.fadeLeft <= 0) {
            texture.oldSample = nullptr;
            sampleSlot.retirePrevious();
        }
        if (sampleSlot.previous() == nullptr && sampleSlot.take()) {
            texture.oldSample = sampleSlot.previous();
            texture.sample = sampleSlot.current();
            texture.fadeLeft = activeCount > 0 && texture.on && texture.oldSample != nullptr ? kFade : 0;
            for (auto& v : voices) {
                if (!v.active)
                    continue;
                v.texture.fade = v.texture.main;
                v.texture.fadeLeft = texture.fadeLeft;
                // The new sample starts from the same relative place.
                const double oldLength = std::max(texture.oldSample != nullptr ? texture.oldSample->frames : 1, 1);
                v.texture.main.position = std::clamp(v.texture.main.position / oldLength, 0.0, 1.0) *
                                          std::max(texture.sample->frames - 1, 0);
                v.texture.main.finished = false;
                if (texture.on)
                    aimTexture(v, true);
            }
        }
    }

    // --- Audio --------------------------------------------------------------------------------

    static void mixToBuses(float bus[3][2][kInterval], const float* left, const float* right, int frames, SourceGain& gain,
                           const float send[3]) {
        float gl = gain.now[0], gr = gain.now[1];
        for (int s = 0; s < frames; ++s) {
            const float l = left[s] * gl, r = right[s] * gr;
            gl += gain.step[0];
            gr += gain.step[1];
            for (int b = 0; b < 3; ++b) {
                bus[b][0][s] += l * send[b];
                bus[b][1][s] += r * send[b];
            }
        }
        gain.now[0] = gl;
        gain.now[1] = gr;
    }

    // Keeps the end of a modulator's run for consumers that read it one base sample late.
    static void keepHistory(float* mod, int samples, int rate) {
        for (int k = 0; k < rate; ++k)
            mod[kHistory - rate + k] = mod[kHistory + samples - rate + k];
    }

    const float* modulatorFor(const Voice& v, int i, int slot) const {
        const OscShared& o = osc[i];
        if (!osc::warpUsesModulator(o.mode[slot]))
            return nullptr;
        const int source = o.modSource[slot];
        const bool playing = source < kOscCount ? osc[source].on : source == kSub ? sub.on : texture.on;
        if (!playing)
            return kZeros;
        return v.mod[source] + (routing.delayed[i][slot] ? kHistory - o.oversample : kHistory);
    }

    void renderOsc(Voice& v, int i, int frames, float bus[3][2][kInterval]) {
        OscShared& o = osc[i];
        OscVoice& ov = v.osc[i];
        const bool modulator = routing.usedAsModulator[i];
        if (ov.gain.silent() && !modulator) { // inaudible and nobody listens: only keep time
            const int steps = frames * o.oversample;
            for (int l = 0; l < o.padded; ++l)
                ov.main.lanes.phase[l] += static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(static_cast<double>(ov.main.lanes.inc[l]) * 4294967296.0 * steps));
            ov.fadeLeft = std::max(ov.fadeLeft - frames, 0);
            return;
        }
        const int rate = o.oversample;
        const int samples = frames * rate;
        const bool stereo = o.count > 1;
        alignas(32) float left[kMaxRun], right[kMaxRun];
        float* mod = modulator ? v.mod[i] + kHistory : nullptr;

        osc::Block block;
        block.samples = samples;
        block.table = o.table->data();
        block.maxFrame = static_cast<float>(o.table->frames() - 1);
        block.levelFade = ov.main.levelFade;
        block.classic = o.classic;
        block.shape = o.shape;
        block.pulseWidth = o.pulseWidth;
        block.modeA = o.mode[0];
        block.modeB = o.mode[1];
        block.modA = modulatorFor(v, i, 0);
        block.modB = modulatorFor(v, i, 1);
        block.curveA = o.curve[0];
        block.curveB = o.curve[1];
        block.outL = left;
        block.outR = stereo ? right : nullptr;
        block.outM = mod;
        o.render(ov.main.lanes, block);

        if (ov.fadeLeft > 0) {
            if (o.oldTable != nullptr && !o.classic) {
                // The previous table plays on from the same phases and fades out under the new one.
                alignas(32) float oldL[kMaxRun], oldR[kMaxRun], oldM[kMaxRun];
                block.table = o.oldTable->data();
                block.maxFrame = static_cast<float>(o.oldTable->frames() - 1);
                block.levelFade = ov.fade.levelFade;
                block.outL = oldL;
                block.outR = stereo ? oldR : nullptr;
                block.outM = modulator ? oldM : nullptr;
                o.render(ov.fade.lanes, block);
                float w = 1.0f - static_cast<float>(ov.fadeLeft) / kFade;
                const float step = 1.0f / (static_cast<float>(kFade) * static_cast<float>(rate));
                for (int s = 0; s < samples; ++s) {
                    const float mix = std::min(w, 1.0f);
                    left[s] = oldL[s] + mix * (left[s] - oldL[s]);
                    if (stereo)
                        right[s] = oldR[s] + mix * (right[s] - oldR[s]);
                    if (modulator)
                        mod[s] = oldM[s] + mix * (mod[s] - oldM[s]);
                    w += step;
                }
            }
            ov.fadeLeft = std::max(ov.fadeLeft - frames, 0);
        }
        if (modulator)
            keepHistory(v.mod[i], samples, rate);
        if (rate > 1) {
            dsp::decimate(*decimator, ov.decimatorL, left, frames, rate);
            if (stereo)
                dsp::decimate(*decimator, ov.decimatorR, right, frames, rate);
        }
        mixToBuses(bus, left, stereo ? right : left, frames, ov.gain, o.send);
    }

    void renderSub(Voice& v, int frames, float bus[3][2][kInterval]) {
        SubVoice& sv = v.sub;
        const bool modulator = routing.usedAsModulator[kSub];
        const int rate = sub.oversample;
        const int samples = frames * rate;
        float wave[kMaxRun];
        double phase = sv.phase, inc = sv.inc;
        for (int s = 0; s < samples; ++s) {
            wave[s] = subWave(sub.shape, static_cast<float>(phase), static_cast<float>(inc));
            phase += inc;
            if (phase >= 1.0)
                phase -= 1.0;
            inc += sv.incStep;
        }
        sv.phase = phase;
        sv.inc = inc;
        if (modulator) {
            std::copy(wave, wave + samples, v.mod[kSub] + kHistory);
            keepHistory(v.mod[kSub], samples, rate);
        }
        if (rate > 1)
            dsp::decimate(*decimator, sv.decimator, wave, frames, rate);
        const float send[3] = {sub.direct ? 0.0f : 1.0f - sub.balance, sub.direct ? 0.0f : sub.balance,
                               sub.direct ? 1.0f : 0.0f};
        mixToBuses(bus, wave, wave, frames, sv.gain, send);
    }

    void playTexture(const SampleData& sample, TexturePlay& play, double rate, double rateStep, int samples, float* left,
                     float* right) const {
        const int frames = sample.frames;
        const bool loop = texture.loop;
        const int loopStart = loop && sample.hasLoop ? sample.loopStart : 0;
        const int loopEnd = loop && sample.hasLoop ? sample.loopEnd : frames;
        const double loopLength = loopEnd - loopStart;
        double position = play.position;
        for (int s = 0; s < samples; ++s) {
            if (play.finished || frames < 2) {
                left[s] = right[s] = 0.0f;
                continue;
            }
            const int i = static_cast<int>(position);
            const auto t = static_cast<float>(position - i);
            // Four-point interpolation through the neighbouring samples.
            const auto read = [&](const std::vector<float>& data) {
                const float a = sampleAt(data, i - 1, frames, loop, loopStart, loopEnd);
                const float b = sampleAt(data, i, frames, loop, loopStart, loopEnd);
                const float c = sampleAt(data, i + 1, frames, loop, loopStart, loopEnd);
                const float d = sampleAt(data, i + 2, frames, loop, loopStart, loopEnd);
                return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
            };
            left[s] = read(sample.left);
            right[s] = read(sample.right);
            position += rate;
            rate += rateStep;
            if (loop) {
                if (position >= loopEnd && loopLength >= 2.0)
                    position -= loopLength * std::floor((position - loopStart) / loopLength);
            } else if (position >= frames - 1) {
                play.finished = true;
            }
        }
        play.position = position;
    }

    void renderTexture(Voice& v, int frames, float bus[3][2][kInterval]) {
        TextureVoice& tv = v.texture;
        const bool modulator = routing.usedAsModulator[kTexture];
        const int rate = texture.oversample;
        const int samples = frames * rate;
        float left[kMaxRun], right[kMaxRun];
        playTexture(*texture.sample, tv.main, tv.rate, tv.rateStep, samples, left, right);
        if (tv.fadeLeft > 0) {
            if (texture.oldSample != nullptr) {
                float oldL[kMaxRun], oldR[kMaxRun];
                const double oldRate = tv.rate * texture.oldSample->sampleRate / texture.sample->sampleRate;
                playTexture(*texture.oldSample, tv.fade, oldRate, 0.0, samples, oldL, oldR);
                float w = 1.0f - static_cast<float>(tv.fadeLeft) / kFade;
                const float step = 1.0f / (static_cast<float>(kFade) * static_cast<float>(rate));
                for (int s = 0; s < samples; ++s) {
                    const float mix = std::min(w, 1.0f);
                    left[s] = oldL[s] + mix * (left[s] - oldL[s]);
                    right[s] = oldR[s] + mix * (right[s] - oldR[s]);
                    w += step;
                }
            }
            tv.fadeLeft = std::max(tv.fadeLeft - frames, 0);
        }
        tv.rate += tv.rateStep * samples;
        if (modulator) {
            float* mod = v.mod[kTexture] + kHistory;
            for (int s = 0; s < samples; ++s)
                mod[s] = 0.5f * (left[s] + right[s]);
            keepHistory(v.mod[kTexture], samples, rate);
        }
        if (rate > 1) {
            dsp::decimate(*decimator, tv.decimatorL, left, frames, rate);
            dsp::decimate(*decimator, tv.decimatorR, right, frames, rate);
        }
        const float send[3] = {texture.direct ? 0.0f : 1.0f - texture.balance, texture.direct ? 0.0f : texture.balance,
                               texture.direct ? 1.0f : 0.0f};
        mixToBuses(bus, left, right, frames, tv.gain, send);
    }

    void renderVoice(Voice& v, float* outL, float* outR, int frames) {
        int done = 0;
        while (done < frames) {
            // A modulation loop needs each source's previous sample: go one sample at a time.
            const int run = routing.loop ? 1 : frames - done;
            float bus[3][2][kInterval] = {}; // to filter 1, to filter 2, direct
            for (const int source : routing.order) {
                if (source < kOscCount) {
                    if (osc[source].on)
                        renderOsc(v, source, run, bus);
                } else if (source == kSub) {
                    if (sub.on)
                        renderSub(v, run, bus);
                } else if (texture.on) {
                    renderTexture(v, run, bus);
                }
            }
            // The two filter slots: in series at 0 %, in parallel at 100 % (plan 3.5).
            filter1.process(bus[0][0], bus[0][1], run);
            const float parallel = filterRouting, serial = 1.0f - filterRouting;
            for (int c = 0; c < 2; ++c)
                for (int s = 0; s < run; ++s)
                    bus[1][c][s] += serial * bus[0][c][s];
            filter2.process(bus[1][0], bus[1][1], run);
            float amp = v.amp;
            for (int s = 0; s < run; ++s) {
                outL[done + s] += (parallel * bus[0][0][s] + bus[1][0][s] + bus[2][0][s]) * amp;
                outR[done + s] += (parallel * bus[0][1][s] + bus[1][1][s] + bus[2][1][s]) * amp;
                amp += v.ampStep;
            }
            v.amp = amp;
            done += run;
        }
    }

    void render(float* left, float* right, int frames) {
        std::fill(left, left + frames, 0.0f);
        std::fill(right, right + frames, 0.0f);
        if (activeCount > 0) {
            for (auto& v : voices)
                if (v.active)
                    renderVoice(v, left, right, frames);
            for (int i = 0; i < frames; ++i) {
                const auto gain = static_cast<float>(master);
                left[i] *= gain;
                right[i] *= gain;
                master += masterStep;
            }
        } else {
            for (int i = 0; i < frames; ++i)
                master += masterStep;
        }
        for (auto& o : osc)
            o.fadeLeft = std::max(o.fadeLeft - frames, 0);
        texture.fadeLeft = std::max(texture.fadeLeft - frames, 0);
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {
    const auto& reg = Registry::get();
    for (const auto& def : reg.params())
        impl_->store.set(def.index, def.def);
    reset();
}

Engine::~Engine() = default;

void Engine::prepare(double sampleRate) {
    impl_->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    impl_->updateSmoothing();
    reset();
}

void Engine::reset() {
    auto& s = *impl_;
    s.allocator.reset();
    for (auto& v : s.voices) {
        v.active = false;
        v.done = false;
        for (auto& o : v.osc) {
            std::fill(o.main.lanes.phase, o.main.lanes.phase + kMaxLanes, 0u);
            o.fadeLeft = 0;
        }
        v.texture.fadeLeft = 0;
    }
    s.activeCount = 0;
    s.lastStarted = -1;
    s.activeForOthers.store(0, std::memory_order_relaxed);
    s.samplesToTick = kControlInterval;
    s.bend = 0.0;
    s.modWheel = 0.0;
    s.rng.seed(s.seed);
    s.snapMaster = true;
    s.snapSmoothing = true;
    s.dirty = true;
}

void Engine::setSeed(std::uint64_t seed) { impl_->seed = seed; }

void Engine::loadPatch(const Patch& patch) {
    for (int i = 0; i < impl_->store.size(); ++i)
        impl_->store.set(i, patch.get(i));
}

void Engine::setOffline(bool offline) { impl_->offline.store(offline, std::memory_order_relaxed); }

ParamStore& Engine::params() noexcept { return impl_->store; }
const ParamStore& Engine::params() const noexcept { return impl_->store; }
int Engine::activeVoices() const noexcept { return impl_->activeForOthers.load(std::memory_order_relaxed); }
double Engine::sampleRate() const noexcept { return impl_->sampleRate; }

double Engine::tailSeconds() const noexcept {
    return impl_->store.get(impl_->ids.envRelease) / 1000.0 + kStealFadeSeconds + 0.01;
}

void Engine::setTable(int osc, std::shared_ptr<const TableData> table) {
    if (osc < 0 || osc >= kOscCount)
        return;
    impl_->tables[osc].set(table != nullptr ? std::move(table) : TableData::builtin());
}

std::shared_ptr<const TableData> Engine::table(int osc) const {
    return osc >= 0 && osc < kOscCount ? impl_->tables[osc].latest() : nullptr;
}

void Engine::setTexture(std::shared_ptr<const SampleData> sample) {
    impl_->sampleSlot.set(sample != nullptr && sample->frames >= 2 ? std::move(sample) : SampleData::builtin());
}

std::shared_ptr<const SampleData> Engine::texture() const { return impl_->sampleSlot.latest(); }

void Engine::setWarpCurve(int osc, int slot, std::span<const float> values) {
    if (osc < 0 || osc >= kOscCount || slot < 0 || slot > 1)
        return;
    auto curve = std::make_shared<WarpCurve>();
    osc::fillWarpCurve(values, curve->values.data());
    impl_->curves[osc][slot].set(std::move(curve));
}

void Engine::collectGarbage() {
    for (int i = 0; i < kOscCount; ++i) {
        impl_->tables[i].collect();
        impl_->curves[i][0].collect();
        impl_->curves[i][1].collect();
    }
    impl_->sampleSlot.collect();
}

void Engine::process(const ProcessArgs& args) noexcept {
    const rt::AudioThreadScope audioThread;
    const rt::ScopedNoDenormals noDenormals;
    auto& s = *impl_;
    if (args.frames <= 0 || args.left == nullptr || args.right == nullptr)
        return;

    s.store.drain([&s](int index, double value) {
        s.p[static_cast<std::size_t>(index)] = value;
        s.dirty = true;
    });
    const bool offline = s.offline.load(std::memory_order_relaxed);
    if (offline != s.offlineSeen) {
        s.offlineSeen = offline;
        s.dirty = true;
    }
    if (s.dirty) {
        s.refresh();
        s.dirty = false;
    }
    s.takeHandoffs();

    const auto& midi = args.midi;
    std::size_t next = 0;
    int pos = 0;
    while (pos < args.frames) {
        while (next < midi.size() && midi[next].offset <= pos)
            s.handle(midi[next++]);
        int end = args.frames;
        if (next < midi.size())
            end = std::min(end, midi[next].offset);
        const int frames = std::min(end - pos, s.samplesToTick);
        s.render(args.left + pos, args.right + pos, frames);
        pos += frames;
        s.samplesToTick -= frames;
        if (s.samplesToTick == 0)
            s.tick();
    }
    while (next < midi.size()) // offsets at or past the end of the block
        s.handle(midi[next++]);
    s.activeForOthers.store(s.activeCount, std::memory_order_relaxed);
}

} // namespace zed
