// The voice filter (plan 3.5, 5.7): one slot of one voice, in stereo. Every type is built from
// zero-delay-feedback (topology-preserving) parts: trapezoidal one-pole and two-pole sections
// whose response is exactly the bilinear image of the analog prototype, a four-pole ladder with a
// solved feedback loop, and delay loops for the comb family.
//
// A slot is: input drive stage -> core -> (state-variable family only) body stage -> dry/wet mix.
// Linear cores run at the base rate. The saturating stages and the nonlinear cores (ladder, howl,
// ring modulator) run oversampled by the quality mode and come back through the same half-band
// filters the oscillators use; the dry signal takes the same path so the mix stays in phase.
//
// The engine sets where the controls should be at the next control tick (aim) and renders the
// samples in between (process); coefficients move in straight lines per sample, and an optional
// audio-rate signal can move the cutoff on top of that.
#pragma once

#include "zederia/dsp/HalfBand.h"

#include <complex>
#include <cstdint>

namespace zed::flt {

enum class Family : std::uint8_t {
    Svf, Ladder, Dual, Morph, Comb, Flange, Phase, Formant, Shelf, RingMod, SampleHold, AllpassCluster,
    Diffuser, BandReject, Howl, GritComb,
};

// Outputs of a two-pole section.
enum class Tap : std::uint8_t { Lp, Hp, Bp, Notch, Peak };

// What a type index (the registry's filterN.type choice) means.
//   Svf:    a = Tap, b = poles (1..4)            Ladder: b = poles (1..4)
//   Dual:   a, b = the two Taps, in series        Morph:  a = variant (0 lbh, 1 lph, 2 lnh, 3 bpn), b = poles
//   Comb, Flange, Phase: a = 0 positive / 1 negative feedback, b = damping (0 lp, 1 hp, 2 bp)
//   Formant: a = voicing                          Shelf:  a = 0 low shelf, 1 high shelf, 2 bell
//   Howl:   a = 0 low-pass, 1 band-pass
struct TypeInfo {
    Family family = Family::Svf;
    std::uint8_t a = 0, b = 0;
};
int typeCount() noexcept;
const TypeInfo& typeInfo(int type) noexcept; // out-of-range indices give type 0

// The controls of one slot, as the engine hands them over: after key tracking, stereo offset,
// modulation and smoothing.
struct Settings {
    int type = 1;
    float cutoffHz[2] = {2000.0f, 2000.0f}; // left, right
    float resonance = 0.0f;                 // 0..1
    float drive = 0.0f;                     // 0..1
    float character = 0.5f;                 // 0..1, meaning depends on the type
    float mix = 1.0f;                       // 0..1
    bool levelMode = false;                 // true: mix is an output level and no dry signal passes
};

// Linear types as a chain (or a parallel bank) of one- and two-pole sections. The same design
// feeds the audio code and the response display, so the two cannot disagree.
inline constexpr int kMaxStages = 6;
struct StageDesign {
    int poles = 0;                 // 1 or 2
    double hz[2] = {1000.0, 1000.0};
    double gScale = 1.0;           // multiplies tan(pi hz / rate): the shelves move their corner this way
    double k = 1.0;                // damping, 1 / Q (two-pole sections)
    double m[3] = {1.0, 0.0, 0.0}; // output = m0 * low + m1 * band + m2 * high
};
struct LinearDesign {
    int count = 0;
    bool parallel = false;
    StageDesign stage[kMaxStages];
};
// False for types that are not a section chain (ladder, the delay types, ring modulator, ...).
bool designLinear(const Settings& s, LinearDesign& out) noexcept;

// The slot's small-signal response at `hz`, dry/wet mix included: what the display draws and what
// the tests measure against. For types without one (ring modulator, sample-and-hold, grit comb)
// hasLinearResponse() is false and this returns the response of their linear part. `oversample`
// is the quality factor in use: the ladder and howl cores are designed at that rate.
std::complex<double> response(const Settings& s, int channel, double sampleRate, double hz, int oversample = 1) noexcept;
bool hasLinearResponse(int type) noexcept;
// True when the type's core distorts by design and therefore runs oversampled.
bool hasNonlinearCore(int type) noexcept;

// Samples of delay memory a slot needs per channel for the comb family at this sample rate
// (a power of two).
int delayLengthFor(double sampleRate) noexcept;

class VoiceFilter {
public:
    static constexpr int kMaxFrames = 16;    // base-rate samples per process() call
    static constexpr int kMaxOversample = 8;

    // `delayMemory` holds 2 * delayLength floats (left, then right) that outlive the filter; null
    // makes the delay types pass their input through.
    void prepare(double sampleRate, float* delayMemory, int delayLength) noexcept;
    void reset() noexcept; // clears every state, the delay memory included

    // Where the controls should be `samples` base-rate samples from now. `oversample` is the
    // quality mode's factor (1, 2, 4 or 8). `snap` jumps there at once (a new note).
    void aim(const Settings& s, int samples, int oversample, bool snap) noexcept;
    // The ramps have arrived: removes rounding drift. Call at each control tick before aim().
    void settle() noexcept;
    // Filters `frames` (1..kMaxFrames) samples in place. `cutoffMod` is null or one value per
    // sample: an offset of the cutoff in octaves on top of the aimed value.
    void process(float* left, float* right, int frames, const float* cutoffMod,
                 const dsp::DecimatorDesign& halfBands) noexcept;

private:
    struct Ramp {
        float now = 0.0f, step = 0.0f, target = 0.0f;
        void aim(float value, float perStep, bool snap) noexcept {
            target = value;
            if (snap)
                now = value;
            step = (value - now) * perStep;
        }
        void settle() noexcept {
            now = target;
            step = 0.0f;
        }
    };
    struct Resampler { // one channel's way up and back down
        dsp::DecimatorState up, down;
    };

    void aimChain(const Settings& s, float perStep, bool snap) noexcept;
    void runChain(float* io, int ch, int frames, const float* modScale) noexcept;
    void runSaturator(float* left, float* right, int frames, Resampler* resamplers, Ramp& gain, Ramp& amount, float& fade,
                      bool wanted, const dsp::DecimatorDesign& hb) noexcept;
    void runLadder(float* io, int ch, int samples, const float* modScale) noexcept;
    void runHowl(float* io, int ch, int samples, const float* modScale) noexcept;
    void runRing(float* io, int ch, int samples, const float* modScale) noexcept;
    void runSampleHold(float* io, int ch, int frames, const float* modScale) noexcept;
    void runComb(float* io, int ch, int frames, const float* modScale) noexcept;
    void runPhaser(float* io, int ch, int frames, const float* modScale) noexcept;
    void runDiffuser(float* io, int ch, int frames, const float* modScale) noexcept;
    void advanceRamps(int coreSteps, int frames) noexcept;
    void guardStates() noexcept;
    float readDelay(int ch, float delay) const noexcept;

    double sampleRate_ = 48000.0;
    float* memory_[2] = {nullptr, nullptr};
    int memoryLength_ = 0, memoryMask_ = 0;
    int writeIndex_[2] = {0, 0};

    int type_ = -1;
    TypeInfo info_;
    int oversample_ = 1; // the quality mode's factor
    int coreRate_ = 1;   // the factor the core runs at (1 for linear cores)

    // Section chain. For the other families stage 0 carries the cutoff (as tan(pi f / rate), a
    // delay in samples, or a phase increment) and the family's own values sit in p_.
    int stages_ = 0;
    bool parallel_ = false;
    int poles_[kMaxStages] = {};
    Ramp g_[kMaxStages][2];
    Ramp k_[kMaxStages];
    Ramp m_[kMaxStages][3];
    float s1_[kMaxStages][2] = {}, s2_[kMaxStages][2] = {};

    Ramp p_[6];          // family-specific values (see Filter.cpp)
    Ramp dampLp_[2], dampHp_[2]; // one-pole coefficients of the loop damping, per channel
    float dampState_[2][2] = {};
    bool dampUseLp_ = false, dampUseHp_ = false;
    float phase_[2] = {0.0f, 0.0f}; // ring modulator carrier, sample-and-hold clock
    float held_[2] = {0.0f, 0.0f}, heldBefore_[2] = {0.0f, 0.0f};

    Ramp dry_, wet_;
    Ramp driveGain_, driveAmount_, bodyGain_, bodyAmount_;
    bool driveWanted_ = false, bodyWanted_ = false;
    float driveFade_ = 0.0f, bodyFade_ = 0.0f; // 0..1: how far the stage is faded in
    Resampler driveRs_[2], bodyRs_[2], coreRs_[2], dryRs_[2];
};

} // namespace zed::flt
