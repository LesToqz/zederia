// The wavetable toolkit (plan 3.10): every frame, drawing, harmonic, process, morph and sort
// operation of the workshop as plain functions on a Wavetable. No interface code; the workshop
// page, the command-line tool and the agent tools all call these.
#pragma once

#include "zederia/table/Wavetable.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace zed {

// A run of key frames, both ends included. `last` < 0 means "to the end of the table".
struct FrameSelection {
    int first = 0;
    int last = -1;
};
inline FrameSelection allFrames() { return {0, -1}; }
inline FrameSelection oneFrame(int index) { return {index, index}; }

// --- Frames strip ------------------------------------------------------------------------------
// All return false (and change nothing) when the request does not fit: an index outside the
// table, more than 256 frames, or fewer than one.
bool insertFrame(Wavetable& table, int at, const Frame& frame); // at == size() appends
bool duplicateFrames(Wavetable& table, FrameSelection selection); // the copies follow the originals
bool deleteFrames(Wavetable& table, FrameSelection selection);    // at least one frame must remain
bool moveFrame(Wavetable& table, int from, int to);
bool thinFrames(Wavetable& table, int keepEvery); // keeps frames 0, N, 2N, ...
bool cropFrames(Wavetable& table, FrameSelection selection);
void reverseFrames(Wavetable& table);

// --- Wave view ---------------------------------------------------------------------------------
enum class DrawShape {
    Flat,         // level `a` throughout
    RampUp,       // a to b in a straight line (the same as Line; kept for the tool palette)
    RampDown,     // b to a
    Line,         // a to b
    Sine,         // one full sine between the ends, centred on a with height b
    HalfSineUp,   // an arch from a up to b and back
    HalfSineDown, // an arch from a down by b and back
    QuarterIn,    // a to b, starting flat and ending steep
    QuarterOut,   // a to b, starting steep and ending flat
    Curve,        // a to b through a smooth S-curve
};
// Draws over the part of the cycle from `from` to `to` (0..1 of the cycle).
void drawSegment(Frame& frame, double from, double to, DrawShape shape, float a, float b);
// Moves that part up or down.
void nudgeSegment(Frame& frame, double from, double to, float amount);
// Adds seeded noise of the given peak size to that part.
void addNoise(Frame& frame, double from, double to, float amount, std::uint64_t seed);
// The nearest grid line for a position in the cycle, with `divisions` lines per cycle.
double snapToGrid(double position, int divisions);

// --- Harmonic view -----------------------------------------------------------------------------
enum class HarmonicOp {
    ClearAll,
    ClearAbove,          // n: the highest harmonic kept
    ClearBelow,          // n: the lowest harmonic kept
    FillSaw,             // 1/h series
    RandomizeLowest,     // n harmonics get random levels up to `amount` (1 = full, 0.5 = half)
    RandomizeAll,        // every harmonic, up to `amount`
    RandomGaps,          // removes a share `amount` of the harmonics at random
    FadeHighs,           // progressive fade: level times h^-amount
    OctaveUp,            // harmonic h moves to 2h
    OctaveDown,          // harmonic 2h moves to h; odd ones are dropped
    OddOnly,
    EvenOnly,
    SnapQuarters,        // levels snap to quarters of the strongest
    ScaleByHarmonic,     // level times h^amount
};
void applyHarmonicOp(Frame& frame, HarmonicOp op, int n = 0, double amount = 1.0, std::uint64_t seed = 1);
bool harmonicOpFromName(std::string_view name, HarmonicOp& out);

// --- Process -----------------------------------------------------------------------------------
enum class ProcessOp {
    NormalizeEach,       // every frame to a peak of 1
    NormalizeShared,     // the loudest frame to a peak of 1, the others by the same factor
    RemoveDc,
    FlipPolarity,
    MirrorTime,
    ShiftToZeroCrossing, // rotates the cycle so it starts on an upward zero crossing
    Clear,
    FadeEdges,           // n samples faded to zero at both ends
    CrossfadeEdges,      // the last n samples lead back into the start, so the cycle loops
    LowPass,             // n: harmonics kept
    RemoveFundamental,
    SampleRateReduce,    // n: steps per cycle
    AlignFundamental,    // rotates the cycle so its fundamental starts at zero phase
};
void processFrames(Wavetable& table, FrameSelection selection, ProcessOp op, int n = 0);
bool processOpFromName(std::string_view name, ProcessOp& out);

// Each frame becomes two: its first half and its second half, each stretched to a full cycle.
bool splitFrames(Wavetable& table);
// Frames are joined in pairs: each pair is squeezed into one cycle. An odd last frame is paired
// with silence.
void joinFramePairs(Wavetable& table);
// A table that sweeps the pulse width of one frame: the cycle is squeezed into a shrinking
// part of the period, from the whole period down to `narrowest` (0..1).
Wavetable pulseWidthSweep(const Frame& frame, int frames, double narrowest = 0.05);
// Gives every frame the harmonic levels (or the phases) of frame `source`.
void copySpectrumToAll(Wavetable& table, int source);
void copyPhasesToAll(Wavetable& table, int source);
// Lowers each harmonic by the level it has in the other table at the same position.
void subtractSpectrum(Wavetable& table, const Wavetable& other);

// --- Morph -------------------------------------------------------------------------------------
// Filling between key frames is the table's morph mode (table.morph); "remove morph" sets it to
// None. bakeMorph makes the generated positions real: `frames` key frames, morph mode None.
void bakeMorph(Wavetable& table, int frames = kMaxFrames);

// --- Sort --------------------------------------------------------------------------------------
enum class SortKey {
    BrightestPeak,    // frequency of the highest prominent peak of the spectrum
    Brightness,       // average brightness: the spectral centroid
    StrongestPartial, // number of the loudest harmonic
    ActivePartials,   // how many harmonics are within 60 dB of the loudest
    HighestPartial,   // the highest such harmonic
    Fundamental,      // the fundamental's share of the frame's energy
};
double sortValue(const Frame& frame, SortKey key);
void sortFrames(Wavetable& table, SortKey key, bool descending = false);
bool sortKeyFromName(std::string_view name, SortKey& out);

// --- Follow-ups and export ---------------------------------------------------------------------
// Removes frames quieter than the threshold (relative to the loudest frame); returns how many.
int trimSilentFrames(Wavetable& table, double thresholdDb = -60.0);
// One single-cycle WAV file per selected frame.
std::vector<std::vector<std::uint8_t>> exportSingleCycles(const Wavetable& table, FrameSelection selection,
                                                          TableSampleFormat format = TableSampleFormat::Float32);

} // namespace zed
