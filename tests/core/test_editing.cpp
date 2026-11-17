// The core parts behind the interface (milestone 5): embedded tables in patches, typed values with
// units, the undo history, Randomize / Mutate / Blend, module and shape copy and paste, the preset
// index, and incremental table builds.
#include "../support/TestUtil.h"
#include "zederia/content/PresetIndex.h"
#include "zederia/params/ValueText.h"
#include "zederia/patch/History.h"
#include "zederia/patch/Modules.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/patch/Variation.h"
#include "zederia/table/TableData.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Compress.h"
#include "zederia/util/Image.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <memory>

using namespace zed;
using Catch::Approx;
using zedtest::idx;

namespace {

double parsed(const char* id, const char* text, ValueContext context = {}) {
    ParsedValue out;
    std::string error;
    INFO(id << " <- \"" << text << "\"");
    REQUIRE(parseValueText(*Registry::get().find(id), text, context, out, error));
    return out.plain;
}

bool rejects(const char* id, const char* text) {
    ParsedValue out;
    std::string error;
    const bool ok = parseValueText(*Registry::get().find(id), text, {}, out, error);
    return !ok && !error.empty();
}

Wavetable sawTable(int frames) {
    Wavetable t;
    t.name = "Test Saw";
    for (int f = 0; f < frames; ++f) {
        Frame frame{};
        for (int i = 0; i < kFrameSize; ++i)
            frame[static_cast<std::size_t>(i)] = static_cast<float>((1.0 - 2.0 * i / kFrameSize) * (1.0 - 0.5 * f / std::max(1, frames)));
        t.frames.push_back(frame);
    }
    return t;
}

} // namespace

TEST_CASE("editing: deflate and base64 round trip", "[editing][compress]") {
    Rng rng(7);
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{100}, std::size_t{70000}}) {
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i)
            data[i] = static_cast<std::uint8_t>(i % 7 == 0 ? rng.next() : (i / 13) & 0xff); // repeats and noise
        const auto packed = deflateZlib(data);
        std::vector<std::uint8_t> back;
        REQUIRE(inflateZlib(packed, back, size + 16));
        CHECK(back == data);
        std::vector<std::uint8_t> decoded;
        REQUIRE(base64Decode(base64Encode(data), decoded));
        CHECK(decoded == data);
    }
    std::vector<std::uint8_t> out;
    CHECK_FALSE(base64Decode("ab$d", out));
    const std::vector<float> values = {0.0f, -1.5f, 3.25f, 1e-20f};
    std::vector<float> restored;
    REQUIRE(unshuffleFloats(shuffleFloats(values), restored));
    CHECK(restored == values);
}

TEST_CASE("editing: a table kept in the patch survives save and load, byte-identically", "[editing][patch]") {
    Patch patch;
    patch.tables[1] = "factory:Basics/Saw Morph";
    patch.tableData[1].table = std::make_shared<Wavetable>(sawTable(16));
    auto morphing = std::make_shared<Wavetable>(sawTable(3));
    morphing->morph = MorphMode::Spectral;
    morphing->stepped = true;
    patch.tableData[2].table = morphing; // a table with no file behind it
    patch.meta.favourite = true;
    const std::string saved = savePatch(patch);
    const LoadResult loaded = loadPatch(saved);
    REQUIRE(loaded.ok());
    CHECK(loaded.issues.empty());
    CHECK(loaded.patch.tables[1] == "factory:Basics/Saw Morph");
    REQUIRE(loaded.patch.tableData[1]);
    CHECK(*loaded.patch.tableData[1].table == *patch.tableData[1].table);
    REQUIRE(loaded.patch.tableData[2]);
    CHECK(loaded.patch.tableData[2].table->morph == MorphMode::Spectral);
    CHECK(loaded.patch.tableData[2].table->stepped);
    CHECK(loaded.patch.tables[2].empty());
    CHECK(loaded.patch.meta.favourite);
    CHECK(loaded.patch == patch);
    CHECK(savePatch(loaded.patch) == saved);
    // Smaller than the raw floats (the planes compress).
    CHECK(saved.size() < 16u * kFrameSize * 4u);

    // Damaged data is an error with a path, never a crash.
    std::string broken = saved;
    const auto at = broken.find("\"samples\": \"") + 20;
    broken[at] = '!';
    const LoadResult bad = loadPatch(broken);
    CHECK_FALSE(bad.ok());
    bool pathFound = false;
    for (const auto& issue : bad.issues)
        pathFound = pathFound || issue.path.find("/osc2/table/data") == 0;
    CHECK(pathFound);
}

TEST_CASE("editing: typed values understand units, note lengths and note names", "[editing][values]") {
    CHECK(parsed("filter1.cutoff", "2k") == Approx(2000.0));
    CHECK(parsed("filter1.cutoff", "2 kHz") == Approx(2000.0));
    CHECK(parsed("filter1.cutoff", "440hz") == Approx(440.0));
    CHECK(parsed("filter1.cutoff", "A4") == Approx(440.0));
    CHECK(parsed("filter1.cutoff", "a3", {120.0, 432.0}) == Approx(216.0));
    CHECK(parsed("filter1.cutoff", "C4") == Approx(261.6256).epsilon(1e-5));
    CHECK(parsed("filter1.cutoff", "99999") == Approx(22000.0)); // clamped
    CHECK(parsed("env2.attack", "250 ms") == Approx(250.0));
    CHECK(parsed("env2.attack", "1.5 s") == Approx(1500.0));
    CHECK(parsed("env2.attack", "1/8", {120.0}) == Approx(250.0));
    CHECK(parsed("env2.attack", "1/4T", {120.0}) == Approx(1000.0 / 3.0));
    CHECK(parsed("env2.attack", "1/8D", {100.0}) == Approx(450.0));
    CHECK(parsed("env2.attack", "3/16", {120.0}) == Approx(375.0));
    CHECK(parsed("env2.attack", "1 bar", {120.0}) == Approx(2000.0));
    CHECK(parsed("lfo1.rate", "1/4", {120.0}) == Approx(2.0));
    CHECK(parsed("lfo1.rate", "500 ms") == Approx(2.0));
    CHECK(parsed("osc1.level", "-6 dB") == Approx(-6.0));
    CHECK(parsed("osc1.level", "-inf") == Approx(-72.0));
    CHECK(parsed("osc1.level", "50%") == Approx(-6.0206).epsilon(1e-4));
    CHECK(parsed("osc1.position", "25 %") == Approx(0.25));
    CHECK(parsed("osc1.position", "25") == Approx(0.25));
    CHECK(parsed("osc1.pan", "-50") == Approx(-0.5));
    CHECK(parsed("osc1.coarse", "+7 st") == Approx(7.0));
    CHECK(parsed("osc1.coarse", "1 oct") == Approx(12.0));
    CHECK(parsed("osc1.coarse", "50 cents") == Approx(0.5));
    CHECK(parsed("osc1.fine", "0.5 st") == Approx(50.0));
    CHECK(parsed("osc1.fine", "12c") == Approx(12.0));
    CHECK(parsed("osc1.phase", "90 deg") == Approx(90.0));
    CHECK(parsed("osc1.unison.voices", "7") == Approx(7.0));
    CHECK(parsed("filter1.type", "Ladder LP 24") == Approx(Registry::get().find("filter1.type")->choiceIndex("ladder_lp24")));
    CHECK(parsed("filter1.type", "ladder_lp24") == Approx(Registry::get().find("filter1.type")->choiceIndex("ladder_lp24")));
    CHECK(parsed("lfo1.division", "1/16t") == Approx(Registry::get().find("lfo1.division")->choiceIndex("1/16T")));
    CHECK(parsed("osc1.on", "off") == 0.0);
    CHECK(rejects("filter1.cutoff", "loud"));
    CHECK(rejects("osc1.level", "3 Hz"));
    CHECK(rejects("filter1.type", "no such filter"));
    CHECK(rejects("osc1.position", ""));

    // Shown the way they are typed.
    const auto shown = [](const char* id, double v) { return formatValueText(*Registry::get().find(id), v); };
    CHECK(shown("filter1.cutoff", 2000.0) == "2.00 kHz");
    CHECK(shown("filter1.cutoff", 440.0) == "440 Hz");
    CHECK(shown("env2.attack", 250.0) == "250 ms");
    CHECK(shown("env2.attack", 1500.0) == "1.50 s");
    CHECK(shown("osc1.level", -72.0) == "-inf dB");
    CHECK(shown("osc1.level", -6.0) == "-6.0 dB");
    CHECK(shown("osc1.position", 0.25) == "25 %");
    CHECK(shown("osc1.pan", 0.5) == "+50 %");
    CHECK(shown("osc1.coarse", 7.0) == "+7 st");
    CHECK(shown("filter1.type", Registry::get().find("filter1.type")->choiceIndex("ladder_lp24")) == "Ladder LP 24");
    CHECK(shown("osc1.warp_a.mode", Registry::get().find("osc1.warp_a.mode")->choiceIndex("bend_plus")) == "Bend +");
    CHECK(shown("chord.key", Registry::get().find("chord.key")->choiceIndex("f_sharp")) == "F#");
    // Every value of every parameter reads back from its own text.
    const auto& reg = Registry::get();
    for (const auto& def : reg.params()) {
        for (const double n : {0.0, 0.37, 1.0}) {
            const double v = def.fromNormalized(n);
            const std::string text = formatValueText(def, v);
            ParsedValue back;
            std::string error;
            INFO(def.id << " " << v << " shown as \"" << text << "\"");
            REQUIRE(parseValueText(def, text, {}, back, error));
            const double tolerance = def.stepped() ? 0.0 : std::max(1e-6, 0.01 * std::abs(v)) + 0.006 * (def.max - def.min);
            CHECK(std::abs(back.plain - v) <= tolerance + 1e-9);
        }
    }
    double note = 0.0;
    CHECK(parseNoteName("Eb2", note));
    CHECK(note == 39.0);
    CHECK(noteName(60) == "C4");
    CHECK(noteName(61) == "C#4");
}

TEST_CASE("editing: history undoes and redoes named steps and folds in outside changes", "[editing][history]") {
    History history;
    Patch a;
    history.reset(a, "Start");
    Patch b = a;
    b.set(idx("osc1.position"), 0.5);
    CHECK(history.push("Osc 1 Position", b));
    CHECK_FALSE(history.push("Nothing changed", b));
    Patch c = b;
    c.routes.push_back({"lfo1", "", "filter1.cutoff"});
    c.set(idx("mod.1.amount"), 0.4);
    CHECK(history.push("Add route", c));
    CHECK(history.size() == 3);
    CHECK(history.undoLabel() == "Add route");
    REQUIRE(history.undo() != nullptr);
    CHECK(history.currentState() == b);
    CHECK(history.redoLabel() == "Add route");
    REQUIRE(history.redo() != nullptr);
    CHECK(history.currentState() == c);
    REQUIRE(history.jumpTo(0) != nullptr);
    CHECK(history.currentState() == a);
    // A new step drops what could have been redone.
    Patch d = a;
    d.set(idx("filter1.cutoff"), 500.0);
    history.push("Cutoff", d);
    CHECK(history.size() == 2);
    CHECK_FALSE(history.canRedo());
    // Host automation moved something: folded into the current step, not undone by the next undo.
    Patch automated = d;
    automated.set(idx("osc1.level"), -12.0);
    history.absorb(automated);
    Patch e = automated;
    e.set(idx("filter1.resonance"), 0.5);
    history.push("Resonance", e);
    history.undo();
    CHECK(history.currentState().get(idx("osc1.level")) == -12.0);

    // Bounded by memory: big tables push the oldest steps out, never the current one.
    History small(std::size_t{4} << 20);
    Patch big;
    small.reset(big);
    for (int i = 0; i < 20; ++i) {
        big.tableData[0].table = std::make_shared<Wavetable>(sawTable(64)); // 0.5 MB each, not shared
        big.set(idx("osc1.position"), i / 40.0);
        small.push("Edit " + std::to_string(i), big);
    }
    CHECK(small.memoryBytes() <= (std::size_t{4} << 20) + (std::size_t{1} << 20));
    CHECK(small.dropped() > 0);
    CHECK(small.currentState() == big);
    CHECK(small.canUndo());
}

TEST_CASE("editing: Randomize, Mutate and Blend respect scope and locks and stay playable", "[editing][variation]") {
    const auto& reg = Registry::get();
    Patch base;
    base.routes.push_back({"lfo1", "", "osc1.position"});
    base.set(idx("mod.1.amount"), 0.3);
    const auto lockedBits = std::make_unique<bool[]>(static_cast<std::size_t>(reg.size()));
    lockedBits[static_cast<std::size_t>(idx("filter1.cutoff"))] = true;
    const std::span<const bool> locked(lockedBits.get(), static_cast<std::size_t>(reg.size()));

    Patch random = base;
    VariationScope scope;
    scope.effects = false;
    randomizePatch(random, scope, 1.0, locked, 42);
    int changed = 0;
    for (int i = 0; i < reg.size(); ++i) {
        if (random.get(i) == base.get(i))
            continue;
        ++changed;
        const VariationGroup group = variationGroup(i);
        INFO(reg.at(i).id);
        CHECK(group != VariationGroup::None);
        CHECK(group != VariationGroup::Effects);
        CHECK(group != VariationGroup::Play);
    }
    CHECK(changed > 100);
    CHECK(random.get(idx("filter1.cutoff")) == base.get(idx("filter1.cutoff")));
    CHECK(random.get(idx("global.quality")) == base.get(idx("global.quality")));
    CHECK(random.get(idx("osc1.on")) == base.get(idx("osc1.on")));
    CHECK(random.get(idx("osc1.level")) >= -24.0);
    CHECK(random.get(idx("osc1.unison.voices")) <= 8.0);
    CHECK(random.get(idx("env1.attack")) <= 2000.0);
    // Deterministic, and amount 0 changes nothing.
    Patch again = base;
    randomizePatch(again, scope, 1.0, locked, 42);
    CHECK(again == random);
    Patch none = base;
    randomizePatch(none, scope, 0.0, locked, 42);
    CHECK(none == base);

    Patch mutated = base;
    mutatePatch(mutated, 1.0, locked, 9);
    int moved = 0;
    for (int i = 0; i < reg.size(); ++i) {
        if (reg.at(i).type != ParamType::Float || mutated.get(i) == base.get(i))
            continue;
        ++moved;
        const ParamDef& def = reg.at(i);
        CHECK(std::abs(def.toNormalized(mutated.get(i)) - def.toNormalized(base.get(i))) <= 0.0601);
    }
    CHECK(moved > 50);
    CHECK(mutated.get(idx("filter1.cutoff")) == base.get(idx("filter1.cutoff")));

    Patch p1 = base, p2;
    p1.set(idx("osc1.position"), 0.9);
    p1.lfoShapes[0].points = {{0.0, -1.0, 0.0}, {1.0, 1.0, 0.0}};
    p2.set(idx("osc1.position"), 0.1);
    p2.set(idx("filter1.cutoff"), 300.0);
    p2.addFxSlot(0, reg.effectIndex("reverb"));
    const Patch parents[] = {p1, p2};
    bool sawEach[2] = {false, false};
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        const Patch child = blendPatches(base, parents, locked, seed);
        const double position = child.get(idx("osc1.position"));
        CHECK((position == 0.9 || position == 0.1));
        sawEach[position == 0.9 ? 0 : 1] = true;
        CHECK(child.get(idx("filter1.cutoff")) == base.get(idx("filter1.cutoff"))); // locked
        CHECK((child.lfoShapes[0] == p1.lfoShapes[0] || child.lfoShapes[0].empty())); // an LFO comes whole, with its shape
        CHECK((child.fxSlotCount(0) == 0 || child.fxSlotType(0, 0) == reg.effectIndex("reverb")));
    }
    CHECK(sawEach[0]);
    CHECK(sawEach[1]);
}

TEST_CASE("editing: modules copy and paste between like modules, shapes between kinds", "[editing][modules]") {
    const auto& reg = Registry::get();
    Patch patch;
    patch.set(idx("lfo2.rate"), 7.5);
    patch.set(idx("lfo2.mode"), reg.find("lfo2.mode")->choiceIndex("one_shot"));
    patch.lfoShapes[1].points = {{0.0, 0.0, 0.0}, {0.5, 1.0, 0.3}, {1.0, -1.0, 0.0}};
    const std::string lfo = copyModule(patch, "lfo2");
    CHECK(moduleDocumentClass(lfo) == "lfo");
    std::vector<Issue> issues;
    REQUIRE(pasteModule(lfo, patch, "lfo7", issues));
    CHECK(patch.get(idx("lfo7.rate")) == Approx(7.5));
    CHECK(patch.get(idx("lfo7.mode")) == patch.get(idx("lfo2.mode")));
    CHECK(patch.lfoShapes[6] == patch.lfoShapes[1]);
    // Not into another kind of module.
    issues.clear();
    Patch before = patch;
    CHECK_FALSE(pasteModule(lfo, patch, "env3", issues));
    CHECK_FALSE(issues.empty());
    CHECK(patch == before);
    CHECK_FALSE(pasteModule("not json", patch, "lfo1", issues));

    // An oscillator takes its table, embedded data and warp curves along.
    patch.tables[0] = "factory:Basics/Saw Morph";
    patch.tableData[0].table = std::make_shared<Wavetable>(sawTable(4));
    patch.warpCurves[0][1].points = {{0.0, 0.0, 0.0}, {1.0, 1.0, -0.5}};
    patch.set(idx("osc1.unison.voices"), 5);
    issues.clear();
    REQUIRE(pasteModule(copyModule(patch, "osc1"), patch, "osc3", issues));
    CHECK(patch.tables[2] == patch.tables[0]);
    CHECK(patch.tableData[2] == patch.tableData[0]);
    CHECK(patch.warpCurves[2][1] == patch.warpCurves[0][1]);
    CHECK(patch.get(idx("osc3.unison.voices")) == 5);

    // Envelopes: a multi-segment shape travels between envelopes 2..4; the amp envelope takes the times only.
    patch.set(idx("env2.attack"), 123.0);
    patch.set(idx("env2.mode"), 1);
    patch.envShapes[1].points = {{10.0, "", 1.0, 0.0}, {200.0, "", 0.2, 0.4}};
    patch.envShapes[1].sustainPoint = 1;
    patch.envSync[1][4] = "1/8";
    issues.clear();
    REQUIRE(pasteModule(copyModule(patch, "env2"), patch, "env4", issues));
    CHECK(patch.envShapes[3] == patch.envShapes[1]);
    CHECK(patch.envSync[3][4] == "1/8");
    issues.clear();
    REQUIRE(pasteModule(copyModule(patch, "env2"), patch, "env1", issues));
    CHECK(patch.get(idx("env1.attack")) == Approx(123.0));

    // Effect slots and the rack, clips and patterns.
    patch.addFxSlot(0, reg.effectIndex("chorus"));
    patch.set(reg.fxSlotMix(0, 0), 0.3);
    issues.clear();
    REQUIRE(pasteModule(copyModule(patch, "fx.a1"), patch, "fx.m1", issues));
    CHECK(patch.fxSlotType(2, 0) == reg.effectIndex("chorus"));
    CHECK(patch.get(reg.fxSlotMix(2, 0)) == Approx(0.3));
    patch.clips[2].notes = {{0.0, 1.0, 60, 0.5}, {1.0, 0.5, 64, 0.9}};
    patch.clips[2].name = "Riff";
    issues.clear();
    REQUIRE(pasteModule(copyModule(patch, "clip3"), patch, "clip8", issues));
    CHECK(patch.clips[7] == patch.clips[2]);
    patch.arpPatterns[0].steps = {{StepState::On, 0.5, 12, 2}, {StepState::Rest, 1.0, 0, 1}};
    issues.clear();
    REQUIRE(pasteModule(copyModule(patch, "arp_pattern1"), patch, "arp_pattern12", issues));
    CHECK(patch.arpPatterns[11] == patch.arpPatterns[0]);
    CHECK(moduleClass("fx.b8") == "fx_slot");
    CHECK(moduleClass("fx.b9").empty());
    CHECK(moduleClass("lfo9").empty());

    // Shapes: an LFO shape into a warp curve maps -1..1 to 0..1, and into an envelope.
    Shape curve;
    issues.clear();
    REQUIRE(pasteShape(copyShape(patch.lfoShapes[1], ShapeKind::Lfo), ShapeKind::Curve, curve, issues));
    REQUIRE(curve.points.size() == 3);
    CHECK(curve.points[1].y == Approx(1.0));
    CHECK(curve.points[2].y == Approx(0.0));
    CHECK(curve.points[1].curve == Approx(0.3));
    EnvShape env;
    issues.clear();
    REQUIRE(pasteEnvShape(copyShape(curve, ShapeKind::Curve), env, issues));
    REQUIRE(env.points.size() == 3);
    CHECK(env.points[1].timeMs == Approx(500.0));
    Shape back;
    issues.clear();
    REQUIRE(pasteShape(copyEnvShape(patch.envShapes[1]), ShapeKind::Lfo, back, issues));
    REQUIRE(back.points.size() == 2);
    CHECK(back.points[1].x == Approx(1.0));
    CHECK(back.points[1].y == Approx(0.2 * 2.0 - 1.0));
}

TEST_CASE("editing: the preset index searches, filters, sorts, counts facets and finds duplicates", "[editing][library]") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("zederia_test_index_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto write = [&](const std::string& rel, const Patch& patch) {
        const fs::path file = root / "Presets" / rel;
        fs::create_directories(file.parent_path());
        std::string error;
        const std::string text = savePatch(patch);
        REQUIRE(writeFile(file.string(), std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), error));
    };
    Patch a;
    a.meta.name = "Warm Pad";
    a.meta.type = "pad";
    a.meta.tags = {"warm", "wide"};
    a.meta.author = "Zed";
    a.meta.rating = 4;
    a.meta.favourite = true;
    Patch b;
    b.meta.name = "Bright Bass";
    b.meta.type = "bass";
    b.meta.tags = {"bright"};
    b.meta.genres = {"house"};
    b.meta.pack = "Night Drive";
    b.set(idx("filter1.cutoff"), 5000.0);
    Patch c = a; // the same sound under another name
    c.meta.name = "Copy of Warm Pad";
    c.meta.rating = 1;
    c.meta.favourite = false;
    write("Pads/warm.zpreset", a);
    write("Basses/bright.zpreset", b);
    write("copy.zpreset", c);
    {
        const fs::path junk = root / "Presets" / "broken.zpreset";
        std::string error;
        const std::string text = "{ not a patch";
        REQUIRE(writeFile(junk.string(), std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), error));
    }
    std::vector<std::string> problems;
    const auto presets = scanPresets("", root.string(), &problems);
    REQUIRE(presets.size() == 3);
    CHECK(problems.size() == 1);
    std::vector<int> all = {0, 1, 2};
    const auto matching = [&](const char* text) {
        const PresetQuery q = parsePresetQuery(text);
        std::vector<std::string> names;
        for (const int i : all)
            if (presetMatches(presets[static_cast<std::size_t>(i)], q))
                names.push_back(presets[static_cast<std::size_t>(i)].meta.name);
        std::sort(names.begin(), names.end());
        return names;
    };
    CHECK(matching("pad") == std::vector<std::string>{"Copy of Warm Pad", "Warm Pad"});
    CHECK(matching("tag:bright") == std::vector<std::string>{"Bright Bass"});
    CHECK(matching("tag:warm fav") == std::vector<std::string>{"Warm Pad"});
    CHECK(matching("pack:\"night drive\"") == std::vector<std::string>{"Bright Bass"});
    CHECK(matching("genre:house BASS") == std::vector<std::string>{"Bright Bass"});
    CHECK(matching("rating:3") == std::vector<std::string>{"Warm Pad"});
    CHECK(matching("basses").size() == 1); // the folder name counts
    CHECK(matching("user").size() == 3);
    CHECK(matching("factory").empty());
    std::vector<int> order = all;
    sortPresets(presets, order, PresetSort::Rating, true);
    CHECK(presets[static_cast<std::size_t>(order[0])].meta.name == "Warm Pad");
    sortPresets(presets, order, PresetSort::Type, false, PresetSort::Rating);
    CHECK(presets[static_cast<std::size_t>(order[0])].meta.type == "bass");
    CHECK(presets[static_cast<std::size_t>(order[1])].meta.name == "Copy of Warm Pad"); // pads, lower rating first
    const auto tags = presetFacet(presets, all, "tag");
    CHECK(tags.at("warm") == 2);
    CHECK(tags.at("bright") == 1);
    const auto dupes = duplicatePresets(presets);
    REQUIRE(dupes.size() == 1);
    CHECK(dupes[0].size() == 2);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("editing: an incremental table build equals a full one and reuses unchanged frames", "[editing][table]") {
    for (const MorphMode mode : {MorphMode::None, MorphMode::Spectral}) {
        Wavetable table = sawTable(32);
        table.morph = mode;
        table.name = mode == MorphMode::None ? "Incremental None" : "Incremental Spectral";
        const auto first = TableData::build(table);
        drawSegment(table.frames[5], 0.2, 0.4, DrawShape::Sine, 0.0f, 0.8f);
        const auto t0 = std::chrono::steady_clock::now();
        const auto quick = TableData::build(table, first.get());
        const double quickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Wavetable copy = table;
        copy.name += " (full)"; // a different name: not taken from the cache
        const auto t1 = std::chrono::steady_clock::now();
        const auto full = TableData::build(copy);
        const double fullMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        REQUIRE(quick->frames() == full->frames());
        bool same = true;
        for (int level = 0; level < TableData::kNumLevels; level += 3)
            for (int f = 0; f < full->frames(); f += 1)
                for (const double phase : {0.0, 0.13, 0.5, 0.77})
                    same = same && quick->read(level, f, phase) == full->read(level, f, phase);
        CHECK(same);
        INFO("incremental " << quickMs << " ms, full " << fullMs << " ms");
        if (mode == MorphMode::None)
            CHECK(quickMs < fullMs);
    }
}
