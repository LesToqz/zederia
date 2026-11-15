// Tuning files (plan 3.6, 6.1): Scala scales and keyboard mappings, .tun tables, and the errors
// each parser reports.
#include "../support/TestUtil.h"
#include "zederia/tuning/Tuning.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;

namespace {

const char* kJust = "! just.scl\n"
                    "!\n"
                    "Five-limit just major scale\n"
                    " 7\n"
                    "!\n"
                    " 9/8\n"
                    " 5/4\n"
                    " 4/3\n"
                    " 3/2\n"
                    " 5/3\n"
                    " 15/8\n"
                    " 2/1\n";

const char* kTwelve = "! twelve.scl\n12 equal steps\n12\n100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n700.0\n800.0\n900.0\n1000.0\n1100.0\n2/1\n";

double centsBetween(const Tuning& t, int a, int b) { return (t.note[static_cast<std::size_t>(b)] - t.note[static_cast<std::size_t>(a)]) * 100.0; }

} // namespace

TEST_CASE("tuning: equal temperament is the default and glides run straight between keys", "[tuning]") {
    const Tuning equal;
    CHECK(equal.isEqual());
    CHECK(equal.pitchOf(60.0) == 60.0);
    CHECK(equal.pitchOf(60.25) == 60.25);
    CHECK(equal.pitchOf(-3.0) == -3.0);
    CHECK(equal.pitchOf(130.5) == 130.5);
    Tuning bent;
    bent.note[61] = 61.5;
    CHECK_FALSE(bent.isEqual());
    CHECK(bent.pitchOf(60.5) == Approx(60.75));
    CHECK(bent.pitchOf(61.5) == Approx(61.75));
}

TEST_CASE("tuning: a Scala scale with ratios and cents", "[tuning]") {
    ScaleDef scale;
    std::string error;
    REQUIRE(parseScl(kJust, scale, error));
    CHECK(scale.description == "Five-limit just major scale");
    REQUIRE(scale.cents.size() == 7);
    CHECK(scale.cents[0] == Approx(203.910).margin(0.001));
    CHECK(scale.cents[3] == Approx(701.955).margin(0.001));
    CHECK(scale.cents[6] == Approx(1200.0));

    // Without a mapping: the first degree on middle C at its equal-tempered pitch, one key per degree.
    const Tuning t = tuningFromScale(scale, KeyMap{}, "Just");
    CHECK(t.name == "Just");
    CHECK(t.note[60] == Approx(60.0).margin(1e-9));
    CHECK(centsBetween(t, 60, 61) == Approx(203.910).margin(0.001));
    CHECK(centsBetween(t, 60, 64) == Approx(701.955).margin(0.001)); // the fifth, on the fourth key up
    CHECK(centsBetween(t, 60, 67) == Approx(1200.0).margin(1e-6));   // seven keys to the octave
    CHECK(centsBetween(t, 53, 60) == Approx(1200.0).margin(1e-6));   // and downward
    CHECK(centsBetween(t, 59, 60) == Approx(1200.0 - 1088.269).margin(0.001));

    ScaleDef twelve;
    REQUIRE(parseScl(kTwelve, twelve, error));
    CHECK(tuningFromScale(twelve, KeyMap{}, "").isEqual() == false); // equal within rounding, checked below
    const Tuning e = tuningFromScale(twelve, KeyMap{}, "");
    for (int key = 0; key < 128; ++key)
        CHECK(e.note[static_cast<std::size_t>(key)] == Approx(key).margin(1e-9));
}

TEST_CASE("tuning: a keyboard mapping places and transposes the scale", "[tuning]") {
    ScaleDef scale;
    std::string error;
    REQUIRE(parseScl(kJust, scale, error));
    // Seven scale degrees on the white keys; the black keys are unmapped. A = 432 Hz on key 69.
    const char* kbm = "! white keys\n12\n0\n127\n60\n69\n432.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n";
    KeyMap map;
    REQUIRE(parseKbm(kbm, map, error));
    CHECK(map.degrees.size() == 12);
    CHECK(map.degrees[1] == -1);
    CHECK(map.reference == 69);
    const Tuning t = tuningFromScale(scale, map, "white");
    // Key 69 (A, the sixth degree, 5/3 above C) sounds at 432 Hz: 31.77 cents below 440.
    CHECK(t.note[69] == Approx(69.0 + 12.0 * std::log2(432.0 / 440.0)).margin(1e-9));
    CHECK(centsBetween(t, 60, 67) == Approx(701.955).margin(0.001)); // C to G is a pure fifth
    CHECK(centsBetween(t, 60, 72) == Approx(1200.0).margin(1e-6));
    CHECK(t.note[61] == t.note[60]); // an unmapped key keeps the pitch of the key below
    CHECK(centsBetween(t, 48, 60) == Approx(1200.0).margin(1e-6));

    // A mapping can cover part of the keyboard.
    const char* partial = "0\n48\n72\n60\n60\n261.6255653\n0\n";
    REQUIRE(parseKbm(partial, map, error));
    const Tuning p = tuningFromScale(scale, map, "");
    CHECK(p.note[60] == Approx(60.0).margin(1e-6));
    CHECK(p.note[100] == p.note[72]);
    CHECK(p.note[10] == p.note[48]);
}

TEST_CASE("tuning: .tun tables in whole and exact cents", "[tuning]") {
    std::string text = "; a comment\n[Tuning]\n";
    for (int i = 0; i < 128; ++i)
        text += "note " + std::to_string(i) + "=" + std::to_string(i * 100 + (i == 60 ? 14 : 0)) + "\n";
    Tuning t;
    std::string error;
    REQUIRE(parseTun(text, t, error));
    CHECK(t.note[59] == Approx(59.0));
    CHECK(t.note[60] == Approx(60.14));
    CHECK(t.note[127] == Approx(127.0));

    // The exact section wins and carries its own base frequency.
    text += "[Exact Tuning]\nbasefreq=8.2\nNote 60 = 6050.5\n";
    REQUIRE(parseTun(text, t, error));
    CHECK(t.note[60] == Approx(60.505 + 12.0 * std::log2(8.2 / 8.1757989156437)).margin(1e-9));
    CHECK(t.note[61] == Approx(61.0));
}

TEST_CASE("tuning: bad files are refused with a message that says where", "[tuning]") {
    ScaleDef scale;
    KeyMap map;
    Tuning tuning;
    std::string error;
    CHECK_FALSE(parseScl("", scale, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(parseScl("name\nseven\n", scale, error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK_FALSE(parseScl("name\n3\n9/8\n5/4\n", scale, error));
    CHECK(error.find("lists only 2") != std::string::npos);
    CHECK_FALSE(parseScl("name\n2\n9/8\nabc\n", scale, error));
    CHECK(error.find("line 4") != std::string::npos);
    CHECK_FALSE(parseScl("name\n2\n9/0\n2/1\n", scale, error));
    CHECK_FALSE(parseScl("name\n1\n-5.0\n", scale, error)); // the repeat interval must rise
    CHECK_FALSE(parseKbm("12\n0\n127\n", map, error));
    CHECK_FALSE(parseKbm("12\n0\n127\n60\n69\nloud\n12\n", map, error));
    CHECK(error.find("line 6") != std::string::npos);
    CHECK_FALSE(parseKbm("2\n0\n127\n60\n69\n440.0\n12\n0\nq\n", map, error));
    CHECK_FALSE(parseTun("[Tuning]\nnote 200=100\n", tuning, error));
    CHECK_FALSE(parseTun("[Tuning]\nnote 1=high\n", tuning, error));
    CHECK_FALSE(parseTun("nothing here\n", tuning, error));
    CHECK_FALSE(loadTuningFiles("no/such/file.scl", "", tuning, error));
    CHECK_FALSE(loadTuningFiles(zedtest::dataPath("patches/glass_pluck.zpreset"), "", tuning, error));

    // Random bytes never crash a parser.
    Rng rng(5);
    for (int i = 0; i < 2000; ++i) {
        std::string junk(rng.next() % 200, ' ');
        for (char& c : junk)
            c = "0123456789./\n\n !x[]=-noteTuningExactbasefreq"[rng.next() % 45];
        parseScl(junk, scale, error);
        parseKbm(junk, map, error);
        parseTun(junk, tuning, error);
        if (parseScl(junk, scale, error))
            for (const double note : tuningFromScale(scale, map, "").note)
                REQUIRE(std::isfinite(note));
    }
}
