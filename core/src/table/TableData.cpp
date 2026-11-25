#include "zederia/table/TableData.h"

#include "zederia/dsp/Fft.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <thread>
#include <cmath>
#include <cstring>
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

// Per level, the gain that undoes the interpolating spline's roll-off at each harmonic
// (1 / sinc^6 of h / size): the same for every frame, so worked out once.
const std::array<std::vector<double>, TableData::kNumLevels>& levelLifts() {
    static const std::array<std::vector<double>, TableData::kNumLevels> lifts = [] {
        std::array<std::vector<double>, TableData::kNumLevels> out;
        for (std::size_t l = 0; l < out.size(); ++l) {
            const int harmonics = kHarmonics[l], n = sizeFor(harmonics);
            out[l].assign(static_cast<std::size_t>(harmonics) + 1, 1.0);
            for (int h = 1; h <= harmonics; ++h) {
                const double x = static_cast<double>(h) / n;
                const double sinc = std::sin(kPi * x) / (kPi * x);
                out[l][static_cast<std::size_t>(h)] = 1.0 / (sinc * sinc * sinc * sinc * sinc * sinc);
            }
        }
        return out;
    }();
    return lifts;
}

// A frame's 64-bit fingerprint, eight bytes at a time (a byte-wise hash of 256 frames costs more
// than the workshop can spend while a stroke is being drawn).
std::uint64_t frameHash(const Frame& frame) {
    static_assert(sizeof(Frame) % sizeof(std::uint64_t) == 0);
    const auto* bytes = reinterpret_cast<const unsigned char*>(frame.data());
    std::uint64_t h = 0x9e3779b97f4a7c15ull;
    for (std::size_t i = 0; i < sizeof(Frame); i += sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, bytes + i, sizeof word);
        h = (h ^ word) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    return h;
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

std::shared_ptr<const TableData> TableData::build(const Wavetable& source) { return build(source, nullptr); }

std::shared_ptr<const TableData> TableData::build(const Wavetable& source, const TableData* previous) {
    // The source is used as it is unless it needs fixing (no frames, too many): no copy of the frames.
    Wavetable fixed;
    const Wavetable* in = &source;
    if (source.frames.empty() || source.size() > kMaxFrames) {
        fixed = source;
        if (fixed.frames.empty())
            fixed.frames.push_back(Frame{});
        if (fixed.size() > kMaxFrames)
            fixed.frames.resize(kMaxFrames);
        in = &fixed;
    }
    const Wavetable& table = *in;

    const bool spectral = !table.stepped && table.size() > 1 &&
                          (table.morph == MorphMode::Spectral || table.morph == MorphMode::SpectralAligned ||
                           table.morph == MorphMode::SpectralZero);
    // Key frames, hashed once: they name the table in the cache, and for a spectral table only the
    // positions between key frames that changed since `previous` are generated again.
    std::vector<std::uint64_t> keyHashes(table.frames.size());
    for (std::size_t k = 0; k < table.frames.size(); ++k)
        keyHashes[k] = frameHash(table.frames[k]);
    std::uint64_t hash = fnv1a(keyHashes.data(), keyHashes.size() * sizeof(std::uint64_t));
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
    std::vector<char> reusable(spectral ? static_cast<std::size_t>(kMaxFrames) : 0, 0);
    const bool incrementalSpectral = spectral && previous != nullptr && previous->spectralMorph_ == static_cast<int>(table.morph) &&
                                     previous->keyHashes_.size() == keyHashes.size() && previous->frames_ == kMaxFrames &&
                                     previous->frameHashes_.size() == static_cast<std::size_t>(kMaxFrames);
    std::vector<Frame> generated;
    if (incrementalSpectral) {
        const int last = table.size() - 1;
        std::vector<char> needed(static_cast<std::size_t>(kMaxFrames), 1);
        for (int p = 0; p < kMaxFrames; ++p) {
            const double at = static_cast<double>(p) / (kMaxFrames - 1) * last;
            const int i = std::min(static_cast<int>(at), std::max(last - 1, 0));
            const int j = std::min(i + 1, last);
            const bool same = keyHashes[static_cast<std::size_t>(i)] == previous->keyHashes_[static_cast<std::size_t>(i)] &&
                              keyHashes[static_cast<std::size_t>(j)] == previous->keyHashes_[static_cast<std::size_t>(j)];
            reusable[static_cast<std::size_t>(p)] = same ? 1 : 0;
            needed[static_cast<std::size_t>(p)] = same ? 0 : 1;
        }
        generated = generatePositions(table, kMaxFrames, needed);
    } else if (spectral) {
        generated = generatePositions(table, kMaxFrames);
    }
    const std::vector<Frame>& frames = spectral ? generated : table.frames;
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
    // The same number of positions as `previous`: start from a copy of its data (one pass, no
    // zeroing) and write only the positions that changed.
    const bool sameLayout = previous != nullptr && previous->frames_ == count && previous->data_.size() == static_cast<std::size_t>(offset) + 16;
    if (sameLayout)
        out->data_ = previous->data_;
    else
        out->data_.assign(static_cast<std::size_t>(offset) + 16, 0.0f);

    out->keyHashes_ = keyHashes;
    out->spectralMorph_ = spectral ? static_cast<int>(table.morph) : -1;
    if (!spectral) {
        out->frameHashes_ = keyHashes; // the positions are the key frames
    } else {
        out->frameHashes_.resize(static_cast<std::size_t>(count));
        for (int f = 0; f < count; ++f)
            out->frameHashes_[static_cast<std::size_t>(f)] = incrementalSpectral && reusable[static_cast<std::size_t>(f)]
                                                                  ? previous->frameHashes_[static_cast<std::size_t>(f)] // not generated: as before
                                                                  : frameHash(frames[static_cast<std::size_t>(f)]);
    }
    const bool reuse = previous != nullptr && previous->data_.size() > 16 && !previous->frameHashes_.empty();

    std::vector<int> synthesize; // the positions whose levels are made from their spectrum
    for (int f = 0; f < count; ++f) {
        if (reuse && f < static_cast<int>(previous->frameHashes_.size()) &&
            previous->frameHashes_[static_cast<std::size_t>(f)] == out->frameHashes_[static_cast<std::size_t>(f)]) {
            if (sameLayout)
                continue; // already in the copied data, its end guard too
            // Unchanged frame: its levels are copied (the level layout is the same for every table).
            for (int l = 0; l < kNumLevels; ++l) {
                const Level& level = out->levels_[static_cast<std::size_t>(l)];
                const Level& old = previous->levels_[static_cast<std::size_t>(l)];
                const float* from = previous->data_.data() + old.offset + f * old.stride;
                float* dest = out->data_.data() + level.offset + f * level.stride;
                std::copy(from, from + level.stride, dest);
                if (f == count - 1)
                    std::copy(dest, dest + level.stride, dest + level.stride);
            }
            continue;
        }
        synthesize.push_back(f);
    }

    // Two positions per inverse transform: their spectra are each conjugate-symmetric, so one goes
    // in as the real part and the other as the imaginary part, and they come out apart.
    const auto& lifts = levelLifts();
    std::array<const dsp::Fft*, kNumLevels> transforms{};
    for (int l = 0; l < kNumLevels; ++l)
        transforms[static_cast<std::size_t>(l)] = &dsp::Fft::forSize(out->levels_[static_cast<std::size_t>(l)].size);
    const std::size_t pairs = (synthesize.size() + 1) / 2;
    // Pairs [first, last) of `synthesize`; each run writes only its own positions.
    const auto synthesizePairs = [&](std::size_t first, std::size_t last) {
        std::array<Complex, kMaxHarmonics + 1> binsA, binsB;
        std::vector<Complex> work(4096);
        for (std::size_t s = first * 2; s < synthesize.size() && s < last * 2; s += 2) {
            const int fa = synthesize[s];
            const int fb = s + 1 < synthesize.size() ? synthesize[s + 1] : -1;
            frameToBins(frames[static_cast<std::size_t>(fa)], binsA);
            if (fb >= 0)
                frameToBins(frames[static_cast<std::size_t>(fb)], binsB);
            else
                binsB.fill(Complex(0.0));
            for (int l = 0; l < kNumLevels; ++l) {
                const Level& level = out->levels_[static_cast<std::size_t>(l)];
                const auto& lift = lifts[static_cast<std::size_t>(l)];
                const int n = level.size;
                const double toLevel = static_cast<double>(n) / kFrameSize;
                std::fill(work.begin(), work.begin() + n, Complex(0.0));
                work[0] = Complex(binsA[0].real() * toLevel, binsB[0].real() * toLevel);
                for (int h = 1; h <= level.harmonics; ++h) { // h < n / 2 at every level: the halves never meet
                    const double gain = lift[static_cast<std::size_t>(h)] * toLevel;
                    Complex a = binsA[static_cast<std::size_t>(h)] * gain, b = binsB[static_cast<std::size_t>(h)] * gain;
                    if (h == kMaxHarmonics) { // the frame's Nyquist bin holds both halves of a cosine
                        a = a.real() * 0.5;
                        b = b.real() * 0.5;
                    }
                    work[static_cast<std::size_t>(h)] = a + Complex(-b.imag(), b.real());              // a + i b
                    work[static_cast<std::size_t>(n - h)] = std::conj(a) + Complex(b.imag(), b.real()); // conj(a) + i conj(b)
                }
                transforms[static_cast<std::size_t>(l)]->inverse(work.data());
                for (const auto& [f, imaginary] : {std::pair{fa, false}, std::pair{fb, true}}) {
                    if (f < 0)
                        continue;
                    float* dest = out->data_.data() + level.offset + f * level.stride;
                    for (int i = 0; i < level.stride; ++i) {
                        const Complex& v = work[static_cast<std::size_t>((i - kGuardBefore + n) % n)];
                        dest[i] = static_cast<float>(imaginary ? v.imag() : v.real());
                    }
                    if (f == count - 1)
                        std::copy(dest, dest + level.stride, dest + level.stride);
                }
            }
        }
    };
    // A table with many positions (a spectral morph: 256) is synthesized on up to four threads;
    // the result is the same as on one, since every position is computed on its own.
    const std::size_t hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t runs = pairs >= 32 ? std::min<std::size_t>({4, hardware, pairs / 16}) : 1;
    std::vector<std::thread> helpers;
    const std::size_t chunk = (pairs + runs - 1) / std::max<std::size_t>(runs, 1);
    std::size_t done = 0;
    for (std::size_t r = 1; r < runs; ++r) {
        try {
            helpers.emplace_back(synthesizePairs, (r - 1) * chunk, std::min(pairs, r * chunk));
            done = std::min(pairs, r * chunk);
        } catch (const std::exception&) { // no thread to be had: the rest is done here
            break;
        }
    }
    synthesizePairs(done, pairs);
    for (std::thread& t : helpers)
        t.join();

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
