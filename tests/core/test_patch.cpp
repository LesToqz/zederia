#include "../support/TestUtil.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/util/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace zed;
using zedtest::idx;

namespace {

const Issue* findIssue(const LoadResult& r, const std::string& code, const std::string& path) {
    for (const auto& i : r.issues)
        if (i.code == code && i.path == path)
            return &i;
    return nullptr;
}

std::string wrap(const std::string& body) {
    return "{\"format\":\"zederia.patch\",\"version\":1" + (body.empty() ? "" : "," + body) + "}";
}

// A patch with every kind of content set to seeded random values.
Patch randomPatch(std::uint64_t seed) {
    const auto& reg = Registry::get();
    Rng rng(seed);
    Patch p;
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        const int slots = static_cast<int>(rng.next() % (kNumFxSlots + 1));
        for (int s = 0; s < slots; ++s)
            p.addFxSlot(lane, static_cast<int>(rng.next() % reg.effectTypes().size()));
    }
    const int routes = static_cast<int>(rng.next() % (kNumModRoutes + 1));
    std::vector<std::string> dests;
    for (const auto& d : reg.params())
        if (d.modulatable)
            dests.push_back(d.id);
    for (int i = 0; i < routes; ++i) {
        ModRoute r;
        r.source = reg.modSources()[rng.next() % reg.modSources().size()].id;
        if (rng.next() % 3 == 0)
            r.via = rng.next() % 2 ? "cc" + std::to_string(rng.next() % 128) : std::string("modwheel");
        r.viaInvert = !r.via.empty() && rng.next() % 2;
        r.dest = dests[rng.next() % dests.size()];
        r.polarity = static_cast<Polarity>(rng.next() % 2);
        r.curve = static_cast<ModCurve>(rng.next() % 5);
        r.enabled = rng.next() % 4 != 0;
        r.mute = rng.next() % 4 == 0;
        r.solo = rng.next() % 8 == 0;
        p.routes.push_back(r);
    }
    for (int i = 0; i < reg.size(); ++i)
        p.set(i, p.effectiveDef(i).fromNormalized(rng.uniform())); // ignored where the patch has nothing
    for (auto& name : p.macroNames)
        if (rng.next() % 2)
            name = "Macro \xC3\xA9 " + std::to_string(rng.next() % 100);
    p.meta.name = "Random \"" + std::to_string(seed) + "\" \xE2\x99\xAA\n";
    p.meta.author = rng.next() % 2 ? "Tester" : "";
    p.meta.type = "pad";
    p.meta.tags = {"wide", "evolving"};
    if (rng.next() % 2)
        p.meta.genres = {"ambient"};
    p.meta.rating = static_cast<int>(rng.next() % 6);
    p.meta.notes = rng.next() % 2 ? "line one\nline two\ttabbed \\ backslash" : "";
    p.tables[0] = "factory:Basics/Saw Morph";
    if (rng.next() % 2)
        p.tables[2] = "user:My Tables/Thing 2";
    if (rng.next() % 2)
        p.textureSample = "factory:Noise/Pink";
    return p;
}

} // namespace

TEST_CASE("patch: the init patch holds every default", "[patch]") {
    const auto& reg = Registry::get();
    const Patch p;
    for (const auto& d : reg.params())
        CHECK(p.get(d.index) == d.def);
    CHECK(p.routes.empty());
    CHECK(p.fxSlotCount(0) == 0);
    CHECK(p.get("filter1.cutoff") == 2000.0);
    CHECK(p.get("unknown.id") == 0.0);
}

TEST_CASE("patch: set clamps to the range and reports what was stored", "[patch]") {
    Patch p;
    CHECK(p.set(idx("filter1.cutoff"), 1e9) == 22000.0);
    CHECK(p.set(idx("filter1.cutoff"), -5) == 8.0);
    CHECK(p.set(idx("global.polyphony"), 7.6) == 8.0);
    CHECK(p.set(idx("osc1.on"), 0.2) == 0.0);
    CHECK(p.set("osc1.level", -3.0));
    CHECK(p.get("osc1.level") == -3.0);
    CHECK_FALSE(p.set("osc9.level", 1.0));
}

TEST_CASE("patch: save, load, save is byte-identical for the init patch", "[patch][roundtrip]") {
    for (const bool compact : {false, true}) {
        SaveOptions options;
        options.compact = compact;
        const Patch original;
        const std::string first = savePatch(original, options);
        const LoadResult loaded = loadPatch(first);
        for (const auto& i : loaded.issues)
            INFO(i.path << ": " << i.message);
        REQUIRE(loaded.ok());
        CHECK(loaded.issues.empty());
        CHECK(loaded.patch == original);
        CHECK(savePatch(loaded.patch, options) == first);
    }
    // A compact init patch is little more than the header.
    CHECK(savePatch(Patch(), {true}).size() < 200);
}

TEST_CASE("patch: save, load, save is byte-identical for random patches", "[patch][roundtrip]") {
    for (std::uint64_t seed = 1; seed <= 150; ++seed) {
        const Patch original = randomPatch(seed);
        for (const bool compact : {false, true}) {
            SaveOptions options;
            options.compact = compact;
            const std::string first = savePatch(original, options);
            const LoadResult loaded = loadPatch(first);
            INFO("seed " << seed << (compact ? " compact" : " full"));
            for (const auto& i : loaded.issues)
                INFO(i.path << ": " << i.message);
            REQUIRE(loaded.ok());
            REQUIRE(loaded.issues.empty());
            REQUIRE(loaded.patch == original);
            REQUIRE(savePatch(loaded.patch, options) == first);
        }
    }
}

TEST_CASE("patch: the plan's example patch loads cleanly", "[patch]") {
    const auto& reg = Registry::get();
    const std::string text = zedtest::readText(zedtest::dataPath("patches/glass_pluck.zpreset"));
    REQUIRE(!text.empty());
    const LoadResult r = loadPatch(text);
    for (const auto& i : r.issues)
        INFO(i.path << ": " << i.message);
    REQUIRE(r.ok());
    CHECK(r.issues.empty());
    const Patch& p = r.patch;
    CHECK(p.meta.name == "Glass Pluck");
    CHECK(p.meta.type == "pluck");
    CHECK(p.meta.tags == std::vector<std::string>{"bright", "short"});
    CHECK(p.get("global.polyphony") == 16);
    CHECK(p.get("global.bend_up") == 2);
    CHECK(p.get("global.bend_down") == 2);
    CHECK(p.get("global.master") == -6.0);
    CHECK(p.tables[0] == "factory:Basics/Saw Morph");
    CHECK(p.get("osc1.position") == 0.25);
    CHECK(p.get("osc1.level") == -3.0);
    CHECK(p.get("osc1.unison.voices") == 5);
    CHECK(p.get("osc1.unison.detune") == 0.18);
    CHECK(p.get("osc1.warp_a.mode") == reg.find("osc1.warp_a.mode")->choiceIndex("bend_plus"));
    CHECK(p.get("osc1.warp_a.amount") == 0.3);
    CHECK(p.get("filter1.on") == 1);
    CHECK(p.get("filter1.type") == reg.find("filter1.type")->choiceIndex("ladder_lp24"));
    CHECK(p.get("filter1.cutoff") == 1800);
    CHECK(p.get("filter1.keytrack") == 0.5);
    CHECK(p.get("env1.decay") == 420);
    CHECK(p.get("env2.release") == 120);
    REQUIRE(p.routes.size() == 2);
    CHECK(p.routes[0].source == "env2");
    CHECK(p.routes[0].dest == "filter1.cutoff");
    CHECK(p.routes[0].polarity == Polarity::Uni);
    CHECK(p.get("mod.1.amount") == 0.6);
    CHECK(p.routes[1].via == "modwheel");
    CHECK(p.routes[1].polarity == Polarity::Bi);
    CHECK(p.get("mod.2.amount") == 0.4);
    REQUIRE(p.fxSlotCount(0) == 1);
    CHECK(p.fxSlotCount(1) == 0);
    REQUIRE(p.fxSlotCount(2) == 1);
    CHECK(p.fxSlotType(0, 0) == reg.effectIndex("chorus"));
    CHECK(p.get("fx.a1.mix") == 0.3);
    CHECK(p.fxSlotType(2, 0) == reg.effectIndex("reverb"));
    CHECK(p.get("fx.m1.mix") == 0.18);
    CHECK(p.macroNames[0] == "Bright");

    // Whatever is saved from it round-trips exactly.
    const std::string first = savePatch(p);
    const LoadResult again = loadPatch(first);
    REQUIRE(again.ok());
    CHECK(again.issues.empty());
    CHECK(again.patch == p);
    CHECK(savePatch(again.patch) == first);
}

TEST_CASE("patch: omitted fields take defaults", "[patch]") {
    const LoadResult r = loadPatch(wrap(""));
    REQUIRE(r.ok());
    CHECK(r.issues.empty());
    CHECK(r.patch == Patch());

    const LoadResult partial = loadPatch(wrap("\"filter1\":{\"cutoff_hz\":440}"));
    REQUIRE(partial.ok());
    Patch expected;
    expected.set("filter1.cutoff", 440);
    CHECK(partial.patch == expected);
}

TEST_CASE("patch: unknown fields are reported with a path and a suggestion", "[patch][validation]") {
    const LoadResult r = loadPatch(wrap("\"osc1\":{\"positon\":0.5,\"level_db\":-9},\"osc4\":{},\"a/b\":1"));
    REQUIRE(r.ok()); // warnings do not stop a load
    CHECK(r.patch.get("osc1.level") == -9.0);
    const Issue* typo = findIssue(r, "unknown_field", "/osc1/positon");
    REQUIRE(typo != nullptr);
    CHECK(typo->severity == Severity::Warning);
    CHECK(typo->fix.find("\"position\"") != std::string::npos);
    CHECK(findIssue(r, "unknown_field", "/osc4") != nullptr);
    CHECK(findIssue(r, "unknown_field", "/a~1b") != nullptr); // JSON-pointer escaping
    CHECK(r.issues.size() == 3);
}

TEST_CASE("patch: out-of-range values are clamped and the clamp is reported", "[patch][validation]") {
    const LoadResult r = loadPatch(wrap("\"filter1\":{\"cutoff_hz\":99999,\"resonance\":-2},\"global\":{\"polyphony\":7.6}"));
    REQUIRE(r.ok());
    CHECK(r.patch.get("filter1.cutoff") == 22000.0);
    CHECK(r.patch.get("filter1.resonance") == 0.0);
    CHECK(r.patch.get("global.polyphony") == 8.0);
    const Issue* clamp = findIssue(r, "out_of_range", "/filter1/cutoff_hz");
    REQUIRE(clamp != nullptr);
    CHECK(clamp->message.find("22000") != std::string::npos);
    CHECK(clamp->fix.find("8 to 22000 Hz") != std::string::npos);
    CHECK(findIssue(r, "out_of_range", "/filter1/resonance") != nullptr);
    CHECK(findIssue(r, "rounded", "/global/polyphony") != nullptr);
}

TEST_CASE("patch: wrong types and unknown choices are errors with a fix", "[patch][validation]") {
    const LoadResult r = loadPatch(wrap("\"osc1\":{\"on\":\"yes\",\"level_db\":\"loud\",\"engine\":3,\"unison\":7},"
                                        "\"filter1\":{\"type\":\"ladder_lp25\"},\"global\":{\"bend_range\":5},\"env1\":[]"));
    CHECK_FALSE(r.ok());
    const Issue* onOff = findIssue(r, "wrong_type", "/osc1/on");
    REQUIRE(onOff != nullptr);
    CHECK(onOff->severity == Severity::Error);
    CHECK(onOff->fix == "Use true or false.");
    CHECK(findIssue(r, "wrong_type", "/osc1/level_db") != nullptr);
    CHECK(findIssue(r, "wrong_type", "/osc1/engine") != nullptr);
    CHECK(findIssue(r, "wrong_type", "/osc1/unison") != nullptr);
    CHECK(findIssue(r, "wrong_type", "/global/bend_range") != nullptr);
    CHECK(findIssue(r, "wrong_type", "/env1") != nullptr);
    const Issue* choice = findIssue(r, "unknown_choice", "/filter1/type");
    REQUIRE(choice != nullptr);
    CHECK(choice->fix.find("Did you mean \"ladder_lp24\"?") != std::string::npos);
    CHECK(r.errors() == 7);

    const LoadResult bend = loadPatch(wrap("\"global\":{\"bend_range\":[12,7]}"));
    REQUIRE(bend.ok());
    CHECK(bend.patch.get("global.bend_up") == 12);
    CHECK(bend.patch.get("global.bend_down") == 7);
    CHECK(findIssue(loadPatch(wrap("\"global\":{\"bend_range\":[1,2,3]}")), "too_many_items", "/global/bend_range") != nullptr);
}

TEST_CASE("patch: modulation routes are validated", "[patch][validation]") {
    const LoadResult r = loadPatch(wrap(
        "\"mod\":[{\"source\":\"lfo9\",\"dest\":\"filter1.cutoff\"},"
        "{\"source\":\"lfo1\",\"dest\":\"filter1.cutof\"},"
        "{\"source\":\"lfo1\",\"dest\":\"osc1.on\"},"
        "{\"amount\":0.5},"
        "{\"source\":\"cc74\",\"via\":\"velocity\",\"via_invert\":true,\"dest\":\"osc2.pan\",\"amount\":-3,"
        "\"polarity\":\"sideways\",\"curve\":\"s\",\"enabled\":false,\"mute\":true,\"solo\":true,\"extra\":1}]"));
    CHECK_FALSE(r.ok());
    const Issue* source = findIssue(r, "unknown_source", "/mod/0/source");
    REQUIRE(source != nullptr);
    CHECK(source->fix.find("lfo1") != std::string::npos);
    const Issue* dest = findIssue(r, "unknown_dest", "/mod/1/dest");
    REQUIRE(dest != nullptr);
    CHECK(dest->fix.find("\"filter1.cutoff\"") != std::string::npos);
    CHECK(findIssue(r, "not_modulatable", "/mod/2/dest") != nullptr);
    CHECK(findIssue(r, "missing_field", "/mod/3") != nullptr);
    CHECK(findIssue(r, "out_of_range", "/mod/4/amount") != nullptr);
    CHECK(findIssue(r, "unknown_choice", "/mod/4/polarity") != nullptr);
    CHECK(findIssue(r, "unknown_field", "/mod/4/extra") != nullptr);
    REQUIRE(r.patch.routes.size() == 5);
    CHECK(r.patch.routes[4].source == "cc74");
    CHECK(r.patch.routes[4].via == "velocity");
    CHECK(r.patch.routes[4].viaInvert);
    CHECK(r.patch.routes[4].curve == ModCurve::S);
    CHECK_FALSE(r.patch.routes[4].enabled);
    CHECK(r.patch.routes[4].mute);
    CHECK(r.patch.routes[4].solo);
    CHECK(r.patch.get("mod.5.amount") == -1.0);

    std::string many = "\"mod\":[";
    for (int i = 0; i < kNumModRoutes + 1; ++i)
        many += std::string(i ? "," : "") + "{\"source\":\"lfo1\",\"dest\":\"osc1.pan\"}";
    const LoadResult tooMany = loadPatch(wrap(many + "]"));
    CHECK_FALSE(tooMany.ok());
    CHECK(findIssue(tooMany, "too_many_items", "/mod") != nullptr);
}

TEST_CASE("patch: effect slots use the loaded effect's names and ranges", "[patch][validation]") {
    const auto& reg = Registry::get();
    const LoadResult r = loadPatch(wrap("\"fx\":{\"b_routing\":\"series\",\"direct_balance\":0.5,"
                                        "\"a\":[{\"type\":\"delay\",\"on\":false,\"mix\":0.4,\"feedback\":2,\"time_l_ms\":250,"
                                        "\"mode\":\"ping_pong\",\"feedbak\":0.1}],"
                                        "\"master\":[{\"type\":\"limiter\",\"ceiling_db\":-1}]}"));
    for (const auto& i : r.issues)
        INFO(i.path << ": " << i.message);
    REQUIRE(r.ok());
    const Patch& p = r.patch;
    CHECK(p.get("fx.b_routing") == 1);
    CHECK(p.get("fx.direct_balance") == 0.5);
    REQUIRE(p.fxSlotType(0, 0) == reg.effectIndex("delay"));
    CHECK(p.get("fx.a1.on") == 0);
    CHECK(p.get("fx.a1.mix") == 0.4);
    const auto& delay = reg.effectTypes()[static_cast<std::size_t>(reg.effectIndex("delay"))];
    const auto param = [&](const char* key) {
        for (const auto& d : delay.params)
            if (d.pointer == key)
                return reg.fxSlotParam(0, 0, d.index);
        return -1;
    };
    CHECK(p.effectiveDef(param("feedback")).name == "Feedback");
    CHECK(p.get(param("feedback")) == 1.0); // clamped to the delay's own range
    CHECK(p.get(param("time_l_ms")) == 250.0);
    CHECK(p.get(param("mode")) == 1.0);
    CHECK(p.get(param("ducking")) == 0.0); // untouched parameters hold the effect's defaults
    CHECK(p.get(param("division_l")) == delay.params[4].def);
    CHECK(findIssue(r, "out_of_range", "/fx/a/0/feedback") != nullptr);
    const Issue* typo = findIssue(r, "unknown_field", "/fx/a/0/feedbak");
    REQUIRE(typo != nullptr);
    CHECK(typo->fix.find("\"feedback\"") != std::string::npos);
    CHECK(p.get("fx.m1.p1") == -1.0); // the limiter's ceiling, in dB
    CHECK(p.inUse(idx("fx.m1.p2")));
    CHECK_FALSE(p.inUse(idx("fx.m1.p3")));
    CHECK_FALSE(p.inUse(idx("fx.b1.p1")));

    const LoadResult bad = loadPatch(wrap("\"fx\":{\"a\":[{\"type\":\"reverbb\"},{\"mix\":1},7],\"c\":[]}"));
    CHECK_FALSE(bad.ok());
    const Issue* type = findIssue(bad, "unknown_choice", "/fx/a/0/type");
    REQUIRE(type != nullptr);
    CHECK(type->fix.find("\"reverb\"") != std::string::npos);
    CHECK(findIssue(bad, "missing_field", "/fx/a/1") != nullptr);
    CHECK(findIssue(bad, "wrong_type", "/fx/a/2") != nullptr);
    CHECK(findIssue(bad, "unknown_field", "/fx/c") != nullptr);

    std::string nine = "\"fx\":{\"a\":[";
    for (int i = 0; i < 9; ++i)
        nine += std::string(i ? "," : "") + "{\"type\":\"eq\"}";
    CHECK(findIssue(loadPatch(wrap(nine + "]}")), "too_many_items", "/fx/a") != nullptr);
}

TEST_CASE("patch: a slot's generic parameters follow the loaded effect", "[patch]") {
    const auto& reg = Registry::get();
    Patch p;
    const int generic = reg.fxSlotParam(1, 0, 1); // fx.b1.p2
    CHECK(p.set(generic, 0.7) == 0.0);            // no effect loaded: the value has no meaning
    REQUIRE(p.addFxSlot(1, reg.effectIndex("compressor")) == 0);
    CHECK(p.effectiveDef(generic).name == "Threshold");
    CHECK(p.get(generic) == -18.0);
    CHECK(p.set(generic, -100.0) == -60.0);
    for (int i = 0; i < kNumFxSlots - 1; ++i)
        CHECK(p.addFxSlot(1, 0) == i + 1);
    CHECK(p.addFxSlot(1, 0) == -1); // the lane is full
    p.clearFx(1);
    CHECK(p.fxSlotCount(1) == 0);
    CHECK(p == Patch());
}

TEST_CASE("patch: meta and macros are validated", "[patch][validation]") {
    const std::string longName(500, 'x');
    const LoadResult r = loadPatch(wrap("\"meta\":{\"name\":\"" + longName + "\",\"tags\":[\"a\",3],\"rating\":9,\"colour\":\"red\"},"
                                        "\"macros\":[{\"name\":\"Bright\",\"value\":0.5,\"bipolar\":true},{\"value\":\"x\"}]"));
    CHECK_FALSE(r.ok());
    CHECK(r.patch.meta.name.size() == 128);
    CHECK(findIssue(r, "too_long", "/meta/name") != nullptr);
    CHECK(findIssue(r, "wrong_type", "/meta/tags/1") != nullptr);
    CHECK(findIssue(r, "out_of_range", "/meta/rating") != nullptr);
    CHECK(r.patch.meta.rating == 5);
    CHECK(findIssue(r, "unknown_field", "/meta/colour") != nullptr);
    CHECK(r.patch.macroNames[0] == "Bright");
    CHECK(r.patch.get("macro1.value") == 0.5);
    CHECK(r.patch.get("macro1.bipolar") == 1.0);
    CHECK(findIssue(r, "wrong_type", "/macros/1/value") != nullptr);

    // Cutting a long name never splits a multi-byte character.
    std::string accented;
    for (int i = 0; i < 100; ++i)
        accented += "\xC3\xA9";
    const LoadResult cut = loadPatch(wrap("\"meta\":{\"name\":\"" + accented + "\"}"));
    REQUIRE(cut.ok());
    CHECK(cut.patch.meta.name.size() == 128);
    CHECK(loadPatch(savePatch(cut.patch)).ok());
}

TEST_CASE("patch: documents that are not patches are refused with a reason", "[patch][validation]") {
    const auto only = [](const std::string& text) {
        const LoadResult r = loadPatch(text);
        REQUIRE(r.issues.size() == 1);
        CHECK_FALSE(r.ok());
        CHECK(!r.issues[0].message.empty());
        CHECK(!r.issues[0].fix.empty());
        return r.issues[0];
    };
    CHECK(only("").code == "bad_json");
    CHECK(only("{\"format\": \"zederia.patch\", ").code == "bad_json");
    CHECK(only("{\"format\":\"zederia.patch\",\"version\":1e999}").code == "bad_json");
    CHECK(only("[1,2,3]").code == "wrong_type");
    CHECK(only("42").path.empty());
    CHECK(only("{}").code == "missing_field");
    CHECK(only("{}").path == "/format");
    CHECK(only("{\"format\":\"something.else\",\"version\":1}").code == "bad_format");
    CHECK(only("{\"format\":7,\"version\":1}").code == "bad_format");
    CHECK(only("{\"format\":\"zederia.patch\"}").path == "/version");
    CHECK(only("{\"format\":\"zederia.patch\",\"version\":\"1\"}").code == "bad_version");
    CHECK(only("{\"format\":\"zederia.patch\",\"version\":0}").code == "bad_version");
    CHECK(only("{\"format\":\"zederia.patch\",\"version\":1.5}").code == "bad_version");
    const Issue newer = only("{\"format\":\"zederia.patch\",\"version\":2}");
    CHECK(newer.code == "version_too_new");
    CHECK(newer.fix.find("newer Zederia") != std::string::npos);
}

TEST_CASE("patch: every issue names a path and a fix", "[patch][validation]") {
    const LoadResult r = loadPatch(wrap("\"osc1\":{\"on\":1,\"bogus\":2,\"fine_cents\":900},\"mod\":[{}],\"fx\":{\"a\":[{}]},"
                                        "\"meta\":{\"rating\":\"x\"},\"macros\":{},\"texture\":{\"sample\":{}},\"osc2\":{\"table\":7}"));
    REQUIRE(r.issues.size() >= 9);
    for (const auto& i : r.issues) {
        INFO(i.code << " at " << i.path);
        CHECK(!i.code.empty());
        CHECK(!i.message.empty());
        CHECK(!i.fix.empty());
        CHECK((i.path.empty() || i.path[0] == '/'));
        // The path points at something in (or to be added to) the document.
        CHECK(i.path.find("//") == std::string::npos);
    }
    CHECK(findIssue(r, "missing_field", "/texture/sample") != nullptr);
    CHECK(findIssue(r, "wrong_type", "/osc2/table") != nullptr);
}

TEST_CASE("patch: migration runs step by step", "[patch][migration]") {
    using J = nlohmann::ordered_json;
    const std::vector<MigrationStep> steps = {
        {1, [](J& d) { d["renamed"] = d["old"]; d.erase("old"); }},
        {2, [](J& d) { d["renamed"] = d["renamed"].get<int>() * 10; }},
    };
    std::vector<Issue> issues;
    J doc = {{"format", "zederia.patch"}, {"version", 1}, {"old", 4}};
    REQUIRE(migratePatch(doc, steps, 3, issues));
    CHECK(issues.empty());
    CHECK(doc["version"] == 3);
    CHECK(doc["renamed"] == 40);
    CHECK_FALSE(doc.contains("old"));

    J current = {{"version", 3}};
    CHECK(migratePatch(current, steps, 3, issues)); // nothing to do
    CHECK(issues.empty());

    J tooNew = {{"version", 4}};
    CHECK_FALSE(migratePatch(tooNew, steps, 3, issues));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "version_too_new");
    CHECK(issues[0].path == "/version");

    issues.clear();
    J gap = {{"version", 1}};
    CHECK_FALSE(migratePatch(gap, std::span<const MigrationStep>(steps).subspan(1), 3, issues));
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].code == "migration_missing");

    issues.clear();
    J noVersion = J::object();
    CHECK_FALSE(migratePatch(noVersion, steps, 3, issues));
    CHECK(issues[0].code == "missing_field");
}
