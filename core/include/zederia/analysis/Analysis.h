// Hearing for agents (plan 4.6, Appendix C): a render or any audio file described in musical
// terms. Agents cannot listen, so every render returns these measurements, `compare` scores one
// sound against another, and a mel spectrogram image is available for clients that can see.
// Nothing here is for the audio thread.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace zed::analysis {

struct Audio {
    std::span<const float> left, right; // equal lengths; a mono file passes the same data twice
    double sampleRate = 48000.0;
};

struct Options {
    // When the last note was let go, in seconds from the start (-1: unknown). The tail is
    // measured from here.
    double noteOffSeconds = -1.0;
};

// The six bands of Appendix C, in hertz: sub, bass, low-mid, high-mid, presence, air.
inline constexpr std::array<const char*, 6> kBandNames = {"sub", "bass", "low_mid", "high_mid", "presence", "air"};
inline constexpr std::array<double, 7> kBandEdges = {20.0, 60.0, 250.0, 1000.0, 4000.0, 8000.0, 20000.0};

struct Descriptors {
    // Level
    double peakDb = -200.0;      // highest sample, dBFS
    double rmsDb = -200.0;       // over the whole file, both channels
    double loudnessLufs = -200.0; // integrated loudness (ITU-R BS.1770, gated)
    double crestDb = 0.0;        // peak minus RMS
    double dcOffset = 0.0;       // mean of the mono sum
    std::int64_t clippedSamples = 0; // samples at or above 0.999 in either channel
    // Time
    double durationSeconds = 0.0;
    double attackMs = -1.0;      // 10 % to 90 % of the envelope's peak; -1 when silent
    std::string decayShape = "none"; // percussive, decaying, sustained, swelling, none
    double decayMs = -1.0;       // from the peak to 20 dB below it; -1 when it never falls that far
    double tailMs = -1.0;        // after note-off until 60 dB below the level at note-off
    bool tailComplete = false;   // false: the file ends before the tail did
    int transients = 0;          // note-like onsets
    // Pitch
    bool pitched = false;
    double fundamentalHz = 0.0;
    std::string note;            // nearest equal-tempered note, "A3"
    double centsOff = 0.0;       // from that note
    double stabilityCents = 0.0; // spread of the pitch over time
    double inharmonicityCents = 0.0; // mean distance of partials 2..12 from whole multiples
    double pitchConfidence = 0.0; // 0..1
    bool pitchVaries = false;    // the pitch moves by more than a semitone (a phrase of several notes)
    // Spectrum
    double centroidHz = 0.0;
    double rolloffHz = 0.0;      // 85 % of the energy lies below
    double flatness = 0.0;       // 0 tonal .. 1 white noise
    double hnrDb = 0.0;          // harmonic-to-noise ratio (pitched sounds only)
    std::array<double, 6> bandShare{}; // fraction of the energy in each band (sums to about 1)
    // Motion
    double centroidVariationOct = 0.0; // spread of the centroid over time, octaves
    double modulationRateHz = 0.0;     // dominant rate of level or colour movement; 0 = none found
    double modulationDepthDb = 0.0;    // its depth in level
    // Stereo
    double width = 0.0;          // side energy over mid energy; 0 = mono
    double correlation = 1.0;    // -1 .. 1
    // Plain words from thresholds (see docs/AGENT_GUIDE.md)
    std::vector<std::string> tags;
};

Descriptors analyze(const Audio& audio, const Options& options = {});
nlohmann::json toJson(const Descriptors& d);

struct Comparison {
    double spectralDb = 0.0;     // mean difference of 40 mel band levels, loudness aligned
    double envelopeDb = 0.0;     // RMS difference of the level envelopes from their onsets
    double loudnessDiffLu = 0.0; // a minus b
    double peakDiffDb = 0.0;
    double centroidDiffOct = 0.0; // a relative to b
    double attackDiffMs = 0.0;
    double widthDiff = 0.0;
    double pitchDiffCents = 0.0; // when both are pitched
    double distance = 0.0;       // one figure to minimise
    double similarity = 0.0;     // 0..1; 1 = identical
};
Comparison compare(const Audio& a, const Audio& b);
nlohmann::json toJson(const Comparison& c);

// A mel spectrogram (time left to right, low frequencies at the bottom) as an RGB PNG.
std::vector<std::uint8_t> melSpectrogramPng(const Audio& audio, int width = 640, int height = 256);

// Reads a WAV file's samples into two channels (a mono file twice). False with a message when unreadable.
bool loadAudioFile(std::span<const std::uint8_t> wavBytes, std::vector<float>& left, std::vector<float>& right,
                   double& sampleRate, std::string& error);

} // namespace zed::analysis
