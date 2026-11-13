// Turning audio and images into wavetables (plan 3.10, Import): single cycles, pitch-tracked and
// fixed-length slicing of pitched recordings, spectral snapshots, images, and the stereo options.
// Plain functions; the import wizard, the command-line tool and the agent tools all call them.
#pragma once

#include "zederia/audio/WavFile.h"
#include "zederia/table/Wavetable.h"
#include "zederia/util/Image.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zed {

// One channel of audio.
struct AudioClip {
    std::vector<float> samples;
    double sampleRate = 48000.0;
};

// Stereo files: choose left, right or mid (plan 3.10). Mono files give the same clip for all three.
enum class StereoChoice { Left, Right, Mid };
AudioClip clipFromWav(const WavData& wav, StereoChoice choice);

// --- Single cycles -----------------------------------------------------------------------------
// Each clip is one cycle of any length and becomes one frame.
bool importSingleCycles(std::span<const AudioClip> cycles, Wavetable& out, std::string& error);

// --- Pitch -------------------------------------------------------------------------------------
struct PitchEstimate {
    double hz = 0.0;
    double periodSamples = 0.0;
    double confidence = 0.0; // 0..1; below about 0.5 the clip has no clear pitch
};
// The pitch of a clip as a whole (the median of several windows).
PitchEstimate detectPitch(const AudioClip& clip, double minHz = 20.0, double maxHz = 5000.0);
// The length of one cycle of a note at this sample rate, for "fixed length as a note name".
double cycleSamplesForNote(double midiNote, double sampleRate, double referenceHz = 440.0);

// How many cycles to keep when a recording holds more than fit in a table.
enum class CycleReduction { Pick, Average };

struct SliceOptions {
    int maxFrames = kMaxFrames;
    bool snapToZeroCrossings = false; // pitch-tracked: move each cut to the nearest upward zero crossing
    double minHz = 20.0, maxHz = 5000.0;
    double cycleSamples = 0.0; // fixed length: samples per cycle; 0 = the recording's average, detected
    CycleReduction reduction = CycleReduction::Pick;
};

struct ImportReport {
    double detectedHz = 0.0;
    double cycleSamples = 0.0;   // average cycle length used
    int cyclesFound = 0;
    int framesMade = 0;
    std::vector<double> markers; // where each cycle starts, in samples of the clip (for the wizard's display)
};

// Follows the pitch from cycle to cycle and cuts where each cycle begins.
bool importPitchTracked(const AudioClip& clip, const SliceOptions& options, Wavetable& out, std::string& error,
                        ImportReport* report = nullptr);
// Cuts cycles of one fixed length (typed, from a note name, or the detected average).
bool importFixedCycles(const AudioClip& clip, const SliceOptions& options, Wavetable& out, std::string& error,
                       ImportReport* report = nullptr);

// --- Spectral snapshots ------------------------------------------------------------------------
enum class SnapshotPhase { Original, Zero, Random };
struct SnapshotOptions {
    int fftSize = 2048; // 256, 512, 1024 or 2048
    int frames = 64;
    SnapshotPhase phase = SnapshotPhase::Zero;
    std::uint64_t seed = 1; // for random phases
};
// Windowed spectra taken at even steps through the clip; bin k becomes harmonic k.
bool importSpectralSnapshots(const AudioClip& clip, const SnapshotOptions& options, Wavetable& out, std::string& error);

// --- Images ------------------------------------------------------------------------------------
enum class ImageMode {
    Amplitude, // each row is a cycle: brightness is the sample value, dark = -1, bright = +1
    Harmonics, // each row is a spectrum: column k is the level of harmonic k + 1
};
// Rows become frames, top row first; more rows than frames are averaged down.
bool importImage(const ImageData& image, ImageMode mode, int maxFrames, Wavetable& out, std::string& error);

// --- Choosing a method (the wizard's suggestion) -----------------------------------------------
struct ImportSuggestion {
    std::string method; // "table", "single_cycle", "pitch_tracked", "fixed_cycles" or "spectral"
    std::string reason; // one sentence for the user
    PitchEstimate pitch;
    int frameSize = 0; // for "table": the frame size found
};
ImportSuggestion suggestImport(const WavData& wav);

} // namespace zed
