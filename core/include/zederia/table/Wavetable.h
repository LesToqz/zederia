// The wavetable data model (plan 3.3). A table is stored as its key frames plus a morph mode; the
// full set of positions is generated from them when the table loads. This is the editable form
// that the toolkit works on; TableData (TableData.h) is the immutable form the oscillator plays.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

inline constexpr int kFrameSize = 2048;     // samples in one cycle
inline constexpr int kMaxFrames = 256;      // key frames in a table, and generated positions
inline constexpr int kMaxHarmonics = 1024;  // harmonics a frame can hold

using Frame = std::array<float, kFrameSize>;

// How the positions between key frames are made.
enum class MorphMode : std::uint8_t {
    None,            // the key frames are the positions
    Crossfade,       // straight mix of the neighbouring key frames
    Spectral,        // harmonic magnitudes and phases move separately, phases as they are
    SpectralAligned, // the same after rotating every key frame so its fundamental starts at zero
    SpectralZero,    // magnitudes only: every harmonic becomes a sine starting at zero
};
std::string_view morphModeName(MorphMode mode); // "none", "crossfade", "spectral", "spectral_aligned", "spectral_zero"
bool morphModeFromName(std::string_view name, MorphMode& out);

struct Wavetable {
    std::string name;
    std::vector<Frame> frames; // the key frames, 1..kMaxFrames
    MorphMode morph = MorphMode::None;
    bool stepped = false; // position snaps to key frames instead of gliding between them

    int size() const { return static_cast<int>(frames.size()); }
    bool operator==(const Wavetable&) const = default;

    static Wavetable silent(int frameCount = 1);
};

// --- Spectra -----------------------------------------------------------------------------------
// A frame as harmonics: frame[n] = dc + sum over h of magnitude[h] * sin(2 pi h n / 2048 + phase[h]).
// Index 0 is unused apart from `dc`. Harmonic 1024 sits at the frame's Nyquist limit, where only
// the cosine part exists; its phase is +pi/2 or -pi/2.
struct Harmonics {
    double dc = 0.0;
    std::array<double, kMaxHarmonics + 1> magnitude{};
    std::array<double, kMaxHarmonics + 1> phase{}; // radians
};
Harmonics analyzeFrame(const Frame& frame);
Frame synthesizeFrame(const Harmonics& harmonics);
// The raw transform, bins 0..1024 of a 2048-point FFT, for code that works on complex values.
void frameToBins(const Frame& frame, std::span<std::complex<double>> bins1025);
Frame binsToFrame(std::span<const std::complex<double>> bins1025);

// --- Positions ---------------------------------------------------------------------------------
// One frame between a and b at t in 0..1, by the given mode (None behaves as Crossfade).
Frame morphFrame(const Frame& a, const Frame& b, double t, MorphMode mode);
// The frame at `position` (0..1) of the table as it sounds: key frames evenly spaced, the gaps
// filled by the table's morph mode; a stepped table returns the nearest key frame.
Frame frameAt(const Wavetable& table, double position);
// `count` evenly spaced positions (the plan's full set is 256).
std::vector<Frame> generatePositions(const Wavetable& table, int count = kMaxFrames);

// --- Files (plan 5.6) --------------------------------------------------------------------------
// A table file is a mono WAV with the key frames end to end, 2048 samples each, and a "ZEDT"
// chunk holding {"format":"zederia.table","version":1,"frame_size":2048,"frames":N,
// "morph":"spectral","stepped":false,"name":"..."}.
enum class TableSampleFormat { Pcm16, Pcm24, Float32 };
std::vector<std::uint8_t> encodeWavetable(const Wavetable& table, TableSampleFormat format = TableSampleFormat::Float32);

struct TableReadOptions {
    int frameSize = 0; // samples per frame in the file; 0 = from the file's metadata, else detected
    int channel = -1;  // -1 = mix all channels to mono, otherwise the channel to read
};
// Reads a table file. Frames of another size are resampled to 2048. Without metadata the frame
// size is taken from a marker chunk when one exists, otherwise detected from the length.
// `detectedFrameSize` (optional) receives the size that was used.
bool decodeWavetable(std::span<const std::uint8_t> wav, Wavetable& out, std::string& error,
                     const TableReadOptions& options = {}, int* detectedFrameSize = nullptr);

// Stretches one cycle of any length onto a frame, exactly (through its spectrum).
Frame resampleCycle(std::span<const float> cycle);

// Largest absolute sample value of a frame.
float framePeak(const Frame& frame);

} // namespace zed
