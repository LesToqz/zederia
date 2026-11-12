// Import (plan 3.10, 6.1): single cycles, pitch-tracked and fixed-length slicing on synthetic
// signals of known pitch, spectral snapshots, images, the stereo options and the suggestion the
// import wizard shows.
#include "../support/TestUtil.h"
#include "zederia/table/Import.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Image.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;

namespace {

// A tone with a few harmonics whose pitch moves from `fromHz` to `toHz` over the clip.
AudioClip tone(double fromHz, double toHz, double seconds, double sampleRate = 48000.0, double phase = 0.0) {
    AudioClip clip;
    clip.sampleRate = sampleRate;
    const auto count = static_cast<std::size_t>(seconds * sampleRate);
    clip.samples.resize(count);
    double angle = phase;
    for (std::size_t i = 0; i < count; ++i) {
        const double hz = fromHz * std::pow(toHz / fromHz, static_cast<double>(i) / static_cast<double>(count));
        clip.samples[i] = static_cast<float>(0.6 * std::sin(angle) + 0.25 * std::sin(2.0 * angle + 0.5) + 0.1 * std::sin(3.0 * angle + 1.1));
        angle += 2.0 * kPi * hz / sampleRate;
    }
    return clip;
}

Harmonics spectrumOf(const Frame& f) { return analyzeFrame(f); }

std::vector<std::uint8_t> load(const std::string& name) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    REQUIRE(readFile(zedtest::dataPath("images/" + name), bytes, error));
    return bytes;
}

// A 24-bit BMP built in memory.
std::vector<std::uint8_t> makeBmp(int width, int height, float (*value)(int, int)) {
    const int rowBytes = (width * 3 + 3) / 4 * 4;
    std::vector<std::uint8_t> b(static_cast<std::size_t>(54 + rowBytes * height), 0);
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int k = 0; k < 4; ++k)
            b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
    };
    b[0] = 'B';
    b[1] = 'M';
    put32(2, static_cast<std::uint32_t>(b.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(width));
    put32(22, static_cast<std::uint32_t>(height)); // positive: rows are stored bottom up
    b[26] = 1;
    b[28] = 24;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto grey = static_cast<std::uint8_t>(std::lround(value(x, y) * 255.0f));
            const std::size_t at = static_cast<std::size_t>(54 + (height - 1 - y) * rowBytes + x * 3);
            b[at] = b[at + 1] = b[at + 2] = grey;
        }
    }
    return b;
}

} // namespace

TEST_CASE("import: single-cycle files become frames", "[import]") {
    std::vector<AudioClip> cycles;
    for (const int length : {600, 2048, 1337}) {
        AudioClip c;
        c.sampleRate = 44100.0;
        c.samples.resize(static_cast<std::size_t>(length));
        const int harmonic = static_cast<int>(cycles.size()) + 1;
        for (int n = 0; n < length; ++n)
            c.samples[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(2.0 * kPi * harmonic * n / length));
        cycles.push_back(c);
    }
    Wavetable table;
    std::string error;
    REQUIRE(importSingleCycles(cycles, table, error));
    REQUIRE(table.size() == 3); // several files fill several frames, in order
    for (int f = 0; f < 3; ++f) {
        const Harmonics h = spectrumOf(table.frames[static_cast<std::size_t>(f)]);
        CHECK(h.magnitude[static_cast<std::size_t>(f + 1)] == Approx(1.0).epsilon(1e-3));
    }
    CHECK_FALSE(importSingleCycles({}, table, error));
    CHECK(error.find("no single-cycle") != std::string::npos);
    std::vector<AudioClip> tooMany(257, cycles[0]);
    CHECK_FALSE(importSingleCycles(tooMany, table, error));
    CHECK(error.find("at most 256") != std::string::npos);
    std::vector<AudioClip> empty(1);
    CHECK_FALSE(importSingleCycles(empty, table, error));
}

TEST_CASE("import: the pitch of a recording is found", "[import][pitch]") {
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        for (const double hz : {41.2, 110.0, 261.63, 440.0, 987.77, 2093.0}) {
            const PitchEstimate p = detectPitch(tone(hz, hz, 1.0, rate));
            INFO(hz << " Hz at " << rate);
            CHECK(p.hz == Approx(hz).epsilon(0.002));
            CHECK(p.periodSamples == Approx(rate / hz).epsilon(0.002));
            CHECK(p.confidence > 0.9);
        }
    }
    // The fundamental is found even when it is weaker than a harmonic.
    AudioClip weak;
    weak.samples.resize(48000);
    for (std::size_t i = 0; i < weak.samples.size(); ++i) {
        const double x = 2.0 * kPi * 200.0 * static_cast<double>(i) / 48000.0;
        weak.samples[i] = static_cast<float>(0.2 * std::sin(x) + 0.7 * std::sin(2.0 * x) + 0.3 * std::sin(3.0 * x));
    }
    CHECK(detectPitch(weak).hz == Approx(200.0).epsilon(0.003));

    // Noise and silence have no pitch to speak of.
    AudioClip noise;
    Rng rng(3);
    noise.samples.resize(48000);
    for (float& s : noise.samples)
        s = static_cast<float>(rng.uniform() * 2.0 - 1.0);
    CHECK(detectPitch(noise).confidence < 0.5);
    AudioClip silence;
    silence.samples.assign(48000, 0.0f);
    CHECK(detectPitch(silence).confidence == 0.0);
    CHECK(detectPitch(AudioClip{}).confidence == 0.0);

    // A note name gives a cycle length.
    CHECK(cycleSamplesForNote(69, 48000.0) == Approx(48000.0 / 440.0));
    CHECK(cycleSamplesForNote(57, 44100.0) == Approx(44100.0 / 220.0));
    CHECK(cycleSamplesForNote(69, 48000.0, 432.0) == Approx(48000.0 / 432.0));
}

TEST_CASE("import: pitch-tracked slicing follows a moving pitch", "[import][pitch]") {
    // A glide from 100 to 150 Hz: a fixed length would drift across the cycles; tracking the
    // pitch keeps every frame one clean cycle.
    const AudioClip clip = tone(100.0, 150.0, 2.0);
    Wavetable table;
    std::string error;
    ImportReport report;
    SliceOptions options;
    REQUIRE(importPitchTracked(clip, options, table, error, &report));
    CHECK(report.detectedHz > 95.0);
    CHECK(report.detectedHz < 155.0);
    CHECK(report.cyclesFound == Approx(245).margin(3)); // the glide holds 246.6 cycles; the last ones are left out
    CHECK(report.framesMade == table.size());
    CHECK(table.size() >= 240);
    CHECK(report.markers.size() == static_cast<std::size_t>(report.cyclesFound) + 1);
    // The cycles get shorter as the pitch rises.
    const double firstLength = report.markers[1] - report.markers[0];
    const double lastLength = report.markers.back() - report.markers[report.markers.size() - 2];
    CHECK(firstLength == Approx(480.0).margin(4.0));
    CHECK(lastLength == Approx(320.0).margin(4.0));
    // Every frame holds exactly the tone's harmonics: 0.6, 0.25 and 0.1, and next to nothing else.
    double worstLevel = 0.0, worstLeak = 0.0;
    for (const Frame& f : table.frames) {
        const Harmonics h = spectrumOf(f);
        worstLevel = std::max({worstLevel, std::abs(h.magnitude[1] - 0.6), std::abs(h.magnitude[2] - 0.25), std::abs(h.magnitude[3] - 0.1)});
        for (int k = 4; k < 40; ++k)
            worstLeak = std::max(worstLeak, h.magnitude[static_cast<std::size_t>(k)]);
    }
    CHECK(worstLevel < 0.02);
    CHECK(worstLeak < 0.02);
    // The cuts stay at the same point of the wave: the fundamental's phase barely moves.
    double phaseSpread = 0.0;
    const double reference = spectrumOf(table.frames[5]).phase[1];
    for (std::size_t f = 5; f < table.frames.size(); f += 10)
        phaseSpread = std::max(phaseSpread, std::abs(spectrumOf(table.frames[f]).phase[1] - reference));
    CHECK(phaseSpread < 0.5);

    // Snapping to zero crossings puts every cut on an upward crossing.
    options.snapToZeroCrossings = true;
    REQUIRE(importPitchTracked(clip, options, table, error, &report));
    for (std::size_t m = 0; m + 1 < report.markers.size(); m += 7) {
        const auto i = static_cast<std::size_t>(report.markers[m]);
        CHECK(clip.samples[i] <= 0.01f);
        CHECK(clip.samples[i + 1] >= -0.01f);
    }
    for (std::size_t f = 0; f < table.frames.size(); f += 9)
        CHECK(std::abs(table.frames[f][0]) < 0.02f); // so every frame starts at zero

    // More cycles than frames: pick evenly, or average groups.
    options = SliceOptions{};
    options.maxFrames = 16;
    REQUIRE(importPitchTracked(clip, options, table, error, &report));
    CHECK(table.size() == 16);
    CHECK(report.cyclesFound > 200);
    options.reduction = CycleReduction::Average;
    REQUIRE(importPitchTracked(clip, options, table, error));
    CHECK(table.size() == 16);
    CHECK(spectrumOf(table.frames[8]).magnitude[1] == Approx(0.6).margin(0.03));

    // No pitch, no tracking: the message says what to do instead.
    AudioClip noise;
    Rng rng(8);
    noise.samples.resize(24000);
    for (float& s : noise.samples)
        s = static_cast<float>(rng.uniform() * 2.0 - 1.0);
    CHECK_FALSE(importPitchTracked(noise, options, table, error));
    CHECK(error.find("no clear pitch") != std::string::npos);
    CHECK(error.find("spectral") != std::string::npos);
}

TEST_CASE("import: fixed cycle length, detected, typed or from a note", "[import][pitch]") {
    const double rate = 48000.0, hz = 187.5; // 256 samples per cycle
    const AudioClip clip = tone(hz, hz, 1.0, rate);
    Wavetable table;
    std::string error;
    ImportReport report;

    SliceOptions automatic; // the average cycle length is detected
    REQUIRE(importFixedCycles(clip, automatic, table, error, &report));
    CHECK(report.cycleSamples == Approx(256.0).epsilon(0.001));
    CHECK(report.detectedHz == Approx(hz).epsilon(0.001));
    CHECK(report.cyclesFound == Approx(187).margin(1));
    CHECK(table.size() == report.framesMade);
    CHECK(spectrumOf(table.frames[100]).magnitude[1] == Approx(0.6).epsilon(0.01));

    SliceOptions typed;
    typed.cycleSamples = 256.0;
    REQUIRE(importFixedCycles(clip, typed, table, error, &report));
    CHECK(report.cyclesFound == 187);
    // With the exact length every frame is identical.
    double difference = 0.0;
    for (std::size_t i = 0; i < kFrameSize; ++i)
        difference = std::max<double>(difference, std::abs(table.frames[3][i] - table.frames[150][i]));
    CHECK(difference < 1e-4);
    const Harmonics h = spectrumOf(table.frames[42]);
    CHECK(h.magnitude[1] == Approx(0.6).epsilon(1e-3));
    CHECK(h.magnitude[2] == Approx(0.25).epsilon(1e-3));
    CHECK(h.magnitude[3] == Approx(0.1).epsilon(1e-3));

    // Twice the length puts two cycles in a frame: the tone moves to the even harmonics.
    typed.cycleSamples = 512.0;
    REQUIRE(importFixedCycles(clip, typed, table, error));
    CHECK(spectrumOf(table.frames[10]).magnitude[2] == Approx(0.6).epsilon(1e-3));
    CHECK(spectrumOf(table.frames[10]).magnitude[1] < 1e-3);

    // As a note name: A3 at 48 kHz is 218.18 samples, a length between samples.
    const AudioClip a3 = tone(220.0, 220.0, 1.0, rate);
    SliceOptions byNote;
    byNote.cycleSamples = cycleSamplesForNote(57, rate);
    byNote.maxFrames = 64;
    REQUIRE(importFixedCycles(a3, byNote, table, error, &report));
    CHECK(table.size() == 64);
    CHECK(report.cyclesFound == 220);
    for (const std::size_t f : {std::size_t{0}, std::size_t{31}, std::size_t{63}})
        CHECK(spectrumOf(table.frames[f]).magnitude[1] == Approx(0.6).epsilon(0.005));

    typed.cycleSamples = 1.0;
    CHECK_FALSE(importFixedCycles(clip, typed, table, error));
    typed.cycleSamples = 1.0e7;
    CHECK_FALSE(importFixedCycles(clip, typed, table, error));
    CHECK(error.find("shorter than one cycle") != std::string::npos);
}

TEST_CASE("import: spectral snapshots", "[import][spectral]") {
    // A tone that sits on bin 8 of a 1024-point transform for the first half and on bin 20 for
    // the second: the snapshots show harmonic 8, then harmonic 20.
    const double rate = 48000.0;
    AudioClip clip;
    clip.sampleRate = rate;
    clip.samples.resize(48000);
    for (std::size_t i = 0; i < clip.samples.size(); ++i) {
        const double bin = i < 24000 ? 8.0 : 20.0;
        clip.samples[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * bin * static_cast<double>(i) / 1024.0));
    }
    Wavetable table;
    std::string error;
    SnapshotOptions options;
    options.fftSize = 1024;
    options.frames = 9;
    REQUIRE(importSpectralSnapshots(clip, options, table, error));
    REQUIRE(table.size() == 9);
    // The analysis window spreads a line over its two neighbours at half the height.
    const Harmonics early = spectrumOf(table.frames[1]), late = spectrumOf(table.frames[7]);
    CHECK(sortValue(table.frames[1], SortKey::StrongestPartial) == 8);
    CHECK(early.magnitude[7] / early.magnitude[8] == Approx(0.5).epsilon(0.01));
    CHECK(early.magnitude[9] / early.magnitude[8] == Approx(0.5).epsilon(0.01));
    CHECK(early.magnitude[20] < early.magnitude[8] * 1e-4);
    CHECK(sortValue(table.frames[7], SortKey::StrongestPartial) == 20);
    CHECK(late.magnitude[8] < late.magnitude[20] * 1e-4);
    CHECK(early.phase[8] == Approx(0.0).margin(1e-6)); // phases zeroed by default
    float loudest = 0.0f;
    for (const Frame& f : table.frames)
        loudest = std::max(loudest, framePeak(f));
    CHECK(loudest == Approx(1.0f).epsilon(1e-4)); // the loudest frame is normalized to a peak of 1

    // Phase handling: original, zeroed or random (seeded).
    options.phase = SnapshotPhase::Random;
    Wavetable random1, random2, random3;
    REQUIRE(importSpectralSnapshots(clip, options, random1, error));
    REQUIRE(importSpectralSnapshots(clip, options, random2, error));
    options.seed = 5;
    REQUIRE(importSpectralSnapshots(clip, options, random3, error));
    CHECK(random1 == random2);
    CHECK_FALSE(random1 == random3);
    const Harmonics scattered = spectrumOf(random1.frames[1]);
    CHECK(scattered.magnitude[7] / scattered.magnitude[8] == Approx(0.5).epsilon(0.01)); // same levels, other phases
    CHECK(std::abs(scattered.phase[8]) > 1e-3);
    options.phase = SnapshotPhase::Original;
    Wavetable original;
    REQUIRE(importSpectralSnapshots(clip, options, original, error));
    CHECK(std::abs(spectrumOf(original.frames[1]).phase[8]) > 0.01); // whatever the recording had at that moment

    // Every allowed size works; smaller sizes give fewer harmonics.
    for (const int size : {256, 512, 1024, 2048}) {
        options.fftSize = size;
        REQUIRE(importSpectralSnapshots(clip, options, table, error));
        const Harmonics h = spectrumOf(table.frames[0]);
        const int expected = 8 * size / 1024;
        INFO("size " << size);
        CHECK(sortValue(table.frames[0], SortKey::StrongestPartial) == expected);
        for (int k = size / 2; k < kMaxHarmonics; ++k)
            REQUIRE(h.magnitude[static_cast<std::size_t>(k)] < 1e-6);
    }
    options.fftSize = 300;
    CHECK_FALSE(importSpectralSnapshots(clip, options, table, error));
    CHECK(error.find("256, 512, 1024 or 2048") != std::string::npos);
    options.fftSize = 2048;
    CHECK_FALSE(importSpectralSnapshots(AudioClip{}, options, table, error));
    // A clip shorter than one snapshot still gives a table.
    AudioClip tiny;
    tiny.samples.assign(100, 0.5f);
    CHECK(importSpectralSnapshots(tiny, options, table, error));
}

TEST_CASE("import: stereo files give left, right or mid", "[import][stereo]") {
    WavData wav;
    wav.sampleRate = 44100.0;
    wav.channels = {std::vector<float>(100, 0.8f), std::vector<float>(100, -0.2f)};
    CHECK(clipFromWav(wav, StereoChoice::Left).samples[50] == 0.8f);
    CHECK(clipFromWav(wav, StereoChoice::Right).samples[50] == -0.2f);
    CHECK(clipFromWav(wav, StereoChoice::Mid).samples[50] == Approx(0.3f));
    CHECK(clipFromWav(wav, StereoChoice::Mid).sampleRate == 44100.0);
    // Left to oscillator 1 and right to oscillator 2 is two imports of the same file.
    wav.channels.pop_back();
    CHECK(clipFromWav(wav, StereoChoice::Right).samples[50] == 0.8f); // mono: the same clip either way
    CHECK(clipFromWav(WavData{}, StereoChoice::Left).samples.empty());
}

TEST_CASE("import: images become tables", "[import][image]") {
    std::string error;
    ImageData image;

    SECTION("PNG in every colour type, with every row filter") {
        REQUIRE(decodeImage(load("gradient_rgb.png"), image, error));
        CHECK(image.width == 64);
        CHECK(image.height == 32);
        for (int y = 0; y < 32; y += 5) {
            for (int x = 0; x < 64; x += 7) {
                const int detail = ((x * 73 + y * 151) ^ (x * y)) & 15;
                const double expected = (0.2126 * ((x * 4 + detail) & 255) + 0.7152 * (y * 8) + 0.0722 * (((x + y) * 2) & 255)) / 255.0;
                REQUIRE(image.at(x, y) == Approx(expected).margin(1e-5));
            }
        }
        REQUIRE(decodeImage(load("ramp_grey16.png"), image, error));
        CHECK(image.width == 32);
        CHECK(image.at(0, 0) == 0.0f);
        CHECK(image.at(31, 0) == Approx(31.0 * 2114.0 / 65535.0).epsilon(1e-5));
        CHECK(image.at(10, 7) > image.at(10, 0));
        REQUIRE(decodeImage(load("alpha_rgba.png"), image, error));
        CHECK(image.at(0, 0) == 0.0f); // transparent pixels are dark
        CHECK(image.at(15, 3) == Approx(1.0f));
        CHECK(image.at(5, 1) == Approx(85.0 / 255.0).epsilon(1e-4));
        REQUIRE(decodeImage(load("steps_palette.png"), image, error));
        CHECK(image.width == 8);
        CHECK(image.at(7, 1) == Approx(252.0 / 255.0).epsilon(1e-4));
        CHECK(image.at(2, 0) == Approx(72.0 / 255.0).epsilon(1e-4));
        REQUIRE(decodeImage(load("grey_alpha.png"), image, error));
        CHECK(image.at(3, 0) == Approx(200.0 / 255.0).epsilon(1e-4));
        CHECK(image.at(1, 0) == Approx(200.0 / 255.0 * 85.0 / 255.0).epsilon(1e-4));
    }
    SECTION("BMP and PGM/PPM") {
        const auto bmp = makeBmp(5, 3, [](int x, int y) { return (x + y * 5) / 14.0f; });
        REQUIRE(decodeImage(bmp, image, error));
        CHECK(image.width == 5);
        CHECK(image.height == 3);
        CHECK(image.at(0, 0) == 0.0f);          // the top row comes first although the file stores it last
        CHECK(image.at(4, 2) == Approx(1.0f));
        CHECK(image.at(2, 1) == Approx(7.0 / 14.0).margin(0.003));

        const std::string pgm = std::string("P5\n# a comment\n3 2\n255\n") + std::string("\x00\x80\xff\x40\x20\x10", 6);
        REQUIRE(decodeImage({reinterpret_cast<const std::uint8_t*>(pgm.data()), pgm.size()}, image, error));
        CHECK(image.width == 3);
        CHECK(image.at(2, 0) == 1.0f);
        CHECK(image.at(0, 1) == Approx(64.0 / 255.0));
        const std::string ppm = std::string("P6 1 1 255\n") + std::string("\xff\x00\x00", 3);
        REQUIRE(decodeImage({reinterpret_cast<const std::uint8_t*>(ppm.data()), ppm.size()}, image, error));
        CHECK(image.at(0, 0) == Approx(0.2126f)); // pure red
    }
    SECTION("rows become frames: brightness as amplitude") {
        // Three rows: black, a left-to-right ramp, white.
        const auto bmp = makeBmp(256, 3, [](int x, int y) { return y == 0 ? 0.0f : y == 2 ? 1.0f : x / 255.0f; });
        REQUIRE(decodeImage(bmp, image, error));
        Wavetable table;
        REQUIRE(importImage(image, ImageMode::Amplitude, kMaxFrames, table, error));
        REQUIRE(table.size() == 3);
        CHECK(table.frames[0][1000] == Approx(-1.0f).margin(1e-3)); // dark is -1
        CHECK(table.frames[2][1000] == Approx(1.0f).margin(1e-3));  // bright is +1
        CHECK(table.frames[1][1024] == Approx(0.0f).margin(0.02));  // the ramp passes zero in the middle
        CHECK(table.frames[1][400] < table.frames[1][1600]);
    }
    SECTION("rows become frames: brightness as harmonic level") {
        // Row y lights pixel y only: frame y is harmonic y + 1.
        const auto bmp = makeBmp(16, 4, [](int x, int y) { return x == y ? 1.0f : 0.0f; });
        REQUIRE(decodeImage(bmp, image, error));
        Wavetable table;
        REQUIRE(importImage(image, ImageMode::Harmonics, kMaxFrames, table, error));
        REQUIRE(table.size() == 4);
        for (int f = 0; f < 4; ++f) {
            const Harmonics h = spectrumOf(table.frames[static_cast<std::size_t>(f)]);
            CHECK(h.magnitude[static_cast<std::size_t>(f + 1)] == Approx(1.0).epsilon(1e-4));
            CHECK(h.magnitude[static_cast<std::size_t>(f + 2)] < 1e-6);
        }
        // More rows than frames are averaged down.
        REQUIRE(importImage(image, ImageMode::Harmonics, 2, table, error));
        REQUIRE(table.size() == 2);
        const Harmonics h = spectrumOf(table.frames[0]);
        CHECK(h.magnitude[1] == Approx(h.magnitude[2]).epsilon(1e-4)); // rows 0 and 1 together
        CHECK(h.magnitude[3] < 1e-6);
    }
    SECTION("damaged and unknown files are refused, never a crash") {
        const std::uint8_t text[] = {'h', 'e', 'l', 'l', 'o'};
        CHECK_FALSE(decodeImage(text, image, error));
        CHECK(error.find("not a PNG, BMP") != std::string::npos);
        CHECK_FALSE(decodeImage({}, image, error));
        Wavetable unused;
        CHECK_FALSE(importImage(ImageData{}, ImageMode::Amplitude, 8, unused, error));

        const auto good = load("gradient_rgb.png");
        Rng rng(31);
        int decoded = 0;
        for (int i = 0; i < 4000; ++i) {
            auto bytes = good;
            const int edits = 1 + static_cast<int>(rng.next() % 4);
            for (int e = 0; e < edits; ++e) {
                const std::size_t at = static_cast<std::size_t>(rng.next() % bytes.size());
                if (rng.next() % 5 == 0)
                    bytes.resize(at + 1);
                else
                    bytes[at] = static_cast<std::uint8_t>(rng.next());
            }
            ImageData out;
            if (decodeImage(bytes, out, error)) {
                ++decoded;
                REQUIRE(out.brightness.size() == static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
            } else {
                REQUIRE(out.brightness.empty());
            }
        }
        (void)decoded;
        for (int i = 0; i < 2000; ++i) { // BMP headers with random sizes
            auto bytes = makeBmp(4, 4, [](int, int) { return 0.5f; });
            for (int e = 0; e < 3; ++e)
                bytes[static_cast<std::size_t>(rng.next() % 54)] = static_cast<std::uint8_t>(rng.next());
            ImageData out;
            (void)decodeImage(bytes, out, error);
        }
    }
}

TEST_CASE("inflate: stored, fixed and dynamic blocks", "[import][image]") {
    const auto expected = load("text.txt");
    std::vector<std::uint8_t> out;
    REQUIRE(inflateZlib(load("text_dynamic.zlib"), out, 1 << 20));
    CHECK(out == expected);
    const std::vector<std::uint8_t> head(expected.begin(), expected.begin() + 60);
    REQUIRE(inflateZlib(load("text_fixed.zlib"), out, 1 << 20));
    CHECK(out == head);
    REQUIRE(inflateZlib(load("text_stored.zlib"), out, 1 << 20));
    CHECK(out == head);
    // The size limit is enforced, and damage is detected or at least survived.
    CHECK_FALSE(inflateZlib(load("text_dynamic.zlib"), out, 100));
    const std::uint8_t notZlib[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_FALSE(inflateZlib(notZlib, out, 1 << 20));
    Rng rng(2);
    const auto good = load("text_dynamic.zlib");
    for (int i = 0; i < 5000; ++i) {
        auto bytes = good;
        bytes[2 + static_cast<std::size_t>(rng.next() % (bytes.size() - 2))] ^= static_cast<std::uint8_t>(1u << (rng.next() % 8));
        (void)inflateZlib(bytes, out, 1 << 16);
        REQUIRE(out.size() <= (1u << 16));
    }
}

TEST_CASE("import: the wizard's suggestion", "[import]") {
    const auto asWav = [](const AudioClip& clip) {
        WavData wav;
        wav.sampleRate = clip.sampleRate;
        wav.channels.push_back(clip.samples);
        return wav;
    };
    // A table: a whole number of 2048-sample frames that repeat every 2048 samples.
    Wavetable ladder;
    for (int k = 1; k <= 8; ++k) {
        Frame f;
        for (int n = 0; n < kFrameSize; ++n)
            f[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(2.0 * kPi * n / kFrameSize) + 0.3 * std::sin(2.0 * kPi * k * n / kFrameSize));
        ladder.frames.push_back(f);
    }
    WavData tableWav;
    std::string error;
    REQUIRE(decodeWav(encodeWavetable(ladder), tableWav, error));
    ImportSuggestion s = suggestImport(tableWav);
    CHECK(s.method == "table");
    CHECK(s.frameSize == kFrameSize);
    tableWav.chunks.clear(); // even without its metadata
    s = suggestImport(tableWav);
    CHECK(s.method == "table");
    CHECK_FALSE(s.reason.empty());

    AudioClip cycle;
    cycle.samples.resize(600);
    for (std::size_t i = 0; i < 600; ++i)
        cycle.samples[i] = static_cast<float>(std::sin(2.0 * kPi * static_cast<double>(i) / 600.0));
    CHECK(suggestImport(asWav(cycle)).method == "single_cycle");

    s = suggestImport(asWav(tone(220.0, 220.0, 1.5)));
    CHECK(s.method == "fixed_cycles");
    CHECK(s.pitch.hz == Approx(220.0).epsilon(0.005));
    CHECK(suggestImport(asWav(tone(110.0, 180.0, 1.5))).method == "pitch_tracked");

    AudioClip noise;
    Rng rng(12);
    noise.samples.resize(60000);
    for (float& v : noise.samples)
        v = static_cast<float>(rng.uniform() * 2.0 - 1.0);
    s = suggestImport(asWav(noise));
    CHECK(s.method == "spectral");
    CHECK_FALSE(s.reason.empty());
}
