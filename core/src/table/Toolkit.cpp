#include "zederia/table/Toolkit.h"

#include "zederia/audio/WavFile.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace zed {
namespace {

constexpr int kTop = kMaxHarmonics - 1; // the highest harmonic the operations touch; 1024 is cosine-only

// Clamps a selection to the table. Returns false when it selects nothing.
bool resolve(const Wavetable& table, FrameSelection s, int& first, int& last) {
    first = std::max(s.first, 0);
    last = s.last < 0 ? table.size() - 1 : std::min(s.last, table.size() - 1);
    return first <= last;
}

int cycleIndex(double position) { return std::clamp(static_cast<int>(std::lround(position * kFrameSize)), 0, kFrameSize); }

double wrapPhase(double radians) { return radians - 2.0 * kPi * std::floor((radians + kPi) / (2.0 * kPi)); }

// The frame as a continuous cycle, read between samples with a cubic through four neighbours.
float readCycle(const Frame& f, double position) {
    position -= std::floor(position);
    const double at = position * kFrameSize;
    const int i = static_cast<int>(at);
    const auto t = static_cast<float>(at - i);
    const auto s = [&](int k) { return f[static_cast<std::size_t>((i + k + kFrameSize) % kFrameSize)]; };
    const float a = s(-1), b = s(0), c = s(1), d = s(2);
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}

Frame lowPassed(const Frame& f, int harmonics) {
    Harmonics h = analyzeFrame(f);
    for (int k = std::max(harmonics, 0) + 1; k <= kMaxHarmonics; ++k)
        h.magnitude[static_cast<std::size_t>(k)] = 0.0;
    return synthesizeFrame(h);
}

template <typename Enum, std::size_t N>
bool fromName(std::string_view name, const std::array<std::pair<std::string_view, Enum>, N>& names, Enum& out) {
    for (const auto& [text, value] : names) {
        if (text == name) {
            out = value;
            return true;
        }
    }
    return false;
}

} // namespace

// --- Frames strip ------------------------------------------------------------------------------

bool insertFrame(Wavetable& table, int at, const Frame& frame) {
    if (at < 0 || at > table.size() || table.size() >= kMaxFrames)
        return false;
    table.frames.insert(table.frames.begin() + at, frame);
    return true;
}

bool duplicateFrames(Wavetable& table, FrameSelection selection) {
    int first = 0, last = 0;
    if (!resolve(table, selection, first, last) || table.size() + (last - first + 1) > kMaxFrames)
        return false;
    const std::vector<Frame> copy(table.frames.begin() + first, table.frames.begin() + last + 1);
    table.frames.insert(table.frames.begin() + last + 1, copy.begin(), copy.end());
    return true;
}

bool deleteFrames(Wavetable& table, FrameSelection selection) {
    int first = 0, last = 0;
    if (!resolve(table, selection, first, last) || last - first + 1 >= table.size())
        return false;
    table.frames.erase(table.frames.begin() + first, table.frames.begin() + last + 1);
    return true;
}

bool moveFrame(Wavetable& table, int from, int to) {
    if (from < 0 || from >= table.size() || to < 0 || to >= table.size())
        return false;
    const Frame moved = table.frames[static_cast<std::size_t>(from)];
    table.frames.erase(table.frames.begin() + from);
    table.frames.insert(table.frames.begin() + to, moved);
    return true;
}

bool thinFrames(Wavetable& table, int keepEvery) {
    if (keepEvery < 1)
        return false;
    std::vector<Frame> kept;
    for (int i = 0; i < table.size(); i += keepEvery)
        kept.push_back(table.frames[static_cast<std::size_t>(i)]);
    table.frames = std::move(kept);
    return true;
}

bool cropFrames(Wavetable& table, FrameSelection selection) {
    int first = 0, last = 0;
    if (!resolve(table, selection, first, last))
        return false;
    table.frames = std::vector<Frame>(table.frames.begin() + first, table.frames.begin() + last + 1);
    return true;
}

void reverseFrames(Wavetable& table) { std::reverse(table.frames.begin(), table.frames.end()); }

// --- Wave view ---------------------------------------------------------------------------------

void drawSegment(Frame& frame, double from, double to, DrawShape shape, float a, float b) {
    if (to < from)
        std::swap(from, to);
    const int first = cycleIndex(from), end = cycleIndex(to);
    const int length = end - first;
    for (int i = first; i < end; ++i) {
        const double x = length > 1 ? static_cast<double>(i - first) / (length - 1) : 0.0; // 0..1 across the segment
        double value = a;
        switch (shape) {
        case DrawShape::Flat: value = a; break;
        case DrawShape::RampUp:
        case DrawShape::Line: value = a + (b - a) * x; break;
        case DrawShape::RampDown: value = b + (a - b) * x; break;
        case DrawShape::Sine: value = a + b * std::sin(2.0 * kPi * x); break;
        case DrawShape::HalfSineUp: value = a + (b - a) * std::sin(kPi * x); break;
        case DrawShape::HalfSineDown: value = a - b * std::sin(kPi * x); break;
        case DrawShape::QuarterIn: value = a + (b - a) * (1.0 - std::cos(kPi * 0.5 * x)); break;
        case DrawShape::QuarterOut: value = a + (b - a) * std::sin(kPi * 0.5 * x); break;
        case DrawShape::Curve: value = a + (b - a) * x * x * (3.0 - 2.0 * x); break;
        }
        frame[static_cast<std::size_t>(i)] = static_cast<float>(value);
    }
}

void nudgeSegment(Frame& frame, double from, double to, float amount) {
    if (to < from)
        std::swap(from, to);
    for (int i = cycleIndex(from); i < cycleIndex(to); ++i)
        frame[static_cast<std::size_t>(i)] += amount;
}

void addNoise(Frame& frame, double from, double to, float amount, std::uint64_t seed) {
    if (to < from)
        std::swap(from, to);
    Rng rng(seed);
    for (int i = cycleIndex(from); i < cycleIndex(to); ++i)
        frame[static_cast<std::size_t>(i)] += amount * static_cast<float>(rng.uniform() * 2.0 - 1.0);
}

double snapToGrid(double position, int divisions) {
    if (divisions < 1)
        return position;
    return std::clamp(std::round(position * divisions) / divisions, 0.0, 1.0);
}

// --- Harmonic view -----------------------------------------------------------------------------

void applyHarmonicOp(Frame& frame, HarmonicOp op, int n, double amount, std::uint64_t seed) {
    Harmonics h = analyzeFrame(frame);
    auto& m = h.magnitude;
    auto& p = h.phase;
    Rng rng(seed);
    const auto at = [](int k) { return static_cast<std::size_t>(k); };
    switch (op) {
    case HarmonicOp::ClearAll:
        h = Harmonics{};
        break;
    case HarmonicOp::ClearAbove:
        for (int k = std::max(n, 0) + 1; k <= kMaxHarmonics; ++k)
            m[at(k)] = 0.0;
        break;
    case HarmonicOp::ClearBelow:
        for (int k = 1; k < std::min(n, kMaxHarmonics + 1); ++k)
            m[at(k)] = 0.0;
        break;
    case HarmonicOp::FillSaw:
        h = Harmonics{};
        for (int k = 1; k <= kTop; ++k) {
            m[at(k)] = 1.0 / k;
            p[at(k)] = kPi;
        }
        break;
    case HarmonicOp::RandomizeLowest:
        for (int k = 1; k <= std::min(n, kTop); ++k)
            m[at(k)] = rng.uniform() * amount;
        break;
    case HarmonicOp::RandomizeAll:
        for (int k = 1; k <= kTop; ++k)
            m[at(k)] = rng.uniform() * amount;
        break;
    case HarmonicOp::RandomGaps:
        for (int k = 1; k <= kMaxHarmonics; ++k)
            if (rng.uniform() < amount)
                m[at(k)] = 0.0;
        break;
    case HarmonicOp::FadeHighs:
        for (int k = 1; k <= kMaxHarmonics; ++k)
            m[at(k)] *= std::pow(static_cast<double>(k), -amount);
        break;
    case HarmonicOp::OctaveUp: {
        const Harmonics old = h;
        for (int k = 1; k <= kMaxHarmonics; ++k) {
            m[at(k)] = k % 2 == 0 && k / 2 >= 1 && k <= kTop ? old.magnitude[at(k / 2)] : 0.0;
            p[at(k)] = k % 2 == 0 && k <= kTop ? old.phase[at(k / 2)] : 0.0;
        }
        break;
    }
    case HarmonicOp::OctaveDown: {
        const Harmonics old = h;
        for (int k = 1; k <= kMaxHarmonics; ++k) {
            m[at(k)] = 2 * k <= kTop ? old.magnitude[at(2 * k)] : 0.0;
            p[at(k)] = 2 * k <= kTop ? old.phase[at(2 * k)] : 0.0;
        }
        break;
    }
    case HarmonicOp::OddOnly:
        for (int k = 2; k <= kMaxHarmonics; k += 2)
            m[at(k)] = 0.0;
        break;
    case HarmonicOp::EvenOnly:
        for (int k = 1; k <= kMaxHarmonics; k += 2)
            m[at(k)] = 0.0;
        break;
    case HarmonicOp::SnapQuarters: {
        const double top = *std::max_element(m.begin() + 1, m.end());
        if (top > 0.0)
            for (int k = 1; k <= kMaxHarmonics; ++k)
                m[at(k)] = std::round(m[at(k)] / top * 4.0) / 4.0 * top;
        break;
    }
    case HarmonicOp::ScaleByHarmonic:
        for (int k = 1; k <= kMaxHarmonics; ++k)
            m[at(k)] *= std::pow(static_cast<double>(k), amount);
        break;
    }
    // The cosine-only top harmonic keeps a valid phase.
    if (std::abs(std::abs(p[at(kMaxHarmonics)]) - kPi / 2.0) > 1e-9)
        p[at(kMaxHarmonics)] = kPi / 2.0;
    frame = synthesizeFrame(h);
}

bool harmonicOpFromName(std::string_view name, HarmonicOp& out) {
    static const std::array<std::pair<std::string_view, HarmonicOp>, 14> names = {{
        {"clear_all", HarmonicOp::ClearAll},
        {"clear_above", HarmonicOp::ClearAbove},
        {"clear_below", HarmonicOp::ClearBelow},
        {"fill_saw", HarmonicOp::FillSaw},
        {"randomize_lowest", HarmonicOp::RandomizeLowest},
        {"randomize_all", HarmonicOp::RandomizeAll},
        {"random_gaps", HarmonicOp::RandomGaps},
        {"fade_highs", HarmonicOp::FadeHighs},
        {"octave_up", HarmonicOp::OctaveUp},
        {"octave_down", HarmonicOp::OctaveDown},
        {"odd_only", HarmonicOp::OddOnly},
        {"even_only", HarmonicOp::EvenOnly},
        {"snap_quarters", HarmonicOp::SnapQuarters},
        {"scale_by_harmonic", HarmonicOp::ScaleByHarmonic},
    }};
    return fromName(name, names, out);
}

// --- Process -----------------------------------------------------------------------------------

void processFrames(Wavetable& table, FrameSelection selection, ProcessOp op, int n) {
    int first = 0, last = 0;
    if (!resolve(table, selection, first, last))
        return;
    float shared = 0.0f;
    if (op == ProcessOp::NormalizeShared)
        for (int f = first; f <= last; ++f)
            shared = std::max(shared, framePeak(table.frames[static_cast<std::size_t>(f)]));

    for (int f = first; f <= last; ++f) {
        Frame& frame = table.frames[static_cast<std::size_t>(f)];
        switch (op) {
        case ProcessOp::NormalizeEach:
        case ProcessOp::NormalizeShared: {
            const float peak = op == ProcessOp::NormalizeEach ? framePeak(frame) : shared;
            if (peak > 0.0f)
                for (float& s : frame)
                    s /= peak;
            break;
        }
        case ProcessOp::RemoveDc: {
            const double mean = std::accumulate(frame.begin(), frame.end(), 0.0) / kFrameSize;
            for (float& s : frame)
                s -= static_cast<float>(mean);
            break;
        }
        case ProcessOp::FlipPolarity:
            for (float& s : frame)
                s = -s;
            break;
        case ProcessOp::MirrorTime:
            std::reverse(frame.begin(), frame.end());
            break;
        case ProcessOp::ShiftToZeroCrossing: {
            // The upward crossing nearest to the start of the cycle, looking both ways.
            int best = -1;
            for (int d = 0; d <= kFrameSize / 2 && best < 0; ++d) {
                for (const int i : {d, kFrameSize - d}) {
                    const int at = i % kFrameSize;
                    const float before = frame[static_cast<std::size_t>((at + kFrameSize - 1) % kFrameSize)];
                    if (before < 0.0f && frame[static_cast<std::size_t>(at)] >= 0.0f) {
                        best = at;
                        break;
                    }
                }
            }
            if (best > 0)
                std::rotate(frame.begin(), frame.begin() + best, frame.end());
            break;
        }
        case ProcessOp::Clear:
            frame.fill(0.0f);
            break;
        case ProcessOp::FadeEdges: {
            const int length = std::clamp(n > 0 ? n : 64, 1, kFrameSize / 2);
            for (int i = 0; i < length; ++i) {
                const float gain = static_cast<float>(i) / static_cast<float>(length);
                frame[static_cast<std::size_t>(i)] *= gain;
                frame[static_cast<std::size_t>(kFrameSize - 1 - i)] *= gain;
            }
            break;
        }
        case ProcessOp::CrossfadeEdges: {
            // The end of the cycle fades into a mirror image of its start, so the last sample
            // arrives where the first one begins.
            const int length = std::clamp(n > 0 ? n : 64, 1, kFrameSize / 2);
            const Frame original = frame;
            for (int i = 0; i < length; ++i) {
                const float w = static_cast<float>(i + 1) / static_cast<float>(length);
                const std::size_t at = static_cast<std::size_t>(kFrameSize - length + i);
                frame[at] = original[at] * (1.0f - w) + original[static_cast<std::size_t>(length - 1 - i)] * w;
            }
            break;
        }
        case ProcessOp::LowPass:
            frame = lowPassed(frame, n);
            break;
        case ProcessOp::RemoveFundamental: {
            Harmonics h = analyzeFrame(frame);
            h.magnitude[1] = 0.0;
            frame = synthesizeFrame(h);
            break;
        }
        case ProcessOp::SampleRateReduce: {
            const int steps = std::clamp(n > 0 ? n : 32, 1, kFrameSize);
            const Frame original = frame;
            for (int i = 0; i < kFrameSize; ++i) {
                const int step = static_cast<int>(static_cast<std::int64_t>(i) * steps / kFrameSize);
                frame[static_cast<std::size_t>(i)] =
                    original[static_cast<std::size_t>(static_cast<std::int64_t>(step) * kFrameSize / steps)];
            }
            break;
        }
        case ProcessOp::AlignFundamental: {
            Harmonics h = analyzeFrame(frame);
            if (h.magnitude[1] > 1e-9) {
                const double shift = h.phase[1];
                for (int k = 1; k < kMaxHarmonics; ++k)
                    h.phase[static_cast<std::size_t>(k)] = wrapPhase(h.phase[static_cast<std::size_t>(k)] - k * shift);
                frame = synthesizeFrame(h);
            }
            break;
        }
        }
    }
}

bool processOpFromName(std::string_view name, ProcessOp& out) {
    static const std::array<std::pair<std::string_view, ProcessOp>, 13> names = {{
        {"normalize_each", ProcessOp::NormalizeEach},
        {"normalize_shared", ProcessOp::NormalizeShared},
        {"remove_dc", ProcessOp::RemoveDc},
        {"flip_polarity", ProcessOp::FlipPolarity},
        {"mirror_time", ProcessOp::MirrorTime},
        {"shift_to_zero_crossing", ProcessOp::ShiftToZeroCrossing},
        {"clear", ProcessOp::Clear},
        {"fade_edges", ProcessOp::FadeEdges},
        {"crossfade_edges", ProcessOp::CrossfadeEdges},
        {"low_pass", ProcessOp::LowPass},
        {"remove_fundamental", ProcessOp::RemoveFundamental},
        {"sample_rate_reduce", ProcessOp::SampleRateReduce},
        {"align_fundamental", ProcessOp::AlignFundamental},
    }};
    return fromName(name, names, out);
}

bool splitFrames(Wavetable& table) {
    if (table.size() * 2 > kMaxFrames)
        return false;
    std::vector<Frame> out;
    out.reserve(table.frames.size() * 2);
    for (const Frame& f : table.frames) {
        out.push_back(resampleCycle(std::span<const float>(f).first(kFrameSize / 2)));
        out.push_back(resampleCycle(std::span<const float>(f).last(kFrameSize / 2)));
    }
    table.frames = std::move(out);
    return true;
}

void joinFramePairs(Wavetable& table) {
    std::vector<Frame> out;
    for (std::size_t i = 0; i < table.frames.size(); i += 2) {
        // Each half keeps the harmonics that still fit after squeezing to half the length.
        const Frame a = lowPassed(table.frames[i], kFrameSize / 4 - 1);
        const Frame b = i + 1 < table.frames.size() ? lowPassed(table.frames[i + 1], kFrameSize / 4 - 1) : Frame{};
        Frame joined;
        for (std::size_t n = 0; n < kFrameSize / 2; ++n) {
            joined[n] = a[2 * n];
            joined[n + kFrameSize / 2] = b[2 * n];
        }
        out.push_back(joined);
    }
    table.frames = std::move(out);
}

Wavetable pulseWidthSweep(const Frame& frame, int frames, double narrowest) {
    Wavetable out;
    frames = std::clamp(frames, 1, kMaxFrames);
    narrowest = std::clamp(narrowest, 0.01, 1.0);
    for (int f = 0; f < frames; ++f) {
        const double width = frames > 1 ? 1.0 + (narrowest - 1.0) * f / (frames - 1) : 1.0;
        Frame squeezed;
        for (int n = 0; n < kFrameSize; ++n) {
            const double p = static_cast<double>(n) / kFrameSize;
            squeezed[static_cast<std::size_t>(n)] = p < width ? readCycle(frame, p / width) : frame[0];
        }
        out.frames.push_back(squeezed);
    }
    return out;
}

void copySpectrumToAll(Wavetable& table, int source) {
    if (source < 0 || source >= table.size())
        return;
    const Harmonics from = analyzeFrame(table.frames[static_cast<std::size_t>(source)]);
    for (int f = 0; f < table.size(); ++f) {
        if (f == source)
            continue;
        Harmonics h = analyzeFrame(table.frames[static_cast<std::size_t>(f)]);
        for (std::size_t k = 1; k <= static_cast<std::size_t>(kMaxHarmonics); ++k) {
            if (h.magnitude[k] < 1e-12)
                h.phase[k] = from.phase[k]; // a harmonic the frame did not have takes the source's phase
            h.magnitude[k] = from.magnitude[k];
        }
        table.frames[static_cast<std::size_t>(f)] = synthesizeFrame(h);
    }
}

void copyPhasesToAll(Wavetable& table, int source) {
    if (source < 0 || source >= table.size())
        return;
    const Harmonics from = analyzeFrame(table.frames[static_cast<std::size_t>(source)]);
    for (int f = 0; f < table.size(); ++f) {
        if (f == source)
            continue;
        Harmonics h = analyzeFrame(table.frames[static_cast<std::size_t>(f)]);
        h.phase = from.phase;
        table.frames[static_cast<std::size_t>(f)] = synthesizeFrame(h);
    }
}

void subtractSpectrum(Wavetable& table, const Wavetable& other) {
    if (other.frames.empty())
        return;
    for (int f = 0; f < table.size(); ++f) {
        const double position = table.size() > 1 ? static_cast<double>(f) / (table.size() - 1) : 0.0;
        const Harmonics minus = analyzeFrame(frameAt(other, position));
        Harmonics h = analyzeFrame(table.frames[static_cast<std::size_t>(f)]);
        for (std::size_t k = 1; k <= static_cast<std::size_t>(kMaxHarmonics); ++k)
            h.magnitude[k] = std::max(0.0, h.magnitude[k] - minus.magnitude[k]);
        table.frames[static_cast<std::size_t>(f)] = synthesizeFrame(h);
    }
}

// --- Morph -------------------------------------------------------------------------------------

void bakeMorph(Wavetable& table, int frames) {
    table.frames = generatePositions(table, std::clamp(frames, 1, kMaxFrames));
    table.morph = MorphMode::None;
}

// --- Sort --------------------------------------------------------------------------------------

double sortValue(const Frame& frame, SortKey key) {
    const Harmonics h = analyzeFrame(frame);
    const auto& m = h.magnitude;
    const double top = *std::max_element(m.begin() + 1, m.end());
    if (top <= 0.0)
        return 0.0;
    switch (key) {
    case SortKey::BrightestPeak: {
        // The highest local maximum that is within 24 dB of the strongest harmonic.
        int best = 1;
        for (int k = 1; k <= kMaxHarmonics; ++k) {
            const double here = m[static_cast<std::size_t>(k)];
            const double below = k > 1 ? m[static_cast<std::size_t>(k - 1)] : 0.0;
            const double above = k < kMaxHarmonics ? m[static_cast<std::size_t>(k + 1)] : 0.0;
            if (here >= top * 0.063 && here >= below && here >= above)
                best = k;
        }
        return best;
    }
    case SortKey::Brightness: {
        double weighted = 0.0, total = 0.0;
        for (int k = 1; k <= kMaxHarmonics; ++k) {
            weighted += k * m[static_cast<std::size_t>(k)];
            total += m[static_cast<std::size_t>(k)];
        }
        return weighted / total;
    }
    case SortKey::StrongestPartial:
        return static_cast<double>(std::max_element(m.begin() + 1, m.end()) - m.begin());
    case SortKey::ActivePartials: {
        int count = 0;
        for (int k = 1; k <= kMaxHarmonics; ++k)
            count += m[static_cast<std::size_t>(k)] >= top * 0.001;
        return count;
    }
    case SortKey::HighestPartial: {
        int highest = 0;
        for (int k = 1; k <= kMaxHarmonics; ++k)
            if (m[static_cast<std::size_t>(k)] >= top * 0.001)
                highest = k;
        return highest;
    }
    case SortKey::Fundamental: {
        double energy = 0.0;
        for (int k = 1; k <= kMaxHarmonics; ++k)
            energy += m[static_cast<std::size_t>(k)] * m[static_cast<std::size_t>(k)];
        return m[1] * m[1] / energy;
    }
    }
    return 0.0;
}

void sortFrames(Wavetable& table, SortKey key, bool descending) {
    std::vector<std::pair<double, std::size_t>> order;
    for (std::size_t i = 0; i < table.frames.size(); ++i)
        order.emplace_back(sortValue(table.frames[i], key), i);
    std::stable_sort(order.begin(), order.end(),
                     [&](const auto& a, const auto& b) { return descending ? a.first > b.first : a.first < b.first; });
    std::vector<Frame> sorted;
    sorted.reserve(table.frames.size());
    for (const auto& [value, index] : order)
        sorted.push_back(table.frames[index]);
    table.frames = std::move(sorted);
}

bool sortKeyFromName(std::string_view name, SortKey& out) {
    static const std::array<std::pair<std::string_view, SortKey>, 6> names = {{
        {"brightest_peak", SortKey::BrightestPeak},
        {"brightness", SortKey::Brightness},
        {"strongest_partial", SortKey::StrongestPartial},
        {"active_partials", SortKey::ActivePartials},
        {"highest_partial", SortKey::HighestPartial},
        {"fundamental", SortKey::Fundamental},
    }};
    return fromName(name, names, out);
}

// --- Follow-ups and export ---------------------------------------------------------------------

int trimSilentFrames(Wavetable& table, double thresholdDb) {
    float loudest = 0.0f;
    for (const Frame& f : table.frames)
        loudest = std::max(loudest, framePeak(f));
    if (loudest <= 0.0f)
        return 0;
    const auto limit = static_cast<float>(loudest * std::pow(10.0, thresholdDb / 20.0));
    std::vector<Frame> kept;
    for (const Frame& f : table.frames)
        if (framePeak(f) > limit)
            kept.push_back(f);
    if (kept.empty())
        return 0;
    const int removed = table.size() - static_cast<int>(kept.size());
    table.frames = std::move(kept);
    return removed;
}

std::vector<std::vector<std::uint8_t>> exportSingleCycles(const Wavetable& table, FrameSelection selection,
                                                          TableSampleFormat format) {
    std::vector<std::vector<std::uint8_t>> files;
    int first = 0, last = 0;
    if (!resolve(table, selection, first, last))
        return files;
    for (int f = first; f <= last; ++f) {
        WavData wav;
        wav.sampleRate = 48000.0;
        wav.channels.emplace_back(table.frames[static_cast<std::size_t>(f)].begin(),
                                  table.frames[static_cast<std::size_t>(f)].end());
        files.push_back(encodeWavData(wav, format == TableSampleFormat::Pcm16   ? WavSampleFormat::Pcm16
                                           : format == TableSampleFormat::Pcm24 ? WavSampleFormat::Pcm24
                                                                                : WavSampleFormat::Float32));
    }
    return files;
}

} // namespace zed
