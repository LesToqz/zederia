#include "zederia/table/TableData.h"

#include "zederia/dsp/Fft.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace zed {
namespace {

using Complex = std::complex<double>;

// Three levels per octave; duplicates at the bottom removed.
constexpr std::array<int, TableData::kNumLevels> kHarmonics = {1024, 812, 645, 512, 406, 322, 256, 203, 161, 128,
                                                               101,  80,  64,  50,  40,  32,  25,  20,  16,  12,
                                                               10,   8,   6,   5,   4,   3,   2,   1};

// Worst spline image of a sawtooth's top harmonic, relative to its fundamental, in decibels.
double worstImageDb(int harmonics, int size) {
    const double x = static_cast<double>(harmonics) / size;
    return 120.0 * std::log10(x / (1.0 - x)) - 20.0 * std::log10(static_cast<double>(harmonics));
}

int sizeFor(int harmonics) {
    int size = 64;
    while (size < 4096 && (size < 4 * harmonics || worstImageDb(harmonics, size) > -104.0))
        size *= 2;
    return size;
}

const std::array<float, TableData::kNumLevels>& maxIncs() {
    static const std::array<float, TableData::kNumLevels> values = [] {
        std::array<float, TableData::kNumLevels> v{};
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = 0.5f / static_cast<float>(kHarmonics[i]);
        return v;
    }();
    return values;
}

struct Cache {
    rt::Mutex mutex;
    std::unordered_map<std::uint64_t, std::weak_ptr<const TableData>> tables;
};
Cache& cache() {
    static Cache c;
    return c;
}

Frame normalized(Frame f) {
    const float peak = framePeak(f);
    if (peak > 0.0f)
        for (float& s : f)
            s /= peak;
    return f;
}

} // namespace

void splineWeights(double t, double w[6]) {
    const double u = 1.0 - t;
    const auto outer = [](double v) { return v * v * v * v * v / 120.0; };
    const auto mid = [](double v) { return (1.0 + v * (5.0 + v * (10.0 + v * (10.0 + v * (5.0 - 5.0 * v))))) / 120.0; };
    const auto inner = [](double v) {
        return (26.0 + v * (50.0 + v * (20.0 + v * (-20.0 + v * (-20.0 + 10.0 * v))))) / 120.0;
    };
    w[0] = outer(u);
    w[1] = mid(u);
    w[2] = inner(u);
    w[3] = inner(t);
    w[4] = mid(t);
    w[5] = outer(t);
}

int TableData::levelHarmonics(int level) noexcept {
    return kHarmonics[static_cast<std::size_t>(std::clamp(level, 0, kNumLevels - 1))];
}

int TableData::levelFor(float inc) noexcept {
    const auto& limits = maxIncs(); // ascending
    int lo = 0, hi = kNumLevels - 1;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (inc <= limits[static_cast<std::size_t>(mid)])
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

Wavetable TableData::builtinWavetable() {
    Wavetable t;
    t.name = "Basic Shapes";
    t.morph = MorphMode::Crossfade;
    Harmonics saw, square, triangle, sine;
    for (int h = 1; h < kMaxHarmonics; ++h) {
        const auto k = static_cast<std::size_t>(h);
        saw.magnitude[k] = 1.0 / h;
        saw.phase[k] = kPi; // a rising ramp
        if (h % 2 == 1) {
            square.magnitude[k] = 1.0 / h;
            triangle.magnitude[k] = 1.0 / (static_cast<double>(h) * h);
            triangle.phase[k] = (h / 2) % 2 == 0 ? 0.0 : kPi;
        }
    }
    sine.magnitude[1] = 1.0;
    for (const Harmonics* h : {&saw, &square, &triangle, &sine})
        t.frames.push_back(normalized(synthesizeFrame(*h)));
    return t;
}

std::shared_ptr<const TableData> TableData::builtin() {
    static const std::shared_ptr<const TableData> table = build(builtinWavetable());
    return table;
}

std::shared_ptr<const TableData> TableData::build(const Wavetable& source) {
    Wavetable table = source;
    if (table.frames.empty())
        table.frames.push_back(Frame{});
    if (table.size() > kMaxFrames)
        table.frames.resize(kMaxFrames);

    const bool spectral = !table.stepped && table.size() > 1 &&
                          (table.morph == MorphMode::Spectral || table.morph == MorphMode::SpectralAligned ||
                           table.morph == MorphMode::SpectralZero);
    std::uint64_t hash = fnv1a(table.frames.data(), table.frames.size() * sizeof(Frame));
    const std::uint8_t flags[2] = {static_cast<std::uint8_t>(spectral ? static_cast<int>(table.morph) : 0),
                                   static_cast<std::uint8_t>(table.stepped)};
    hash = fnv1a(flags, sizeof flags, hash);
    hash = fnv1a(table.name.data(), table.name.size(), hash);
    {
        const std::lock_guard<rt::Mutex> lock(cache().mutex);
        if (const auto it = cache().tables.find(hash); it != cache().tables.end())
            if (auto existing = it->second.lock())
                return existing;
    }

    const std::vector<Frame> frames = spectral ? generatePositions(table, kMaxFrames) : table.frames;
    const int count = static_cast<int>(frames.size());

    std::shared_ptr<TableData> out(new TableData());
    out->frames_ = count;
    out->stepped_ = table.stepped;
    out->name_ = table.name;
    out->hash_ = hash;
    int offset = 0;
    for (int l = 0; l < kNumLevels; ++l) {
        Level& level = out->levels_[static_cast<std::size_t>(l)];
        level.harmonics = kHarmonics[static_cast<std::size_t>(l)];
        level.size = sizeFor(level.harmonics);
        level.stride = level.size + kGuard;
        level.offset = offset;
        level.maxInc = maxIncs()[static_cast<std::size_t>(l)];
        offset += level.stride * (count + 1); // one more frame: a copy of the last, so a blend never reads past the end
    }
    out->data_.assign(static_cast<std::size_t>(offset) + 16, 0.0f);

    std::array<Complex, kMaxHarmonics + 1> bins;
    std::vector<Complex> work(4096);
    for (int f = 0; f < count; ++f) {
        frameToBins(frames[static_cast<std::size_t>(f)], bins);
        for (int l = 0; l < kNumLevels; ++l) {
            const Level& level = out->levels_[static_cast<std::size_t>(l)];
            const int n = level.size;
            std::fill(work.begin(), work.begin() + n, Complex(0.0));
            work[0] = bins[0].real() * (static_cast<double>(n) / kFrameSize);
            for (int h = 1; h <= level.harmonics; ++h) {
                const double x = static_cast<double>(h) / n;
                const double sinc = std::sin(kPi * x) / (kPi * x);
                const double lift = 1.0 / (sinc * sinc * sinc * sinc * sinc * sinc); // undo the spline's roll-off
                Complex value = bins[static_cast<std::size_t>(h)] * (lift * n / kFrameSize);
                if (h == kMaxHarmonics)
                    value = value.real() * 0.5; // the frame's Nyquist bin holds both halves of a cosine
                work[static_cast<std::size_t>(h)] = value;
                work[static_cast<std::size_t>(n - h)] = std::conj(value);
            }
            dsp::Fft::forSize(n).inverse(work.data());
            float* dest = out->data_.data() + level.offset + f * level.stride;
            for (int i = 0; i < level.stride; ++i)
                dest[i] = static_cast<float>(work[static_cast<std::size_t>((i - kGuardBefore + n) % n)].real());
            if (f == count - 1)
                std::copy(dest, dest + level.stride, dest + level.stride);
        }
    }

    const std::lock_guard<rt::Mutex> lock(cache().mutex);
    auto& slot = cache().tables[hash];
    if (auto existing = slot.lock())
        return existing; // another thread built the same table meanwhile
    slot = out;
    for (auto it = cache().tables.begin(); it != cache().tables.end();) // drop entries whose tables are gone
        it = it->second.expired() ? cache().tables.erase(it) : std::next(it);
    return out;
}

double TableData::read(int levelIndex, double framePosition, double phase) const {
    const Level& level = levels_[static_cast<std::size_t>(std::clamp(levelIndex, 0, kNumLevels - 1))];
    framePosition = std::clamp(framePosition, 0.0, static_cast<double>(frames_ - 1));
    const int frame = static_cast<int>(framePosition);
    const double blend = framePosition - frame;
    phase -= std::floor(phase);
    const double at = phase * level.size;
    const int i = static_cast<int>(at);
    double w[6];
    splineWeights(at - i, w);
    const float* a = data_.data() + level.offset + frame * level.stride + i;
    const float* b = a + level.stride;
    double sum = 0.0;
    for (int k = 0; k < 6; ++k)
        sum += w[k] * (a[k] + blend * (static_cast<double>(b[k]) - a[k]));
    return sum;
}

} // namespace zed
