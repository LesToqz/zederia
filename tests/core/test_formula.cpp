// The formula language (plan Appendix B, 6.1): precedence, functions, symbols, error messages
// that name the position and the problem, the complexity cap, and generating frames.
#include "../support/TestUtil.h"
#include "zederia/table/Formula.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;

namespace {

double eval(const std::string& text, const FormulaVars& vars = {}) {
    Formula f;
    FormulaError error;
    INFO(text << "  ->  " << error.describe());
    REQUIRE(Formula::compile(text, f, error));
    return f.evaluate(vars);
}

FormulaError failure(const std::string& text) {
    Formula f;
    FormulaError error;
    INFO(text);
    REQUIRE_FALSE(Formula::compile(text, f, error));
    REQUIRE(f.empty());
    REQUIRE_FALSE(error.message.empty());
    return error;
}

bool mentions(const FormulaError& e, const char* text) { return e.message.find(text) != std::string::npos; }

} // namespace

TEST_CASE("formula: arithmetic and precedence", "[formula]") {
    CHECK(eval("1 + 2 * 3") == 7.0);
    CHECK(eval("(1 + 2) * 3") == 9.0);
    CHECK(eval("10 - 4 - 3") == 3.0);      // left to right
    CHECK(eval("24 / 4 / 2") == 3.0);
    CHECK(eval("2 ^ 3 ^ 2") == 512.0);     // powers group to the right
    CHECK(eval("-2 ^ 2") == -4.0);         // and bind tighter than a leading minus
    CHECK(eval("2 ^ -1") == 0.5);
    CHECK(eval("2 * -3") == -6.0);
    CHECK(eval("--3") == 3.0);
    CHECK(eval("+3") == 3.0);
    CHECK(eval("7 % 3") == 1.0);
    CHECK(eval("-1 % 3") == 2.0);          // the remainder is never negative for a positive divisor
    CHECK(eval("2 + 3 % 2 * 4") == 6.0);   // % shares the rank of * and /
    CHECK(eval("1.5e2 + .5") == 150.5);
    CHECK(eval("  3   *( 2 +1 ) ") == 9.0);

    // Comparisons give 1 or 0 and rank below arithmetic; && above ||; ! above everything else.
    CHECK(eval("1 + 1 == 2") == 1.0);
    CHECK(eval("3 < 2") == 0.0);
    CHECK(eval("2 <= 2") == 1.0);
    CHECK(eval("3 > 2 == 1") == 1.0);
    CHECK(eval("1 != 1") == 0.0);
    CHECK(eval("2 >= 3") == 0.0);
    CHECK(eval("1 || 0 && 0") == 1.0);
    CHECK(eval("(1 || 0) && 0") == 0.0);
    CHECK(eval("!0") == 1.0);
    CHECK(eval("!5") == 0.0);
    CHECK(eval("!0 + 1") == 2.0);
    CHECK(eval("!(1 != 2)") == 0.0);

    // cond ? a : b, lowest of all, nesting to the right.
    CHECK(eval("1 ? 2 : 3") == 2.0);
    CHECK(eval("0 ? 2 : 3") == 3.0);
    CHECK(eval("1 < 2 ? 10 + 1 : 20") == 11.0);
    CHECK(eval("0 ? 1 : 0 ? 2 : 3") == 3.0);
    CHECK(eval("1 ? 0 ? 5 : 6 : 7") == 6.0);
}

TEST_CASE("formula: symbols and constants", "[formula]") {
    FormulaVars v;
    v.t = 0.25;
    v.x = -0.5;
    v.y = 0.75;
    v.z = 0.5;
    v.h = 7.0;
    v.in = 0.125;
    v.sel = -0.3;
    v.rand = 0.9;
    CHECK(eval("t", v) == 0.25);
    CHECK(eval("x", v) == -0.5);
    CHECK(eval("y", v) == 0.75);
    CHECK(eval("z", v) == 0.5);
    CHECK(eval("h", v) == 7.0);
    CHECK(eval("in", v) == 0.125);
    CHECK(eval("sel", v) == -0.3);
    CHECK(eval("rand", v) == 0.9);
    CHECK(eval("pi") == Approx(kPi));
    CHECK(eval("tau") == Approx(2.0 * kPi));
    CHECK(eval("e") == Approx(std::exp(1.0)));
    CHECK(eval("in * 2 + sel", v) == Approx(-0.05));
    CHECK(eval("1/h * (h % 2)", v) == Approx(1.0 / 7.0)); // the plan's odd series
    v.h = 8.0;
    CHECK(eval("1/h * (h % 2)", v) == 0.0);
}

TEST_CASE("formula: every function", "[formula]") {
    CHECK(eval("sin(pi / 2)") == Approx(1.0));
    CHECK(eval("cos(0)") == 1.0);
    CHECK(eval("tan(pi / 4)") == Approx(1.0));
    CHECK(eval("asin(1)") == Approx(kPi / 2.0));
    CHECK(eval("acos(1)") == 0.0);
    CHECK(eval("atan(1)") == Approx(kPi / 4.0));
    CHECK(eval("sinh(0)") == 0.0);
    CHECK(eval("cosh(0)") == 1.0);
    CHECK(eval("tanh(100)") == Approx(1.0));
    CHECK(eval("exp(1)") == Approx(std::exp(1.0)));
    CHECK(eval("ln(e)") == Approx(1.0));
    CHECK(eval("log2(8)") == Approx(3.0));
    CHECK(eval("log10(1000)") == Approx(3.0));
    CHECK(eval("sqrt(16)") == 4.0);
    CHECK(eval("abs(-3)") == 3.0);
    CHECK(eval("sign(-3) + sign(0) + sign(9)") == 0.0);
    CHECK(eval("floor(-1.5)") == -2.0);
    CHECK(eval("ceil(1.2)") == 2.0);
    CHECK(eval("round(2.5)") == 3.0);
    CHECK(eval("frac(2.75)") == 0.75);
    CHECK(eval("frac(-0.25)") == 0.75);
    CHECK(eval("min(2, 5)") == 2.0);
    CHECK(eval("max(2, 5)") == 5.0);
    CHECK(eval("clamp(7, 0, 1)") == 1.0);
    CHECK(eval("clamp(-7, 0, 1)") == 0.0);
    CHECK(eval("mix(10, 20, 0.25)") == 12.5);
    CHECK(eval("pow(2, 10)") == 1024.0);
    CHECK(eval("mod(7.5, 2)") == 1.5);
    CHECK(eval("mod(1, 0)") == 0.0);
    CHECK(eval("step(0.5, 0.4)") == 0.0);
    CHECK(eval("step(0.5, 0.5)") == 1.0);
    CHECK(eval("smoothstep(0, 1, 0.5)") == 0.5);
    CHECK(eval("smoothstep(0, 1, 2)") == 1.0);
    CHECK(eval("smoothstep(2, 4, 1)") == 0.0);
    // The basic waves have period 1.
    CHECK(eval("saw(0)") == -1.0);
    CHECK(eval("saw(0.5)") == 0.0);
    CHECK(eval("saw(1.75)") == 0.5);
    CHECK(eval("square(0.25)") == 1.0);
    CHECK(eval("square(0.75)") == -1.0);
    CHECK(eval("tri(0)") == Approx(0.0).margin(1e-12));
    CHECK(eval("tri(0.25)") == 1.0);
    CHECK(eval("tri(0.75)") == -1.0);
    // noise: the same value for the same input and seed, within -1..1.
    FormulaVars a, b;
    b.seed = 2;
    CHECK(eval("noise(3.7)", a) == eval("noise(3.7)", a));
    CHECK(eval("noise(3.7)", a) != eval("noise(3.8)", a));
    CHECK(eval("noise(3.7)", a) != eval("noise(3.7)", b));
    for (int i = 0; i < 200; ++i) {
        FormulaVars v;
        v.t = i * 0.013;
        const double n = eval("noise(t)", v);
        REQUIRE(n >= -1.0);
        REQUIRE(n <= 1.0);
    }
    // Nested calls and arguments that are expressions.
    CHECK(eval("max(min(4, 9), 2 + 1) * clamp(sin(0), -1, 1 + 1)") == 0.0);
    CHECK(eval("mix(1, 3, 1 > 0 ? 0.5 : 0)") == 2.0);
}

TEST_CASE("formula: errors name the position and the problem", "[formula][errors]") {
    FormulaError e = failure("");
    CHECK(mentions(e, "empty"));
    e = failure("   ");
    CHECK(mentions(e, "empty"));

    e = failure("1 + ");
    CHECK(e.position == 4);
    CHECK(mentions(e, "a value is expected"));

    e = failure("sin(t");
    CHECK(e.position == 5);
    CHECK(mentions(e, "missing \")\""));

    e = failure("(1 + 2");
    CHECK(mentions(e, "missing \")\""));
    CHECK(mentions(e, "column 1")); // where the bracket was opened

    e = failure("1 + 2)");
    CHECK(e.position == 5);
    CHECK(mentions(e, "no matching"));

    e = failure("2 * sine(t)");
    CHECK(e.position == 4);
    CHECK(mentions(e, "unknown function \"sine\""));
    CHECK(mentions(e, "did you mean \"sin\""));

    e = failure("t + foo");
    CHECK(e.position == 4);
    CHECK(mentions(e, "unknown name \"foo\""));

    e = failure("tua * t");
    CHECK(mentions(e, "did you mean \"tau\""));

    e = failure("sin * 2");
    CHECK(e.position == 0);
    CHECK(mentions(e, "is a function"));

    e = failure("t(3)");
    CHECK(mentions(e, "is a value, not a function"));
    e = failure("pi(3)");
    CHECK(mentions(e, "is a value, not a function"));

    e = failure("clamp(t, 0)");
    CHECK(e.position == 0);
    CHECK(mentions(e, "clamp takes 3 arguments, but 2 were given"));
    e = failure("sin()");
    CHECK(mentions(e, "sin takes 1 argument, but 0 were given"));
    e = failure("sin(1, 2)");
    CHECK(mentions(e, "but 2 were given"));

    e = failure("1 ? 2");
    CHECK(mentions(e, "needs a \":\""));

    e = failure("3 $ 4");
    CHECK(e.position == 2);
    CHECK(mentions(e, "unexpected"));

    e = failure("2 3");
    CHECK(e.position == 2);
    CHECK(mentions(e, "operator is missing"));

    e = failure("1 + * 2");
    CHECK(e.position == 4);

    e = failure("4 +)");
    CHECK(mentions(e, "before this \")\""));

    e = failure("1..2");
    CHECK(e.position == 2);

    // The message a user sees starts with the column.
    CHECK(failure("2 * sine(t)").describe() == "column 5: unknown function \"sine\"; did you mean \"sin\"?");

    // There are no variables, assignments, strings or loops.
    failure("a = 3");
    failure("t; x");
    failure("\"text\"");
    failure("for(1)");
}

TEST_CASE("formula: size and nesting are capped", "[formula][limits]") {
    // 255 additions of a symbol: 511 parts, just inside the limit.
    std::string sum = "t";
    for (int i = 0; i < 255; ++i)
        sum += "+t";
    Formula f;
    FormulaError error;
    REQUIRE(Formula::compile(sum, f, error));
    CHECK(f.nodeCount() == 511);
    FormulaVars v;
    v.t = 0.5;
    CHECK(f.evaluate(v) == 128.0);

    sum += "+t";
    FormulaError e = failure(sum);
    CHECK(mentions(e, "too complex"));
    CHECK(mentions(e, "512"));

    // Deep nesting is refused instead of exhausting the stack.
    std::string deep(200, '(');
    deep += "1";
    deep += std::string(200, ')');
    e = failure(deep);
    CHECK(mentions(e, "nested too deeply"));
    std::string negations(500, '-');
    negations += "1";
    e = failure(negations);
    CHECK(mentions(e, "nested too deeply"));
    // Moderate nesting is fine.
    std::string fine(20, '(');
    fine += "2";
    fine += std::string(20, ')');
    CHECK(eval(fine) == 2.0);

    e = failure(std::string(5000, '1'));
    CHECK(mentions(e, "longer than"));

    // Garbage never crashes the compiler.
    Rng rng(4);
    const std::string alphabet = "tx yzh1234567890.+-*/^%()?:<>=!&|,sincoabrmpqe \"$#";
    for (int i = 0; i < 20000; ++i) {
        std::string text;
        const int length = static_cast<int>(rng.next() % 40);
        for (int k = 0; k < length; ++k)
            text += alphabet[static_cast<std::size_t>(rng.next() % alphabet.size())];
        Formula g;
        FormulaError ignored;
        if (Formula::compile(text, g, ignored))
            (void)g.evaluate(v); // whatever it computes, it returns
        else
            REQUIRE_FALSE(ignored.message.empty());
    }
}

TEST_CASE("formula: generating frames in the wave domain", "[formula][table]") {
    Wavetable table = Wavetable::silent(5);
    FormulaRequest request;
    request.expression = "sin(tau*t) * (1 - y) + saw(t) * y"; // the plan's example: sine morphing to saw
    std::string error;
    REQUIRE(applyFormula(table, request, error));
    for (int n = 0; n < kFrameSize; n += 97) {
        const double t = static_cast<double>(n) / kFrameSize;
        REQUIRE(table.frames[0][static_cast<std::size_t>(n)] == Approx(std::sin(2.0 * kPi * t)).margin(1e-6));
        REQUIRE(table.frames[4][static_cast<std::size_t>(n)] == Approx(2.0 * t - 1.0).margin(1e-6));
        REQUIRE(table.frames[2][static_cast<std::size_t>(n)] == Approx(0.5 * std::sin(2.0 * kPi * t) + 0.5 * (2.0 * t - 1.0)).margin(1e-6));
    }

    // x and z run from -1 to 1; in reads the frame being written; sel reads the selected frame.
    request.expression = "x * z";
    REQUIRE(applyFormula(table, request, error));
    CHECK(table.frames[0][0] == Approx(1.0f));    // -1 * -1
    CHECK(table.frames[4][0] == Approx(-1.0f));   // -1 * 1
    CHECK(table.frames[2][512] == Approx(0.0f).margin(1e-7));
    request.expression = "in * 0.5";
    REQUIRE(applyFormula(table, request, error));
    CHECK(table.frames[0][0] == Approx(0.5f));
    request.expression = "sel";
    request.selectedFrame = 4;
    REQUIRE(applyFormula(table, request, error));
    CHECK(table.frames[1][0] == Approx(-0.5f));
    CHECK(table.frames[0] == table.frames[4]);

    // Scope: the current frame, a run of frames, or all.
    table = Wavetable::silent(5);
    request = FormulaRequest{};
    request.expression = "1";
    request.scope = oneFrame(2);
    REQUIRE(applyFormula(table, request, error));
    CHECK(framePeak(table.frames[1]) == 0.0f);
    CHECK(table.frames[2][7] == 1.0f);
    CHECK(framePeak(table.frames[3]) == 0.0f);
    request.scope = {3, 4};
    request.expression = "0.25";
    REQUIRE(applyFormula(table, request, error));
    CHECK(table.frames[2][7] == 1.0f);
    CHECK(table.frames[4][7] == 0.25f);

    // rand is noise fixed for each position: the same in every frame, repeatable, seed-dependent.
    request = FormulaRequest{};
    request.expression = "rand";
    REQUIRE(applyFormula(table, request, error));
    CHECK(table.frames[0] == table.frames[3]);
    CHECK(table.frames[0][10] != table.frames[0][11]);
    const Frame first = table.frames[0];
    REQUIRE(applyFormula(table, request, error));
    CHECK(table.frames[0] == first);
    request.seed = 99;
    REQUIRE(applyFormula(table, request, error));
    CHECK_FALSE(table.frames[0] == first);
    CHECK(framePeak(table.frames[0]) <= 1.0f);

    // Values that are not numbers become silence instead of spreading.
    request.expression = "1 / (t - 0.5) + ln(-1)";
    REQUIRE(applyFormula(table, request, error));
    for (const float s : table.frames[0])
        REQUIRE(std::isfinite(s));
    request.expression = "t * 4";
    request.normalize = true;
    REQUIRE(applyFormula(table, request, error));
    CHECK(framePeak(table.frames[0]) == Approx(1.0f));

    // A bad expression leaves the table alone and says where the problem is.
    const Wavetable before = table;
    request.expression = "sin(tau * t";
    CHECK_FALSE(applyFormula(table, request, error));
    CHECK(error.find("column 12") != std::string::npos);
    CHECK(error.find("missing \")\"") != std::string::npos);
    CHECK(table == before);
}

TEST_CASE("formula: generating frames in the harmonics domain", "[formula][table]") {
    Wavetable table = Wavetable::silent(3);
    FormulaRequest request;
    request.domain = FormulaDomain::Harmonics;
    request.expression = "1/h * (h % 2)"; // the plan's example: the odd series
    std::string error;
    REQUIRE(applyFormula(table, request, error));
    Harmonics h = analyzeFrame(table.frames[1]);
    CHECK(h.magnitude[1] == Approx(1.0).epsilon(1e-5));
    CHECK(h.magnitude[2] < 1e-7);
    CHECK(h.magnitude[3] == Approx(1.0 / 3.0).epsilon(1e-4));
    CHECK(h.magnitude[1023] == Approx(1.0 / 1023.0).epsilon(1e-3));
    CHECK(h.phase[3] == Approx(0.0).margin(1e-4)); // sines from zero: a square wave
    CHECK(table.frames[1][400] > 0.5f);
    CHECK(table.frames[1][1500] < -0.5f);

    // h runs to 1024 and y across the frames.
    request.expression = "h == 1 + round(y * 4) ? 1 : 0";
    REQUIRE(applyFormula(table, request, error));
    CHECK(analyzeFrame(table.frames[0]).magnitude[1] == Approx(1.0).epsilon(1e-5));
    CHECK(analyzeFrame(table.frames[1]).magnitude[3] == Approx(1.0).epsilon(1e-5));
    CHECK(analyzeFrame(table.frames[2]).magnitude[5] == Approx(1.0).epsilon(1e-5));
    request.expression = "h == 1024";
    REQUIRE(applyFormula(table, request, error));
    CHECK(analyzeFrame(table.frames[0]).magnitude[1024] > 0.0); // the top harmonic exists (as a cosine)

    // A second expression sets the phases.
    request.expression = "h == 2 ? 0.5 : 0";
    request.phaseExpression = "pi / 2";
    REQUIRE(applyFormula(table, request, error));
    h = analyzeFrame(table.frames[0]);
    CHECK(h.magnitude[2] == Approx(0.5).epsilon(1e-5));
    CHECK(h.phase[2] == Approx(kPi / 2.0).margin(1e-5));
    CHECK(table.frames[0][0] == Approx(0.5f).epsilon(1e-4)); // a cosine starts at its peak

    // in is the existing level of harmonic h; without a phase expression existing phases stay.
    request.expression = "in * 2";
    request.phaseExpression.clear();
    REQUIRE(applyFormula(table, request, error));
    h = analyzeFrame(table.frames[0]);
    CHECK(h.magnitude[2] == Approx(1.0).epsilon(1e-5));
    CHECK(h.phase[2] == Approx(kPi / 2.0).margin(1e-5));
    // sel is the selected frame's level.
    Wavetable two = Wavetable::silent(2);
    request.scope = oneFrame(0);
    request.expression = "h <= 3";
    REQUIRE(applyFormula(two, request, error));
    request.scope = oneFrame(1);
    request.selectedFrame = 0;
    request.expression = "sel / h";
    REQUIRE(applyFormula(two, request, error));
    CHECK(analyzeFrame(two.frames[1]).magnitude[3] == Approx(1.0 / 3.0).epsilon(1e-4));
    CHECK(analyzeFrame(two.frames[1]).magnitude[4] < 1e-7);

    // A negative level is the harmonic upside down; normalize brings the peak to 1.
    request = FormulaRequest{};
    request.domain = FormulaDomain::Harmonics;
    request.expression = "h == 1 ? -1 : 0";
    REQUIRE(applyFormula(two, request, error));
    CHECK(two.frames[0][512] == Approx(-1.0f).epsilon(1e-4));
    request.expression = "1/h";
    request.normalize = true;
    REQUIRE(applyFormula(two, request, error));
    CHECK(framePeak(two.frames[0]) == Approx(1.0f).epsilon(1e-5));

    request.phaseExpression = "h +";
    CHECK_FALSE(applyFormula(two, request, error));
    CHECK(error.find("phase expression") != std::string::npos);
    CHECK(error.find("column 4") != std::string::npos);
}
