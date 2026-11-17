// The form of a wavetable the oscillator plays (plan 5.7): immutable, built off the audio thread
// and handed over by pointer.
//
// Band-limiting. Every frame is stored at 28 band-limited levels, three per octave, from 1024
// harmonics down to 1. A voice reads the level whose highest harmonic stays at or below half the
// sample rate for its pitch, so playback itself creates nothing above the Nyquist limit.
//
// Interpolation. Samples are read with a six-point quintic B-spline. Its images of a harmonic h
// in a level of size N are down by (x / (1 - x))^6 with x = h / N; level sizes are chosen so the
// worst image of a full-level sawtooth stays below -104 dBc. The spline also rolls off the top of
// the pass band by sinc^6(x); because the levels are synthesized from spectra, each harmonic is
// raised by exactly that amount beforehand and the played spectrum is flat.
//
// Frames. For None and Crossfade tables (and stepped ones) only the key frames are stored, since
// the oscillator's own blend between neighbours is the same thing; spectral morph modes store
// the generated 256 positions.
#pragma once

#include "zederia/table/Wavetable.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zed {

class TableData {
public:
    static constexpr int kNumLevels = 28;
    static constexpr int kGuardBefore = 2; // samples copied in front of each frame for the spline
    static constexpr int kGuard = 8;       // total extra samples per frame

    struct Level {
        int harmonics = 0; // highest harmonic kept
        int size = 0;      // samples per cycle (a power of two)
        int stride = 0;    // size + kGuard
        int offset = 0;    // index in data() of frame 0's first guard sample
        float maxInc = 0;  // largest phase increment (cycles per sample) this level may play at
    };

    // Builds the playable form, or returns the existing one when an identical table is already
    // loaded somewhere in this process. Not for the audio thread.
    static std::shared_ptr<const TableData> build(const Wavetable& table);
    // The same, reusing the band-limited levels of every position whose frame is unchanged from
    // `previous` (the workshop rebuilds after each stroke; only the frames it touched cost time).
    // The result is identical to a full build.
    static std::shared_ptr<const TableData> build(const Wavetable& table, const TableData* previous);
    // The table an oscillator plays when its patch names none: sawtooth, square, triangle and
    // sine as four key frames.
    static std::shared_ptr<const TableData> builtin();
    static Wavetable builtinWavetable();

    int frames() const { return frames_; } // positions the oscillator moves across
    bool stepped() const { return stepped_; }
    const std::string& name() const { return name_; }
    std::uint64_t hash() const { return hash_; }
    const float* data() const { return data_.data(); }
    std::size_t sizeInBytes() const { return data_.size() * sizeof(float); }
    const Level& level(int index) const { return levels_[static_cast<std::size_t>(index)]; }

    // The level for a phase increment in cycles per sample: the one with the most harmonics that
    // all stay at or below half the sample rate.
    static int levelFor(float inc) noexcept;
    static int levelHarmonics(int level) noexcept;

    // Reference read in double precision (tests and tools): frame position 0..frames()-1, phase 0..1.
    double read(int level, double framePosition, double phase) const;

private:
    TableData() = default;
    int frames_ = 1;
    bool stepped_ = false;
    std::string name_;
    std::uint64_t hash_ = 0;
    std::array<Level, kNumLevels> levels_{};
    std::vector<float> data_;
    std::vector<std::uint64_t> frameHashes_; // of each stored position's frame, for incremental builds
    std::vector<std::uint64_t> keyHashes_;   // of each key frame (spectral tables regenerate only what changed)
    int spectralMorph_ = -1;                 // the morph mode when positions were generated, else -1
};

// The six spline weights for a fractional position t in 0..1, for the samples at -2..+3.
void splineWeights(double t, double w[6]);

} // namespace zed
