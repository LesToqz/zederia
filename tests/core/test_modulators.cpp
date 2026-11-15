// The modulation building blocks on their own (plan 3.7, 6.1): the curve function and shapes,
// curved and multi-segment envelopes, LFOs, Flux generators and the arithmetic of a route.
#include "../support/TestUtil.h"
#include "zederia/engine/Envelope.h"
#include "zederia/engine/Lfo.h"
#include "zederia/engine/ModMath.h"
#include "zederia/patch/PatchIO.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;

TEST_CASE("shape: the curve function bends, mirrors and keeps its ends", "[modulation][shape]") {
    for (const double c : {-1.0, -0.5, 0.0, 0.3, 1.0}) {
        CHECK(curveValue(0.0, c) == Approx(0.0).margin(1e-12));
        CHECK(curveValue(1.0, c) == Approx(1.0));
        for (const double x : {0.1, 0.25, 0.5, 0.9}) {
            CHECK(curveValue(x, -c) == Approx(1.0 - curveValue(1.0 - x, c)).margin(1e-12)); // mirror image
            if (c > 0.0)
                CHECK(curveValue(x, c) > x); // fast at first
            if (c < 0.0)
                CHECK(curveValue(x, c) < x);
        }
    }
    CHECK(curveValue(0.5, 0.0) == 0.5);
    CHECK(curveValue(0.25, 1.0) > 0.75); // the strongest bend
    // Monotonic for every curve.
    for (const double c : {-1.0, 0.4, 1.0})
        for (int i = 1; i <= 100; ++i)
            CHECK(curveValue(i / 100.0, c) > curveValue((i - 1) / 100.0, c));
}

TEST_CASE("shape: points, curves, steps and held ends", "[modulation][shape]") {
    Shape shape;
    shape.points = {{0.25, -1.0, 0.0}, {0.5, 1.0, 0.0}, {0.5, 0.0, 0.5}, {1.0, 0.5, 0.0}};
    CHECK(shapeValue(shape, 0.0) == -1.0);                // held before the first point
    CHECK(shapeValue(shape, 0.375) == Approx(0.0));       // halfway up a straight segment
    CHECK(shapeValue(shape, 0.4999) == Approx(1.0).margin(0.01));
    CHECK(shapeValue(shape, 0.5) == 0.0);                 // a step: two points at one x
    CHECK(shapeValue(shape, 0.75) == Approx(0.5 * curveValue(0.5, 0.5))); // a curved segment
    CHECK(shapeValue(shape, 1.0) == 0.5);
    CHECK(shapeValue(Shape{}, 0.3) == 0.0);

    float table[65];
    compileShape(shape, table, 64);
    CHECK(table[0] == -1.0f);
    CHECK(table[24] == Approx(0.0f).margin(1e-6));
    CHECK(table[64] == 0.5f);
}

TEST_CASE("envelope: curves shape the segments and leave the times exact", "[modulation][envelope]") {
    EnvelopeTimes t;
    t.attack = 1000;
    t.decay = 2000;
    t.sustain = 0.2;
    t.release = 4000;
    t.attackCurve = 0.5;
    t.decayCurve = 0.5;
    t.releaseCurve = -0.5;
    Envelope env;
    env.trigger(0.0);
    CHECK(env.advance(500, t) == Approx(curveValue(0.5, 0.5)));
    CHECK(env.advance(500, t) == Approx(1.0));
    CHECK(env.advance(1000, t) == Approx(1.0 - 0.8 * curveValue(0.5, 0.5)));
    CHECK(env.advance(999, t) > 0.2);
    CHECK(env.advance(1, t) == Approx(0.2)); // the decay ends on the sample
    CHECK(env.stage() == Envelope::Stage::Sustain);
    env.release(0.2);
    CHECK(env.advance(2000, t) == Approx(0.2 * (1.0 - curveValue(0.5, -0.5))));
    CHECK(env.advance(1999, t) > 0.0);
    CHECK(env.advance(1, t) == 0.0);
    CHECK(env.finished());

    // However it is stepped, it arrives at the same level.
    Envelope a, b;
    a.trigger(0.0);
    b.trigger(0.0);
    double level = 0.0;
    for (int i = 0; i < 150; ++i)
        level = a.advance(16, t);
    CHECK(b.advance(2400, t) == Approx(level).margin(1e-12));
}

TEST_CASE("envelope: multi-segment mode with a sustain point and a loop", "[modulation][envelope]") {
    MultiEnvelopeDef d;
    d.count = 5;
    d.points[0] = {0, 0.0, 0.0};
    d.points[1] = {100, 1.0, 0.0};
    d.points[2] = {200, 0.5, 0.0};
    d.points[3] = {100, 0.8, 0.0};
    d.points[4] = {400, 0.0, 0.0};

    SECTION("plays through once without a sustain point") {
        MultiEnvelope env;
        env.trigger(0.0);
        CHECK(env.advance(50, d) == Approx(0.5));
        CHECK(env.advance(50, d) == Approx(1.0));
        CHECK(env.advance(100, d) == Approx(0.75));
        CHECK(env.advance(100, d) == Approx(0.5));
        CHECK(env.advance(100, d) == Approx(0.8));
        CHECK(env.advance(200, d) == Approx(0.4));
        CHECK_FALSE(env.finished());
        CHECK(env.advance(200, d) == Approx(0.0).margin(1e-12));
        CHECK(env.advance(16, d) == Approx(0.0).margin(1e-12));
        CHECK(env.finished());
    }
    SECTION("waits at the sustain point until the key is released") {
        d.sustainPoint = 2;
        MultiEnvelope env;
        env.trigger(0.0);
        CHECK(env.advance(300, d) == Approx(0.5));
        CHECK(env.advance(100000, d) == Approx(0.5));
        env.release(d);
        CHECK(env.advance(50, d) == Approx(0.65));
        CHECK(env.advance(50, d) == Approx(0.8));
        CHECK(env.advance(400, d) == Approx(0.0).margin(1e-12));
    }
    SECTION("a release before the sustain point leaves from the current level") {
        d.sustainPoint = 2;
        MultiEnvelope env;
        env.trigger(0.0);
        CHECK(env.advance(50, d) == Approx(0.5));
        env.release(d);
        CHECK(env.advance(50, d) == Approx(0.65)); // halfway from 0.5 to the 0.8 of point 3
        CHECK(env.advance(50, d) == Approx(0.8));
    }
    SECTION("loops between two points while the key is held") {
        d.loopStart = 1;
        d.loopEnd = 3;
        MultiEnvelope env;
        env.trigger(0.0);
        CHECK(env.advance(400, d) == Approx(1.0)); // reached point 3, back at point 1's level
        CHECK(env.advance(100, d) == Approx(0.75));
        CHECK(env.advance(200, d) == Approx(1.0)); // around again: 100 to point 2, 100 to point 3
        for (int i = 0; i < 1000; ++i)
            env.advance(37, d);
        CHECK_FALSE(env.finished());
        env.release(d);
        const double from = env.level();
        CHECK(env.advance(200, d) == Approx(from * 0.5)); // the last segment, from where it was
        CHECK(env.advance(200, d) == Approx(0.0).margin(1e-12));
        env.advance(1, d);
        CHECK(env.finished());
    }
    SECTION("stepping in small pieces gives the same levels") {
        d.sustainPoint = 2;
        MultiEnvelope a, b;
        a.trigger(0.0);
        b.trigger(0.0);
        double level = 0.0;
        for (int i = 0; i < 15; ++i)
            level = a.advance(16, d);
        CHECK(b.advance(240, d) == Approx(level).margin(1e-12));
    }
    SECTION("a loop of zero-length segments does not hang") {
        MultiEnvelopeDef z;
        z.count = 3;
        z.loopStart = 0;
        z.loopEnd = 2;
        MultiEnvelope env;
        env.trigger(0.0);
        CHECK(std::isfinite(env.advance(1000, z)));
    }
}

TEST_CASE("lfo: shape, rate, phase, delay, rise, smooth, polarity and modes", "[modulation][lfo]") {
    LfoTable sine;
    buildLfoTable(Shape{}, sine);
    LfoParams p;
    p.cyclesPerSample = 1.0 / 4800.0; // 10 Hz at 48 kHz
    LfoState s;

    SECTION("a sine by default, at the given rate and start phase") {
        lfoTrigger(s);
        CHECK(lfoAdvance(s, p, sine, 1200) == Approx(1.0).margin(1e-4));
        CHECK(lfoAdvance(s, p, sine, 1200) == Approx(0.0).margin(1e-4));
        CHECK(lfoAdvance(s, p, sine, 1200) == Approx(-1.0).margin(1e-4));
        CHECK(lfoAdvance(s, p, sine, 4800 * 7 + 1200) == Approx(0.0).margin(1e-4)); // whole cycles later
        p.phaseOffset = 0.25;
        lfoTrigger(s);
        CHECK(lfoAdvance(s, p, sine, 0) == Approx(1.0).margin(1e-4));
    }
    SECTION("delay holds it at rest, rise fades it in") {
        p.delaySamples = 2400;
        p.riseSamples = 4800;
        lfoTrigger(s);
        CHECK(lfoAdvance(s, p, sine, 2400) == 0.0f);
        CHECK(lfoAdvance(s, p, sine, 1200) == Approx(0.25).margin(1e-4)); // top of the cycle, a quarter faded in
        CHECK(lfoAdvance(s, p, sine, 4800) == Approx(1.0).margin(1e-4));  // fully faded in, at the top again
    }
    SECTION("unipolar runs from 0 to 1") {
        p.unipolar = true;
        lfoTrigger(s);
        CHECK(lfoAdvance(s, p, sine, 0) == Approx(0.5));
        CHECK(lfoAdvance(s, p, sine, 1200) == Approx(1.0).margin(1e-4));
        CHECK(lfoAdvance(s, p, sine, 2400) == Approx(0.0).margin(1e-4));
    }
    SECTION("one-shot plays once and holds the end") {
        Shape ramp;
        ramp.points = {{0.0, -1.0, 0.0}, {1.0, 0.75, 0.0}};
        LfoTable table;
        buildLfoTable(ramp, table);
        p.mode = LfoMode::OneShot;
        lfoTrigger(s);
        CHECK(lfoAdvance(s, p, table, 2400) == Approx(-0.125).margin(1e-3));
        CHECK(lfoAdvance(s, p, table, 2400) == Approx(0.75).margin(1e-3));
        CHECK(lfoAdvance(s, p, table, 100000) == Approx(0.75));
    }
    SECTION("loop-point plays through, then repeats from the marker") {
        Shape ramp;
        ramp.points = {{0.0, -1.0, 0.0}, {1.0, 1.0, 0.0}};
        ramp.loopStart = 0.5;
        LfoTable table;
        buildLfoTable(ramp, table);
        p.mode = LfoMode::LoopPoint;
        lfoTrigger(s);
        CHECK(lfoAdvance(s, p, table, 1200) == Approx(-0.5).margin(1e-3));
        CHECK(lfoAdvance(s, p, table, 3600 + 1200) == Approx(0.5).margin(1e-3)); // wrapped to 0.5, then 0.75
        for (int i = 0; i < 50; ++i)
            CHECK(lfoAdvance(s, p, table, 777) >= -0.001f); // never below the marker's value again
    }
    SECTION("smooth rounds a square without moving its rate") {
        Shape square;
        square.points = {{0.0, 1.0, 0.0}, {0.5, 1.0, 0.0}, {0.5, -1.0, 0.0}, {1.0, -1.0, 0.0}};
        LfoTable table;
        buildLfoTable(square, table);
        LfoState plain, smooth;
        LfoParams q = p;
        q.smooth = 0.5;
        lfoTrigger(plain);
        lfoTrigger(smooth);
        float largestPlain = 0.0f, largestSmooth = 0.0f, lastPlain = 1.0f, lastSmooth = 1.0f;
        int flipsPlain = 0, flipsSmooth = 0;
        for (int i = 0; i < 3000; ++i) {
            const float a = lfoAdvance(plain, p, table, 16), b = lfoAdvance(smooth, q, table, 16);
            largestPlain = std::max(largestPlain, std::abs(a - lastPlain));
            largestSmooth = std::max(largestSmooth, std::abs(b - lastSmooth));
            flipsPlain += (a > 0.0f) != (lastPlain > 0.0f);
            flipsSmooth += (b > 0.0f) != (lastSmooth > 0.0f);
            lastPlain = a;
            lastSmooth = b;
        }
        CHECK(largestPlain > 1.5f);
        CHECK(largestSmooth < 0.2f);
        CHECK(flipsSmooth == flipsPlain);
    }
    SECTION("audio-rate rendering agrees with stepping") {
        p.cyclesPerSample = 1000.0 / 48000.0;
        LfoState a, b;
        lfoTrigger(a);
        lfoTrigger(b);
        float out[64];
        lfoRender(a, p, sine, out, 64);
        for (int i = 0; i < 64; ++i)
            CHECK(out[i] == Approx(lfoAdvance(b, p, sine, 1)).margin(1e-6));
        CHECK(out[11] == Approx(std::sin(2.0 * kPi * 12.0 * 1000.0 / 48000.0)).margin(2e-4));
    }
}

TEST_CASE("flux: four kinds of random motion, repeatable from a seed", "[modulation][flux]") {
    for (const FluxType type : {FluxType::Orbit, FluxType::Burst, FluxType::Stepped, FluxType::Smooth}) {
        FluxState a, b, c;
        fluxReset(a, 42);
        fluxReset(b, 42);
        fluxReset(c, 43);
        float lowest = 1.0f, highest = -1.0f, largestStep = 0.0f, last = 0.0f;
        int changes = 0, differs = 0;
        for (int i = 0; i < 20000; ++i) {
            const float v = fluxAdvance(a, type, 0.01);
            REQUIRE(v == fluxAdvance(b, type, 0.01)); // the same seed, the same sequence
            differs += v != fluxAdvance(c, type, 0.01);
            REQUIRE(std::isfinite(v));
            lowest = std::min(lowest, v);
            highest = std::max(highest, v);
            if (i > 0) {
                largestStep = std::max(largestStep, std::abs(v - last));
                changes += v != last;
            }
            last = v;
        }
        INFO("type " << static_cast<int>(type));
        CHECK(lowest >= -1.0f);
        CHECK(highest <= 1.0f);
        CHECK(lowest < -0.4f); // uses the range, both ways
        CHECK(highest > 0.4f);
        CHECK(differs > 1000);
        if (type == FluxType::Stepped) {
            CHECK(changes > 150); // one new value per cycle: 200 cycles
            CHECK(changes <= 200);
        } else if (type == FluxType::Smooth || type == FluxType::Orbit) {
            CHECK(largestStep < 0.08f); // no jumps
        } else {
            CHECK(largestStep > 0.3f); // bursts jump
        }
    }
}

TEST_CASE("matrix: polarity, curves and Via", "[modulation][matrix]") {
    // Polarity: uni reads the source as it is, bi recentres a 0..1 source.
    CHECK(modShape(0.75f, false, Polarity::Uni, ModCurve::Linear, nullptr) == 0.75f);
    CHECK(modShape(0.75f, false, Polarity::Bi, ModCurve::Linear, nullptr) == 0.5f);
    CHECK(modShape(0.0f, false, Polarity::Bi, ModCurve::Linear, nullptr) == -1.0f);
    CHECK(modShape(-0.5f, true, Polarity::Bi, ModCurve::Linear, nullptr) == -0.5f);  // already centred
    CHECK(modShape(-0.5f, true, Polarity::Uni, ModCurve::Linear, nullptr) == -0.5f);
    CHECK(modShape(3.0f, false, Polarity::Uni, ModCurve::Linear, nullptr) == 1.0f);  // never past the ends

    // Curves keep 0 and 1, differ in between, and keep the sign of a negative value.
    for (const ModCurve curve : {ModCurve::Linear, ModCurve::Exp, ModCurve::Log, ModCurve::S}) {
        CHECK(modCurve(curve, 0.0f, nullptr) == Approx(0.0f).margin(1e-6));
        CHECK(modCurve(curve, 1.0f, nullptr) == Approx(1.0f));
    }
    CHECK(modCurve(ModCurve::Exp, 0.5f, nullptr) < 0.3f);
    CHECK(modCurve(ModCurve::Log, 0.5f, nullptr) > 0.7f);
    CHECK(modCurve(ModCurve::Exp, 0.5f, nullptr) == Approx(1.0f - modCurve(ModCurve::Log, 0.5f, nullptr)));
    CHECK(modCurve(ModCurve::S, 0.5f, nullptr) == 0.5f);
    CHECK(modCurve(ModCurve::S, 0.25f, nullptr) == Approx(0.15625f));
    CHECK(modShape(-0.5f, true, Polarity::Bi, ModCurve::Exp, nullptr) == Approx(-modCurve(ModCurve::Exp, 0.5f, nullptr)));

    // A drawn curve: a table from a shape.
    Shape drawn;
    drawn.points = {{0.0, 1.0, 0.0}, {0.5, 0.0, 0.0}, {1.0, 1.0, 0.0}}; // a "V"
    float table[kModCurveSize + 1];
    compileShape(drawn, table, kModCurveSize);
    CHECK(modCurve(ModCurve::Drawn, 0.0f, table) == 1.0f);
    CHECK(modCurve(ModCurve::Drawn, 0.5f, table) == Approx(0.0f).margin(1e-6));
    CHECK(modCurve(ModCurve::Drawn, 0.75f, table) == Approx(0.5f));
    CHECK(modCurve(ModCurve::Drawn, 0.3f, nullptr) == 0.3f);

    // Via scales, and can be turned around.
    CHECK(modVia(0.25f, false, false) == 0.25f);
    CHECK(modVia(0.25f, false, true) == 0.75f);
    CHECK(modVia(0.0f, true, false) == 0.5f); // a centred source is half way
    CHECK(modVia(-1.0f, true, false) == 0.0f);
    CHECK(modVia(5.0f, false, false) == 1.0f);
}

TEST_CASE("patch: shapes, tempo-linked times, multi-segment envelopes, drawn curves and tuning round-trip",
          "[patch][modulation]") {
    Patch patch;
    patch.lfoShapes[2].points = {{0.0, -1.0, 0.25}, {0.3, 1.0, 0.0}, {0.3, 0.0, 0.0}, {1.0, -1.0, -0.5}};
    patch.lfoShapes[2].loopStart = 0.3;
    patch.envSync[0][1] = "1/8";
    patch.envSync[3][4] = "3/16";
    patch.envShapes[1].points = {{0.0, "", 0.0, 0.0}, {12.5, "", 1.0, 0.4}, {0.0, "1/4T", 0.3, 0.0}, {250.0, "", 0.0, -0.2}};
    patch.envShapes[1].sustainPoint = 2;
    patch.envShapes[2].points = {{0.0, "", 0.0, 0.0}, {10.0, "", 1.0, 0.0}, {10.0, "", 0.0, 0.0}};
    patch.envShapes[2].loopStart = 0;
    patch.envShapes[2].loopEnd = 2;
    patch.warpCurves[0][1].points = {{0.0, 0.0, 0.6}, {1.0, 1.0, 0.0}};
    ModRoute route;
    route.source = "lfo3";
    route.dest = "filter1.cutoff";
    route.curve = ModCurve::Drawn;
    route.curveShape.points = {{0.0, 0.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, 0.25, 0.0}};
    patch.routes.push_back(route);
    patch.set("mod.1.amount", 0.5);
    patch.tuning.name = "Test";
    for (int i = 0; i < 128; ++i)
        patch.tuning.keys.push_back(i + 0.01 * (i % 12));

    for (const bool compact : {false, true}) {
        SaveOptions options;
        options.compact = compact;
        const std::string text = savePatch(patch, options);
        const LoadResult loaded = loadPatch(text);
        INFO(text.substr(0, 400));
        for (const auto& issue : loaded.issues)
            UNSCOPED_INFO(issue.path << ": " << issue.message);
        REQUIRE(loaded.ok());
        CHECK(loaded.issues.empty());
        CHECK(loaded.patch == patch);
        CHECK(savePatch(loaded.patch, options) == text); // byte-identical
    }
}

TEST_CASE("patch: mistakes in shapes, note lengths and tuning are reported with a path and a fix", "[patch][modulation]") {
    const auto issuesOf = [](const std::string& body) {
        return loadPatch("{\"format\":\"zederia.patch\",\"version\":1," + body + "}");
    };
    const auto has = [](const LoadResult& r, const char* code, const char* path) {
        for (const auto& i : r.issues)
            if (i.code == code && i.path == path && !i.fix.empty())
                return true;
        return false;
    };
    auto r = issuesOf("\"env1\":{\"sync\":{\"attack\":\"fast\"}}");
    CHECK_FALSE(r.ok());
    CHECK(has(r, "bad_note_length", "/env1/sync/attack"));
    r = issuesOf("\"env1\":{\"sync\":{\"atack\":\"1/8\"}}");
    CHECK(r.ok());
    CHECK(has(r, "unknown_field", "/env1/sync/atack"));
    r = issuesOf("\"env1\":{\"multi\":{\"points\":[]}}"); // envelope 1 has no multi-segment mode
    CHECK(has(r, "unknown_field", "/env1/multi"));
    r = issuesOf("\"lfo1\":{\"shape\":{\"points\":[{\"x\":0.5,\"y\":3},{\"x\":0.2,\"y\":0}]}}");
    CHECK(r.ok());
    CHECK(has(r, "out_of_range", "/lfo1/shape/points/0/y"));
    CHECK(has(r, "out_of_order", "/lfo1/shape/points/1/x"));
    CHECK(r.patch.lfoShapes[0].points[0].y == 1.0);
    CHECK(r.patch.lfoShapes[0].points[1].x == 0.5);
    r = issuesOf("\"lfo1\":{\"shape\":{\"points\":\"sine\"}}");
    CHECK(has(r, "wrong_type", "/lfo1/shape/points"));
    r = issuesOf("\"env2\":{\"multi\":{\"points\":[{\"time_ms\":5,\"level\":1}],\"loop\":[3,1],\"sustain_point\":9}}");
    CHECK(r.ok());
    CHECK(has(r, "out_of_range", "/env2/multi/sustain_point"));
    CHECK(r.patch.envShapes[1].loopStart == -1);
    r = issuesOf("\"tuning\":{\"name\":\"x\",\"keys\":[60,61]}");
    CHECK(has(r, "wrong_type", "/tuning/keys"));
    r = issuesOf("\"mod\":[{\"source\":\"lfo1\",\"dest\":\"osc1.pan\",\"curve\":\"drawn\",\"curve_shape\":{\"points\":[{\"x\":0,\"y\":-1}]}}]");
    CHECK(r.ok());
    CHECK(has(r, "out_of_range", "/mod/0/curve_shape/points/0/y"));
}
