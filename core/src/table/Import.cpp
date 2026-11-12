#include "zederia/table/Import.h"

#include "zederia/dsp/Fft.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>

namespace zed {
namespace {

float sampleOf(const AudioClip& clip, std::int64_t index) {
    const auto n = static_cast<std::int64_t>(clip.samples.size());
    return n == 0 ? 0.0f : clip.samples[static_cast<std::size_t>(std::clamp<std::int64_t>(index, 0, n - 1))];
}

// The clip between its samples, through a cubic over four neighbours.
float readClip(const AudioClip& clip, double position) {
    const auto i = static_cast<std::int64_t>(std::floor(position));
    const auto t = static_cast<float>(position - static_cast<double>(i));
    const float a = sampleOf(clip, i - 1), b = sampleOf(clip, i), c = sampleOf(clip, i + 1), d = sampleOf(clip, i + 2);
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}

// One window of the difference-function pitch detector: the lag at which the signal best
// matches itself, with the usual normalization that keeps it from choosing a multiple.
PitchEstimate pitchOfWindow(const float* x, int available, double sampleRate, double minHz, double maxHz) {
    PitchEstimate result;
    const int maxLag = std::min(static_cast<int>(sampleRate / minHz), available / 2);
    const int minLag = std::max(2, static_cast<int>(sampleRate / maxHz));
    if (maxLag <= minLag + 2)
        return result;
    const int window = std::min(available - maxLag, 4096);
    std::vector<double> normalized(static_cast<std::size_t>(maxLag + 1), 1.0);
    double running = 0.0;
    for (int lag = 1; lag <= maxLag; ++lag) {
        double difference = 0.0;
        for (int j = 0; j < window; ++j) {
            const double d = static_cast<double>(x[j]) - x[j + lag];
            difference += d * d;
        }
        running += difference;
        normalized[static_cast<std::size_t>(lag)] = running > 0.0 ? difference * lag / running : 1.0;
    }
    int best = -1;
    for (int lag = minLag; lag < maxLag; ++lag) {
        if (normalized[static_cast<std::size_t>(lag)] < 0.15) {
            while (lag + 1 < maxLag && normalized[static_cast<std::size_t>(lag + 1)] < normalized[static_cast<std::size_t>(lag)])
                ++lag;
            best = lag;
            break;
        }
    }
    if (best < 0) {
        best = minLag;
        for (int lag = minLag; lag < maxLag; ++lag)
            if (normalized[static_cast<std::size_t>(lag)] < normalized[static_cast<std::size_t>(best)])
                best = lag;
    }
    double period = best;
    if (best > 1 && best + 1 <= maxLag) { // the minimum between the lags, from a parabola through three points
        const double a = normalized[static_cast<std::size_t>(best - 1)], b = normalized[static_cast<std::size_t>(best)],
                     c = normalized[static_cast<std::size_t>(best + 1)];
        const double curvature = a - 2.0 * b + c;
        if (curvature > 1e-12)
            period += 0.5 * (a - c) / curvature;
    }
    result.periodSamples = period;
    result.hz = sampleRate / period;
    result.confidence = std::clamp(1.0 - normalized[static_cast<std::size_t>(best)], 0.0, 1.0);
    return result;
}

double rmsOf(const float* x, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return n > 0 ? std::sqrt(sum / n) : 0.0;
}

// The upward zero crossing nearest to `position` within `reach` samples; `position` itself when there is none.
double nearestUpwardCrossing(const AudioClip& clip, double position, double reach) {
    const auto n = static_cast<std::int64_t>(clip.samples.size());
    const auto centre = static_cast<std::int64_t>(std::llround(position));
    double best = position, bestDistance = reach + 1.0;
    for (std::int64_t i = std::max<std::int64_t>(centre - static_cast<std::int64_t>(reach) - 1, 1);
         i <= std::min<std::int64_t>(centre + static_cast<std::int64_t>(reach) + 1, n - 1); ++i) {
        const float before = clip.samples[static_cast<std::size_t>(i - 1)], here = clip.samples[static_cast<std::size_t>(i)];
        if (before < 0.0f && here >= 0.0f) {
            const double exact = static_cast<double>(i - 1) + static_cast<double>(-before) / (static_cast<double>(here) - before);
            if (std::abs(exact - position) < bestDistance) {
                bestDistance = std::abs(exact - position);
                best = exact;
            }
        }
    }
    return best;
}

Frame cycleFrame(const AudioClip& clip, double from, double length) {
    const int count = std::max(2, static_cast<int>(std::lround(length)));
    std::vector<float> cycle(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        cycle[static_cast<std::size_t>(i)] = readClip(clip, from + length * i / count);
    return resampleCycle(cycle);
}

// Turns cycle start markers into at most `maxFrames` frames.
void framesFromMarkers(const AudioClip& clip, const std::vector<double>& markers, const SliceOptions& options, Wavetable& out) {
    const int cycles = static_cast<int>(markers.size()) - 1;
    const int frames = std::clamp(std::min(cycles, options.maxFrames), 1, kMaxFrames);
    out.frames.clear();
    for (int f = 0; f < frames; ++f) {
        if (cycles <= frames) {
            out.frames.push_back(cycleFrame(clip, markers[static_cast<std::size_t>(f)],
                                            markers[static_cast<std::size_t>(f + 1)] - markers[static_cast<std::size_t>(f)]));
        } else if (options.reduction == CycleReduction::Pick) {
            const int c = frames > 1 ? static_cast<int>(std::llround(static_cast<double>(f) * (cycles - 1) / (frames - 1))) : 0;
            out.frames.push_back(cycleFrame(clip, markers[static_cast<std::size_t>(c)],
                                            markers[static_cast<std::size_t>(c + 1)] - markers[static_cast<std::size_t>(c)]));
        } else {
            const int first = static_cast<int>(static_cast<std::int64_t>(f) * cycles / frames);
            const int end = std::max(first + 1, static_cast<int>(static_cast<std::int64_t>(f + 1) * cycles / frames));
            Frame sum{};
            for (int c = first; c < end; ++c) {
                const Frame one = cycleFrame(clip, markers[static_cast<std::size_t>(c)],
                                             markers[static_cast<std::size_t>(c + 1)] - markers[static_cast<std::size_t>(c)]);
                for (std::size_t i = 0; i < sum.size(); ++i)
                    sum[i] += one[i] / static_cast<float>(end - first);
            }
            out.frames.push_back(sum);
        }
    }
}

} // namespace

AudioClip clipFromWav(const WavData& wav, StereoChoice choice) {
    AudioClip clip;
    clip.sampleRate = wav.sampleRate;
    if (wav.channels.empty())
        return clip;
    const auto& left = wav.channels[0];
    const auto& right = wav.channels.size() > 1 ? wav.channels[1] : wav.channels[0];
    if (choice == StereoChoice::Left) {
        clip.samples = left;
    } else if (choice == StereoChoice::Right) {
        clip.samples = right;
    } else {
        clip.samples.resize(left.size());
        for (std::size_t i = 0; i < left.size(); ++i)
            clip.samples[i] = 0.5f * (left[i] + right[i]);
    }
    return clip;
}

bool importSingleCycles(std::span<const AudioClip> cycles, Wavetable& out, std::string& error) {
    out = Wavetable{};
    if (cycles.empty()) {
        error = "no single-cycle files were given";
        return false;
    }
    if (cycles.size() > static_cast<std::size_t>(kMaxFrames)) {
        error = "a table holds at most " + std::to_string(kMaxFrames) + " frames, but " + std::to_string(cycles.size()) +
                " files were given";
        return false;
    }
    for (std::size_t i = 0; i < cycles.size(); ++i) {
        if (cycles[i].samples.size() < 2) {
            error = "file " + std::to_string(i + 1) + " holds no audio";
            return false;
        }
        if (cycles[i].samples.size() > (1u << 20)) {
            error = "file " + std::to_string(i + 1) + " is too long to be one cycle; import it as a recording";
            return false;
        }
        out.frames.push_back(resampleCycle(cycles[i].samples));
    }
    return true;
}

PitchEstimate detectPitch(const AudioClip& clip, double minHz, double maxHz) {
    PitchEstimate none;
    const auto total = static_cast<std::int64_t>(clip.samples.size());
    minHz = std::clamp(minHz, 5.0, 20000.0);
    maxHz = std::clamp(maxHz, minHz * 1.5, clip.sampleRate / 4.0);
    const auto span = static_cast<std::int64_t>(2.0 * clip.sampleRate / minHz) + 4096; // samples one window looks at
    if (total < 64)
        return none;
    double loudest = 0.0;
    std::vector<PitchEstimate> found;
    const int windows = total > span ? 9 : 1;
    for (int w = 0; w < windows; ++w) {
        const std::int64_t start = windows > 1 ? (total - span) * w / (windows - 1) : 0;
        const int available = static_cast<int>(std::min<std::int64_t>(span, total - start));
        const float* x = clip.samples.data() + start;
        const double level = rmsOf(x, available);
        loudest = std::max(loudest, level);
        if (level < 1e-5)
            continue;
        PitchEstimate estimate = pitchOfWindow(x, available, clip.sampleRate, minHz, maxHz);
        estimate.confidence *= std::min(1.0, level / std::max(loudest, 1e-9) * 4.0); // faint windows count for less
        if (estimate.periodSamples > 0.0)
            found.push_back(estimate);
    }
    if (found.empty())
        return none;
    std::vector<PitchEstimate> clear;
    for (const auto& e : found)
        if (e.confidence >= 0.6)
            clear.push_back(e);
    if (clear.empty())
        return *std::max_element(found.begin(), found.end(),
                                 [](const PitchEstimate& a, const PitchEstimate& b) { return a.confidence < b.confidence; });
    std::sort(clear.begin(), clear.end(),
              [](const PitchEstimate& a, const PitchEstimate& b) { return a.periodSamples < b.periodSamples; });
    return clear[clear.size() / 2];
}

double cycleSamplesForNote(double midiNote, double sampleRate, double referenceHz) {
    return sampleRate / noteToHz(midiNote, referenceHz);
}

bool importPitchTracked(const AudioClip& clip, const SliceOptions& options, Wavetable& out, std::string& error,
                        ImportReport* report) {
    out = Wavetable{};
    const PitchEstimate pitch = detectPitch(clip, options.minHz, options.maxHz);
    if (pitch.periodSamples < 2.0 || pitch.confidence < 0.5) {
        error = "no clear pitch was found in the recording; use a fixed cycle length or spectral snapshots instead";
        return false;
    }
    const auto total = static_cast<double>(clip.samples.size());
    double period = pitch.periodSamples;
    double position = options.snapToZeroCrossings ? nearestUpwardCrossing(clip, period * 0.5, period * 0.5) : 0.0;
    std::vector<double> markers = {position};
    constexpr std::size_t kMaxCycles = 200000;
    while (position + 2.3 * period < total && markers.size() < kMaxCycles) {
        // The lag near the current period at which the next stretch best matches this one.
        const int length = std::max(32, static_cast<int>(std::lround(period)));
        const int low = std::max(2, static_cast<int>(period * 0.85)), high = static_cast<int>(period * 1.18) + 1;
        const auto base = static_cast<std::int64_t>(std::llround(position));
        double energyA = 0.0;
        for (int j = 0; j < length; ++j)
            energyA += static_cast<double>(sampleOf(clip, base + j)) * sampleOf(clip, base + j);
        std::vector<double> score(static_cast<std::size_t>(high - low + 1), 0.0);
        int best = 0;
        for (int lag = low; lag <= high; ++lag) {
            double cross = 0.0, energyB = 0.0;
            for (int j = 0; j < length; ++j) {
                const double b = sampleOf(clip, base + lag + j);
                cross += sampleOf(clip, base + j) * b;
                energyB += b * b;
            }
            const double value = energyA > 0.0 && energyB > 0.0 ? cross / std::sqrt(energyA * energyB) : 0.0;
            score[static_cast<std::size_t>(lag - low)] = value;
            if (value > score[static_cast<std::size_t>(best)])
                best = lag - low;
        }
        double lag = low + best;
        if (best > 0 && best + 1 < static_cast<int>(score.size())) {
            const double a = score[static_cast<std::size_t>(best - 1)], b = score[static_cast<std::size_t>(best)],
                         c = score[static_cast<std::size_t>(best + 1)];
            const double curvature = a - 2.0 * b + c;
            if (curvature < -1e-12)
                lag += 0.5 * (a - c) / curvature;
        }
        if (score[static_cast<std::size_t>(best)] < 0.3)
            lag = period; // nothing matches here (silence, noise): keep the pace
        period = 0.5 * period + 0.5 * lag;
        position += lag;
        if (options.snapToZeroCrossings)
            position = nearestUpwardCrossing(clip, position, period * 0.25);
        if (position <= markers.back() + 1.0)
            position = markers.back() + period;
        markers.push_back(position);
    }
    if (markers.size() < 2) {
        error = "the recording is shorter than two cycles";
        return false;
    }
    framesFromMarkers(clip, markers, options, out);
    if (report != nullptr) {
        report->detectedHz = pitch.hz;
        report->cyclesFound = static_cast<int>(markers.size()) - 1;
        report->cycleSamples = (markers.back() - markers.front()) / report->cyclesFound;
        report->framesMade = out.size();
        report->markers = markers;
    }
    return true;
}

bool importFixedCycles(const AudioClip& clip, const SliceOptions& options, Wavetable& out, std::string& error,
                       ImportReport* report) {
    out = Wavetable{};
    double length = options.cycleSamples;
    double hz = 0.0;
    if (length <= 0.0) {
        const PitchEstimate pitch = detectPitch(clip, options.minHz, options.maxHz);
        if (pitch.periodSamples < 2.0 || pitch.confidence < 0.5) {
            error = "no clear pitch was found, so the cycle length cannot be worked out; give it in samples or as a note";
            return false;
        }
        length = pitch.periodSamples;
        hz = pitch.hz;
    } else {
        hz = clip.sampleRate / length;
    }
    if (length < 2.0) {
        error = "the cycle length must be at least 2 samples";
        return false;
    }
    const int cycles = static_cast<int>(std::min(static_cast<double>(clip.samples.size()) / length, 200000.0));
    if (cycles < 1) {
        error = "the recording is shorter than one cycle of " + std::to_string(length) + " samples";
        return false;
    }
    std::vector<double> markers;
    for (int c = 0; c <= cycles; ++c)
        markers.push_back(c * length);
    framesFromMarkers(clip, markers, options, out);
    if (report != nullptr) {
        report->detectedHz = hz;
        report->cycleSamples = length;
        report->cyclesFound = cycles;
        report->framesMade = out.size();
        report->markers = markers;
    }
    return true;
}

bool importSpectralSnapshots(const AudioClip& clip, const SnapshotOptions& options, Wavetable& out, std::string& error) {
    out = Wavetable{};
    const int size = options.fftSize;
    if (size != 256 && size != 512 && size != 1024 && size != 2048) {
        error = "the snapshot size must be 256, 512, 1024 or 2048";
        return false;
    }
    if (clip.samples.empty()) {
        error = "the recording holds no audio";
        return false;
    }
    const int frames = std::clamp(options.frames, 1, kMaxFrames);
    const auto total = static_cast<std::int64_t>(clip.samples.size());
    std::vector<double> window(static_cast<std::size_t>(size));
    double windowSum = 0.0;
    for (int i = 0; i < size; ++i) {
        window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / size);
        windowSum += window[static_cast<std::size_t>(i)];
    }
    const dsp::Fft& fft = dsp::Fft::forSize(size);
    std::vector<dsp::Fft::Complex> work(static_cast<std::size_t>(size));
    Rng rng(options.seed);
    for (int f = 0; f < frames; ++f) {
        const std::int64_t start = frames > 1 && total > size ? (total - size) * f / (frames - 1) : 0;
        for (int i = 0; i < size; ++i)
            work[static_cast<std::size_t>(i)] =
                start + i < total ? clip.samples[static_cast<std::size_t>(start + i)] * window[static_cast<std::size_t>(i)] : 0.0;
        fft.forward(work.data());
        Harmonics h;
        for (int k = 1; k < size / 2 && k < kMaxHarmonics; ++k) {
            const auto at = static_cast<std::size_t>(k);
            h.magnitude[at] = 2.0 * std::abs(work[at]) / windowSum;
            switch (options.phase) {
            case SnapshotPhase::Original: h.phase[at] = std::arg(work[at]) + kPi / 2.0; break;
            case SnapshotPhase::Zero: h.phase[at] = 0.0; break;
            case SnapshotPhase::Random: h.phase[at] = (rng.uniform() * 2.0 - 1.0) * kPi; break;
            }
        }
        out.frames.push_back(synthesizeFrame(h));
    }
    processFrames(out, allFrames(), ProcessOp::NormalizeShared);
    return true;
}

bool importImage(const ImageData& image, ImageMode mode, int maxFrames, Wavetable& out, std::string& error) {
    out = Wavetable{};
    if (image.width < 1 || image.height < 1 ||
        image.brightness.size() != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height)) {
        error = "the image is empty";
        return false;
    }
    const int frames = std::clamp(std::min(image.height, maxFrames), 1, kMaxFrames);
    std::vector<float> row(static_cast<std::size_t>(image.width));
    for (int f = 0; f < frames; ++f) {
        // The rows that belong to this frame, averaged.
        const int first = static_cast<int>(static_cast<std::int64_t>(f) * image.height / frames);
        const int end = std::max(first + 1, static_cast<int>(static_cast<std::int64_t>(f + 1) * image.height / frames));
        std::fill(row.begin(), row.end(), 0.0f);
        for (int y = first; y < end; ++y)
            for (int x = 0; x < image.width; ++x)
                row[static_cast<std::size_t>(x)] += image.at(x, y) / static_cast<float>(end - first);
        if (mode == ImageMode::Amplitude) {
            std::vector<float> cycle(row.size());
            for (std::size_t x = 0; x < row.size(); ++x)
                cycle[x] = 2.0f * row[x] - 1.0f;
            if (cycle.size() == 1)
                cycle.push_back(cycle[0]);
            out.frames.push_back(resampleCycle(cycle));
        } else {
            Harmonics h;
            for (int k = 0; k < image.width && k + 1 < kMaxHarmonics; ++k)
                h.magnitude[static_cast<std::size_t>(k + 1)] = row[static_cast<std::size_t>(k)];
            out.frames.push_back(synthesizeFrame(h));
        }
    }
    if (mode == ImageMode::Harmonics)
        processFrames(out, allFrames(), ProcessOp::NormalizeShared);
    return true;
}

ImportSuggestion suggestImport(const WavData& wav) {
    ImportSuggestion s;
    const auto total = static_cast<std::int64_t>(wav.frames());
    if (wav.chunks.count("ZEDT") != 0 || wav.chunks.count("clm ") != 0) {
        s.method = "table";
        s.frameSize = kFrameSize;
        s.reason = "The file carries wavetable metadata, so it is read as a table.";
        return s;
    }
    const AudioClip clip = clipFromWav(wav, StereoChoice::Mid);
    s.pitch = detectPitch(clip);
    const bool pitched = s.pitch.confidence >= 0.6 && s.pitch.periodSamples >= 2.0;
    if (total % kFrameSize == 0 && total / kFrameSize >= 2 && total / kFrameSize <= kMaxFrames) {
        // A table's frames repeat every 2048 samples or a whole fraction of that.
        const double perFrame = pitched ? kFrameSize / s.pitch.periodSamples : 0.0;
        if (!pitched || std::abs(perFrame - std::round(perFrame)) < 0.03) {
            s.method = "table";
            s.frameSize = kFrameSize;
            s.reason = "The length is a whole number of 2048-sample frames, so it is read as a table of " +
                       std::to_string(total / kFrameSize) + " frames.";
            return s;
        }
    }
    if (total <= 8192 && (!pitched || s.pitch.periodSamples > static_cast<double>(total) / 3.0)) {
        s.method = "single_cycle";
        s.reason = "The file is short and holds about one cycle, so it becomes one frame.";
        return s;
    }
    if (!pitched) {
        s.method = "spectral";
        s.reason = "No clear pitch was found, so spectral snapshots are taken through the recording.";
        return s;
    }
    // Does the pitch move? Compare the two halves.
    AudioClip firstHalf, secondHalf;
    firstHalf.sampleRate = secondHalf.sampleRate = clip.sampleRate;
    firstHalf.samples.assign(clip.samples.begin(), clip.samples.begin() + static_cast<std::ptrdiff_t>(clip.samples.size() / 2));
    secondHalf.samples.assign(clip.samples.begin() + static_cast<std::ptrdiff_t>(clip.samples.size() / 2), clip.samples.end());
    const PitchEstimate a = detectPitch(firstHalf), b = detectPitch(secondHalf);
    const bool steady = a.periodSamples > 0.0 && b.periodSamples > 0.0 &&
                        std::abs(a.periodSamples / b.periodSamples - 1.0) < 0.003;
    s.method = steady ? "fixed_cycles" : "pitch_tracked";
    s.reason = steady ? "The recording holds a steady pitch, so it is cut into cycles of one fixed length."
                      : "The pitch of the recording moves, so each cycle is found by following the pitch.";
    return s;
}

} // namespace zed
