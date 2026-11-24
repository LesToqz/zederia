// The factory content of milestone 7 (plan 7, 9): the whole content folder builds the same way
// every time, every file in it is checked, the new texture layers and the clip recipes do what
// they say, a patch's tables load side by side, the content check finds what it must, and the
// soak render survives random automation and preset switches.
#include "../support/Analysis.h"
#include "../support/TestUtil.h"
#include "zederia/bench/Soak.h"
#include "zederia/content/ContentCheck.h"
#include "zederia/content/Library.h"
#include "zederia/content/Recipe.h"
#include "zederia/midi/MidiFile.h"
#include "zederia/music/Theory.h"
#include "zederia/patch/Modules.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

using namespace zed;
using Catch::Approx;
namespace fs = std::filesystem;
using J = nlohmann::json;

namespace {

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

std::vector<std::uint8_t> fileBytes(const fs::path& file) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    REQUIRE(readFile(file.string(), bytes, error));
    return bytes;
}

void writeText(const fs::path& file, const std::string& text) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::string error;
    REQUIRE(writeFile(file.string(), {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, error));
}

WavData texture(const std::string& layers, double seconds = 1.0, bool loop = false) {
    const J recipe = J::parse(R"({"kind":"texture","name":"T","seed":3,"peak_db":-3,"seconds":)" + std::to_string(seconds) +
                              R"(,"loop":)" + (loop ? "true" : "false") + R"(,"layers":)" + layers + "}");
    WavData wav;
    std::string error;
    INFO(error);
    REQUIRE(buildTextureRecipe(recipe, wav, error));
    return wav;
}

// Strength of the spectrum around `hz`, relative to its strongest line.
double lineDb(const zedtest::Spectrum& s, double hz) {
    double strongest = 1e-30, here = 1e-30;
    for (const double m : s.magnitude)
        strongest = std::max(strongest, m);
    const int centre = static_cast<int>(std::lround(hz / s.binHz));
    for (int k = std::max(0, centre - 3); k <= centre + 3 && k < static_cast<int>(s.magnitude.size()); ++k)
        here = std::max(here, s.magnitude[static_cast<std::size_t>(k)]);
    return 20.0 * std::log10(here / strongest);
}

std::map<std::string, J> recipesOfKind(const std::string& kind) {
    std::map<std::string, J> out;
    for (const auto& entry : fs::recursive_directory_iterator(fs::path(ZED_CONTENT_DIR))) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        const auto bytes = fileBytes(entry.path());
        const J recipe = J::parse(bytes.begin(), bytes.end());
        if (recipeKind(recipe) == kind)
            out[recipeName(recipe)] = recipe;
    }
    return out;
}

} // namespace

TEST_CASE("factory: the content folder builds the whole factory set, the same way every time", "[content][factory]") {
    TempDir first("factory_a"), second("factory_b");
    const ContentBuild a = buildContentFolder(ZED_CONTENT_DIR, first.str());
    for (const auto& e : a.errors)
        FAIL_CHECK(e);
    // Plan 7: about 60 wavetables and 20 textures, 24 LFO shapes, 16 clips; the presets that come
    // with Zederia; rack and effect presets.
    CHECK(a.tables >= 60);
    CHECK(a.textures >= 20);
    CHECK(a.shapes == 24);
    CHECK(a.clips == 16);
    CHECK(a.presets >= 183);
    CHECK(a.racks >= 12);
    CHECK(a.written.size() == static_cast<std::size_t>(a.tables + a.textures + a.shapes + a.clips + a.presets + a.racks));
    const ContentBuild b = buildContentFolder(ZED_CONTENT_DIR, second.str());
    REQUIRE(b.written == a.written);

    const auto textures = recipesOfKind("texture");
    const auto clips = recipesOfKind("clip");
    std::set<std::string> categories, morphs;
    bool stepped = false;
    int fewest = 1000, most = 0;
    std::size_t bytes = 0;
    for (const std::string& file : a.written) {
        INFO(file);
        const auto one = fileBytes(first.path / file), two = fileBytes(second.path / file);
        REQUIRE(one == two); // reproducible
        bytes += one.size();
        std::string error;
        const std::string text(reinterpret_cast<const char*>(one.data()), one.size());
        if (file.rfind("Wavetables/", 0) == 0) {
            Wavetable table;
            REQUIRE(decodeWavetable(one, table, error));
            categories.insert(fs::path(file).parent_path().filename().string());
            CHECK_FALSE(table.name.empty());
            float loudest = 0.0f;
            for (const Frame& f : table.frames) {
                for (const float s : f)
                    REQUIRE(std::isfinite(s));
                loudest = std::max(loudest, framePeak(f));
                CHECK(std::abs(analyzeFrame(f).dc) < 0.05); // no frame carries an offset
                CHECK(framePeak(f) > 0.05f);                // and none is silent
            }
            CHECK(loudest > 0.5f);
            CHECK(loudest <= 1.0001f);
            morphs.insert(std::string(morphModeName(table.morph)));
            stepped = stepped || table.stepped;
            fewest = std::min(fewest, table.size());
            most = std::max(most, table.size());
        } else if (file.rfind("Textures/", 0) == 0) {
            WavData wav;
            REQUIRE(decodeWav(one, wav, error));
            CHECK(wav.channelCount() == 2);
            CHECK(wav.sampleRate == 48000.0);
            float peak = 0.0f;
            for (const auto& channel : wav.channels)
                for (const float s : channel)
                    peak = std::max(peak, std::abs(s));
            CHECK(peak <= 1.0f);
            CHECK(peak > 0.25f);
            const std::string name = fs::path(file).stem().string();
            REQUIRE(textures.count(name) == 1);
            CHECK(wav.hasLoop == textures.at(name).value("loop", true)); // one-shots play once
            CHECK(SampleData::fromWav(wav, file)->frames > 1000);
        } else if (file.rfind("Clips/", 0) == 0) {
            Clip clip;
            REQUIRE(midi::clipFromFile(one, clip, error));
            const std::string name = fs::path(file).stem().string();
            REQUIRE(clips.count(name) == 1);
            CHECK(clip.notes.size() >= 4);
            CHECK(clip.bars == Approx(clips.at(name)["bars"].get<double>()));
            CHECK(clip.tempo == Approx(clips.at(name)["tempo"].get<double>()));
            music::Key key;
            REQUIRE(music::parseKey(clips.at(name)["key"].get<std::string>(), key));
            for (const auto& issue : music::validateClip(clip, key))
                if (!(issue.code == "outside_key" && clips.at(name).value("chromatic", false)))
                    FAIL_CHECK(issue.message);
        } else if (file.rfind("Presets/", 0) == 0) {
            CHECK(text.find('\r') == std::string::npos); // LF, whatever the checkout
            const LoadResult loaded = loadPatch(text);
            CHECK(loaded.ok());
            CHECK(loaded.issues.empty());
            CHECK_FALSE(loaded.patch.meta.name.empty());
        } else if (file.rfind("Shapes/", 0) == 0) {
            Shape shape;
            std::vector<Issue> issues;
            REQUIRE(pasteShape(text, ShapeKind::Lfo, shape, issues));
            CHECK(issues.empty());
            CHECK(shape.points.size() >= 2);
            CHECK(shape.points.front().x == 0.0);
            CHECK(shape.points.back().x == 1.0);
        } else if (file.rfind("Racks/", 0) == 0) {
            Patch patch;
            std::vector<Issue> issues;
            if (fs::path(file).extension() == ".zrack")
                CHECK(loadRackPreset(text, patch, issues));
            else
                CHECK(loadSlotPreset(text, patch, 0, 0, nullptr, issues));
            CHECK(issues.empty());
            int effects = 0;
            for (int lane = 0; lane < kNumFxLanes; ++lane)
                effects += patch.fxSlotCount(lane);
            CHECK(effects >= 1);
        } else {
            FAIL_CHECK("unexpected file in the factory folder");
        }
    }
    WARN("factory content: " << a.tables << " wavetables, " << a.textures << " textures, " << a.clips << " clips, " << a.presets
                             << " presets, " << a.shapes << " shapes, " << a.racks << " rack and effect presets; " << bytes / (1024 * 1024)
                             << " MB built from the content folder");
    // The tables cover the families the plan asks for, and every table feature.
    CHECK(categories == std::set<std::string>{"Basics", "Harmonic", "Formant", "Metallic", "Digital", "Motion"});
    CHECK(morphs == std::set<std::string>{"none", "crossfade", "spectral", "spectral_aligned", "spectral_zero"});
    CHECK(stepped);
    CHECK(fewest <= 2);
    CHECK(most >= 24);
}

TEST_CASE("factory: struck partials, a steady hum, a chirp and fades", "[content][texture]") {
    // Partials sit at their ratios and the higher one dies first.
    const WavData struck = texture(R"([{"type":"partials","hz":1000,"ratios":[1,2.5],"levels":[1,1],"decay_ms":200,"decay_tilt":1}])");
    const auto& x = struck.channels[0];
    const auto early = zedtest::spectrumOf(x, 0, 4096, 48000.0);
    CHECK(lineDb(early, 1000.0) > -1.0);
    CHECK(lineDb(early, 2500.0) > -12.0);
    CHECK(lineDb(early, 1700.0) < -40.0); // nothing between them
    const auto later = zedtest::spectrumOf(x, 24000, 4096, 48000.0);
    CHECK(lineDb(later, 2500.0) < lineDb(early, 2500.0) - 10.0); // decay_ms / 2.5: gone first
    CHECK(zedtest::rmsOf(x, 0, 2400) > 10.0 * zedtest::rmsOf(x, x.size() - 2400, x.size()));
    // Two ratio lists of different lengths are refused with the reason.
    WavData wav;
    std::string error;
    CHECK_FALSE(buildTextureRecipe(J::parse(R"({"kind":"texture","layers":[{"type":"partials","ratios":[1,2],"levels":[1]}]})"), wav, error));
    CHECK(error.find("one for each ratio") != std::string::npos);
    CHECK_FALSE(buildTextureRecipe(J::parse(R"({"kind":"texture","layers":[{"type":"partials"}]})"), wav, error));
    CHECK(error.find("\"ratios\"") != std::string::npos);

    // A looped hum: whole cycles fit the loop, so the loop point is seamless.
    const WavData hum = texture(R"([{"type":"hum","hz":61.3,"harmonics":3,"tilt_db":-6}])", 1.0, true);
    REQUIRE(hum.hasLoop);
    const auto& h = hum.channels[0];
    float inside = 0.0f;
    for (std::size_t i = 1; i < h.size(); ++i)
        inside = std::max(inside, std::abs(h[i] - h[i - 1]));
    CHECK(std::abs(h.front() - h.back()) <= inside);
    CHECK(zedtest::measureFrequency(h, 0, h.size(), 48000.0) > 0.0);
    const auto humSpectrum = zedtest::spectrumOf(h, 0, 32768, 48000.0);
    CHECK(lineDb(humSpectrum, 61.0) > -1.0);
    CHECK(lineDb(humSpectrum, 122.0) == Approx(-6.0).margin(1.5)); // -6 dB per octave
    CHECK(lineDb(humSpectrum, 183.0) == Approx(-6.0 * std::log2(3.0)).margin(1.5));

    // A chirp glides upward: slow crossings at its start, fast at its end.
    const WavData chirp = texture(R"([{"type":"chirp","from_hz":200,"to_hz":2000,"duration":1}])");
    const auto& c = chirp.channels[0];
    const double start = zedtest::measureFrequency(c, 480, 4800, 48000.0), end = zedtest::measureFrequency(c, 43000, 47000, 48000.0);
    CHECK(start == Approx(250.0).margin(60.0));
    CHECK(end == Approx(1850.0).margin(250.0));
    // The noise form makes a moving band of noise, not a tone.
    const WavData band = texture(R"([{"type":"chirp","noise":true,"from_hz":300,"to_hz":6000,"duration":1,"q":3}])");
    CHECK(zedtest::rmsOf(band.channels[0], 0, 48000) > 0.01);

    // Fades: quiet at the start of a fade-in, quiet at the end of a fade-out.
    const WavData fades = texture(R"([{"type":"noise","color":"white","fade_in":0.5,"fade_out":0.3}])");
    const auto& f = fades.channels[0];
    CHECK(zedtest::rmsOf(f, 0, 960) < 0.05 * zedtest::rmsOf(f, 26400, 28800));
    CHECK(zedtest::rmsOf(f, f.size() - 480, f.size()) < 0.05 * zedtest::rmsOf(f, 26400, 28800));
}

TEST_CASE("factory: clip recipes become checked clips", "[content][clip]") {
    Clip clip;
    std::string error;
    const auto build = [&](const std::string& text) { return buildClipRecipe(J::parse(text), clip, error); };
    // Chords repeat until the clip is full, voice-led, each struck at every hit, with a bass.
    REQUIRE(build(R"({"kind":"clip","name":"C","tempo":100,"bars":4,"key":"C major",
        "parts":[{"chords":["I","V"],"beats_per_chord":4,"bass":true,"hits":[[0,1],[2,1]],"velocity":0.5,"bass_velocity":0.9}]})"));
    CHECK(clip.name == "C");
    CHECK(clip.tempo == 100.0);
    CHECK(clip.bars == 4.0);
    CHECK(clip.root == 60);
    CHECK(clip.notes.size() == 4 * 2 * 4); // four chords (I V I V), two hits, three notes and a bass
    int bass = 0;
    for (const ClipNote& n : clip.notes) {
        CHECK(n.length == 1.0);
        bass += n.velocity > 0.8 ? 1 : 0;
    }
    CHECK(bass == 8);
    CHECK(clip.notes.front().start == 0.0);
    CHECK(clip.notes.back().start == 14.0);
    // Notes as text, repeated.
    REQUIRE(build(R"({"kind":"clip","name":"N","bars":2,"key":"A minor","parts":[{"notes":"0:0.5:A2:0.8, 1:0.5:57:100","repeat":2,"every":4}]})"));
    REQUIRE(clip.notes.size() == 4);
    CHECK(clip.notes[0].key == 45);
    CHECK(clip.notes[1].key == 57);
    CHECK(clip.notes[1].velocity == Approx(100.0 / 127.0));
    CHECK(clip.notes[3].start == 5.0);
    CHECK(clip.root == 57); // A, the tonic nearest middle C
    // Every check of the clip validator applies.
    CHECK_FALSE(build(R"({"kind":"clip","name":"X","bars":1,"key":"C major","parts":[{"notes":"0:1:C4 0.5:1:C4"}]})"));
    CHECK(error.find("verlap") != std::string::npos);
    CHECK_FALSE(build(R"({"kind":"clip","name":"X","bars":1,"key":"C major","parts":[{"notes":"0:1:C#4"}]})"));
    CHECK_FALSE(build(R"({"kind":"clip","name":"X","bars":1,"key":"C major","chromatic":true,"parts":[{"notes":"3.5:1:C4"}]})"));
    CHECK(build(R"({"kind":"clip","name":"X","bars":1,"key":"C major","chromatic":true,"parts":[{"notes":"0:1:C#4"}]})"));
    CHECK_FALSE(build(R"({"kind":"clip","name":"X","bars":1,"parts":[{"notes":"0:1:H4"}]})"));
    CHECK(error.find("note 1") != std::string::npos);
    CHECK_FALSE(build(R"({"kind":"clip","name":"X","bars":1,"key":"C major","parts":[{"chords":["IX"]}]})"));
    CHECK(error.find("part 1") != std::string::npos);
    CHECK_FALSE(build(R"({"kind":"clip","name":"X","bars":0,"parts":[{"notes":"0:1:C4"}]})"));
    CHECK_FALSE(build(R"({"kind":"clip","name":"X","bars":1,"parts":[{"nothing":1}]})"));
}

TEST_CASE("factory: presets, shapes and racks are checked before they are copied", "[content][factory]") {
    TempDir in("factory_check_in"), out("factory_check_out");
    writeText(in.path / "one.json", R"json({"kind":"table","name":"One","category":"Test","steps":[{"op":"formula","expr":"saw(t)"}]})json");
    Patch patch;
    patch.meta.name = "Good";
    writeText(in.path / "presets" / "Bass" / "Good.zpreset", savePatch(patch));
    // Windows line endings in the checkout: copied with LF.
    std::string crlf = savePatch(patch);
    for (std::size_t at = crlf.find('\n'); at != std::string::npos; at = crlf.find('\n', at + 2))
        crlf.insert(at, "\r");
    writeText(in.path / "presets" / "Bass" / "Crlf.zpreset", crlf);
    writeText(in.path / "presets" / "Bad.zpreset", R"({"format":"zederia.patch","version":1,"filter1":{"cutoff_hz":"high"}})");
    writeText(in.path / "presets" / "Warned.zpreset", R"({"format":"zederia.patch","version":1,"meta":{"name":"W"},"whatever":1})");
    writeText(in.path / "shapes" / "Line.zshape", copyShape({{{0.0, -1.0, 0.0}, {1.0, 1.0, 0.0}}}, ShapeKind::Lfo));
    writeText(in.path / "shapes" / "Dot.zshape", copyShape({{{0.0, 0.0, 0.0}}}, ShapeKind::Lfo));
    writeText(in.path / "racks" / "Space.zrack", R"({"format":"zederia.rack","version":1,"fx":{"master":[{"type":"reverb","mix":0.2}]}})");
    writeText(in.path / "racks" / "Effects" / "Odd.zfx", R"({"format":"zederia.fx","version":1,"slot":{"type":"reverb","sparkle":1}})");
    const ContentBuild built = buildContentFolder(in.str(), out.str());
    CHECK(built.tables == 1);
    CHECK(built.presets == 2);
    CHECK(built.shapes == 1);
    CHECK(built.racks == 1);
    REQUIRE(built.errors.size() == 4);
    const auto names = [&](const std::string& part) {
        return std::any_of(built.errors.begin(), built.errors.end(), [&](const std::string& e) { return e.find(part) != std::string::npos; });
    };
    CHECK(names("presets/Bad.zpreset: preset error at /filter1/cutoff_hz"));
    CHECK(names("presets/Warned.zpreset: preset warning at /whatever"));
    CHECK(names("shapes/Dot.zshape: a shape needs at least two points"));
    CHECK(names("racks/Effects/Odd.zfx: effect preset warning"));
    CHECK(fs::exists(out.path / "Presets" / "Bass" / "Good.zpreset"));
    const auto copied = fileBytes(out.path / "Presets" / "Bass" / "Crlf.zpreset");
    CHECK(std::find(copied.begin(), copied.end(), static_cast<std::uint8_t>('\r')) == copied.end());
    CHECK(fs::exists(out.path / "Racks" / "Space.zrack"));
    CHECK_FALSE(fs::exists(out.path / "Presets" / "Bad.zpreset"));
}

TEST_CASE("factory: a patch's tables load side by side, and equal tables share one build", "[content][library]") {
    Wavetable a, b;
    a.name = "A";
    b.name = "B";
    a.morph = b.morph = MorphMode::Spectral;
    Frame sine{}, saw{};
    for (int n = 0; n < kFrameSize; ++n) {
        sine[static_cast<std::size_t>(n)] = static_cast<float>(std::sin(2.0 * kPi * n / kFrameSize));
        saw[static_cast<std::size_t>(n)] = static_cast<float>(2.0 * n / kFrameSize - 1.0);
    }
    a.frames = {sine, saw};
    b.frames = {saw, sine, saw};
    Patch patch;
    patch.tableData[0].table = std::make_shared<const Wavetable>(a);
    patch.tableData[1].table = std::make_shared<const Wavetable>(b);
    patch.tableData[2].table = std::make_shared<const Wavetable>(a); // the same table again
    std::vector<std::string> problems;
    ContentLibrary library;
    const auto tables = loadPatchTables(patch, library, problems);
    CHECK(problems.empty());
    REQUIRE(tables[0] != nullptr);
    REQUIRE(tables[1] != nullptr);
    CHECK(tables[0].get() == tables[2].get());
    CHECK(tables[0]->frames() == 256);
    CHECK(tables[1]->name() == "B");
    // A spectral table is synthesized on several threads, each position on its own, so two builds
    // give exactly the same data however the work was split (the second build starts after every
    // copy of the first is gone, or the cache would hand back the same object).
    const auto sampleOf = [](const TableData& t) {
        std::vector<double> values;
        for (int level = 0; level < 6; ++level)
            for (int i = 0; i < 2048; ++i)
                values.push_back(t.read(level, static_cast<double>(i % 256), static_cast<double>(i) / 2048.0));
        return values;
    };
    Wavetable c = b;
    c.name = "C"; // not shared with anything above
    std::vector<double> first, second;
    {
        const auto built = TableData::build(c);
        first = sampleOf(*built);
    }
    {
        const auto built = TableData::build(c);
        second = sampleOf(*built);
    }
    CHECK(first == second);
    // A missing reference is reported for its oscillator and leaves it null.
    Patch missing;
    missing.tables[1] = "factory:Nowhere/Nothing";
    problems.clear();
    const auto none = loadPatchTables(missing, library, problems);
    CHECK(none[0] != nullptr); // the built-in table
    CHECK(none[1] == nullptr);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].rfind("osc2: ", 0) == 0);
    CHECK(problems[0].find("built-in table is used") != std::string::npos);
    CHECK(none[2].get() == none[0].get());
}

TEST_CASE("factory: the content check finds load errors, missing tables, clipping and silence", "[content][check]") {
    TempDir dir("factory_content_check");
    const auto save = [&](const std::string& name, const Patch& p) { writeText(dir.path / name, savePatch(p)); };
    Patch good;
    good.meta.name = "Good";
    good.meta.type = "lead";
    save("Good.zpreset", good);
    Patch loud = good;
    loud.meta.name = "Loud";
    const auto& reg = Registry::get();
    loud.set(reg.indexOf("global.master"), 6.0);
    loud.set(reg.indexOf("osc1.level"), 6.0);
    std::vector<Issue> issues;
    REQUIRE(loadRackPreset(R"({"format":"zederia.rack","version":1,"fx":{"master":[{"type":"utility","gain_db":24}]}})", loud, issues));
    save("Loud.zpreset", loud);
    Patch silent = good;
    silent.meta.name = "Silent";
    silent.set(reg.indexOf("osc1.on"), 0.0);
    save("Silent.zpreset", silent);
    Patch lost = good;
    lost.meta.name = "Lost";
    lost.tables[0] = "factory:Nowhere/Nothing";
    save("Lost.zpreset", lost);
    writeText(dir.path / "Broken.zpreset", "{ not json");

    PresetCheckOptions options;
    options.measureCpu = false;
    int reported = 0;
    options.progress = [&](int done, int total, const PresetCheck&) {
        ++reported;
        CHECK(done <= total);
    };
    const ContentCheckReport report = checkPresets(dir.str(), ContentLibrary{}, options);
    CHECK(reported == 5);
    REQUIRE(report.presets.size() == 5);
    CHECK(report.failures() == 4);
    std::map<std::string, const PresetCheck*> by;
    for (const auto& c : report.presets)
        by[c.relative] = &c;
    const auto has = [&](const char* preset, const char* part) {
        const PresetCheck& c = *by.at(preset);
        return std::any_of(c.errors.begin(), c.errors.end(), [&](const std::string& e) { return e.find(part) != std::string::npos; });
    };
    CHECK(by.at("Good.zpreset")->ok());
    CHECK(by.at("Good.zpreset")->peakDbfs < -6.0);
    CHECK(by.at("Good.zpreset")->releaseSeconds >= 0.0);
    CHECK(has("Loud.zpreset", "clipping"));
    CHECK(has("Silent.zpreset", "silent"));
    CHECK(has("Lost.zpreset", "osc1"));
    CHECK_FALSE(by.at("Broken.zpreset")->ok());
    CHECK(toJson(report)["failures"] == 4);

    // With the CPU reading: init costs a little of one core and loads quickly.
    PresetCheckOptions cpu;
    const PresetCheck reading = checkPreset((dir.path / "Good.zpreset").string(), ContentLibrary{}, cpu);
    CHECK(reading.ok());
    CHECK(reading.cpuPercent > 0.0);
#ifdef NDEBUG
    CHECK(reading.cpuPercent < cpu.cpuBudgetPercent);
    CHECK(reading.loadMs < 100.0);
#endif
    // A budget nothing can meet fails the preset with the reading.
    cpu.cpuBudgetPercent = 1e-6;
    cpu.cpuAttempts = 1;
    const PresetCheck over = checkPreset((dir.path / "Good.zpreset").string(), ContentLibrary{}, cpu);
    REQUIRE_FALSE(over.ok());
    CHECK(over.errors.back().find("CPU") != std::string::npos);
    // Every type has a phrase.
    for (const char* type : {"bass", "808", "lead", "pluck", "keys", "pad", "strings", "brass", "synth", "sequence", "fx", "percussion", ""}) {
        std::string name;
        CHECK_FALSE(auditionPhrase(type, name).empty());
        CHECK_FALSE(name.empty());
    }
}

TEST_CASE("factory: a soak with random automation, notes and preset switches stays sound", "[content][soak]") {
    SoakOptions options;
    options.seconds = 40.0;
    options.presetSeconds = 4.0;
    options.seed = 7;
    for (const char* name : {"bass", "pad", "pluck", "lead", "arp_fx"})
        options.presets.push_back(zedtest::dataPath(std::string("patches/") + name + ".zpreset"));
    int calls = 0, passStarts = 0;
    options.progress = [&](const SoakProgress& p) {
        ++calls;
        passStarts += p.passStart ? 1 : 0;
        return true;
    };
    const SoakReport report = runSoak(options);
    for (const auto& e : report.events)
        UNSCOPED_INFO(e.atSeconds << " s " << e.preset << ": " << e.what);
    CHECK(report.ok());
    CHECK(report.loadProblems == 0);
    CHECK(report.audioSeconds >= 40.0);
    CHECK(report.presetsLoaded == calls);
    CHECK(report.presetsLoaded >= 3); // a release the moves made long takes its time
    CHECK(passStarts >= 1);
    CHECK(report.moves > 500);
    CHECK(report.notes > 50);
    CHECK(report.releaseChecks == report.presetsLoaded);
    CHECK(report.peakDbfs > -60.0);

    // The detector is wired: with a ceiling no sound stays under, every audible block is reported.
    SoakOptions strict;
    strict.seconds = 3.0;
    strict.presetSeconds = 2.0;
    strict.runawayDbfs = -120.0;
    const SoakReport flagged = runSoak(strict);
    CHECK(flagged.runawayBlocks > 0);
    CHECK_FALSE(flagged.ok());
    // Stopping early from the progress callback ends the soak.
    SoakOptions stopped = strict;
    stopped.runawayDbfs = 60.0;
    stopped.seconds = 600.0;
    stopped.progress = [](const SoakProgress&) { return false; };
    CHECK(runSoak(stopped).audioSeconds == 0.0);
}
