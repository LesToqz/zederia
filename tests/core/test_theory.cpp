// Music helpers for agents (plan 4.7): chord symbols, roman numerals, keys, voice leading,
// quantizing, clip checks and pack file names.
#include "zederia/music/Theory.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace zed;
using namespace zed::music;

namespace {
std::vector<int> pcs(const Chord& c) {
    std::vector<int> out;
    for (const int i : c.intervals)
        out.push_back((c.root + i) % 12);
    std::sort(out.begin(), out.end());
    return out;
}
Chord chord(const char* s) {
    Chord c;
    std::string error;
    REQUIRE(parseChord(s, c, error));
    return c;
}
} // namespace

TEST_CASE("theory: chord symbols", "[theory]") {
    CHECK(chord("C").intervals == std::vector<int>{0, 4, 7});
    CHECK(chord("Fm7").intervals == std::vector<int>{0, 3, 7, 10});
    CHECK(chord("Bbmaj7").intervals == std::vector<int>{0, 4, 7, 11});
    CHECK(chord("Bbmaj7").root == 10);
    CHECK(chord("G7b9").intervals == std::vector<int>{0, 4, 7, 10, 13});
    CHECK(chord("Cm7b5").intervals == std::vector<int>{0, 3, 6, 10});
    CHECK(chord("Cdim7").intervals == std::vector<int>{0, 3, 6, 9});
    CHECK(chord("Csus4").intervals == std::vector<int>{0, 5, 7});
    CHECK(chord("Cadd9").intervals == std::vector<int>{0, 4, 7, 14});
    CHECK(chord("C6/9").intervals == std::vector<int>{0, 4, 7, 9, 14});
    CHECK(chord("CmMaj7").intervals == std::vector<int>{0, 3, 7, 11});
    CHECK(chord("C5").intervals == std::vector<int>{0, 7});
    CHECK(chord("Db/F").bass == 5);
    CHECK(chord("E7(#9)").intervals == std::vector<int>{0, 4, 7, 10, 15});
    Chord c;
    std::string error;
    CHECK_FALSE(parseChord("Hm7", c, error));
    CHECK_FALSE(parseChord("Cxyz", c, error));
    CHECK(error.find("xyz") != std::string::npos);
}

TEST_CASE("theory: keys and roman numerals", "[theory]") {
    Key k;
    REQUIRE(parseKey("F minor", k));
    CHECK(k.tonic == 5);
    CHECK(k.name() == "F minor");
    CHECK(k.shortName() == "Fmin");
    REQUIRE(parseKey("Bb major", k));
    CHECK(k.shortName() == "Bbmaj");
    REQUIRE(parseKey("F#m", k));
    CHECK(k.name() == "F# minor");
    CHECK_FALSE(parseKey("Q lydian", k));
    REQUIRE(parseKey("F minor", k));
    Chord c;
    std::string error;
    REQUIRE(parseRoman("i", k, c, error));
    CHECK(pcs(c) == std::vector<int>{0, 5, 8}); // F Ab C
    REQUIRE(parseRoman("VI", k, c, error));
    CHECK(c.root == 1); // Db
    REQUIRE(parseRoman("V7", k, c, error));
    CHECK(c.root == 0);
    CHECK(c.intervals == std::vector<int>{0, 4, 7, 10}); // C7, the major dominant
    REQUIRE(parseRoman("bII", k, c, error)); // F minor's ii is G; flattened: Gb
    CHECK(c.root == 6);
    REQUIRE(parseChordOrRoman("iv", &k, c, error));
    CHECK(c.root == 10);
    REQUIRE(parseChordOrRoman("Bbm", &k, c, error)); // a symbol in a key still reads as a symbol
    CHECK(c.root == 10);
}

TEST_CASE("theory: voice leading moves little and stays in range", "[theory]") {
    std::vector<Chord> prog = {chord("Fm"), chord("Db"), chord("Ab"), chord("Eb")};
    const auto v = voiceLead(prog, 64, 53, 76, true);
    REQUIRE(v.size() == 4);
    for (std::size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i].notes.front() >= 53);
        CHECK(v[i].notes.back() <= 76);
        CHECK(v[i].bass >= 0);
        CHECK(v[i].bass < v[i].notes.front());
        CHECK(v[i].bass % 12 == prog[i].root);
        std::vector<int> got;
        for (const int n : v[i].notes)
            got.push_back(n % 12);
        std::sort(got.begin(), got.end());
        CHECK(got == pcs(prog[i]));
    }
    for (std::size_t i = 1; i < v.size(); ++i) {
        int move = 0;
        for (std::size_t k = 0; k < v[i].notes.size(); ++k)
            move += std::abs(v[i].notes[k] - v[i - 1].notes[k]);
        CHECK(move <= 6); // common tones stay, the others step
    }
}

TEST_CASE("theory: quantizing to a key", "[theory]") {
    Key k;
    REQUIRE(parseKey("C major", k));
    CHECK(quantize(61, k) == 62); // C# goes up to D
    CHECK(quantize(61, k, -1) == 60);
    CHECK(quantize(60, k) == 60);
    REQUIRE(parseKey("A minor_pentatonic", k));
    CHECK(quantize(70, k) == 69); // A# is a semitone above A, two below C
}

TEST_CASE("theory: clip checks report range, overlaps, key and bar length", "[theory]") {
    Clip c;
    c.bars = 2;
    c.tempo = 124;
    c.notes = {{0.0, 1.0, 65, 0.8}, {0.5, 1.0, 65, 0.8}, {2.0, 1.0, 66, 0.8}, {7.5, 1.0, 60, 0.8}, {9.0, 0.5, 60, 0.8}, {1.0, 0.5, 200, 0.8}};
    Key k;
    REQUIRE(parseKey("F minor", k));
    const auto issues = validateClip(c, k);
    const auto has = [&](const char* code, bool error) {
        return std::any_of(issues.begin(), issues.end(), [&](const ClipIssue& i) { return i.code == code && i.error == error; });
    };
    CHECK(has("overlap", false));
    CHECK(has("outside_key", false)); // F#
    CHECK(has("past_end", false));    // the note at 7.5 runs past beat 8
    CHECK(has("past_end", true));     // the note at 9 starts after the end
    CHECK(has("out_of_range", true));
    for (const auto& i : issues) {
        CHECK_FALSE(i.path.empty());
        CHECK_FALSE(i.fix.empty());
    }
    c.notes = {{0.0, 1.0, 65, 0.8}};
    c.bars = 2.5;
    CHECK(validateClip(c, k).front().code == "partial_bar");
    CHECK(validateClip(c, k).front().message.find("(2.5 bars)") != std::string::npos); // no "2.500000"
    // Shorter than the clip player's 1/16 of a beat: refused here, so no loop of zero samples is
    // ever rendered from it.
    c.bars = 0.00001;
    c.beatsPerBar = 1;
    CHECK(validateClip(c, k).front().code == "too_short");
    CHECK(validateClip(c, k).front().error);
    c.bars = 1.0 / 16.0;
    CHECK(validateClip(c, k).front().code != "too_short");
}

TEST_CASE("theory: transposing clips and naming files", "[theory]") {
    CHECK(transposeBetween(5, 7) == 2);
    CHECK(transposeBetween(5, 0) == -5);
    CHECK(transposeBetween(0, 11) == -1);
    Clip c;
    c.notes = {{0, 1, 120, 0.8}};
    CHECK(transposeClip(c, 12).notes[0].key == 120); // folded back by an octave
    CHECK(loopFileName("chords", "Glass Pluck", "Fmin", 124.0, "wav") == "ZED_chords_GlassPluck_Fmin_124bpm.wav");
    CHECK(loopFileName("Lead Melody!", "deep  sub/bass", "", 0.0, "mid") == "ZED_lead-melody_DeepSubBass.mid");
    CHECK(oneShotFileName("Glass Pluck", 48, "wav") == "ZED_oneshot_GlassPluck_C3.wav");
    CHECK(multiSampleFileName("Glass Pluck", 61, 100, "wav") == "ZED_GlassPluck_C#4_v100.wav");
}
