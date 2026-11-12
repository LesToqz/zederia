#include "zederia/table/Wavetable.h"

#include "zederia/audio/WavFile.h"
#include "zederia/dsp/Fft.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace zed {
namespace {

using Complex = std::complex<double>;
constexpr int kBins = kMaxHarmonics + 1;

double wrapPhase(double radians) { return radians - 2.0 * kPi * std::floor((radians + kPi) / (2.0 * kPi)); }

// Rotates the cycle so the fundamental starts at zero phase. Frames without a fundamental are left alone.
void alignFundamental(Harmonics& h) {
    if (h.magnitude[1] < 1e-9)
        return;
    const double shift = h.phase[1];
    for (int k = 1; k < kMaxHarmonics; ++k)
        h.phase[static_cast<std::size_t>(k)] = wrapPhase(h.phase[static_cast<std::size_t>(k)] - k * shift);
}

void prepareForMode(Harmonics& h, MorphMode mode) {
    if (mode == MorphMode::SpectralAligned)
        alignFundamental(h);
    else if (mode == MorphMode::SpectralZero)
        for (int k = 1; k < kMaxHarmonics; ++k)
            h.phase[static_cast<std::size_t>(k)] = 0.0;
}

// Both inputs are already prepared for the mode.
Harmonics mixHarmonics(const Harmonics& a, const Harmonics& b, double t) {
    Harmonics out;
    out.dc = a.dc + (b.dc - a.dc) * t;
    for (std::size_t k = 1; k < static_cast<std::size_t>(kBins); ++k) {
        out.magnitude[k] = a.magnitude[k] + (b.magnitude[k] - a.magnitude[k]) * t;
        const bool hasA = a.magnitude[k] > 1e-12, hasB = b.magnitude[k] > 1e-12;
        if (hasA && hasB)
            out.phase[k] = a.phase[k] + wrapPhase(b.phase[k] - a.phase[k]) * t; // the short way round
        else
            out.phase[k] = hasA ? a.phase[k] : b.phase[k];
    }
    // At the frame's Nyquist limit only a cosine exists: mix the signed values instead.
    const std::size_t top = kMaxHarmonics;
    const double va = a.magnitude[top] * std::sin(a.phase[top]), vb = b.magnitude[top] * std::sin(b.phase[top]);
    const double v = va + (vb - va) * t;
    out.magnitude[top] = std::abs(v);
    out.phase[top] = v < 0.0 ? -kPi / 2.0 : kPi / 2.0;
    return out;
}

Frame mixFrames(const Frame& a, const Frame& b, double t) {
    Frame out;
    const auto ft = static_cast<float>(t);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = a[i] + (b[i] - a[i]) * ft;
    return out;
}

bool isSpectral(MorphMode mode) {
    return mode == MorphMode::Spectral || mode == MorphMode::SpectralAligned || mode == MorphMode::SpectralZero;
}

} // namespace

std::string_view morphModeName(MorphMode mode) {
    switch (mode) {
    case MorphMode::None: return "none";
    case MorphMode::Crossfade: return "crossfade";
    case MorphMode::Spectral: return "spectral";
    case MorphMode::SpectralAligned: return "spectral_aligned";
    case MorphMode::SpectralZero: return "spectral_zero";
    }
    return "none";
}

bool morphModeFromName(std::string_view name, MorphMode& out) {
    for (const MorphMode mode : {MorphMode::None, MorphMode::Crossfade, MorphMode::Spectral, MorphMode::SpectralAligned,
                                 MorphMode::SpectralZero}) {
        if (morphModeName(mode) == name) {
            out = mode;
            return true;
        }
    }
    return false;
}

Wavetable Wavetable::silent(int frameCount) {
    Wavetable t;
    t.frames.assign(static_cast<std::size_t>(std::clamp(frameCount, 1, kMaxFrames)), Frame{});
    return t;
}

float framePeak(const Frame& frame) {
    float peak = 0.0f;
    for (const float s : frame)
        peak = std::max(peak, std::abs(s));
    return peak;
}

// --- Spectra -----------------------------------------------------------------------------------

void frameToBins(const Frame& frame, std::span<Complex> bins) {
    std::array<Complex, kFrameSize> work;
    for (std::size_t i = 0; i < work.size(); ++i)
        work[i] = frame[i];
    dsp::Fft::forSize(kFrameSize).forward(work.data());
    for (std::size_t k = 0; k < static_cast<std::size_t>(kBins) && k < bins.size(); ++k)
        bins[k] = work[k];
}

Frame binsToFrame(std::span<const Complex> bins) {
    std::array<Complex, kFrameSize> work{};
    for (std::size_t k = 0; k < static_cast<std::size_t>(kBins) && k < bins.size(); ++k) {
        work[k] = bins[k];
        if (k > 0 && k < static_cast<std::size_t>(kMaxHarmonics))
            work[kFrameSize - k] = std::conj(bins[k]);
    }
    work[0] = work[0].real();
    work[kMaxHarmonics] = work[kMaxHarmonics].real();
    dsp::Fft::forSize(kFrameSize).inverse(work.data());
    Frame out;
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<float>(work[i].real());
    return out;
}

Harmonics analyzeFrame(const Frame& frame) {
    std::array<Complex, kBins> bins;
    frameToBins(frame, bins);
    Harmonics h;
    h.dc = bins[0].real() / kFrameSize;
    // m sin(x + p) transforms to (N m / 2) e^(i (p - pi/2)).
    for (std::size_t k = 1; k < static_cast<std::size_t>(kMaxHarmonics); ++k) {
        h.magnitude[k] = 2.0 * std::abs(bins[k]) / kFrameSize;
        // Below the rounding noise of 32-bit samples a harmonic is absent: its phase reads as zero.
        h.phase[k] = h.magnitude[k] > 1e-7 ? wrapPhase(std::arg(bins[k]) + kPi / 2.0) : 0.0;
    }
    const double top = bins[kMaxHarmonics].real() / kFrameSize;
    h.magnitude[kMaxHarmonics] = std::abs(top);
    h.phase[kMaxHarmonics] = top < 0.0 ? -kPi / 2.0 : kPi / 2.0;
    return h;
}

Frame synthesizeFrame(const Harmonics& h) {
    std::array<Complex, kBins> bins{};
    bins[0] = h.dc * kFrameSize;
    for (std::size_t k = 1; k < static_cast<std::size_t>(kMaxHarmonics); ++k)
        bins[k] = std::polar(h.magnitude[k] * kFrameSize / 2.0, h.phase[k] - kPi / 2.0);
    bins[kMaxHarmonics] = h.magnitude[kMaxHarmonics] * std::sin(h.phase[kMaxHarmonics]) * kFrameSize;
    return binsToFrame(bins);
}

// --- Positions ---------------------------------------------------------------------------------

Frame morphFrame(const Frame& a, const Frame& b, double t, MorphMode mode) {
    t = std::clamp(t, 0.0, 1.0);
    if (!isSpectral(mode))
        return mixFrames(a, b, t);
    Harmonics ha = analyzeFrame(a), hb = analyzeFrame(b);
    prepareForMode(ha, mode);
    prepareForMode(hb, mode);
    return synthesizeFrame(mixHarmonics(ha, hb, t));
}

Frame frameAt(const Wavetable& table, double position) {
    if (table.frames.empty())
        return Frame{};
    const int last = table.size() - 1;
    const double at = std::clamp(position, 0.0, 1.0) * last;
    if (table.stepped)
        return table.frames[static_cast<std::size_t>(std::min(static_cast<int>(at + 0.5), last))];
    const int i = std::min(static_cast<int>(at), std::max(last - 1, 0));
    const Frame& a = table.frames[static_cast<std::size_t>(i)];
    const Frame& b = table.frames[static_cast<std::size_t>(std::min(i + 1, last))];
    return morphFrame(a, b, at - i, table.morph);
}

std::vector<Frame> generatePositions(const Wavetable& table, int count) {
    count = std::clamp(count, 1, 4096);
    std::vector<Frame> out(static_cast<std::size_t>(count));
    if (table.frames.empty())
        return out;
    const int last = table.size() - 1;
    const bool spectral = isSpectral(table.morph) && !table.stepped;
    std::vector<Harmonics> keys; // analysed once, not once per position
    if (spectral) {
        keys.reserve(table.frames.size());
        for (const Frame& f : table.frames) {
            keys.push_back(analyzeFrame(f));
            prepareForMode(keys.back(), table.morph);
        }
    }
    for (int p = 0; p < count; ++p) {
        const double position = count > 1 ? static_cast<double>(p) / (count - 1) : 0.0;
        if (!spectral) {
            out[static_cast<std::size_t>(p)] = frameAt(table, position);
            continue;
        }
        const double at = position * last;
        const int i = std::min(static_cast<int>(at), std::max(last - 1, 0));
        const int j = std::min(i + 1, last);
        out[static_cast<std::size_t>(p)] =
            synthesizeFrame(mixHarmonics(keys[static_cast<std::size_t>(i)], keys[static_cast<std::size_t>(j)], at - i));
    }
    return out;
}

// --- Files -------------------------------------------------------------------------------------

Frame resampleCycle(std::span<const float> cycle) {
    Frame out{};
    const int n = static_cast<int>(cycle.size());
    if (n == 0)
        return out;
    if (n == kFrameSize) {
        std::copy(cycle.begin(), cycle.end(), out.begin());
        return out;
    }
    std::vector<Complex> in(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        in[static_cast<std::size_t>(i)] = cycle[static_cast<std::size_t>(i)];
    const std::vector<Complex> spectrum = dsp::dftAnyLength(in);
    // Copy the harmonics both sizes can hold. A harmonic at the source's own Nyquist limit is
    // dropped: its phase is not known.
    std::array<Complex, kBins> bins{};
    const double scale = static_cast<double>(kFrameSize) / n;
    const int top = std::min((n - 1) / 2, kMaxHarmonics - 1);
    for (int k = 0; k <= top; ++k)
        bins[static_cast<std::size_t>(k)] = spectrum[static_cast<std::size_t>(k)] * scale;
    return binsToFrame(bins);
}

std::vector<std::uint8_t> encodeWavetable(const Wavetable& table, TableSampleFormat format) {
    WavData wav;
    wav.sampleRate = 48000.0;
    wav.channels.emplace_back();
    auto& samples = wav.channels.front();
    samples.reserve(table.frames.size() * kFrameSize);
    for (const Frame& f : table.frames)
        samples.insert(samples.end(), f.begin(), f.end());

    nlohmann::ordered_json meta;
    meta["format"] = "zederia.table";
    meta["version"] = 1;
    meta["frame_size"] = kFrameSize;
    meta["frames"] = table.size();
    meta["morph"] = morphModeName(table.morph);
    meta["stepped"] = table.stepped;
    meta["name"] = table.name;
    const std::string text = meta.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
    wav.chunks["ZEDT"].assign(text.begin(), text.end());

    const auto wavFormat = format == TableSampleFormat::Pcm16   ? WavSampleFormat::Pcm16
                           : format == TableSampleFormat::Pcm24 ? WavSampleFormat::Pcm24
                                                                : WavSampleFormat::Float32;
    return encodeWavData(wav, wavFormat);
}

bool decodeWavetable(std::span<const std::uint8_t> bytes, Wavetable& out, std::string& error,
                     const TableReadOptions& options, int* detectedFrameSize) {
    out = Wavetable{};
    WavData wav;
    if (!decodeWav(bytes, wav, error))
        return false;
    if (wav.frames() == 0) {
        error = "the file holds no audio";
        return false;
    }
    // One channel, or the mix of all of them.
    std::vector<float> mono(wav.frames());
    if (options.channel >= 0) {
        if (options.channel >= wav.channelCount()) {
            error = "the file has " + std::to_string(wav.channelCount()) + " channel(s); channel " +
                    std::to_string(options.channel + 1) + " does not exist";
            return false;
        }
        mono = wav.channels[static_cast<std::size_t>(options.channel)];
    } else {
        for (const auto& channel : wav.channels)
            for (std::size_t i = 0; i < mono.size(); ++i)
                mono[i] += channel[i] / static_cast<float>(wav.channelCount());
    }

    int frameSize = options.frameSize;
    bool fromMetadata = false;
    if (const auto it = wav.chunks.find("ZEDT"); it != wav.chunks.end()) {
        const auto meta = nlohmann::json::parse(it->second.begin(), it->second.end(), nullptr, false);
        if (meta.is_object()) {
            if (frameSize <= 0 && meta.contains("frame_size") && meta["frame_size"].is_number_integer()) {
                frameSize = meta["frame_size"].get<int>();
                fromMetadata = true;
            }
            if (meta.contains("morph") && meta["morph"].is_string())
                morphModeFromName(meta["morph"].get<std::string>(), out.morph);
            if (meta.contains("stepped") && meta["stepped"].is_boolean())
                out.stepped = meta["stepped"].get<bool>();
            if (meta.contains("name") && meta["name"].is_string())
                out.name = meta["name"].get<std::string>().substr(0, 200);
        }
    }
    if (frameSize <= 0) {
        // A widely used marker chunk carries the frame size as text: "<!>2048 ...".
        if (const auto it = wav.chunks.find("clm "); it != wav.chunks.end() && it->second.size() >= 4 &&
                                                       it->second[0] == '<' && it->second[1] == '!' && it->second[2] == '>') {
            int value = 0;
            for (std::size_t i = 3; i < it->second.size() && it->second[i] >= '0' && it->second[i] <= '9' && value < 1000000; ++i)
                value = value * 10 + (it->second[i] - '0');
            if (value >= 2)
                frameSize = value;
        }
    }
    const auto total = static_cast<std::int64_t>(mono.size());
    if (frameSize <= 0) {
        for (const int candidate : {2048, 1024, 4096, 512, 256}) {
            if (total % candidate == 0 && total / candidate <= kMaxFrames) {
                frameSize = candidate;
                break;
            }
        }
        if (frameSize <= 0 && total <= 16384)
            frameSize = static_cast<int>(total); // one cycle of unusual length
    }
    if (frameSize < 2 || frameSize > 1 << 20) {
        error = "the frame size could not be worked out from the file's length (" + std::to_string(total) +
                " samples); give it explicitly";
        return false;
    }
    const std::int64_t count = total / frameSize;
    if (count < 1) {
        error = "the file is shorter than one frame of " + std::to_string(frameSize) + " samples";
        return false;
    }
    if (count > kMaxFrames) {
        error = "the file holds " + std::to_string(count) + " frames of " + std::to_string(frameSize) +
                " samples; a table holds at most " + std::to_string(kMaxFrames) +
                (fromMetadata ? "" : " (give another frame size, or import it as a recording)");
        return false;
    }
    out.frames.reserve(static_cast<std::size_t>(count));
    for (std::int64_t f = 0; f < count; ++f)
        out.frames.push_back(resampleCycle(std::span<const float>(mono).subspan(static_cast<std::size_t>(f * frameSize),
                                                                                static_cast<std::size_t>(frameSize))));
    if (detectedFrameSize != nullptr)
        *detectedFrameSize = frameSize;
    return true;
}

} // namespace zed
