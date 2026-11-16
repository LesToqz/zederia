#include "../support/TestUtil.h"
#include "zederia/bench/Bench.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace zed;
using Catch::Approx;

namespace {

std::vector<SeqEvent> phrase(double sampleRate = 48000.0) {
    const NoteSpec notes[] = {{48, 0.0, 0.4, 0.9}, {55, 0.1, 0.4, 0.7}, {60, 0.2, 0.4, 0.8}, {64, 0.3, 0.5, 1.0}};
    return sequenceFromNotes(notes, sampleRate);
}

// Builds a Standard MIDI File in memory.
struct Smf {
    std::vector<std::uint8_t> bytes;
    void raw(std::initializer_list<int> values) {
        for (const int v : values)
            bytes.push_back(static_cast<std::uint8_t>(v));
    }
    void be(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i)
            bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void header(int format, int tracks, int division) {
        raw({'M', 'T', 'h', 'd'});
        be(6, 4);
        be(static_cast<std::uint32_t>(format), 2);
        be(static_cast<std::uint32_t>(tracks), 2);
        be(static_cast<std::uint32_t>(division), 2);
    }
    void track(const std::vector<std::uint8_t>& data) {
        raw({'M', 'T', 'r', 'k'});
        be(static_cast<std::uint32_t>(data.size()), 4);
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
};

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(le16(b, at)) | (static_cast<std::uint32_t>(le16(b, at + 2)) << 16);
}

} // namespace

TEST_CASE("render: same patch, notes and seed give identical output", "[render][determinism]") {
    Patch patch;
    patch.set("env1.release", 200);
    const auto events = phrase();
    RenderSettings settings;
    settings.seed = 7;
    const RenderResult a = renderOffline(patch, events, settings);
    const RenderResult b = renderOffline(patch, events, settings);
    REQUIRE(a.left.size() == b.left.size());
    CHECK(a.left == b.left);
    CHECK(a.right == b.right);
    CHECK(a.hash() == b.hash());
    CHECK(a.peak() > 0.05f);

    // The seed is what the randomness hangs on: another seed, another result ...
    settings.seed = 8;
    CHECK(renderOffline(patch, events, settings).hash() != a.hash());
    // ... unless the patch uses no randomness.
    patch.set("osc1.phase_random", 0);
    const std::uint64_t fixed = renderOffline(patch, events, settings).hash();
    settings.seed = 9;
    CHECK(renderOffline(patch, events, settings).hash() == fixed);
}

TEST_CASE("render: length follows the events and the tail", "[render]") {
    Patch patch;
    patch.set("env1.release", 250);
    const NoteSpec one[] = {{60, 0.5, 1.0, 1.0}};
    const auto events = sequenceFromNotes(one, 48000.0);
    REQUIRE(events.size() == 2);
    CHECK(events[0].sample == 24000);
    CHECK(events[1].sample == 72000);

    const RenderResult automatic = renderOffline(patch, events, {});
    // The whole release is there, then 0.3 s for the output DC filter to settle (14400 samples),
    // and the file ends in silence.
    CHECK(automatic.left.size() > 72000u + 12000u + 14400u);
    CHECK(automatic.left.size() < 72000u + 14000u + 14400u);
    CHECK(zedtest::peakOf(automatic.left, 0, 24000) == 0.0f);
    CHECK(zedtest::peakOf(automatic.left, 72000 + 6000, 72000 + 11000) > 0.0f);
    CHECK(automatic.left.back() == 0.0f);

    RenderSettings fixedTail;
    fixedTail.tailSeconds = 0.0;
    CHECK(renderOffline(patch, events, fixedTail).left.size() == 72001u);
    CHECK(renderOffline(patch, {}, fixedTail).left.size() == 1u);

    RenderSettings other;
    other.sampleRate = 44100.0;
    const RenderResult r = renderOffline(patch, sequenceFromNotes(one, 44100.0), other);
    CHECK(r.sampleRate == 44100.0);
    CHECK(zedtest::cents(zedtest::measureFrequency(r.left, 30000, 60000, 44100.0), noteToHz(60)) == Approx(0.0).margin(0.1));
}

TEST_CASE("render: note lists are parsed with clear errors", "[render]") {
    std::vector<NoteSpec> notes;
    std::string error;
    REQUIRE(parseNoteList("C3:0:0.5, 64:0.5:0.25:127 ,F#4:1:2:1,", notes, error));
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].note == 48);
    CHECK(notes[0].velocity == Approx(100.0 / 127.0));
    CHECK(notes[1].note == 64);
    CHECK(notes[1].start == 0.5);
    CHECK(notes[1].duration == 0.25);
    CHECK(notes[1].velocity == 1.0);
    CHECK(notes[2].note == 66);

    for (const char* bad : {"", " , ", "60", "60:0", "60:0:1:100:5", "H2:0:1", "200:0:1", "60:-1:1", "60:0:0", "60:0:1:0",
                            "60:0:1:128", "60:x:1", "60:0:1e999"}) {
        INFO(bad);
        error.clear();
        CHECK_FALSE(parseNoteList(bad, notes, error));
        CHECK(!error.empty());
    }

    CHECK(parseNoteName("C4") == 60);
    CHECK(parseNoteName("A4") == 69);
    CHECK(parseNoteName("C-1") == 0);
    CHECK(parseNoteName("G9") == 127);
    CHECK(parseNoteName("Bb2") == 46);
    CHECK(parseNoteName("f#3") == 54);
    CHECK(parseNoteName("A9") == -1);
    CHECK(parseNoteName("C") == -1);
    CHECK(parseNoteName("X1") == -1);
    CHECK(parseNoteName("") == -1);
}

TEST_CASE("render: WAV files are well formed", "[render][wav]") {
    const std::vector<float> left = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f};
    const std::vector<float> right = {0.25f, -0.25f, 0.0f, 0.0f, 0.0f, -2.0f};

    const auto pcm16 = encodeWav(left, right, 44100.0, WavFormat::Pcm16);
    REQUIRE(pcm16.size() == 44u + 6u * 2u * 2u);
    CHECK(std::memcmp(pcm16.data(), "RIFF", 4) == 0);
    CHECK(le32(pcm16, 4) == pcm16.size() - 8);
    CHECK(std::memcmp(pcm16.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK(le16(pcm16, 20) == 1);     // PCM
    CHECK(le16(pcm16, 22) == 2);     // stereo
    CHECK(le32(pcm16, 24) == 44100); // sample rate
    CHECK(le32(pcm16, 28) == 44100u * 4u);
    CHECK(le16(pcm16, 32) == 4);
    CHECK(le16(pcm16, 34) == 16);
    CHECK(std::memcmp(pcm16.data() + 36, "data", 4) == 0);
    CHECK(le32(pcm16, 40) == 24);
    const auto sample16 = [&](std::size_t frame, int channel) {
        return static_cast<std::int16_t>(le16(pcm16, 44 + (frame * 2 + static_cast<std::size_t>(channel)) * 2));
    };
    CHECK(sample16(0, 0) == 0);
    CHECK(sample16(0, 1) == 8192);
    CHECK(sample16(1, 0) == 16384);
    CHECK(sample16(2, 0) == -16384);
    CHECK(sample16(3, 0) == 32767);
    CHECK(sample16(4, 0) == -32767);
    CHECK(sample16(5, 0) == 32767); // clipped, not wrapped
    CHECK(sample16(5, 1) == -32767);

    const auto pcm24 = encodeWav(left, right, 48000.0, WavFormat::Pcm24);
    REQUIRE(pcm24.size() == 44u + 6u * 2u * 3u);
    CHECK(le16(pcm24, 34) == 24);
    CHECK(pcm24[44 + 6] == 0x00); // frame 1, left: 0.5 * 8388607 = 4194304 = 0x400000
    CHECK(pcm24[44 + 7] == 0x00);
    CHECK(pcm24[44 + 8] == 0x40);

    const auto f32 = encodeWav(left, right, 96000.0, WavFormat::Float32);
    REQUIRE(f32.size() == 56u + 6u * 2u * 4u);
    CHECK(le16(f32, 20) == 3); // IEEE float
    CHECK(std::memcmp(f32.data() + 36, "fact", 4) == 0);
    CHECK(le32(f32, 44) == 6);
    float back = 0.0f;
    std::memcpy(&back, f32.data() + 56 + 5 * 8, 4);
    CHECK(back == 2.0f); // float files keep values beyond full scale
    CHECK(le32(f32, 4) == f32.size() - 8);
}

TEST_CASE("render: Standard MIDI Files are read with their tempo map", "[render][midi]") {
    // Format 1, 480 ticks per quarter. Track 1: tempo 120, then 60 BPM from tick 960.
    // Track 2: C4 at 0 for one quarter; E4 at tick 960 for a quarter (running status, velocity 0 as note-off).
    Smf smf;
    smf.header(1, 2, 480);
    smf.track({0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,        // 500000 us per quarter
               0x87, 0x40, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40,  // delta 960: 1000000 us per quarter
               0x00, 0xFF, 0x2F, 0x00});
    smf.track({0x00, 0xFF, 0x03, 0x04, 'L', 'e', 'a', 'd',      // track name: ignored
               0x00, 0x90, 60, 100,
               0x83, 0x60, 60, 0,                                // delta 480, running status, velocity 0
               0x83, 0x60, 0x90, 64, 127,                        // delta 480: tick 960
               0x00, 0xE0, 0x00, 0x60,                           // pitch bend up
               0x00, 0xF0, 0x03, 0x01, 0x02, 0xF7,               // a system-exclusive message: skipped
               0x83, 0x60, 0x80, 64, 0,                          // delta 480: tick 1440
               0x00, 0xFF, 0x2F, 0x00});
    std::vector<SeqEvent> events;
    std::string error;
    REQUIRE(sequenceFromMidiFile(smf.bytes, 48000.0, events, error));
    REQUIRE(events.size() == 5);
    CHECK(events[0].sample == 0);
    CHECK(events[0].event.type == MidiEvent::Type::NoteOn);
    CHECK(events[0].event.number == 60);
    CHECK(events[0].event.value == Approx(100.0 / 127.0));
    CHECK(events[1].sample == 24000); // one quarter at 120 BPM
    CHECK(events[1].event.type == MidiEvent::Type::NoteOff);
    CHECK(events[2].sample == 48000); // two quarters at 120 BPM
    CHECK(events[2].event.number == 64);
    CHECK(events[3].event.type == MidiEvent::Type::PitchBend);
    CHECK(events[3].event.value > 0.49f);
    CHECK(events[4].sample == 96000); // the third quarter runs at 60 BPM
    CHECK(events[4].event.type == MidiEvent::Type::NoteOff);

    // Rendering the file equals rendering the same notes given as a list.
    Smf plain;
    plain.header(0, 1, 96);
    plain.track({0x00, 0x90, 60, 127, 0x60, 0x80, 60, 0, 0x00, 0x90, 67, 127, 0x60, 0x80, 67, 0, 0x00, 0xFF, 0x2F, 0x00});
    REQUIRE(sequenceFromMidiFile(plain.bytes, 48000.0, events, error));
    const NoteSpec same[] = {{60, 0.0, 0.5, 1.0}, {67, 0.5, 0.5, 1.0}};
    CHECK(renderOffline(Patch(), events, {}).hash() == renderOffline(Patch(), sequenceFromNotes(same, 48000.0), {}).hash());
}

TEST_CASE("render: damaged MIDI files are refused, never a crash", "[render][midi][fuzz]") {
    std::vector<SeqEvent> events;
    std::string error;
    const auto refuse = [&](const std::vector<std::uint8_t>& bytes) {
        error.clear();
        const bool ok = sequenceFromMidiFile(bytes, 48000.0, events, error);
        if (!ok)
            CHECK(!error.empty());
        return !ok;
    };
    CHECK(refuse({}));
    CHECK(refuse({'R', 'I', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    Smf smpte;
    smpte.header(1, 1, 0xE728);
    smpte.track({0x00, 0xFF, 0x2F, 0x00});
    CHECK(refuse(smpte.bytes));
    CHECK(error.find("SMPTE") != std::string::npos);
    Smf format2;
    format2.header(2, 1, 480);
    CHECK(refuse(format2.bytes));
    Smf cut;
    cut.header(1, 1, 480);
    cut.raw({'M', 'T', 'r', 'k', 0, 0, 1, 0, 0x00, 0x90, 60});
    CHECK(refuse(cut.bytes));
    Smf orphan; // running status with no status before it
    orphan.header(0, 1, 480);
    orphan.track({0x00, 60, 100});
    CHECK(refuse(orphan.bytes));

    Smf good;
    good.header(1, 2, 480);
    good.track({0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x00, 0xFF, 0x2F, 0x00});
    good.track({0x00, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0, 0x00, 0xB0, 64, 127, 0x10, 0xE0, 0, 64, 0x00, 0xFF, 0x2F, 0x00});
    REQUIRE(sequenceFromMidiFile(good.bytes, 48000.0, events, error));
    Rng rng(4242);
    int accepted = 0;
    for (int i = 0; i < 5000; ++i) {
        auto bytes = good.bytes;
        const int edits = 1 + static_cast<int>(rng.next() % 3);
        for (int e = 0; e < edits && !bytes.empty(); ++e) {
            const std::size_t at = rng.next() % bytes.size();
            switch (rng.next() % 4) {
            case 0: bytes[at] = static_cast<std::uint8_t>(rng.next()); break;
            case 1: bytes.resize(at); break;
            case 2: bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::uint8_t>(rng.next())); break;
            default: bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at)); break;
            }
        }
        if (sequenceFromMidiFile(bytes, 48000.0, events, error)) {
            ++accepted;
            for (const auto& e : events)
                REQUIRE(e.sample >= 0);
        } else {
            REQUIRE(!error.empty());
        }
    }
    CHECK(accepted > 100);
    CHECK(accepted < 4900);
}

TEST_CASE("bench: scenarios run and report a realtime factor", "[bench]") {
    REQUIRE(benchScenarios().size() >= 3);
    for (const auto& s : benchScenarios()) {
        INFO(s.name);
        CHECK(s.budgetPercent > 0.0);
        BenchResult result;
        REQUIRE(runBench(s.name, 0.25, 1, result));
        CHECK(result.percent > 0.0);
        CHECK(result.audioSeconds == Approx(0.25).epsilon(0.05));
    }
    BenchResult result;
    CHECK_FALSE(runBench("no_such_scenario", 0.25, 1, result));
}
