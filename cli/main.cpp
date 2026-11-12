// zederia-cli: render, validate-patch, params, bench, and the wavetable toolkit (table, texture,
// content; see TableCommands.cpp).
#include "CliUtil.h"
#include "zederia/Version.h"
#include "zederia/bench/Bench.h"
#include "zederia/content/Library.h"
#include "zederia/params/ParamDocs.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

using namespace cli;
using zed::Issue;
using J = nlohmann::ordered_json;

const char* kHelp = R"(zederia-cli %s

Usage: zederia-cli <command> [options]

  render          Render a patch to a WAV file
      --patch FILE|init        patch to play ("init" is the default patch)
      --notes "LIST"           notes as note:start:duration[:velocity], comma separated;
                               note is a MIDI number or a name (C4 = 60), times in seconds
      --midi FILE              or: a Standard MIDI File to play
      --out FILE.wav           output file
      --rate HZ                sample rate (default 48000)
      --block N                block size (default 512)
      --bits 16|24|32f         sample format (default 24)
      --tail SECONDS           time rendered after the last event (default: the patch's own tail)
      --seed N                 random seed (default 1); same seed, same file
      --tempo BPM              tempo reported to the engine (default 120)
      --content DIR            factory content folder for "factory:" table and sample references
      --live-quality           render with the patch's realtime quality instead of its offline quality

  validate-patch FILE          Check a patch; exit code 1 when it has errors
      --json                   print the result as JSON
      --print [--compact]      print the patch as Zederia would save it

  params                       Print the parameter registry
      --format md|json         Markdown (default) or JSON
      --out FILE               write to a file instead of the console
      --check FILE             compare with a file; exit code 1 when it is out of date

  bench                        Measure the realtime factor of a scenario
      --scenario NAME|all      scenario to run (default all); --list shows them
      --seconds S              audio processed per run (default 20)
      --runs N                 runs; the fastest counts (default 5)
      --baseline FILE          compare with stored results; exit code 1 on a regression of more than 10 %%
      --update-baseline        write the measured results to the baseline file

  table SUBCOMMAND             The wavetable toolkit. Tables are WAV files of 2048-sample frames.
                               Frame numbers start at 0; --scope is all, a frame, or a range such as 2-7.
      info FILE [--json]       name, frames, morph mode, level and size
      new --frames N --out FILE
      formula --expr "E" [--in FILE | --frames N] [--domain wave|harmonics] [--phase "E"] [--scope S]
              [--selected N] [--seed N] [--normalize] [--morph MODE] --out FILE
                               symbols t x y z h in sel rand pi tau e; see docs/ARCHITECTURE.md
      process --in FILE --op NAME [--n N] [--scope S] --out FILE
                               normalize_each normalize_shared remove_dc flip_polarity mirror_time
                               shift_to_zero_crossing clear fade_edges crossfade_edges low_pass
                               remove_fundamental sample_rate_reduce align_fundamental
      harmonic --in FILE --op NAME [--n N] [--amount X] [--seed N] [--scope S] --out FILE
                               clear_all clear_above clear_below fill_saw randomize_lowest randomize_all
                               random_gaps fade_highs octave_up octave_down odd_only even_only
                               snap_quarters scale_by_harmonic
      frames --in FILE --op NAME [--at N] [--from N --to N] [--every N] [--source N] [--scope S] --out FILE
                               insert duplicate delete move thin crop reverse split join
                               copy_spectrum copy_phases trim_silent
      morph --in FILE [--mode none|crossfade|spectral|spectral_aligned|spectral_zero]
            [--stepped on|off] [--bake N] --out FILE
      sort --in FILE --by brightest_peak|brightness|strongest_partial|active_partials|
           highest_partial|fundamental [--descending] --out FILE
      import FILE... [--method auto|table|cycles|pitch|fixed|spectral|image] --out FILE
              [--channel left|right|mid|split]     stereo files; split writes _left and _right tables
              [--frame-size N]                     table: samples per frame in the file
              [--snap] [--max-frames N] [--average]  pitch, fixed: cut on zero crossings; reduce cycles
              [--cycle-samples X | --note A3]      fixed: cycle length (default: detected)
              [--fft 256..2048] [--frames N] [--phase original|zero|random]   spectral
              [--image-mode amplitude|harmonics]   image (PNG, BMP, PGM, PPM): rows become frames
              [--fade-edges N] [--normalize] [--trim-silent DB]   follow-ups
      resynth --op bake-warp --in FILE --mode WARP [--position P] [--frames N] --out FILE
      resynth --op bake-patch [--patch FILE|init] [--bars B] [--tempo BPM] [--note C3] [--frames N] --out FILE
      resynth --op shape --values "0,1,0.5,0" [--unipolar] --out FILE
      export --in FILE [--scope S] --out FILE | --generated N --out FILE | --out-dir DIR
                               a selection, the generated positions, or a folder of single cycles
      build RECIPE.json --out FILE

  texture build RECIPE.json --out FILE.wav
  content build --in RECIPES_DIR --out FACTORY_DIR
                               Build every wavetable and texture recipe (see content/README.md)

  --version                    Print the version
)";

void printIssues(const std::vector<Issue>& issues, std::FILE* to) {
    for (const auto& i : issues)
        std::fprintf(to, "%s %s  %s\n    fix: %s\n", i.severity == zed::Severity::Error ? "error  " : "warning",
                     i.path.empty() ? "(document)" : i.path.c_str(), i.message.c_str(), i.fix.c_str());
}

bool loadPatchArg(const std::string& arg, zed::Patch& patch, std::string* presetDir = nullptr) {
    if (arg.empty() || arg == "init")
        return true;
    if (presetDir != nullptr)
        *presetDir = std::filesystem::path(arg).parent_path().string();
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!zed::readFile(arg, bytes, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }
    auto result = zed::loadPatch({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    printIssues(result.issues, stderr);
    if (!result.ok()) {
        std::fprintf(stderr, "error: %s is not a usable patch (%d error(s))\n", arg.c_str(), result.errors());
        return false;
    }
    patch = std::move(result.patch);
    return true;
}

// --- render ------------------------------------------------------------------------------------

int cmdRender(const Args& args) {
    if (!args.has("out") || (args.has("notes") == args.has("midi"))) {
        std::fprintf(stderr, "error: render needs --out and exactly one of --notes or --midi\n");
        return kUsage;
    }
    zed::RenderSettings settings;
    double block = settings.blockSize, seed = 1, tail = -1;
    if (!numberOption(args, "rate", 8000, 384000, settings.sampleRate) || !numberOption(args, "block", 1, 8192, block) ||
        !numberOption(args, "seed", 0, 9007199254740992.0, seed) || !numberOption(args, "tail", 0, 600, tail) ||
        !numberOption(args, "tempo", 20, 999, settings.bpm))
        return kUsage;
    settings.blockSize = static_cast<int>(block);
    settings.seed = static_cast<std::uint64_t>(seed);
    settings.tailSeconds = tail;

    const std::string bits = args.get("bits", "24");
    if (bits != "16" && bits != "24" && bits != "32f") {
        std::fprintf(stderr, "error: --bits must be 16, 24 or 32f\n");
        return kUsage;
    }
    const auto format = bits == "16" ? zed::WavFormat::Pcm16 : bits == "24" ? zed::WavFormat::Pcm24 : zed::WavFormat::Float32;

    zed::Patch patch;
    std::string presetDir;
    if (!loadPatchArg(args.get("patch", "init"), patch, &presetDir))
        return kFailed;
    settings.offlineQuality = !args.has("live-quality");

    // The tables and the sample the patch names. What cannot be found is replaced by the built-in
    // table or noise, with a warning.
    zed::ContentLibrary library = zed::ContentLibrary::standard();
    if (args.has("content"))
        library.setFactoryDir(args.get("content"));
    for (int i = 0; i < zed::kNumOscs; ++i) {
        std::string problem;
        settings.tables[static_cast<std::size_t>(i)] = library.loadTable(patch.tables[static_cast<std::size_t>(i)], problem, presetDir);
        if (settings.tables[static_cast<std::size_t>(i)] == nullptr)
            std::fprintf(stderr, "warning: osc%d: %s; the built-in table is used instead\n", i + 1, problem.c_str());
    }
    {
        std::string problem;
        settings.texture = library.loadTexture(patch.textureSample, problem, presetDir);
        if (settings.texture == nullptr)
            std::fprintf(stderr, "warning: texture: %s; the built-in noise is used instead\n", problem.c_str());
    }

    std::vector<zed::SeqEvent> events;
    std::string error;
    if (args.has("notes")) {
        std::vector<zed::NoteSpec> notes;
        if (!zed::parseNoteList(args.get("notes"), notes, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kUsage;
        }
        events = zed::sequenceFromNotes(notes, settings.sampleRate);
    } else {
        std::vector<std::uint8_t> bytes;
        if (!zed::readFile(args.get("midi"), bytes, error) ||
            !zed::sequenceFromMidiFile(bytes, settings.sampleRate, events, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kFailed;
        }
    }
    if (!events.empty() && static_cast<double>(events.back().sample) / settings.sampleRate > 3600.0) {
        std::fprintf(stderr, "error: the render would be longer than an hour\n");
        return kFailed;
    }

    const zed::RenderResult audio = zed::renderOffline(patch, events, settings);
    const auto wav = zed::encodeWav(audio.left, audio.right, audio.sampleRate, format);
    if (!zed::writeFile(args.get("out"), wav, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kFailed;
    }
    const double peak = audio.peak();
    std::printf("wrote %s\n", args.get("out").c_str());
    std::printf("  %.3f s, %zu frames, %g Hz, %s-bit, %zu events\n", static_cast<double>(audio.left.size()) / audio.sampleRate,
                audio.left.size(), audio.sampleRate, bits.c_str(), events.size());
    std::printf("  peak %.2f dBFS\n", zed::gainToDb(peak));
    std::printf("  audio hash %016llx, file hash %016llx\n", static_cast<unsigned long long>(audio.hash()),
                static_cast<unsigned long long>(zed::fnv1a(wav.data(), wav.size())));
    return kOk;
}

// --- validate-patch ----------------------------------------------------------------------------

int cmdValidate(const Args& args) {
    if (args.positional.size() != 1) {
        std::fprintf(stderr, "error: validate-patch needs one file\n");
        return kUsage;
    }
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!zed::readFile(args.positional[0], bytes, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kFailed;
    }
    const auto result = zed::loadPatch({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    const int errors = result.errors();
    const int warnings = static_cast<int>(result.issues.size()) - errors;

    if (args.has("print")) {
        printIssues(result.issues, stderr);
        if (!result.ok())
            return kFailed;
        zed::SaveOptions options;
        options.compact = args.has("compact");
        std::fputs(zed::savePatch(result.patch, options).c_str(), stdout);
        return kOk;
    }
    if (args.has("json")) {
        J out = J::object();
        out["ok"] = result.ok();
        out["errors"] = errors;
        out["warnings"] = warnings;
        J issues = J::array();
        for (const auto& i : result.issues)
            issues.push_back({{"severity", i.severity == zed::Severity::Error ? "error" : "warning"},
                              {"code", i.code},
                              {"path", i.path},
                              {"message", i.message},
                              {"fix", i.fix}});
        out["issues"] = std::move(issues);
        std::printf("%s\n", out.dump(2, ' ', false, J::error_handler_t::replace).c_str());
    } else {
        printIssues(result.issues, stdout);
        std::printf("%s: %s (%d error(s), %d warning(s))\n", args.positional[0].c_str(), result.ok() ? "valid" : "INVALID",
                    errors, warnings);
    }
    return result.ok() ? kOk : kFailed;
}

// --- params ------------------------------------------------------------------------------------

std::string withoutCarriageReturns(std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

int cmdParams(const Args& args) {
    const std::string format = args.get("format", "md");
    if (format != "md" && format != "json") {
        std::fprintf(stderr, "error: --format must be md or json\n");
        return kUsage;
    }
    const std::string text = format == "md" ? zed::registryMarkdown() : zed::registryJson();
    if (args.has("check")) {
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!zed::readFile(args.get("check"), bytes, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kFailed;
        }
        const std::string onDisk = withoutCarriageReturns({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        if (onDisk != text) {
            std::fprintf(stderr, "%s is out of date; regenerate it with: zederia-cli params --format %s --out %s\n",
                         args.get("check").c_str(), format.c_str(), args.get("check").c_str());
            return kFailed;
        }
        std::printf("%s is up to date\n", args.get("check").c_str());
        return kOk;
    }
    if (args.has("out"))
        return writeText(args.get("out"), text) ? kOk : kFailed;
    std::fputs(text.c_str(), stdout);
    return kOk;
}

// --- bench -------------------------------------------------------------------------------------

// A result fails when it is over the plan's budget, or more than 10 % slower than the baseline.
// The small absolute allowance keeps timer noise on near-zero loads from failing the step.
constexpr double kRegression = 1.10;
constexpr double kNoiseAllowance = 0.02; // percentage points of realtime

int cmdBench(const Args& args) {
    const auto scenarios = zed::benchScenarios();
    if (args.has("list")) {
        for (const auto& s : scenarios)
            std::printf("%-12s %s (budget %.2f %%)\n", s.name.c_str(), s.description.c_str(), s.budgetPercent);
        return kOk;
    }
    double seconds = 20, runs = 5;
    if (!numberOption(args, "seconds", 0.1, 600, seconds) || !numberOption(args, "runs", 1, 100, runs))
        return kUsage;
    const std::string wanted = args.get("scenario", "all");

    J baseline = J::object();
    const std::string baselinePath = args.get("baseline");
    if (!baselinePath.empty() && !args.has("update-baseline")) {
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!zed::readFile(baselinePath, bytes, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kFailed;
        }
        baseline = J::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (!baseline.is_object() || !baseline.contains("scenarios") || !baseline["scenarios"].is_object()) {
            std::fprintf(stderr, "error: %s is not a baseline file\n", baselinePath.c_str());
            return kFailed;
        }
    }
#ifndef NDEBUG
    std::printf("note: this is a debug build; the numbers are only meaningful in Release\n");
#endif

    bool found = false, failed = false;
    J measured = J::object();
    std::printf("%-12s %12s %10s %10s  %s\n", "scenario", "realtime %", "budget %", "baseline %", "result");
    for (const auto& s : scenarios) {
        if (wanted != "all" && wanted != s.name)
            continue;
        found = true;
        zed::BenchResult result;
        zed::runBench(s.name, seconds, static_cast<int>(runs), result);
        measured[s.name] = std::round(result.percent * 10000.0) / 10000.0;

        std::string verdict = "ok";
        std::string baselineText = "-";
        if (result.percent > s.budgetPercent) {
            verdict = "FAIL: over budget";
            failed = true;
        }
        if (baseline.contains("scenarios") && baseline["scenarios"].contains(s.name) &&
            baseline["scenarios"][s.name].is_number()) {
            const double stored = baseline["scenarios"][s.name].get<double>();
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.4f", stored);
            baselineText = buf;
            if (result.percent > stored * kRegression + kNoiseAllowance) {
                verdict = "FAIL: more than 10 % slower than the baseline";
                failed = true;
            }
        } else if (!baselinePath.empty() && !args.has("update-baseline")) {
            verdict += " (no baseline stored)";
        }
        std::printf("%-12s %12.4f %10.2f %10s  %s\n", s.name.c_str(), result.percent, s.budgetPercent, baselineText.c_str(),
                    verdict.c_str());
    }
    if (!found) {
        std::fprintf(stderr, "error: unknown scenario \"%s\"; --list shows the scenarios\n", wanted.c_str());
        return kUsage;
    }
    if (args.has("update-baseline")) {
        if (baselinePath.empty()) {
            std::fprintf(stderr, "error: --update-baseline needs --baseline FILE\n");
            return kUsage;
        }
        J doc = J::object();
        doc["about"] = "Realtime factor in percent: one core, 48 kHz, 512-sample blocks, Release build. "
                       "Written by zederia-cli bench --update-baseline.";
        doc["zederia_version"] = zed::kVersion;
        doc["scenarios"] = measured;
        if (!writeText(baselinePath, doc.dump(2) + "\n"))
            return kFailed;
        std::printf("baseline written to %s\n", baselinePath.c_str());
    }
    return failed ? kFailed : kOk;
}

} // namespace

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "help";
    if (command == "--version" || command == "version") {
        std::printf("zederia-cli %s\n", zed::kVersion);
        return kOk;
    }
    Args args;
    if (!parseArgs(argc, argv, 2, args))
        return kUsage;
    try {
        if (command == "render") return cmdRender(args);
        if (command == "validate-patch") return cmdValidate(args);
        if (command == "params") return cmdParams(args);
        if (command == "bench") return cmdBench(args);
        if (command == "table") return cmdTable(args);
        if (command == "texture") return cmdTexture(args);
        if (command == "content") return cmdContent(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return kFailed;
    }
    const bool help = command == "help" || command == "--help" || command == "-h";
    if (!help)
        std::fprintf(stderr, "error: unknown command \"%s\"\n\n", command.c_str());
    std::fprintf(help ? stdout : stderr, kHelp, zed::kVersion);
    return help ? kOk : kUsage;
}
