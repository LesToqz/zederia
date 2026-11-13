// The oscillator's inner loop (plan 3.2 to 3.4, 5.4): one call renders a run of samples for all
// unison lanes of one oscillator of one voice. The same source (src/engine/OscKernel.inl) is
// compiled three times, for one, four and eight lanes at a time; all three give bit-identical
// results, so the engine may pick whichever suits the lane count and the processor, and tests
// compare the wide paths against the scalar one exactly.
#pragma once

#include <cstdint>
#include <span>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // structures padded because of alignas: intended
#endif

namespace zed::osc {

inline constexpr int kMaxLanes = 16;      // unison voices per oscillator
inline constexpr int kMaxOversample = 8;  // Ultra quality
inline constexpr int kMaxRun = 16 * kMaxOversample; // samples per call: one control interval, oversampled
inline constexpr int kCurvePoints = 256;  // intervals of a warp curve; the table holds two more entries

// In registry order (osc.warp_a.mode).
enum class WarpMode : std::uint8_t {
    Off, Sync, SoftSync, BendPlus, BendMinus, BendBoth, PulseWidth, SkewPlus, SkewMinus, SkewBoth, Mirror,
    Flip, Quantize, CurveFull, CurveHalf, CurveSine, CurveQuad, Fold, Drive, Pm, Am, Rm,
};
inline constexpr int kNumWarpModes = 22;

// What a mode needs. Phase modes reshape the phase before the wave is read; amplitude modes act
// on the result. Modes that take another source as modulator are pm, am and rm.
bool warpIsPhaseShaper(WarpMode mode) noexcept; // sync .. curve_quad: needs a wavetable
bool warpUsesModulator(WarpMode mode) noexcept;
bool warpUsesCurve(WarpMode mode) noexcept;

// The kernel does not work with the 0..1 amount directly but with a value derived from it once
// per control tick (a ratio, a curve constant, a gain), which it then moves in a straight line.
// Keeping the derivation here means displays and audio agree (plan 5.7).
float warpDerive(WarpMode mode, float amount) noexcept;
// How much steeper the phase can run through the wave than without the warp; the oscillator
// picks a band-limited level for pitch times this factor.
float warpSlope(WarpMode mode, float derived) noexcept;

// Reference implementation of one warp slot's phase stage in plain floats: returns the warped
// phase and multiplies `gain` by the slot's amplitude factor (soft sync window, flip).
float warpPhase(WarpMode mode, float phase, float derived, float modulator, const float* curve, float& gain) noexcept;
// Reference implementation of a slot's amplitude stage.
float warpAmplitude(WarpMode mode, float sample, float derived, float modulator) noexcept;

// State of one oscillator's unison lanes: structure of arrays, 16 entries each. Entries past
// `count` must hold valid values with zero gains; the wide kernels process them as padding.
// "Step" members are added once per rendered sample.
struct Lanes {
    alignas(32) std::uint32_t phase[kMaxLanes]; // 32-bit phase: wraps by itself
    alignas(32) float inc[kMaxLanes];           // cycles per rendered sample
    alignas(32) float incStep[kMaxLanes];
    alignas(32) float fpos[kMaxLanes]; // frame position, 0 .. frames - 1
    alignas(32) float fposStep[kMaxLanes];
    alignas(32) float dA[kMaxLanes]; // derived warp values of slots A and B
    alignas(32) float dAStep[kMaxLanes];
    alignas(32) float dB[kMaxLanes];
    alignas(32) float dBStep[kMaxLanes];
    alignas(32) float gL[kMaxLanes]; // lane gains into left, right and the mono modulator output
    alignas(32) float gLStep[kMaxLanes];
    alignas(32) float gR[kMaxLanes];
    alignas(32) float gRStep[kMaxLanes];
    alignas(32) float gM[kMaxLanes];
    alignas(32) float gMStep[kMaxLanes];
    // The band-limited level each lane reads, as floats holding whole numbers: index of frame 0,
    // samples from one frame to the next, samples per cycle.
    alignas(32) float tabOff[kMaxLanes];
    alignas(32) float tabStride[kMaxLanes];
    alignas(32) float tabSize[kMaxLanes];
    // The level being faded in while a lane changes level, and the fade position 0..1.
    alignas(32) float tabOff2[kMaxLanes];
    alignas(32) float tabStride2[kMaxLanes];
    alignas(32) float tabSize2[kMaxLanes];
    alignas(32) float xf[kMaxLanes];
    alignas(32) float xfStep[kMaxLanes];
    int count = 1;
};

struct Block {
    int samples = 0;              // 1 .. kMaxRun
    const float* table = nullptr; // TableData::data()
    float maxFrame = 0.0f;        // frames - 1
    bool levelFade = false;       // some lane is between two levels: read both
    bool classic = false;         // the Classic engine instead of the wavetable
    float shape = 0.0f, pulseWidth = 0.5f; // Classic engine
    WarpMode modeA = WarpMode::Off, modeB = WarpMode::Off;
    const float* modA = nullptr; // modulator signal per rendered sample (pm, am, rm), else null
    const float* modB = nullptr;
    const float* curveA = nullptr; // kCurvePoints + 2 values in 0..1 (curve modes), else null
    const float* curveB = nullptr;
    float* outL = nullptr; // overwritten with the sum of the lanes
    float* outR = nullptr; // may be null (a single lane has no stereo spread)
    float* outM = nullptr; // modulator output; may be null
};

using RenderFn = void (*)(Lanes&, const Block&);

void renderScalar(Lanes& lanes, const Block& block); // one lane at a time: the reference
void renderSse2(Lanes& lanes, const Block& block);   // four lanes
void renderAvx2(Lanes& lanes, const Block& block);   // eight lanes; call only when cpuHasAvx2()
// The cheapest kernel for a lane count on this processor.
RenderFn pickRender(int lanes) noexcept;
// For tests and benchmarks: 0 = automatic, 1 = scalar only, 4 = SSE2 only, 8 = AVX2 where possible.
void forceKernelWidth(int width) noexcept;

// The default warp curve (an S-shaped ease): kCurvePoints + 2 values.
const float* defaultWarpCurve() noexcept;
// Fills a curve table (kCurvePoints + 2 values) from any number of evenly spaced values across
// the cycle, clamped to 0..1; no values gives the default curve.
void fillWarpCurve(std::span<const float> values, float* table) noexcept;

} // namespace zed::osc

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
