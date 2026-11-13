// The wavetable data model (plan 3.3): spectra, morph modes, generated positions, table files
// with the metadata chunk, the playable form with its band-limited levels, and the handoff that
// lets tables change while notes play.
#include "../support/TestUtil.h"
#include "zederia/audio/WavFile.h"
#include "zederia/rt/Handoff.h"
#include "zederia/table/TableData.h"
#include "zederia/table/Wavetable.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <nlohmann/json.hpp>

using namespace zed;
using Catch::Approx;

namespace {

Frame sineFrame(int harmonic, double phase = 0.0, double level = 1.0) {
    Frame f;
    for (int n = 0; n < kFrameSize; ++n)
        f[static_cast<std::size_t>(n)] = static_cast<float>(level * std::sin(2.0 * kPi * harmonic * n / kFrameSize + phase));
    return f;
}

Frame randomFrame(std::uint64_t seed) {
    Rng rng(seed);
    Frame f;
    for (float& s : f)
        s = static_cast<float>(rng.uniform() * 2.0 - 1.0);
    return f;
}

double maxDifference(const Frame& a, const Frame& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max<double>(worst, std::abs(a[i] - b[i]));
    return worst;
}

Wavetable sampleTable() {
    Wavetable t;
    t.name = "Test Table";
    t.morph = MorphMode::Spectral;
    t.frames = {sineFrame(1), sineFrame(2, 0.7, 0.5), randomFrame(3)};
    return t;
}

} // namespace

TEST_CASE("wavetable: a frame and its harmonics convert both ways", "[table]") {
    Harmonics h = analyzeFrame(sineFrame(3));
    CHECK(h.magnitude[3] == Approx(1.0).epsilon(1e-6));
    CHECK(h.phase[3] == Approx(0.0).margin(1e-6));
    for (int k = 1; k <= kMaxHarmonics; ++k)
        if (k != 3)
            REQUIRE(h.magnitude[static_cast<std::size_t>(k)] < 1e-6);
    h = analyzeFrame(sineFrame(5, kPi / 2.0, 0.25)); // a cosine
    CHECK(h.magnitude[5] == Approx(0.25).epsilon(1e-5));
    CHECK(h.phase[5] == Approx(kPi / 2.0).margin(1e-5));

    const Frame noise = randomFrame(11);
    CHECK(maxDifference(synthesizeFrame(analyzeFrame(noise)), noise) < 1e-5);

    // A constant offset is the "dc" part.
    Frame offset = sineFrame(1);
    for (float& s : offset)
        s += 0.3f;
    CHECK(analyzeFrame(offset).dc == Approx(0.3).epsilon(1e-5));
}

TEST_CASE("wavetable: morph modes fill the space between key frames differently", "[table][morph]") {
    const Frame up = sineFrame(1), down = sineFrame(1, kPi); // the same sine, opposite polarity
    // Crossfade: the two cancel halfway.
    CHECK(framePeak(morphFrame(up, down, 0.5, MorphMode::Crossfade)) < 1e-5f);
    // Spectral: the level stays and the phase turns, so nothing cancels.
    const Frame turned = morphFrame(up, down, 0.5, MorphMode::Spectral);
    CHECK(analyzeFrame(turned).magnitude[1] == Approx(1.0).epsilon(1e-4));
    CHECK(std::abs(analyzeFrame(turned).phase[1]) == Approx(kPi / 2.0).margin(1e-3));
    // Aligned: both frames are first rotated to start at zero phase, so they become identical.
    CHECK(maxDifference(morphFrame(up, down, 0.5, MorphMode::SpectralAligned), up) < 1e-4);
    // Zeroed: every harmonic becomes a sine from zero, whatever its phase was.
    Frame mixed = sineFrame(1, 1.1);
    const Frame other = sineFrame(2, -2.0);
    const Harmonics zeroed = analyzeFrame(morphFrame(mixed, other, 0.5, MorphMode::SpectralZero));
    CHECK(zeroed.magnitude[1] == Approx(0.5).epsilon(1e-4));
    CHECK(zeroed.magnitude[2] == Approx(0.5).epsilon(1e-4));
    CHECK(zeroed.phase[1] == Approx(0.0).margin(1e-4));
    CHECK(zeroed.phase[2] == Approx(0.0).margin(1e-4));
    // Levels move in a straight line in every spectral mode.
    const Frame quiet = sineFrame(4, 0.0, 0.2), loud = sineFrame(4, 0.0, 1.0);
    CHECK(analyzeFrame(morphFrame(quiet, loud, 0.25, MorphMode::Spectral)).magnitude[4] == Approx(0.4).epsilon(1e-4));
    // The ends are the key frames themselves.
    CHECK(maxDifference(morphFrame(up, other, 0.0, MorphMode::Spectral), up) < 1e-5);
    CHECK(maxDifference(morphFrame(up, other, 1.0, MorphMode::Spectral), other) < 1e-5);
}

TEST_CASE("wavetable: the full set of positions is generated from the key frames", "[table][morph]") {
    Wavetable t;
    t.frames = {sineFrame(1), sineFrame(2), sineFrame(3)};
    for (const MorphMode mode : {MorphMode::None, MorphMode::Crossfade, MorphMode::Spectral}) {
        t.morph = mode;
        const auto positions = generatePositions(t);
        REQUIRE(positions.size() == 256);
        CHECK(maxDifference(positions.front(), t.frames[0]) < 1e-5);
        CHECK(maxDifference(positions.back(), t.frames[2]) < 1e-5);
        // Position 127.5 of 255 is the middle key frame; the neighbours on either side sit just off it.
        const Harmonics near = analyzeFrame(positions[127]);
        CHECK(near.magnitude[2] == Approx(1.0 - 1.0 / 255.0).epsilon(0.01));
        CHECK(near.magnitude[1] == Approx(1.0 / 255.0).epsilon(0.05));
        CHECK(maxDifference(frameAt(t, 0.5), t.frames[1]) < 1e-5);
    }
    // A stepped table has no in-between frames.
    t.stepped = true;
    CHECK(maxDifference(frameAt(t, 0.2), t.frames[0]) == 0.0);
    CHECK(maxDifference(frameAt(t, 0.3), t.frames[1]) == 0.0);
    CHECK(maxDifference(frameAt(t, 0.8), t.frames[2]) == 0.0);
    // One frame is one position.
    Wavetable single;
    single.frames = {sineFrame(7)};
    CHECK(maxDifference(frameAt(single, 0.9), single.frames[0]) == 0.0);
    CHECK(generatePositions(single, 4).size() == 4);
}

TEST_CASE("wavetable: table files round-trip with their metadata", "[table][wav]") {
    const Wavetable table = sampleTable();
    const auto bytes = encodeWavetable(table);
    Wavetable back;
    std::string error;
    int frameSize = 0;
    REQUIRE(decodeWavetable(bytes, back, error, {}, &frameSize));
    CHECK(frameSize == kFrameSize);
    CHECK(back == table); // 32-bit float: exact

    // The file is a plain mono WAV with the frames end to end and a metadata chunk.
    WavData wav;
    REQUIRE(decodeWav(bytes, wav, error));
    CHECK(wav.channelCount() == 1);
    CHECK(wav.isFloat);
    CHECK(wav.frames() == 3u * kFrameSize);
    REQUIRE(wav.chunks.count("ZEDT") == 1);
    const auto meta = nlohmann::json::parse(wav.chunks["ZEDT"].begin(), wav.chunks["ZEDT"].end());
    CHECK(meta["format"] == "zederia.table");
    CHECK(meta["version"] == 1);
    CHECK(meta["frame_size"] == 2048);
    CHECK(meta["frames"] == 3);
    CHECK(meta["morph"] == "spectral");
    CHECK(meta["stepped"] == false);
    CHECK(meta["name"] == "Test Table");

    // 16 and 24 bit are accepted; they differ only by their rounding.
    for (const auto format : {TableSampleFormat::Pcm16, TableSampleFormat::Pcm24}) {
        Wavetable rounded;
        REQUIRE(decodeWavetable(encodeWavetable(table, format), rounded, error));
        REQUIRE(rounded.size() == 3);
        CHECK(rounded.morph == MorphMode::Spectral);
        CHECK(maxDifference(rounded.frames[1], table.frames[1]) < (format == TableSampleFormat::Pcm16 ? 4e-5 : 2e-7));
    }
    // Stepped and every morph mode survive.
    for (const MorphMode mode : {MorphMode::None, MorphMode::Crossfade, MorphMode::Spectral, MorphMode::SpectralAligned,
                                 MorphMode::SpectralZero}) {
        Wavetable t = table;
        t.morph = mode;
        t.stepped = true;
        Wavetable r;
        REQUIRE(decodeWavetable(encodeWavetable(t), r, error));
        CHECK(r.morph == mode);
        CHECK(r.stepped);
        MorphMode parsed = MorphMode::None;
        CHECK(morphModeFromName(morphModeName(mode), parsed));
        CHECK(parsed == mode);
    }
}

TEST_CASE("wavetable: other frame sizes are detected or chosen on import", "[table][wav]") {
    std::string error;
    const auto fileOf = [](const std::vector<float>& samples, const char* marker = nullptr) {
        WavData wav;
        wav.sampleRate = 44100.0;
        wav.channels.push_back(samples);
        if (marker != nullptr)
            wav.chunks["clm "].assign(marker, marker + std::strlen(marker));
        return encodeWavData(wav, WavSampleFormat::Float32);
    };
    const auto cycles = [](int count, int size) {
        std::vector<float> x(static_cast<std::size_t>(count * size));
        for (int c = 0; c < count; ++c)
            for (int n = 0; n < size; ++n)
                x[static_cast<std::size_t>(c * size + n)] = static_cast<float>(std::sin(2.0 * kPi * (c + 1) * n / size));
        return x;
    };
    Wavetable t;
    int used = 0;

    // Three frames of 1024 samples: detected, and stretched to 2048.
    REQUIRE(decodeWavetable(fileOf(cycles(3, 1024)), t, error, {}, &used));
    CHECK(used == 1024);
    REQUIRE(t.size() == 3);
    CHECK(maxDifference(t.frames[2], sineFrame(3)) < 1e-4);

    // A single cycle of an unusual length.
    REQUIRE(decodeWavetable(fileOf(cycles(1, 600)), t, error, {}, &used));
    CHECK(used == 600);
    REQUIRE(t.size() == 1);
    CHECK(maxDifference(t.frames[0], sineFrame(1)) < 1e-4);

    // Ambiguous lengths follow the 2048 convention unless a size is chosen ...
    REQUIRE(decodeWavetable(fileOf(cycles(4, 1024)), t, error, {}, &used));
    CHECK(used == 2048);
    CHECK(t.size() == 2);
    TableReadOptions chosen;
    chosen.frameSize = 1024;
    REQUIRE(decodeWavetable(fileOf(cycles(4, 1024)), t, error, chosen, &used));
    CHECK(t.size() == 4);
    CHECK(maxDifference(t.frames[3], sineFrame(4)) < 1e-4);
    // ... or the file says so in a marker chunk.
    REQUIRE(decodeWavetable(fileOf(cycles(4, 1024), "<!>1024 00000000 wavetable"), t, error, {}, &used));
    CHECK(used == 1024);
    CHECK(t.size() == 4);

    // Stereo files: mixed, or one channel.
    WavData stereo;
    stereo.channels = {cycles(1, 2048), std::vector<float>(2048, 0.0f)};
    const auto stereoBytes = encodeWavData(stereo, WavSampleFormat::Pcm24);
    REQUIRE(decodeWavetable(stereoBytes, t, error));
    CHECK(framePeak(t.frames[0]) == Approx(0.5f).epsilon(0.001));
    TableReadOptions leftOnly;
    leftOnly.channel = 0;
    REQUIRE(decodeWavetable(stereoBytes, t, error, leftOnly));
    CHECK(framePeak(t.frames[0]) == Approx(1.0f).epsilon(0.001));
    TableReadOptions third;
    third.channel = 2;
    CHECK_FALSE(decodeWavetable(stereoBytes, t, error, third));
    CHECK(error.find("channel") != std::string::npos);

    // Problems are reported in plain words.
    TableReadOptions standard;
    standard.frameSize = 2048;
    CHECK_FALSE(decodeWavetable(fileOf(cycles(300, 2048)), t, error, standard));
    CHECK(error.find("at most 256") != std::string::npos);
    CHECK_FALSE(decodeWavetable(fileOf(std::vector<float>(20000 + 13, 0.1f)), t, error));
    CHECK(error.find("frame size") != std::string::npos);
    const std::uint8_t junk[] = {'n', 'o', 'p', 'e'};
    CHECK_FALSE(decodeWavetable(junk, t, error));
    CHECK(error.find("not a WAV") != std::string::npos);
}

TEST_CASE("wavetable: damaged table files are refused, never a crash", "[table][wav][fuzz]") {
    const auto good = encodeWavetable(sampleTable());
    Rng rng(77);
    int loaded = 0;
    for (int i = 0; i < 3000; ++i) {
        auto bytes = good;
        const int edits = 1 + static_cast<int>(rng.next() % 6);
        for (int e = 0; e < edits; ++e) {
            const std::size_t at = static_cast<std::size_t>(rng.next() % 200); // headers and metadata are at the front
            switch (rng.next() % 4) {
            case 0: bytes[std::min(at, bytes.size() - 1)] = static_cast<std::uint8_t>(rng.next()); break;
            case 1: bytes.resize(static_cast<std::size_t>(rng.next() % bytes.size())); break;
            case 2: bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(std::min(at, bytes.size())), static_cast<std::uint8_t>(rng.next())); break;
            default: bytes[std::min(at, bytes.size() - 1)] ^= 0x80; break;
            }
            if (bytes.empty())
                bytes.push_back(0);
        }
        Wavetable t;
        std::string error;
        if (decodeWavetable(bytes, t, error)) {
            ++loaded;
            REQUIRE(t.size() >= 1);
            REQUIRE(t.size() <= kMaxFrames);
            for (const Frame& f : t.frames)
                for (const float s : f)
                    REQUIRE(std::isfinite(s));
        } else {
            REQUIRE_FALSE(error.empty());
        }
    }
    CHECK(loaded > 0);
}

TEST_CASE("wavetable: one cycle of any length is stretched exactly", "[table]") {
    for (const int length : {2, 3, 64, 600, 2048, 3001, 4096, 9973}) {
        std::vector<float> cycle(static_cast<std::size_t>(length));
        for (int n = 0; n < length; ++n)
            cycle[static_cast<std::size_t>(n)] = static_cast<float>(0.8 * std::sin(2.0 * kPi * n / length + 0.4));
        INFO("length " << length);
        if (length > 2)
            CHECK(maxDifference(resampleCycle(cycle), sineFrame(1, 0.4, 0.8)) < 2e-4);
        else
            CHECK(framePeak(resampleCycle(cycle)) < 2.0f); // two samples hold only an offset and a top harmonic
    }
    CHECK(framePeak(resampleCycle({})) == 0.0f);
}

TEST_CASE("table data: levels, frames and sharing", "[table][data]") {
    // Which level plays at which pitch: the one with the most harmonics that all stay at or
    // below half the sample rate.
    CHECK(TableData::levelFor(0.0f) == 0);
    CHECK(TableData::levelFor(0.5f / 1024.0f) == 0);
    CHECK(TableData::levelFor(0.5f / 1024.0f * 1.001f) == 1);
    CHECK(TableData::levelFor(0.49f) == TableData::kNumLevels - 1);
    for (float inc = 1e-5f; inc < 0.5f; inc *= 1.07f) {
        const int level = TableData::levelFor(inc);
        const int harmonics = TableData::levelHarmonics(level);
        REQUIRE(harmonics * inc <= 0.5f * 1.0001f);
        if (level > 0) // the next richer level would not fit
            REQUIRE(TableData::levelHarmonics(level - 1) * inc > 0.5f);
        // Three levels per octave: the lowest harmonic a level leaves out lies above 79 % of
        // half the sample rate (17.4 kHz at 44.1 kHz), whatever the pitch.
        if (level > 0)
            REQUIRE((harmonics + 1) * inc > 0.5f * 0.79f);
    }
    CHECK(TableData::levelHarmonics(0) == 1024);
    CHECK(TableData::levelHarmonics(TableData::kNumLevels - 1) == 1);

    Wavetable t;
    t.frames = {sineFrame(1), sineFrame(2), sineFrame(3)};
    CHECK(TableData::build(t)->frames() == 3); // key frames only: the oscillator blends neighbours itself
    t.morph = MorphMode::Crossfade;
    CHECK(TableData::build(t)->frames() == 3);
    t.morph = MorphMode::Spectral;
    const auto spectral = TableData::build(t);
    CHECK(spectral->frames() == 256); // generated positions
    CHECK_FALSE(spectral->stepped());
    t.stepped = true;
    CHECK(TableData::build(t)->frames() == 3);
    CHECK(TableData::build(t)->stepped());

    // The same table loaded twice is one object.
    t.stepped = false;
    CHECK(TableData::build(t).get() == spectral.get());
    Wavetable different = t;
    different.frames[1][100] += 0.01f;
    CHECK(TableData::build(different).get() != spectral.get());

    // The built-in table: sawtooth, square, triangle, sine.
    const auto builtin = TableData::builtin();
    CHECK(builtin->frames() == 4);
    CHECK(builtin->name() == "Basic Shapes");
    CHECK(TableData::builtin().get() == builtin.get());
    const Wavetable shapes = TableData::builtinWavetable();
    CHECK(analyzeFrame(shapes.frames[0]).magnitude[2] / analyzeFrame(shapes.frames[0]).magnitude[1] == Approx(0.5).epsilon(1e-3));
    CHECK(analyzeFrame(shapes.frames[1]).magnitude[2] < 1e-6); // a square has no even harmonics
    CHECK(analyzeFrame(shapes.frames[2]).magnitude[3] / analyzeFrame(shapes.frames[2]).magnitude[1] == Approx(1.0 / 9.0).epsilon(1e-3));
    CHECK(analyzeFrame(shapes.frames[3]).magnitude[2] < 1e-6);
    for (const Frame& f : shapes.frames)
        CHECK(framePeak(f) == Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("table data: every level plays a flat spectrum up to its limit and nothing above", "[table][data]") {
    // A frame with all harmonics at the same level. Reading one cycle from each level through
    // the spline must give those harmonics back unchanged (the spline's roll-off is undone in
    // the stored data) and none above the level's limit.
    Harmonics flat;
    for (int k = 1; k < kMaxHarmonics; ++k) {
        flat.magnitude[static_cast<std::size_t>(k)] = 1.0 / kMaxHarmonics;
        flat.phase[static_cast<std::size_t>(k)] = 0.37 * k * k; // scattered phases keep the peak low
    }
    Wavetable t;
    t.frames = {synthesizeFrame(flat)};
    const auto data = TableData::build(t);
    for (int level = 0; level < TableData::kNumLevels; level += 3) {
        const int limit = TableData::levelHarmonics(level);
        Frame cycle;
        for (int n = 0; n < kFrameSize; ++n)
            cycle[static_cast<std::size_t>(n)] = static_cast<float>(data->read(level, 0.0, static_cast<double>(n) / kFrameSize));
        const Harmonics h = analyzeFrame(cycle);
        double worstKept = 0.0, worstRemoved = 0.0;
        for (int k = 1; k < kMaxHarmonics; ++k) {
            const double ratio = h.magnitude[static_cast<std::size_t>(k)] * kMaxHarmonics;
            if (k <= limit)
                worstKept = std::max(worstKept, std::abs(20.0 * std::log10(ratio)));
            else
                worstRemoved = std::max(worstRemoved, ratio);
        }
        INFO("level " << level << " (" << limit << " harmonics, " << data->level(level).size << " samples)");
        // In decibels. Reading only 2048 points folds each harmonic's own spline image back
        // onto it, which accounts for up to 0.012 dB here; the played spectrum is flatter still.
        CHECK(worstKept < 0.02);
        CHECK(worstRemoved < 2e-3);  // -54 dB against one harmonic: only the spline's images are left
    }
}

TEST_CASE("handoff: objects change hands without the audio side freeing anything", "[rt][table]") {
    rt::Handoff<TableData> slot;
    CHECK(slot.current() == nullptr);
    CHECK_FALSE(slot.take());

    Wavetable a, b, c;
    a.frames = {sineFrame(1)};
    b.frames = {sineFrame(2)};
    c.frames = {sineFrame(3)};
    auto first = TableData::build(a);
    const std::weak_ptr<const TableData> firstAlive = first;
    slot.set(first);
    first.reset();
    CHECK(slot.hasPending());
    CHECK(slot.take());
    CHECK(slot.current() == firstAlive.lock().get());
    CHECK(slot.previous() == nullptr);

    slot.set(TableData::build(b));
    CHECK(slot.latest()->frames() == 1);
    CHECK(slot.take());
    CHECK(slot.previous() == firstAlive.lock().get()); // still alive: a fade may be reading it
    CHECK_FALSE(slot.take());                          // nothing new, and the previous one is not retired yet
    CHECK(slot.retirePrevious());
    CHECK(slot.previous() == nullptr);
    CHECK_FALSE(firstAlive.expired()); // retired, not freed: freeing is for the other side
    slot.collect();
    CHECK(firstAlive.expired());

    // An offer that was never taken is replaced and freed by the offering side.
    auto unused = TableData::build(c);
    const std::weak_ptr<const TableData> unusedAlive = unused;
    slot.set(std::move(unused));
    slot.set(TableData::build(a));
    CHECK(unusedAlive.expired());
    CHECK(slot.take());
    CHECK(slot.retirePrevious());
}
