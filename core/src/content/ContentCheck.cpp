#include "zederia/content/ContentCheck.h"

#include "zederia/analysis/Analysis.h"
#include "zederia/engine/Engine.h"
#include "zederia/params/Registry.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/rt/AudioThreadGuard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <system_error>
#include <thread>

namespace zed {
namespace {

namespace fs = std::filesystem;
using J = nlohmann::json;

double since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return out;
}

std::string fixed(double value, int digits) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}

// The tables and the sample a patch plays, built once for every render of the check.
struct Assets {
    std::array<std::shared_ptr<const TableData>, kNumOscs> tables;
    std::shared_ptr<const SampleData> texture;
    std::vector<std::string> problems;
};

Assets loadAssets(const Patch& patch, const ContentLibrary& library, const std::string& presetDir) {
    Assets assets;
    assets.tables = loadPatchTables(patch, library, assets.problems, presetDir);
    std::string error;
    assets.texture = library.loadTexture(patch.textureSample, error, presetDir);
    if (assets.texture == nullptr)
        assets.problems.push_back("texture: " + error);
    return assets;
}

void prepareEngine(Engine& engine, const Patch& patch, const Assets& assets, double sampleRate, bool offlineQuality) {
    engine.setSeed(1);
    engine.prepare(sampleRate);
    engine.setOffline(offlineQuality);
    engine.loadPatch(patch);
    for (int i = 0; i < kNumOscs; ++i)
        if (assets.tables[static_cast<std::size_t>(i)] != nullptr)
            engine.setTable(i, assets.tables[static_cast<std::size_t>(i)]);
    if (assets.texture != nullptr)
        engine.setTexture(assets.texture);
}

struct Played {
    std::vector<float> left, right;
    double releaseSeconds = -1.0; // -1: the voices had not ended when the render stopped
    int recoveries = 0;
};

// Plays the notes (with every velocity replaced by `velocity`) and goes on after the last note-off
// until every voice has ended and at least a second and a half has passed, or `maxRelease` seconds.
Played play(const Patch& patch, const Assets& assets, std::vector<NoteSpec> notes, double velocity, bool offlineQuality,
            double maxRelease, const PresetCheckOptions& options) {
    Engine engine;
    prepareEngine(engine, patch, assets, options.sampleRate, offlineQuality);
    for (NoteSpec& n : notes)
        n.velocity = velocity;
    const std::vector<SeqEvent> events = sequenceFromNotes(notes, options.sampleRate);
    const std::int64_t lastEvent = events.empty() ? 0 : events.back().sample;
    const auto minTail = static_cast<std::int64_t>(1.5 * options.sampleRate);
    const auto maxTail = static_cast<std::int64_t>(maxRelease * options.sampleRate);
    const int blockSize = std::clamp(options.blockSize, 16, 4096);

    Played out;
    std::vector<float> left(static_cast<std::size_t>(blockSize)), right(static_cast<std::size_t>(blockSize));
    std::vector<MidiEvent> block;
    block.reserve(256);
    std::size_t next = 0;
    Transport transport;
    transport.playing = true;
    for (std::int64_t pos = 0;; pos += blockSize) {
        block.clear();
        while (next < events.size() && events[next].sample < pos + blockSize) {
            MidiEvent e = events[next].event;
            e.offset = static_cast<int>(std::max<std::int64_t>(events[next].sample - pos, 0));
            block.push_back(e);
            ++next;
        }
        transport.ppqPosition = static_cast<double>(pos) / options.sampleRate * transport.bpm / 60.0;
        engine.process({left.data(), right.data(), blockSize, block, transport});
        out.left.insert(out.left.end(), left.begin(), left.end());
        out.right.insert(out.right.end(), right.begin(), right.end());
        const std::int64_t after = pos + blockSize - lastEvent;
        if (after <= 0)
            continue;
        if (engine.activeVoices() == 0 && out.releaseSeconds < 0.0)
            out.releaseSeconds = static_cast<double>(after) / options.sampleRate;
        if ((out.releaseSeconds >= 0.0 && after >= minTail) || after >= maxTail)
            break;
    }
    out.recoveries = engine.outputRecoveries();
    return out;
}

// The realtime factor of an 8-note chord at the realtime quality: processing time over audio time,
// in percent, timed over two seconds after the notes have started.
double measureCpu(const Patch& patch, const Assets& assets, const PresetCheckOptions& options) {
    Engine engine;
    prepareEngine(engine, patch, assets, options.sampleRate, false);
    const int blockSize = std::clamp(options.blockSize, 16, 4096);
    std::vector<float> left(static_cast<std::size_t>(blockSize)), right(static_cast<std::size_t>(blockSize));
    std::vector<MidiEvent> chord;
    for (const int note : {48, 52, 55, 59, 62, 65, 69, 72})
        chord.push_back(MidiEvent::noteOn(0, note, 0.8f));
    Transport transport;
    transport.playing = true;
    engine.process({left.data(), right.data(), blockSize, chord, transport});
    const int settle = static_cast<int>(0.25 * options.sampleRate / blockSize);
    for (int b = 0; b < settle; ++b)
        engine.process({left.data(), right.data(), blockSize, {}, transport});
    const int blocks = std::max(1, static_cast<int>(2.0 * options.sampleRate / blockSize));
    const auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) {
        transport.ppqPosition += blockSize / options.sampleRate * transport.bpm / 60.0;
        engine.process({left.data(), right.data(), blockSize, {}, transport});
    }
    const double audio = static_cast<double>(blocks) * blockSize / options.sampleRate;
    return since(start) / audio * 100.0;
}

double peakOf(const Played& p) {
    float peak = 0.0f;
    for (std::size_t i = 0; i < p.left.size(); ++i)
        peak = std::max({peak, std::abs(p.left[i]), std::abs(p.right[i])});
    return peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -200.0;
}

// Everything but the CPU reading; `patch` and `assets` are kept for it.
PresetCheck checkWithoutCpu(const std::string& file, const std::string& relative, const ContentLibrary& library,
                            const PresetCheckOptions& options, Patch& patch, bool& loaded) {
    PresetCheck check;
    check.path = file;
    check.relative = relative;
    loaded = false;
    std::vector<std::uint8_t> bytes;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    if (!readFile(file, bytes, error)) {
        check.errors.push_back("cannot be read: " + error);
        return check;
    }
    LoadResult result = loadPatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    for (const Issue& issue : result.issues)
        (issue.severity == Severity::Error ? check.errors : check.warnings)
            .push_back((issue.path.empty() ? std::string("(document)") : issue.path) + ": " + issue.message);
    if (!result.ok())
        return check;
    patch = std::move(result.patch);
    check.name = patch.meta.name;
    check.type = patch.meta.type;
    const std::string presetDir = fs::path(file).parent_path().string();
    const Assets assets = loadAssets(patch, library, presetDir);
    check.loadMs = since(start) * 1000.0; // beside other renders; measured again alone with the CPU reading
    for (const std::string& problem : assets.problems)
        check.errors.push_back(problem);
    loaded = true;

    const std::vector<NoteSpec> phrase = auditionPhrase(check.type, check.phrase);
    const auto& reg = Registry::get();
    const auto on = [&](const char* id) { return patch.get(reg.indexOf(id)) >= 0.5; };
    // Notes that go on after the keys are let go by design: a latched arpeggiator, a clip that
    // follows the transport or is latched.
    const bool latched = (on("arp.on") && on("arp.latch")) ||
                         (on("clip.on") && (patch.get(reg.indexOf("clip.trigger")) < 0.5 || on("clip.latch")));
    const double maxRelease = latched ? 2.0 : options.maxReleaseSeconds;

    const Played normal = play(patch, assets, phrase, 100.0 / 127.0, false, maxRelease, options);
    const Played full = play(patch, assets, phrase, 1.0, false, maxRelease, options);
    const analysis::Descriptors a = analysis::analyze({normal.left, normal.right, options.sampleRate});
    const analysis::Descriptors b = analysis::analyze({full.left, full.right, options.sampleRate});
    check.peakDbfs = peakOf(normal);
    check.peakFullVelocityDbfs = peakOf(full);
    check.loudnessLufs = a.loudnessLufs;
    check.clippedSamples = a.clippedSamples + b.clippedSamples;
    check.releaseSeconds = std::max(normal.releaseSeconds, full.releaseSeconds);

    if (normal.recoveries + full.recoveries > 0)
        check.errors.push_back("the output held values that are not numbers (" + std::to_string(normal.recoveries + full.recoveries) +
                               " time(s)); the engine muted them");
    if (check.peakDbfs < options.silenceDbfs)
        check.errors.push_back("silent: the phrase peaks at " + fixed(check.peakDbfs, 1) + " dBFS (below " +
                               fixed(options.silenceDbfs, 0) + ")");
    if (check.clippedSamples > 0)
        check.errors.push_back("clipping: " + std::to_string(check.clippedSamples) + " sample(s) at full scale (peak " +
                               fixed(check.peakDbfs, 2) + " dBFS at velocity 100, " + fixed(check.peakFullVelocityDbfs, 2) +
                               " dBFS at 127)");
    if (!latched && (normal.releaseSeconds < 0.0 || full.releaseSeconds < 0.0))
        check.errors.push_back("voices still sounding " + fixed(options.maxReleaseSeconds, 0) + " s after the last note-off");
    if (latched)
        check.warnings.push_back("notes go on after the keys are let go by design (latched arpeggiator or clip); the release was not checked");

    if (check.peakDbfs > options.peakMarginDbfs && check.clippedSamples == 0)
        check.warnings.push_back("peak " + fixed(check.peakDbfs, 2) + " dBFS is above the " + fixed(options.peakMarginDbfs, 0) +
                                 " dBFS margin");
    const LoudnessTarget target = loudnessTarget(check.type);
    if (!target.peakDecides && check.loudnessLufs > -100.0 && std::abs(check.loudnessLufs - target.lufs) > target.tolerance)
        check.warnings.push_back("loudness " + fixed(check.loudnessLufs, 1) + " LUFS on its phrase; the guide's target for " +
                                 (check.type.empty() ? std::string("this type") : check.type) + " is about " + fixed(target.lufs, 0) +
                                 " LUFS");
    if (target.peakDecides && check.peakDbfs > options.silenceDbfs && check.peakDbfs < -9.0)
        check.warnings.push_back("peak " + fixed(check.peakDbfs, 1) + " dBFS; short, percussive sounds should peak near -1 to -3 dBFS");
    return check;
}

// Alone, with nothing rendering beside it: a load as the plug-in does one (the file read and
// checked, its tables built, everything handed to a running engine and taken over by its next
// block), then the CPU reading.
void measureCpuInto(PresetCheck& check, const Patch& patch, const ContentLibrary& library, const PresetCheckOptions& options) {
    const int blockSize = std::clamp(options.blockSize, 16, 4096);
    std::vector<float> left(static_cast<std::size_t>(blockSize)), right(static_cast<std::size_t>(blockSize));
    Assets assets;
    const auto timedLoad = [&] {
        Engine running;
        running.prepare(options.sampleRate);
        running.process({left.data(), right.data(), blockSize, {}, {}});
        const auto start = std::chrono::steady_clock::now();
        Patch reread = patch;
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (readFile(check.path, bytes, error)) {
            LoadResult result = loadPatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            if (result.ok())
                reread = std::move(result.patch);
        }
        assets = loadAssets(reread, library, fs::path(check.path).parent_path().string());
        running.loadPatch(reread);
        for (int i = 0; i < kNumOscs; ++i)
            running.setTable(i, assets.tables[static_cast<std::size_t>(i)]);
        if (assets.texture != nullptr)
            running.setTexture(assets.texture);
        running.process({left.data(), right.data(), blockSize, {}, {}});
        return since(start) * 1000.0;
    };
    // Built tables are cached while they are in use: `assets` is let go before each new attempt,
    // so every attempt builds them again. A reading over the budget is taken again; the lowest counts.
    check.loadMs = timedLoad();
    for (int attempt = 1; attempt < std::max(1, options.cpuAttempts) && check.loadMs > options.loadBudgetMs; ++attempt) {
        assets = Assets{};
        check.loadMs = std::min(check.loadMs, timedLoad());
    }
    if (check.loadMs > options.loadBudgetMs)
        check.warnings.push_back("loading took " + fixed(check.loadMs, 0) + " ms (budget " + fixed(options.loadBudgetMs, 0) + " ms)");
    double best = 1e300;
    for (int attempt = 0; attempt < std::max(1, options.cpuAttempts); ++attempt) {
        best = std::min(best, measureCpu(patch, assets, options));
        if (best <= options.cpuBudgetPercent)
            break;
    }
    check.cpuPercent = best;
    if (best > options.cpuBudgetPercent)
        check.errors.push_back("CPU " + fixed(best, 1) + " % of one core for an 8-note chord (budget " +
                               fixed(options.cpuBudgetPercent, 0) + " %)");
}

} // namespace

std::vector<NoteSpec> auditionPhrase(std::string_view typeText, std::string& name) {
    const std::string type = lower(typeText);
    const auto notes = [](std::initializer_list<NoteSpec> list) { return std::vector<NoteSpec>(list); };
    constexpr double v = 100.0 / 127.0;
    if (type == "bass") {
        name = "bass line around C2";
        return notes({{36, 0.0, 0.45, v}, {36, 0.5, 0.45, v}, {39, 1.0, 0.45, v}, {31, 1.5, 0.9, v}, {36, 2.5, 0.9, v}});
    }
    if (type == "808") {
        name = "three long hits around C2";
        return notes({{36, 0.0, 1.2, v}, {29, 1.5, 1.2, v}, {31, 3.0, 0.6, v}});
    }
    if (type == "lead" || type == "brass") {
        name = "melodic line around C4";
        return notes({{60, 0.0, 0.45, v}, {63, 0.5, 0.45, v}, {65, 1.0, 0.45, v}, {67, 1.5, 0.9, v}, {70, 2.5, 0.45, v}, {67, 3.0, 0.9, v}});
    }
    if (type == "pluck") {
        name = "seven short notes from A3";
        return notes({{57, 0.0, 0.2, v}, {60, 0.5, 0.2, v}, {64, 1.0, 0.2, v}, {67, 1.5, 0.2, v}, {64, 2.0, 0.2, v}, {60, 2.5, 0.2, v}, {57, 3.0, 0.4, v}});
    }
    if (type == "keys") {
        name = "two four-note chords";
        return notes({{48, 0.0, 1.8, v}, {52, 0.0, 1.8, v}, {55, 0.0, 1.8, v}, {59, 0.0, 1.8, v},
                      {45, 2.0, 1.8, v}, {48, 2.0, 1.8, v}, {52, 2.0, 1.8, v}, {55, 2.0, 1.8, v}});
    }
    if (type == "pad" || type == "strings") {
        name = "a held four-note chord";
        return notes({{48, 0.0, 5.0, v}, {55, 0.0, 5.0, v}, {60, 0.0, 5.0, v}, {63, 0.0, 5.0, v}});
    }
    if (type == "synth") {
        name = "three chord stabs and a held chord";
        return notes({{48, 0.0, 0.3, v}, {51, 0.0, 0.3, v}, {55, 0.0, 0.3, v}, {48, 0.5, 0.3, v}, {51, 0.5, 0.3, v}, {55, 0.5, 0.3, v},
                      {48, 1.0, 0.3, v}, {51, 1.0, 0.3, v}, {55, 1.0, 0.3, v}, {44, 1.5, 2.0, v}, {48, 1.5, 2.0, v}, {51, 1.5, 2.0, v}});
    }
    if (type == "sequence" || type == "arp") {
        name = "a held four-note chord for the pattern";
        return notes({{48, 0.0, 5.0, v}, {51, 0.0, 5.0, v}, {55, 0.0, 5.0, v}, {58, 0.0, 5.0, v}});
    }
    if (type == "fx") {
        name = "one note held for five seconds";
        return notes({{48, 0.0, 5.0, v}});
    }
    if (type == "percussion" || type == "drums" || type == "drum") {
        name = "six hits";
        return notes({{36, 0.0, 0.25, v}, {36, 0.5, 0.25, v}, {36, 1.0, 0.25, v}, {36, 1.5, 0.25, v}, {38, 2.0, 0.25, v}, {40, 2.5, 0.25, v}});
    }
    name = "a held chord and a single note";
    return notes({{48, 0.0, 2.0, v}, {52, 0.0, 2.0, v}, {55, 0.0, 2.0, v}, {60, 2.5, 1.0, v}});
}

LoudnessTarget loudnessTarget(std::string_view typeText) {
    const std::string type = lower(typeText);
    if (type == "bass")
        return {-13.0, 4.0, false};
    if (type == "pad" || type == "synth" || type == "keys")
        return {-13.0, 4.0, false};
    if (type == "lead" || type == "strings" || type == "brass")
        return {-14.0, 4.0, false};
    if (type == "pluck" || type == "sequence" || type == "arp" || type == "percussion" || type == "drums" || type == "808" ||
        type == "fx")
        return {-16.0, 4.0, true};
    return {-14.0, 4.0, false};
}

int ContentCheckReport::failures() const {
    return static_cast<int>(std::count_if(presets.begin(), presets.end(), [](const PresetCheck& p) { return !p.ok(); }));
}

PresetCheck checkPreset(const std::string& file, const ContentLibrary& library, const PresetCheckOptions& options) {
    Patch patch;
    bool loaded = false;
    PresetCheck check = checkWithoutCpu(file, fs::path(file).filename().string(), library, options, patch, loaded);
    if (loaded && options.measureCpu)
        measureCpuInto(check, patch, library, options);
    return check;
}

ContentCheckReport checkPresets(const std::string& presetsDir, const ContentLibrary& library, const PresetCheckOptions& options) {
    ContentCheckReport report;
    std::error_code ec;
    const fs::path base(presetsDir);
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".zpreset")
            files.push_back(it->path());
    if (ec)
        report.problems.push_back("the folder " + presetsDir + " cannot be read: " + ec.message());
    if (files.empty()) {
        report.problems.push_back("no presets (*.zpreset) were found in " + presetsDir);
        return report;
    }
    std::sort(files.begin(), files.end());
    const int total = static_cast<int>(files.size());
    report.presets.resize(files.size());
    std::vector<Patch> patches(files.size());
    std::vector<char> loaded(files.size(), 0);

    // Renders run in parallel; each worker takes the next preset.
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const int threads = std::clamp(options.threads > 0 ? options.threads : static_cast<int>(std::min(hardware, 4u)), 1, 16);
    std::atomic<int> nextIndex{0};
    std::atomic<int> done{0};
    rt::Mutex reporting;
    const auto worker = [&] {
        for (int i = nextIndex.fetch_add(1); i < total; i = nextIndex.fetch_add(1)) {
            const auto k = static_cast<std::size_t>(i);
            std::error_code relativeError;
            const std::string relative = fs::relative(files[k], base, relativeError).generic_string();
            bool ok = false;
            report.presets[k] = checkWithoutCpu(files[k].string(), relative, library, options, patches[k], ok);
            loaded[k] = ok ? 1 : 0;
            if (!options.measureCpu && options.progress) {
                const std::lock_guard<rt::Mutex> lock(reporting);
                options.progress(done.fetch_add(1) + 1, total, report.presets[k]);
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t)
        pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool)
        t.join();

    // The CPU readings run one at a time, with nothing else rendering beside them.
    if (options.measureCpu) {
        for (std::size_t k = 0; k < files.size(); ++k) {
            if (loaded[k] != 0)
                measureCpuInto(report.presets[k], patches[k], library, options);
            patches[k] = Patch{};
            if (options.progress)
                options.progress(static_cast<int>(k) + 1, total, report.presets[k]);
        }
    }
    return report;
}

nlohmann::json toJson(const PresetCheck& c) {
    J o = J::object();
    o["preset"] = c.relative;
    o["name"] = c.name;
    o["type"] = c.type;
    o["ok"] = c.ok();
    o["phrase"] = c.phrase;
    o["load_ms"] = std::round(c.loadMs * 10.0) / 10.0;
    o["peak_dbfs"] = std::round(c.peakDbfs * 100.0) / 100.0;
    o["peak_velocity_127_dbfs"] = std::round(c.peakFullVelocityDbfs * 100.0) / 100.0;
    o["loudness_lufs"] = std::round(c.loudnessLufs * 10.0) / 10.0;
    o["clipped_samples"] = c.clippedSamples;
    o["release_s"] = std::round(c.releaseSeconds * 100.0) / 100.0;
    o["cpu_percent"] = std::round(c.cpuPercent * 100.0) / 100.0;
    o["errors"] = c.errors;
    o["warnings"] = c.warnings;
    return o;
}

nlohmann::json toJson(const ContentCheckReport& report) {
    J o = J::object();
    o["format"] = "zederia.content_check";
    o["presets"] = static_cast<int>(report.presets.size());
    o["failures"] = report.failures();
    o["problems"] = report.problems;
    J list = J::array();
    for (const PresetCheck& c : report.presets)
        list.push_back(toJson(c));
    o["results"] = std::move(list);
    return o;
}

} // namespace zed
