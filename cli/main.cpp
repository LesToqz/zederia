// zederia-cli: render, validate-patch, params, bench, and the wavetable toolkit (table, texture,
// content; see TableCommands.cpp).
#include "CliUtil.h"
#include "zederia/Version.h"
#include "zederia/bench/Bench.h"
#include "zederia/content/Library.h"
#include "zederia/midi/MidiFile.h"
#include "zederia/params/ParamDocs.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
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
      --midi-out FILE.mid      also write the notes the arpeggiator, chord tools and clips made

  clip import FILE.mid --patch IN --slot N --out OUT.zpreset
                               Put a Standard MIDI File into clip slot N (1..8) of a patch
  clip export --patch IN --slot N --out FILE.mid
                               Write clip slot N of a patch as a Standard MIDI File

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
      --profile                also show where the time goes, section by section
      --report-only            print the verdicts but always exit with code 0

  ui-snapshot                  Render editor pages offscreen to PNG and check them (plan 6.1)
      --page NAME|all          synth fx mod table play library agent kit, or all (default all)
      --scale LIST             percentages, comma separated (default 100,150,200)
      --theme NAME|all         Graphite (the only theme), or all
      --out DIR                folder for the images (default ui-snapshots)
      --patch FILE             a patch to load first
      --content DIR            factory content folder
      --user DIR               user folder (presets for the library page)
      --settings FILE          preferences file to use (default: the defaults, not yours)
      --play NOTES             hold these keys (for example 48,52,55) so the displays move
      --trace                  print each step as it happens
      --audit                  check clipping, overlap, truncated and small text, unlabeled
                               controls and theme contrast; exit code 1 on any finding

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
  content build --in CONTENT_DIR --out FACTORY_DIR [--verbose]
                               Build the factory folder: every wavetable, texture and clip recipe,
                               and the presets, shapes and racks, each checked (content/README.md)
  content check                Load and play every preset; exit code 1 on a load error, a missing
                               table or sample, clipping, silence, stuck voices or CPU above budget
      --presets DIR            presets to check (default: the factory folder's Presets)
      --content DIR            factory folder for "factory:" references
      --budget PCT             CPU budget for an 8-note chord, percent of one core (default 35)
      --threads N              parallel renders (default: up to 4); CPU is measured alone
      --no-cpu                 skip the CPU readings
      --report FILE.json       write every result
      --verbose                a line for every preset, not only those with findings

  soak                         Long render under random automation, notes and preset switches;
                               exit code 1 on non-finite output, runaway levels, stuck voices,
                               presets that fail to load or memory that grows between passes
      --minutes M              audio to render (default 10)
      --presets DIR            presets to switch between (default: the init patch only)
      --content DIR            factory folder for "factory:" references
      --seed N                 random seed (default 1)
      --preset-seconds S       how long each preset plays (default 12)
      --moves N                parameter moves per second (default 60)
      --notes N                notes per second (default 8)
      --growth-limit MB        memory growth allowed from the second pass on (default 48)
      --report FILE.json       write the results

  brand --svg FILE.svg --out DIR [--installer DIR]
                               Make the icons from the logo (plugin/brand/zederia-mark.svg):
                               DIR/zederia.ico (16 to 256 px) and DIR/png/zederia-<size>.png, and with
                               --installer the installer's wizard images; exit code 1 when the gap
                               between the slabs closes at 16 or 32 px (scripts/make-icons.ps1)

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
    {
        std::vector<std::string> problems; // tables kept in the patch or named by it, built side by side
        settings.tables = zed::loadPatchTables(patch, library, problems, presetDir);
        for (const auto& problem : problems)
            std::fprintf(stderr, "warning: %s\n", problem.c_str());
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
    if (args.has("midi-out")) { // the notes the arpeggiator, chord tools and clips made
        zed::Clip made;
        made.name = "Render";
        made.tempo = settings.bpm;
        const double beatsPerSample = settings.bpm / 60.0 / audio.sampleRate;
        std::map<int, std::pair<double, double>> open; // key -> start, velocity
        double last = 0.0;
        for (const zed::SeqEvent& e : audio.midiOut) {
            const double beat = static_cast<double>(e.sample) * beatsPerSample;
            last = std::max(last, beat);
            if (e.event.type == zed::MidiEvent::Type::NoteOn) {
                open[e.event.number] = {beat, e.event.value};
            } else if (e.event.type == zed::MidiEvent::Type::NoteOff) {
                const auto it = open.find(e.event.number);
                if (it != open.end()) {
                    made.notes.push_back({it->second.first, std::max(beat - it->second.first, 1.0 / 960.0), e.event.number, it->second.second});
                    open.erase(it);
                }
            }
        }
        made.bars = std::max(1.0, std::ceil(last / made.beatsPerBar));
        if (!zed::writeFile(args.get("midi-out"), zed::midi::clipToFile(made), error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kFailed;
        }
        std::printf("wrote %s (%zu notes the play features made)\n", args.get("midi-out").c_str(), made.notes.size());
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

// --- clip --------------------------------------------------------------------------------------

int cmdClip(const Args& args) {
    const std::string action = args.positional.empty() ? "" : args.positional[0];
    double slot = 1;
    if (!numberOption(args, "slot", 1, zed::kNumClips, slot))
        return kUsage;
    const auto index = static_cast<std::size_t>(slot) - 1;
    zed::Patch patch;
    if (!loadPatchArg(args.get("patch", "init"), patch))
        return kFailed;
    std::string error;
    if (action == "import" && args.positional.size() == 2 && args.has("out")) {
        std::vector<std::uint8_t> bytes;
        zed::Clip clip;
        if (!zed::readFile(args.positional[1], bytes, error) || !zed::midi::clipFromFile(bytes, clip, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kFailed;
        }
        patch.clips[index] = clip;
        if (!writeText(args.get("out"), zed::savePatch(patch)))
            return kFailed;
        std::printf("clip %zu: %zu notes, %g bars at %g bpm%s%s; wrote %s\n", index + 1, clip.notes.size(), clip.bars, clip.tempo,
                    clip.key.empty() ? "" : ", ", clip.key.c_str(), args.get("out").c_str());
        return kOk;
    }
    if (action == "export" && args.has("out")) {
        if (patch.clips[index].empty()) {
            std::fprintf(stderr, "error: clip slot %zu is empty\n", index + 1);
            return kFailed;
        }
        if (!zed::writeFile(args.get("out"), zed::midi::clipToFile(patch.clips[index]), error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kFailed;
        }
        std::printf("wrote %s (%zu notes)\n", args.get("out").c_str(), patch.clips[index].notes.size());
        return kOk;
    }
    std::fprintf(stderr, "error: use clip import FILE.mid --patch IN --slot N --out OUT, or clip export --patch IN --slot N --out FILE.mid\n");
    return kUsage;
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
            std::printf("%-18s %s (budget %.2f %%)\n", s.name.c_str(), s.description.c_str(), s.budgetPercent);
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
    std::printf("%-18s %12s %-2s %8s %10s  %s\n", "scenario", "result", "", "budget", "baseline", "verdict");
    for (const auto& s : scenarios) {
        if (wanted != "all" && wanted != s.name)
            continue;
        found = true;
        zed::BenchResult result;
        zed::runBench(s.name, seconds, static_cast<int>(runs), result, args.has("profile"));
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
            const double allowance = s.unit == "ms" ? 2.0 : kNoiseAllowance; // load times wander by a millisecond or two
            if (result.percent > stored * kRegression + allowance) {
                verdict = "FAIL: more than 10 % slower than the baseline";
                failed = true;
            }
        } else if (!baselinePath.empty() && !args.has("update-baseline")) {
            verdict += " (no baseline stored)";
        }
        if (s.maxRatio > 0.0 && result.ratio > s.maxRatio) {
            verdict = "FAIL: late in the tail it costs more than " + std::to_string(s.maxRatio) + " times as much as early";
            failed = true;
        }
        std::printf("%-18s %12.4f %-2s %8.2f %10s  %s\n", s.name.c_str(), result.percent, s.unit.c_str(), s.budgetPercent,
                    baselineText.c_str(), verdict.c_str());
        if (result.ratio > 0.0)
            std::printf("%-18s late tail / early tail cost: %.2f (limit %.2f)\n", "", result.ratio, s.maxRatio);
        auto sections = result.profile;
        std::sort(sections.begin(), sections.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [section, share] : sections)
            std::printf("%-18s   %8.3f %s  %s\n", "", share, s.unit == "ms" ? "  " : "%", section.c_str());
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
    // --report-only: the table is the result. The tests use it to check that the scenarios run;
    // a half-second run on a busy or unoptimised build says nothing about the budgets.
    return failed && !args.has("report-only") ? kFailed : kOk;
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
        if (command == "clip") return cmdClip(args);
        if (command == "soak") return cmdSoak(args);
        if (command == "brand") return cmdBrand(args);
        if (command == "ui-snapshot") return cmdUiSnapshot(args);
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
