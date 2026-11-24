// zederia-cli content check and soak: the release checks of milestone 7.
#include "CliUtil.h"
#include "ProcessMemory.h"
#include "zederia/bench/Soak.h"
#include "zederia/content/ContentCheck.h"
#include "zederia/content/Library.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace cli {
namespace {

namespace fs = std::filesystem;

zed::ContentLibrary libraryFor(const Args& args) {
    zed::ContentLibrary library = zed::ContentLibrary::standard();
    if (args.has("content"))
        library.setFactoryDir(args.get("content"));
    if (args.has("user"))
        library.setUserDir(args.get("user"));
    return library;
}

void printCheck(const zed::PresetCheck& c, bool verbose) {
    const bool failed = !c.ok();
    if (!failed && !verbose && c.warnings.empty())
        return;
    std::printf("%-5s %-38s %-9s %6.1f LUFS  peak %6.1f / %6.1f dBFS  cpu %5.2f %%  load %4.0f ms\n", failed ? "FAIL" : "ok",
                c.relative.c_str(), c.type.c_str(), c.loudnessLufs, c.peakDbfs, c.peakFullVelocityDbfs, c.cpuPercent, c.loadMs);
    for (const auto& e : c.errors)
        std::printf("        error: %s\n", e.c_str());
    for (const auto& w : c.warnings)
        std::printf("        warning: %s\n", w.c_str());
}

} // namespace

int cmdContentCheck(const Args& args) {
    const zed::ContentLibrary library = libraryFor(args);
    const std::string presets = args.has("presets") ? args.get("presets") : (fs::path(library.factoryDir()) / "Presets").string();
    zed::PresetCheckOptions options;
    double budget = options.cpuBudgetPercent, threads = 0;
    if (!numberOption(args, "budget", 0.1, 100.0, budget) || !numberOption(args, "threads", 1, 16, threads))
        return kUsage;
    options.cpuBudgetPercent = budget;
    options.threads = static_cast<int>(threads);
    options.measureCpu = !args.has("no-cpu");
    const bool verbose = args.has("verbose");
    options.progress = [&](int done, int total, const zed::PresetCheck& c) {
        printCheck(c, verbose);
        if (done % 20 == 0 || done == total)
            std::fprintf(stderr, "  checked %d of %d\n", done, total);
    };
    std::printf("content check: %s (factory content %s)\n", presets.c_str(), library.factoryDir().c_str());
    const zed::ContentCheckReport report = zed::checkPresets(presets, library, options);
    for (const auto& p : report.problems)
        std::fprintf(stderr, "error: %s\n", p.c_str());

    // A summary: loudness per type, the costliest presets, the warnings.
    std::map<std::string, std::pair<double, double>> range;
    int warned = 0;
    double cpuMax = 0.0, loadMax = 0.0;
    std::string cpuWorst, loadWorst;
    for (const auto& c : report.presets) {
        warned += c.warnings.empty() ? 0 : 1;
        if (c.loudnessLufs > -100.0) {
            auto [it, fresh] = range.try_emplace(c.type, c.loudnessLufs, c.loudnessLufs);
            if (!fresh)
                it->second = {std::min(it->second.first, c.loudnessLufs), std::max(it->second.second, c.loudnessLufs)};
        }
        if (c.cpuPercent > cpuMax) {
            cpuMax = c.cpuPercent;
            cpuWorst = c.relative;
        }
        if (c.loadMs > loadMax) {
            loadMax = c.loadMs;
            loadWorst = c.relative;
        }
    }
    std::printf("\nloudness on the audition phrase, by type:\n");
    for (const auto& [type, r] : range)
        std::printf("  %-10s %6.1f to %6.1f LUFS\n", type.c_str(), r.first, r.second);
    if (options.measureCpu)
        std::printf("highest CPU: %.2f %% of one core (%s); budget %.0f %%\n", cpuMax, cpuWorst.c_str(), options.cpuBudgetPercent);
    std::printf("slowest load: %.0f ms (%s)\n", loadMax, loadWorst.c_str());
    std::printf("%zu preset(s): %zu passed, %d failed, %d with warnings\n", report.presets.size(),
                report.presets.size() - static_cast<std::size_t>(report.failures()), report.failures(), warned);
    if (args.has("report")) {
        if (!writeText(args.get("report"), zed::toJson(report).dump(2) + "\n"))
            return kFailed;
        std::printf("wrote %s\n", args.get("report").c_str());
    }
    return report.ok() ? kOk : kFailed;
}

int cmdSoak(const Args& args) {
    zed::SoakOptions options;
    options.library = libraryFor(args);
    double minutes = 10.0, seed = 1, presetSeconds = options.presetSeconds, moves = options.movesPerSecond, notes = options.notesPerSecond;
    double growthLimit = 48.0;
    if (!numberOption(args, "minutes", 0.01, 24 * 60, minutes) || !numberOption(args, "seed", 0, 1e15, seed) ||
        !numberOption(args, "preset-seconds", 0.5, 600, presetSeconds) || !numberOption(args, "moves", 0, 10000, moves) ||
        !numberOption(args, "notes", 0, 1000, notes) || !numberOption(args, "growth-limit", 1, 100000, growthLimit))
        return kUsage;
    options.seconds = minutes * 60.0;
    options.seed = static_cast<std::uint64_t>(seed);
    options.presetSeconds = presetSeconds;
    options.movesPerSecond = moves;
    options.notesPerSecond = notes;
    options.switchWhileHeld = !args.has("release-before-switch");
    for (std::string list = args.get("keep-still"); !list.empty();) {
        const std::size_t comma = std::min(list.find(','), list.size());
        if (comma > 0)
            options.keepStill.push_back(list.substr(0, comma));
        list = comma < list.size() ? list.substr(comma + 1) : std::string();
    }
    if (args.has("presets")) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(fs::path(args.get("presets")), ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".zpreset")
                options.presets.push_back(it->path().string());
        std::sort(options.presets.begin(), options.presets.end());
        if (options.presets.empty()) {
            std::fprintf(stderr, "error: no presets (*.zpreset) were found in %s\n", args.get("presets").c_str());
            return kFailed;
        }
    }
    // Memory at the start of each pass through the presets: after the first pass it must hold still.
    std::vector<double> passMemory;
    double lastReport = -1.0;
    options.progress = [&](const zed::SoakProgress& p) {
        if (p.passStart)
            passMemory.push_back(privateMegabytes());
        if (p.audioSeconds - lastReport >= 60.0 || lastReport < 0.0) {
            lastReport = p.audioSeconds;
            std::fprintf(stderr, "  %7.0f s of audio in %5.0f s, pass %d, memory %.1f MB\n", p.audioSeconds, p.wallSeconds, p.pass + 1,
                         privateMegabytes());
        }
        return true;
    };
    std::printf("soak: %.1f minutes of audio, seed %llu, %zu preset(s), %.0f s each, %.0f moves and %.0f notes per second\n", minutes,
                static_cast<unsigned long long>(options.seed), options.presets.size(), presetSeconds, moves, notes);
    const double memoryBefore = privateMegabytes();
    const zed::SoakReport r = zed::runSoak(options);
    const double memoryAfter = privateMegabytes();

    std::printf("rendered %.0f s of audio in %.0f s (%.1f x real time)\n", r.audioSeconds, r.wallSeconds,
                r.wallSeconds > 0.0 ? r.audioSeconds / r.wallSeconds : 0.0);
    std::printf("presets loaded %d (%d pass(es)), parameter moves %d, notes %d, release checks %d\n", r.presetsLoaded, r.passes, r.moves, r.notes,
                r.releaseChecks);
    std::printf("non-finite blocks %d, output recoveries %d, runaway blocks %d, stuck releases %d, load problems %d, loudest %.1f dBFS\n",
                r.nonFiniteBlocks, r.outputRecoveries, r.runawayBlocks, r.stuckReleases, r.loadProblems, r.peakDbfs);
    bool memoryOk = true;
    double growth = 0.0;
    if (passMemory.size() >= 3 && passMemory.front() > 0.0) {
        // The first pass fills caches and pools; growth is counted from the second pass on.
        growth = passMemory.back() - passMemory[1];
        memoryOk = growth <= growthLimit;
        std::printf("memory at the start of each pass (MB):");
        for (const double m : passMemory)
            std::printf(" %.1f", m);
        std::printf("\nmemory growth from pass 2 to pass %zu: %+.1f MB (limit %.0f MB)\n", passMemory.size(), growth, growthLimit);
    } else {
        std::printf("memory: %.1f MB before, %.1f MB after (fewer than three passes: growth not judged)\n", memoryBefore, memoryAfter);
    }
    for (const auto& e : r.events)
        std::printf("  at %8.1f s  %s: %s\n", e.atSeconds, e.preset.c_str(), e.what.c_str());
    if (args.has("report")) {
        nlohmann::json j;
        j["format"] = "zederia.soak";
        j["audio_s"] = r.audioSeconds;
        j["wall_s"] = r.wallSeconds;
        j["presets_loaded"] = r.presetsLoaded;
        j["passes"] = r.passes;
        j["moves"] = r.moves;
        j["notes"] = r.notes;
        j["release_checks"] = r.releaseChecks;
        j["non_finite_blocks"] = r.nonFiniteBlocks;
        j["output_recoveries"] = r.outputRecoveries;
        j["runaway_blocks"] = r.runawayBlocks;
        j["stuck_releases"] = r.stuckReleases;
        j["load_problems"] = r.loadProblems;
        j["peak_dbfs"] = r.peakDbfs;
        j["pass_memory_mb"] = passMemory;
        j["memory_growth_mb"] = growth;
        nlohmann::json events = nlohmann::json::array();
        for (const auto& e : r.events)
            events.push_back({{"at_s", e.atSeconds}, {"preset", e.preset}, {"what", e.what}});
        j["events"] = events;
        if (!writeText(args.get("report"), j.dump(2) + "\n"))
            return kFailed;
    }
    const bool ok = r.ok() && memoryOk && r.loadProblems == 0;
    std::printf("%s\n", ok ? "soak: PASS" : "soak: FAIL");
    return ok ? kOk : kFailed;
}

} // namespace cli
