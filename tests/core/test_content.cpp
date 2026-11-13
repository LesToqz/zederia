// Recipes and the starter content (plan 7, 9), the content library that resolves a patch's table
// and sample references, and resynthesis (plan 3.10): bake warp, bake patch, shape to wave.
#include "../support/Analysis.h"
#include "../support/TestUtil.h"
#include "zederia/content/Library.h"
#include "zederia/content/Recipe.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/table/Resynth.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <set>

using namespace zed;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

int choiceOf(const char* id, const char* token) { return Registry::get().find(id)->choiceIndex(token); }

// A fresh, empty folder under the system's temporary folder; removed when the test ends.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path(fs::temp_directory_path() / (std::string("zederia_test_") + name)) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

nlohmann::json recipeFile(const std::string& relative) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    REQUIRE(readFile(std::string(ZED_CONTENT_DIR) + "/" + relative, bytes, error));
    return nlohmann::json::parse(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> fileBytes(const fs::path& file) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    REQUIRE(readFile(file.string(), bytes, error));
    return bytes;
}

Frame sineFrame(int harmonic) {
    Frame f;
    for (int n = 0; n < kFrameSize; ++n)
        f[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(2.0 * kPi * harmonic * n / kFrameSize));
    return f;
}

double maxDifference(const Frame& a, const Frame& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max<double>(worst, std::abs(a[i] - b[i]));
    return worst;
}

} // namespace

TEST_CASE("content: the starter set builds from its recipes, the same way every time", "[content]") {
    TempDir first("content_a"), second("content_b");
    const ContentBuild a = buildContentFolder(ZED_CONTENT_DIR, first.str());
    for (const auto& e : a.errors)
        FAIL_CHECK(e);
    CHECK(a.tables == 12);
    CHECK(a.textures == 6);
    CHECK(a.written.size() == 18);
    const ContentBuild b = buildContentFolder(ZED_CONTENT_DIR, second.str());
    REQUIRE(b.written == a.written);

    std::size_t bytes = 0;
    for (const std::string& file : a.written) {
        INFO(file);
        const auto one = fileBytes(first.path / file), two = fileBytes(second.path / file);
        CHECK(one == two); // reproducible
        bytes += one.size();
        std::string error;
        if (file.rfind("Wavetables/", 0) == 0) {
            Wavetable table;
            REQUIRE(decodeWavetable(one, table, error));
            CHECK(table.size() >= 1);
            CHECK_FALSE(table.name.empty());
            float loudest = 0.0f;
            for (const Frame& f : table.frames) {
                for (const float s : f)
                    REQUIRE(std::isfinite(s));
                loudest = std::max(loudest, framePeak(f));
            }
            CHECK(loudest > 0.5f);
            CHECK(loudest <= 1.0001f);
            // No frame carries a noticeable offset, and every table loads into the oscillator.
            for (const Frame& f : table.frames)
                CHECK(std::abs(analyzeFrame(f).dc) < 0.05);
            CHECK(TableData::build(table)->frames() >= 1);
        } else {
            WavData wav;
            REQUIRE(decodeWav(one, wav, error));
            CHECK(wav.channelCount() == 2);
            CHECK(wav.sampleRate == 48000.0);
            float peak = 0.0f;
            double energy = 0.0;
            for (const auto& channel : wav.channels)
                for (const float s : channel) {
                    peak = std::max(peak, std::abs(s));
                    energy += static_cast<double>(s) * s;
                }
            CHECK(peak <= 1.0f);
            CHECK(peak > 0.3f);
            CHECK(energy > 0.0);
            const auto sample = SampleData::fromWav(wav, file);
            CHECK(sample->frames > 1000);
            if (file.find("Click Transient") != std::string::npos)
                CHECK_FALSE(sample->hasLoop); // the one-shot of the set
            else
                CHECK(sample->hasLoop);
        }
    }
    WARN("starter content: " << a.tables << " wavetables and " << a.textures << " textures, " << bytes / 1024 << " KB built from recipes");

    // The set covers the table features: every morph mode but "none" with generated positions,
    // a stepped table, and tables from one frame count to dozens.
    std::set<std::string> morphs;
    bool stepped = false;
    int fewest = 1000, most = 0;
    for (const std::string& file : a.written) {
        if (file.rfind("Wavetables/", 0) != 0)
            continue;
        Wavetable table;
        std::string error;
        REQUIRE(decodeWavetable(fileBytes(first.path / file), table, error));
        morphs.insert(std::string(morphModeName(table.morph)));
        stepped = stepped || table.stepped;
        fewest = std::min(fewest, table.size());
        most = std::max(most, table.size());
    }
    CHECK(morphs == std::set<std::string>{"none", "crossfade", "spectral", "spectral_aligned", "spectral_zero"});
    CHECK(stepped);
    CHECK(fewest <= 2);
    CHECK(most >= 24);
}

TEST_CASE("content: looped textures are seamless and the one-shot ends in silence", "[content][texture]") {
    WavData wav;
    std::string error;
    REQUIRE(buildTextureRecipe(recipeFile("textures/pink_bed.json"), wav, error));
    REQUIRE(wav.hasLoop);
    CHECK(wav.loopStart == 0);
    CHECK(wav.loopEnd == static_cast<std::int64_t>(wav.frames()));
    // Across the loop point the signal moves no more than it does anywhere else.
    const auto& x = wav.channels[0];
    float inside = 0.0f;
    for (std::size_t i = 1; i < x.size(); ++i)
        inside = std::max(inside, std::abs(x[i] - x[i - 1]));
    CHECK(std::abs(x.front() - x.back()) <= inside);
    // Pink: about three decibels less per octave. Compare two octave bands.
    const auto s = zedtest::spectrumOf(x, 0, 131072, wav.sampleRate);
    const auto band = [&](double from, double to) {
        double sum = 0.0;
        for (int k = static_cast<int>(from / s.binHz); k < static_cast<int>(to / s.binHz); ++k)
            sum += s.magnitude[static_cast<std::size_t>(k)] * s.magnitude[static_cast<std::size_t>(k)];
        return 10.0 * std::log10(sum);
    };
    CHECK(band(200.0, 400.0) - band(1600.0, 3200.0) == Approx(0.0).margin(1.5)); // equal energy per octave
    WavData white;
    REQUIRE(buildTextureRecipe(recipeFile("textures/white_noise.json"), white, error));
    const auto w = zedtest::spectrumOf(white.channels[0], 0, 65536, white.sampleRate);
    double low = 0.0, high = 0.0;
    for (int k = static_cast<int>(200.0 / w.binHz); k < static_cast<int>(400.0 / w.binHz); ++k)
        low += w.magnitude[static_cast<std::size_t>(k)] * w.magnitude[static_cast<std::size_t>(k)];
    for (int k = static_cast<int>(1600.0 / w.binHz); k < static_cast<int>(3200.0 / w.binHz); ++k)
        high += w.magnitude[static_cast<std::size_t>(k)] * w.magnitude[static_cast<std::size_t>(k)];
    CHECK(10.0 * std::log10(high / low) == Approx(9.0).margin(1.5)); // white: three octaves up, eight times the energy
    // The two sides of a wide texture differ.
    double difference = 0.0;
    for (std::size_t i = 0; i < white.channels[0].size(); i += 3)
        difference = std::max<double>(difference, std::abs(white.channels[0][i] - white.channels[1][i]));
    CHECK(difference > 0.1);

    WavData click;
    REQUIRE(buildTextureRecipe(recipeFile("textures/click_transient.json"), click, error));
    CHECK_FALSE(click.hasLoop);
    const auto& c = click.channels[0];
    CHECK(std::abs(c.back()) < 1e-4f);
    CHECK(zedtest::rmsOf(c, 0, 480) > 10.0 * zedtest::rmsOf(c, c.size() - 2400, c.size())); // loud start, quiet end
}

TEST_CASE("content: recipe errors name the step and the problem", "[content][errors]") {
    using J = nlohmann::json;
    Wavetable table;
    WavData texture;
    std::string error;
    const auto tableError = [&](const J& recipe) {
        REQUIRE_FALSE(buildTableRecipe(recipe, table, error));
        CHECK(table.frames.empty());
        return error;
    };
    CHECK(tableError(J::parse(R"({"kind":"texture"})")).find("not a table recipe") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","frames":0,"steps":[{"op":"reverse"}]})")).find("from 1 to 256") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","frames":2})")).find("no \"steps\"") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","steps":[{"op":"reverse"},{"op":"warble"}]})")) == "step 2 (warble): unknown step \"warble\"");
    CHECK(tableError(J::parse(R"({"kind":"table","steps":[{"op":"formula","expr":"sin(tau*t"}]})")).find("step 1 (formula): column 10") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","steps":[{"op":"process","name":"polish"}]})")).find("unknown process \"polish\"") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","steps":[{"op":"harmonic","name":"x"}]})")).find("unknown harmonic operation") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","steps":[{"op":"sort","by":"age"}]})")).find("unknown sort key") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","steps":[{"op":"delete"}]})")).find("at least one frame") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","steps":[{"op":"bake_warp","mode":"wobble"}]})")).find("unknown warp mode") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","steps":[7]})")).find("a step must be an object") != std::string::npos);
    CHECK(tableError(J::parse(R"({"kind":"table","morph":"sideways","steps":[{"op":"reverse"}]})")).find("unknown morph mode") != std::string::npos);
    CHECK(tableError(J::parse("[1,2]")).find("not a table recipe") != std::string::npos);

    const auto textureError = [&](const J& recipe) {
        REQUIRE_FALSE(buildTextureRecipe(recipe, texture, error));
        return error;
    };
    CHECK(textureError(J::parse(R"({"kind":"texture","seconds":0})")).find("\"seconds\" must be") != std::string::npos);
    CHECK(textureError(J::parse(R"({"kind":"texture"})")).find("no \"layers\"") != std::string::npos);
    CHECK(textureError(J::parse(R"({"kind":"texture","layers":[{"type":"hum"}]})")) == "layer 1: unknown layer type \"hum\"");
    CHECK(textureError(J::parse(R"({"kind":"texture","layers":[{"type":"noise","color":"green"}]})")).find("white, pink or brown") != std::string::npos);

    // A folder without recipes, and one with a broken recipe among good ones.
    TempDir empty("content_empty"), out("content_out");
    CHECK(buildContentFolder(empty.str(), out.str()).errors.size() == 1);
    std::string writeError;
    const std::string bad = R"({"kind":"table","name":"Broken","steps":[{"op":"nope"}]})";
    const std::string good = R"json({"kind":"table","name":"Fine","category":"Test","steps":[{"op":"formula","expr":"saw(t)"}]})json";
    REQUIRE(writeFile((empty.path / "bad.json").string(), {reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()}, writeError));
    REQUIRE(writeFile((empty.path / "good.json").string(), {reinterpret_cast<const std::uint8_t*>(good.data()), good.size()}, writeError));
    const ContentBuild mixed = buildContentFolder(empty.str(), out.str());
    CHECK(mixed.tables == 1);
    REQUIRE(mixed.errors.size() == 1);
    CHECK(mixed.errors[0].find("bad.json: step 1 (nope)") != std::string::npos);
    CHECK(fs::exists(out.path / "Wavetables" / "Test" / "Fine.wav"));
}

TEST_CASE("library: references resolve to files and load into the engine", "[content][library]") {
    TempDir factory("library_factory"), user("library_user"), presets("library_presets");
    const ContentBuild built = buildContentFolder(ZED_CONTENT_DIR, factory.str());
    REQUIRE(built.errors.empty());
    ContentLibrary library;
    library.setFactoryDir(factory.str());
    library.setUserDir(user.str());

    std::string error;
    CHECK(library.loadTable("", error).get() == TableData::builtin().get());
    CHECK(library.loadTexture("", error).get() == SampleData::builtin().get());

    const auto saw = library.loadTable("factory:Basics/Saw Morph", error);
    REQUIRE(saw != nullptr);
    CHECK(saw->name() == "Saw Morph");
    CHECK(saw->frames() == 256); // two key frames, spectral morph: the full set is generated on load
    CHECK(library.loadTable("factory:Basics/Saw Morph", error).get() == saw.get()); // loaded once, shared
    const auto wind = library.loadTexture("factory:Wind", error);
    REQUIRE(wind != nullptr);
    CHECK(wind->hasLoop);
    CHECK(wind->frames == 6 * 48000);

    // User content and plain paths, relative to the preset's folder or absolute.
    fs::create_directories(user.path / "Wavetables" / "Mine");
    Wavetable mine;
    mine.name = "Growl";
    mine.frames = {sineFrame(3)};
    std::string writeError;
    REQUIRE(writeFile((user.path / "Wavetables" / "Mine" / "Growl.wav").string(), encodeWavetable(mine), writeError));
    REQUIRE(writeFile((presets.path / "beside.wav").string(), encodeWavetable(mine), writeError));
    CHECK(library.loadTable("user:Mine/Growl", error) != nullptr);
    CHECK(library.loadTable("beside.wav", error, presets.str()) != nullptr);
    CHECK(library.loadTable((presets.path / "beside.wav").string(), error) != nullptr);

    // Listing finds everything, by reference.
    const auto tables = library.listTables();
    CHECK(tables.size() == 13);
    CHECK(std::find(tables.begin(), tables.end(), "factory:Basics/Saw Morph") != tables.end());
    CHECK(std::find(tables.begin(), tables.end(), "user:Mine/Growl") != tables.end());
    CHECK(library.listTextures().size() == 6);

    // Missing files and references that try to leave their folder are refused with a reason.
    CHECK(library.loadTable("factory:Basics/Nothing Here", error) == nullptr);
    CHECK(error.find("Nothing Here") != std::string::npos);
    CHECK(library.loadTable("factory:../../secret", error) == nullptr);
    CHECK(library.loadTable("factory:C:/Windows/win", error) == nullptr);
    CHECK(library.tablePath("factory:../x").empty());
    CHECK(library.loadTexture("factory:Basics/Saw Morph", error) == nullptr); // a table is not in the Textures folder
    const std::string junk = "not audio";
    REQUIRE(writeFile((presets.path / "junk.wav").string(), {reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size()}, writeError));
    CHECK(library.loadTable("junk.wav", error, presets.str()) == nullptr);
    CHECK(error.find("not a WAV") != std::string::npos);
    ContentLibrary unset;
    CHECK(unset.loadTable("factory:Basics/Saw Morph", error) == nullptr);
    CHECK(error.find("cannot be resolved") != std::string::npos);

    // A patch's references reach the engine; what is missing falls back with a message.
    Patch patch;
    patch.tables[0] = "factory:Basics/Saw Morph";
    patch.tables[1] = "factory:Lost/Table";
    patch.textureSample = "factory:Wind";
    Engine engine;
    const auto problems = applyPatchAssets(engine, patch, library);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("osc2") != std::string::npos);
    CHECK(problems[0].find("built-in table is used") != std::string::npos);
    CHECK(engine.table(0).get() == saw.get());
    CHECK(engine.table(1).get() == TableData::builtin().get());
    CHECK(engine.texture()->name == "Wind");
    CHECK(engine.texture()->frames == wind->frames);

    // The test patch from milestone 1 names a factory table: it now renders with it.
    std::vector<std::uint8_t> bytes;
    REQUIRE(readFile(zedtest::dataPath("patches/glass_pluck.zpreset"), bytes, error));
    const LoadResult loaded = loadPatch({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    REQUIRE(loaded.ok());
    CHECK(loaded.patch.tables[0] == "factory:Basics/Saw Morph");
    CHECK(library.loadTable(loaded.patch.tables[0], error) != nullptr);
}

TEST_CASE("resynthesis: the warped cycle is what the oscillator plays", "[resynth][warp]") {
    // Plan 5.7: displays and audio agree. One cycle from renderWarpedCycle against the engine's
    // output for the same table, position and warp, at a pitch where a cycle is 2048 samples.
    const auto table = TableData::build(TableData::builtinWavetable());
    for (const char* mode : {"off", "sync", "bend_plus", "pulse_width", "mirror", "curve_half", "fold"}) {
        const auto& modes = Registry::get().find("osc1.warp_a.mode")->choices;
        const auto warpMode = static_cast<osc::WarpMode>(std::find(modes.begin(), modes.end(), mode) - modes.begin());
        WarpSetup warp;
        warp.modeA = warpMode;
        warp.amountA = 0.4;
        const Frame cycle = renderWarpedCycle(*table, 2.0 / 3.0, warp); // the triangle

        Patch p;
        p.set("osc1.phase_random", 0);
        p.set("osc1.level", 0);
        p.set("global.master", 0);
        p.set("env1.attack", 0);
        p.set("env1.sustain", 1);
        p.set("osc1.position", 2.0 / 3.0);
        p.set("osc1.keytrack", 0);
        p.set("global.quality_offline", choiceOf("global.quality_offline", "draft"));
        p.set("osc1.warp_a.mode", choiceOf("osc1.warp_a.mode", mode));
        p.set("osc1.warp_a.amount", 0.4);
        const double rate = noteToHz(60) * kFrameSize; // middle C takes exactly 2048 samples per cycle
        const NoteSpec notes[] = {{60, 0.0, 0.05, 1.0}};
        RenderSettings settings;
        settings.sampleRate = rate;
        const RenderResult r = renderOffline(p, sequenceFromNotes(notes, rate), settings);
        double worst = 0.0;
        for (int n = 0; n < kFrameSize; ++n) // the second cycle: past the envelope's first samples
            worst = std::max<double>(worst, std::abs(r.left[static_cast<std::size_t>(kFrameSize + n)] / std::sqrt(0.5) - cycle[static_cast<std::size_t>(n)]));
        INFO(mode << ": largest difference " << worst);
        CHECK(worst < 2e-3);
    }
    // The unwarped cycle is the frame itself (band-limited as stored).
    const Wavetable shapes = TableData::builtinWavetable();
    CHECK(maxDifference(renderWarpedCycle(*table, 1.0, WarpSetup{}), shapes.frames[3]) < 1e-4);
    CHECK(maxDifference(renderWarpedCycle(*table, 0.0, WarpSetup{}), shapes.frames[0]) < 2e-3);
}

TEST_CASE("resynthesis: bake warp, bake patch and shape to wave", "[resynth]") {
    SECTION("bake warp renders one frame across the warp's range") {
        Wavetable sine;
        sine.name = "Sine";
        sine.frames = {sineFrame(1)};
        const Wavetable baked = bakeWarp(sine, 0.0, osc::WarpMode::Sync, 256);
        REQUIRE(baked.size() == 256);
        CHECK(baked.name == "Sine");
        CHECK(maxDifference(baked.frames[0], sine.frames[0]) < 1e-4); // amount 0: untouched
        // Amount 25 %, 50 %, 75 % and 100 % are ratios 2, 4, 8 and 16: whole cycles of the sine.
        CHECK(sortValue(baked.frames[64], SortKey::StrongestPartial) == Approx(2.0).margin(1.0));
        CHECK(analyzeFrame(baked.frames[255]).magnitude[16] == Approx(1.0).epsilon(0.01));
        const Wavetable folded = bakeWarp(sine, 0.0, osc::WarpMode::Fold, 8);
        CHECK(folded.size() == 8);
        CHECK(sortValue(folded.frames[7], SortKey::Brightness) > sortValue(folded.frames[1], SortKey::Brightness));
        // Modes that need another source have nothing to bake: the frames stay as they are.
        const Wavetable modulated = bakeWarp(sine, 0.0, osc::WarpMode::Pm, 4);
        CHECK(maxDifference(modulated.frames[3], sine.frames[0]) < 1e-4);
    }
    SECTION("bake patch captures the playing sound as a table") {
        Patch patch;
        patch.meta.name = "Bake Me";
        patch.set("osc1.phase_random", 0);
        patch.set("osc1.level", 0);
        patch.set("global.master", 0);
        patch.set("env1.attack", 0);
        patch.set("env1.decay", 30000);
        patch.set("env1.sustain", 1);
        patch.set("osc1.position", 1.0); // the sine of the built-in table
        BakePatchOptions options;
        Wavetable table;
        std::string error;
        REQUIRE(bakePatch(patch, options, table, error));
        CHECK(table.name == "Bake Me");
        CHECK(table.size() == 256); // one bar at 120: two seconds of C3, 261 cycles
        const Harmonics h = analyzeFrame(table.frames[100]);
        CHECK(h.magnitude[1] == Approx(std::sqrt(0.5)).epsilon(0.01)); // each frame is one cycle of the note
        CHECK(h.magnitude[2] < 1e-3);
        // The frames follow the sound: a falling envelope gives falling frames.
        patch.set("env1.decay", 1500);
        patch.set("env1.sustain", 0);
        REQUIRE(bakePatch(patch, options, table, error));
        CHECK(framePeak(table.frames[10]) > framePeak(table.frames[120]) * 1.5f);
        CHECK(framePeak(table.frames[120]) > framePeak(table.frames[250]));
        // An octave up puts two cycles in every frame.
        patch.set("osc1.octave", 1);
        patch.set("env1.sustain", 1);
        options.frames = 16;
        options.bars = 0.5;
        REQUIRE(bakePatch(patch, options, table, error));
        CHECK(table.size() == 16);
        CHECK(sortValue(table.frames[8], SortKey::StrongestPartial) == 2);
        // The same seed gives the same table.
        Wavetable again;
        REQUIRE(bakePatch(patch, options, again, error));
        CHECK(again == table);
        options.bars = 0.0;
        CHECK_FALSE(bakePatch(patch, options, table, error));
        CHECK(error.find("bars") != std::string::npos);
    }
    SECTION("shape to wave") {
        const float triangle[] = {0.0f, 1.0f, 0.0f, -1.0f, 0.0f};
        Frame f = shapeToFrame(triangle, false);
        CHECK(f[0] == 0.0f);
        CHECK(f[512] == Approx(1.0f));
        CHECK(f[1024] == Approx(0.0f).margin(1e-6));
        CHECK(f[1536] == Approx(-1.0f));
        CHECK(f[256] == Approx(0.5f));
        const float unipolar[] = {0.0f, 1.0f};
        f = shapeToFrame(unipolar, true); // 0..1 becomes -1..1
        CHECK(f[0] == -1.0f);
        CHECK(f[1024] == Approx(0.0f).margin(1e-3));
        CHECK(f[2047] == Approx(1.0f).margin(2e-3));
        CHECK(framePeak(shapeToFrame({}, false)) == 0.0f);
    }
}
