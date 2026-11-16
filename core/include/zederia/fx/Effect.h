// The effects (plan 3.8). Each effect type in the registry has one implementation here. The rack
// (fx/Rack.h) owns the instances, feeds them their parameter values at control ticks and runs
// them on the summed signal.
//
// Threading: makeEffect() and prepare() allocate and run off the audio thread. clear(), set(),
// process() and the queries run on the audio thread and never allocate, lock or wait.
#pragma once

#include "zederia/dsp/HalfBand.h"

#include <complex>
#include <memory>
#include <span>

namespace zed::fx {

// Base-rate samples per process() call at most: one control interval of the engine.
inline constexpr int kMaxFrames = 16;
// Points across a drawn transfer curve (Drive's custom and dual curves): kCurveSize + 1 values of y
// (0..1) for x evenly from 0 to 1.
inline constexpr int kCurveSize = 256;

// Where an effect runs: what the engine knows that the parameters do not say.
struct Context {
    double sampleRate = 48000.0;
    double bpm = 120.0;
    int oversample = 2;   // the quality mode's factor (1, 2, 4, 8) for paths that distort
    bool draft = false;   // Draft quality: cheaper interpolation in the delay-based effects
    const dsp::DecimatorDesign* halfBands = nullptr;
    bool operator==(const Context&) const = default;
};

class Effect {
public:
    virtual ~Effect() = default;

    // Allocates the delay memory for a sample rate. Never on the audio thread.
    virtual void prepare(double sampleRate) = 0;
    // Silences every state (delay lines, filters, envelopes). Audio thread.
    virtual void clear() noexcept = 0;
    // The parameter values in the effect's own units, in the order of its registry parameters.
    // Called at a control tick when a value or the context changed. Values given with snap = true
    // apply at once; otherwise the effect moves to them over the next control interval.
    virtual void set(const double* values, const Context& context, bool snap) noexcept = 0;
    // Processes `frames` (1..kMaxFrames) samples in place and leaves the wet signal there.
    virtual void process(float* left, float* right, int frames) noexcept = 0;

    // How long input and output must stay silent before nothing the effect holds can still be
    // heard (an echo on its way through a delay line, a pre-delay).
    virtual int holdSamples() const noexcept { return 0; }
    // False while the effect makes sound on its own (a frozen reverb).
    virtual bool mayIdle() const noexcept { return true; }
    // A note started somewhere in the instrument (Swarm restarts its movement).
    virtual void noteOn() noexcept {}
    // The drawn transfer curves (kCurveSize + 1 values each); null restores the built-in ones.
    virtual void setCurves(const float* curveA, const float* curveB) noexcept { (void)curveA, (void)curveB; }
    // A reading for the interface, if the effect has one (the Widener's stereo correlation, -1..1).
    virtual float meter() const noexcept { return 0.0f; }
};

// An instance of the registry's effect type `type`, prepared for `sampleRate`; null when the type
// is unknown.
std::unique_ptr<Effect> makeEffect(int type, double sampleRate);

// The steady-state response of an effect with these parameter values at `hz`, for the displays
// (EQ curve, filter response). False when the type has no such response.
bool effectResponse(int type, std::span<const double> values, double sampleRate, double hz, std::complex<double>& out);

// Parameter names each implementation expects, in order. A test checks them against the registry.
std::span<const char* const> implementedParams(int type);

// Length of the registry's tempo division `index` in quarter notes (1 when out of range).
double divisionBeats(int index) noexcept;

} // namespace zed::fx
