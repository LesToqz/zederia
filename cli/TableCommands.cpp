// zederia-cli table / texture / content: the wavetable toolkit from the command line (plan 3.10).
#include "CliUtil.h"
#include "zederia/content/Library.h"
#include "zederia/content/Recipe.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/table/Formula.h"
#include "zederia/table/Import.h"
#include "zederia/table/Resynth.h"
#include "zederia/table/TableData.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Math.h"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace cli {
namespace {

using zed::Wavetable;
using J = nlohmann::ordered_json;

bool fail(const std::string& message, int& code, int value = kFailed) {
    std::fprintf(stderr, "error: %s\n", message.c_str());
    code = value;
    return false;
}

bool loadTable(const std::string& path, Wavetable& table, int frameSize = 0, int channel = -1) {
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, bytes))
        return false;
    std::string error;
    zed::TableReadOptions options;
    options.frameSize = frameSize;
    options.channel = channel;
    if (!zed::decodeWavetable(bytes, table, error, options)) {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    if (table.name.empty())
        table.name = std::filesystem::path(path).stem().string();
    return true;
}

bool saveTable(const Args& args, const Wavetable& table) {
    const std::string out = args.get("out");
    if (out.empty()) {
        std::fprintf(stderr, "error: --out FILE.wav is needed\n");
        return false;
    }
    const std::string bits = args.get("bits", "32f");
    if (bits != "16" && bits != "24" && bits != "32f") {
        std::fprintf(stderr, "error: --bits must be 16, 24 or 32f\n");
        return false;
    }
    const auto format = bits == "16"   ? zed::TableSampleFormat::Pcm16
                        : bits == "24" ? zed::TableSampleFormat::Pcm24
                                       : zed::TableSampleFormat::Float32;
    if (!writeBytes(out, zed::encodeWavetable(table, format)))
        return false;
    std::printf("wrote %s: %d frame%s, morph %s%s\n", out.c_str(), table.size(), table.size() == 1 ? "" : "s",
                std::string(zed::morphModeName(table.morph)).c_str(), table.stepped ? ", stepped" : "");
    return true;
}

// "all", "3" or "2-7" (frame numbers start at 0).
bool scopeOf(const Args& args, zed::FrameSelection& selection) {
    const std::string text = args.get("scope", "all");
    if (text == "all") {
        selection = zed::allFrames();
        return true;
    }
    const std::size_t dash = text.find('-');
    double first = 0, last = 0;
    if (dash == std::string::npos ? toNumber(text, first) : (toNumber(text.substr(0, dash), first) && toNumber(text.substr(dash + 1), last))) {
        if (dash == std::string::npos)
            last = first;
        if (first >= 0 && last >= first && last < zed::kMaxFrames) {
            selection = {static_cast<int>(first), static_cast<int>(last)};
            return true;
        }
    }
    std::fprintf(stderr, "error: --scope must be all, a frame number, or a range such as 2-7\n");
    return false;
}

bool intOption(const Args& args, const char* key, int min, int max, int& value) {
    double v = value;
    if (!numberOption(args, key, min, max, v))
        return false;
    value = static_cast<int>(v);
    return true;
}

bool warpModeOf(const std::string& name, zed::osc::WarpMode& mode) {
    const auto& modes = zed::Registry::get().find("osc1.warp_a.mode")->choices;
    const auto it = std::find(modes.begin(), modes.end(), name);
    if (it == modes.end() || name == "off") {
        std::string list;
        for (std::size_t i = 1; i < modes.size(); ++i)
            list += (i > 1 ? ", " : "") + modes[i];
        std::fprintf(stderr, "error: --mode must be one of: %s\n", list.c_str());
        return false;
    }
    mode = static_cast<zed::osc::WarpMode>(it - modes.begin());
    return true;
}

bool loadWav(const std::string& path, zed::WavData& wav) {
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, bytes))
        return false;
    std::string error;
    if (!zed::decodeWav(bytes, wav, error)) {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    return true;
}

// --- info --------------------------------------------------------------------------------------

int tableInfo(const Args& args) {
    if (args.positional.size() != 2) {
        std::fprintf(stderr, "error: table info needs one file\n");
        return kUsage;
    }
    Wavetable table;
    if (!loadTable(args.positional[1], table))
        return kFailed;
    const auto data = zed::TableData::build(table);
    float peak = 0.0f;
    J frames = J::array();
    for (const auto& frame : table.frames) {
        peak = std::max(peak, zed::framePeak(frame));
        frames.push_back({{"peak", zed::framePeak(frame)},
                          {"brightness", zed::sortValue(frame, zed::SortKey::Brightness)},
                          {"highest_partial", zed::sortValue(frame, zed::SortKey::HighestPartial)}});
    }
    if (args.has("json")) {
        J out;
        out["name"] = table.name;
        out["frames"] = table.size();
        out["morph"] = zed::morphModeName(table.morph);
        out["stepped"] = table.stepped;
        out["positions_played"] = data->frames();
        out["peak"] = peak;
        out["memory_bytes"] = data->sizeInBytes();
        out["frame_details"] = frames;
        std::printf("%s\n", out.dump(2).c_str());
        return kOk;
    }
    std::printf("%s\n  name        %s\n  key frames  %d\n  morph       %s\n  stepped     %s\n  positions   %d played by the oscillator\n"
                "  peak        %.2f dBFS\n  in memory   %.1f MB when loaded\n",
                args.positional[1].c_str(), table.name.c_str(), table.size(), std::string(zed::morphModeName(table.morph)).c_str(),
                table.stepped ? "yes" : "no", data->frames(), zed::gainToDb(peak), static_cast<double>(data->sizeInBytes()) / 1048576.0);
    return kOk;
}

// --- editing -----------------------------------------------------------------------------------

int tableNew(const Args& args) {
    int frames = 1;
    if (!intOption(args, "frames", 1, zed::kMaxFrames, frames))
        return kUsage;
    Wavetable table = Wavetable::silent(frames);
    table.name = args.get("name");
    return saveTable(args, table) ? kOk : kFailed;
}

int tableFormula(const Args& args) {
    Wavetable table;
    int frames = 1;
    if (args.has("in")) {
        if (!loadTable(args.get("in"), table))
            return kFailed;
    } else {
        if (!intOption(args, "frames", 1, zed::kMaxFrames, frames))
            return kUsage;
        table = Wavetable::silent(frames);
        table.name = args.get("name");
    }
    zed::FormulaRequest request;
    const std::string domain = args.get("domain", "wave");
    if (domain != "wave" && domain != "harmonics") {
        std::fprintf(stderr, "error: --domain must be wave or harmonics\n");
        return kUsage;
    }
    request.domain = domain == "wave" ? zed::FormulaDomain::Wave : zed::FormulaDomain::Harmonics;
    request.expression = args.get("expr");
    request.phaseExpression = args.get("phase");
    request.normalize = args.has("normalize");
    double seed = 1;
    int selected = 0;
    if (!scopeOf(args, request.scope) || !numberOption(args, "seed", 0, 4294967295.0, seed) ||
        !intOption(args, "selected", 0, zed::kMaxFrames - 1, selected))
        return kUsage;
    request.seed = static_cast<std::uint64_t>(seed);
    request.selectedFrame = selected;
    if (request.expression.empty()) {
        std::fprintf(stderr, "error: --expr \"...\" is needed, for example --expr \"sin(tau*t) * (1 - y) + saw(t) * y\"\n");
        return kUsage;
    }
    std::string error;
    if (!zed::applyFormula(table, request, error)) {
        std::fprintf(stderr, "error: formula, %s\n  %s\n  %*s^\n", error.c_str(), request.expression.c_str(),
                     error.rfind("column ", 0) == 0 ? std::atoi(error.c_str() + 7) - 1 : 0, "");
        return kFailed;
    }
    if (args.has("morph") && !zed::morphModeFromName(args.get("morph"), table.morph)) {
        std::fprintf(stderr, "error: --morph must be none, crossfade, spectral, spectral_aligned or spectral_zero\n");
        return kUsage;
    }
    return saveTable(args, table) ? kOk : kFailed;
}

int tableProcess(const Args& args) {
    Wavetable table;
    zed::ProcessOp op = zed::ProcessOp::Clear;
    zed::FrameSelection scope;
    int n = 0;
    if (!args.has("in") || !zed::processOpFromName(args.get("op"), op)) {
        std::fprintf(stderr, "error: table process needs --in FILE and --op: normalize_each, normalize_shared, remove_dc, "
                             "flip_polarity, mirror_time, shift_to_zero_crossing, clear, fade_edges, crossfade_edges, low_pass, "
                             "remove_fundamental, sample_rate_reduce or align_fundamental\n");
        return kUsage;
    }
    if (!scopeOf(args, scope) || !intOption(args, "n", 0, zed::kFrameSize, n))
        return kUsage;
    if (!loadTable(args.get("in"), table))
        return kFailed;
    zed::processFrames(table, scope, op, n);
    return saveTable(args, table) ? kOk : kFailed;
}

int tableHarmonic(const Args& args) {
    Wavetable table;
    zed::HarmonicOp op = zed::HarmonicOp::ClearAll;
    zed::FrameSelection scope;
    int n = 0;
    double amount = 1.0, seed = 1;
    if (!args.has("in") || !zed::harmonicOpFromName(args.get("op"), op)) {
        std::fprintf(stderr, "error: table harmonic needs --in FILE and --op: clear_all, clear_above, clear_below, fill_saw, "
                             "randomize_lowest, randomize_all, random_gaps, fade_highs, octave_up, octave_down, odd_only, "
                             "even_only, snap_quarters or scale_by_harmonic\n");
        return kUsage;
    }
    if (!scopeOf(args, scope) || !intOption(args, "n", 0, zed::kMaxHarmonics, n) || !numberOption(args, "amount", -8, 8, amount) ||
        !numberOption(args, "seed", 0, 4294967295.0, seed))
        return kUsage;
    if (!loadTable(args.get("in"), table))
        return kFailed;
    const int last = scope.last < 0 ? table.size() - 1 : std::min(scope.last, table.size() - 1);
    for (int f = std::max(scope.first, 0); f <= last; ++f)
        zed::applyHarmonicOp(table.frames[static_cast<std::size_t>(f)], op, n, amount, static_cast<std::uint64_t>(seed) + static_cast<std::uint64_t>(f));
    return saveTable(args, table) ? kOk : kFailed;
}

int tableFrames(const Args& args) {
    Wavetable table;
    zed::FrameSelection scope;
    if (!args.has("in") || !args.has("op")) {
        std::fprintf(stderr, "error: table frames needs --in FILE and --op: insert, duplicate, delete, move, thin, crop, reverse, "
                             "split, join, copy_spectrum, copy_phases or trim_silent\n");
        return kUsage;
    }
    if (!scopeOf(args, scope) || !loadTable(args.get("in"), table))
        return kFailed;
    const std::string op = args.get("op");
    int at = table.size(), from = 0, to = 0, every = 2, source = 0;
    if (!intOption(args, "at", 0, zed::kMaxFrames, at) || !intOption(args, "from", 0, zed::kMaxFrames, from) ||
        !intOption(args, "to", 0, zed::kMaxFrames, to) || !intOption(args, "every", 1, zed::kMaxFrames, every) ||
        !intOption(args, "source", 0, zed::kMaxFrames, source))
        return kUsage;
    bool ok = true;
    if (op == "insert")
        ok = zed::insertFrame(table, at, zed::Frame{});
    else if (op == "duplicate")
        ok = zed::duplicateFrames(table, scope);
    else if (op == "delete")
        ok = zed::deleteFrames(table, scope);
    else if (op == "move")
        ok = zed::moveFrame(table, from, to);
    else if (op == "thin")
        ok = zed::thinFrames(table, every);
    else if (op == "crop")
        ok = zed::cropFrames(table, scope);
    else if (op == "reverse")
        zed::reverseFrames(table);
    else if (op == "split")
        ok = zed::splitFrames(table);
    else if (op == "join")
        zed::joinFramePairs(table);
    else if (op == "copy_spectrum")
        zed::copySpectrumToAll(table, source);
    else if (op == "copy_phases")
        zed::copyPhasesToAll(table, source);
    else if (op == "trim_silent")
        std::printf("removed %d silent frame(s)\n", zed::trimSilentFrames(table));
    else {
        std::fprintf(stderr, "error: unknown frame operation \"%s\"\n", op.c_str());
        return kUsage;
    }
    if (!ok) {
        std::fprintf(stderr, "error: \"%s\" does not fit this table (%d frames; a table holds 1 to %d)\n", op.c_str(), table.size(),
                     zed::kMaxFrames);
        return kFailed;
    }
    return saveTable(args, table) ? kOk : kFailed;
}

int tableMorph(const Args& args) {
    Wavetable table;
    if (!args.has("in") || !loadTable(args.get("in"), table))
        return args.has("in") ? kFailed : (std::fprintf(stderr, "error: table morph needs --in FILE\n"), kUsage);
    if (args.has("mode") && !zed::morphModeFromName(args.get("mode"), table.morph)) {
        std::fprintf(stderr, "error: --mode must be none, crossfade, spectral, spectral_aligned or spectral_zero\n");
        return kUsage;
    }
    if (args.has("stepped")) {
        const std::string value = args.get("stepped");
        if (value != "on" && value != "off") {
            std::fprintf(stderr, "error: --stepped must be on or off\n");
            return kUsage;
        }
        table.stepped = value == "on";
    }
    int bake = 0;
    if (!intOption(args, "bake", 1, zed::kMaxFrames, bake))
        return kUsage;
    if (bake > 0)
        zed::bakeMorph(table, bake);
    return saveTable(args, table) ? kOk : kFailed;
}

int tableSort(const Args& args) {
    Wavetable table;
    zed::SortKey key = zed::SortKey::Brightness;
    if (!args.has("in") || !zed::sortKeyFromName(args.get("by"), key)) {
        std::fprintf(stderr, "error: table sort needs --in FILE and --by: brightest_peak, brightness, strongest_partial, "
                             "active_partials, highest_partial or fundamental\n");
        return kUsage;
    }
    if (!loadTable(args.get("in"), table))
        return kFailed;
    zed::sortFrames(table, key, args.has("descending"));
    return saveTable(args, table) ? kOk : kFailed;
}

// --- import ------------------------------------------------------------------------------------

int tableImport(const Args& args) {
    const std::vector<std::string> files(args.positional.begin() + 1, args.positional.end());
    if (files.empty()) {
        std::fprintf(stderr, "error: table import needs one or more files\n");
        return kUsage;
    }
    std::string method = args.get("method", "auto");
    const std::string channelName = args.get("channel", "mid");
    if (channelName != "left" && channelName != "right" && channelName != "mid" && channelName != "split") {
        std::fprintf(stderr, "error: --channel must be left, right, mid or split\n");
        return kUsage;
    }
    // "split" sends the left channel to one table and the right to another.
    const bool split = channelName == "split";
    std::vector<zed::StereoChoice> choices;
    if (split)
        choices = {zed::StereoChoice::Left, zed::StereoChoice::Right};
    else
        choices = {channelName == "left" ? zed::StereoChoice::Left : channelName == "right" ? zed::StereoChoice::Right : zed::StereoChoice::Mid};

    zed::SliceOptions slice;
    zed::SnapshotOptions snapshots;
    int frameSize = 0, maxFrames = zed::kMaxFrames, note = -1;
    double seed = 1;
    if (!intOption(args, "frame-size", 2, 1 << 20, frameSize) || !intOption(args, "max-frames", 1, zed::kMaxFrames, maxFrames) ||
        !numberOption(args, "cycle-samples", 2, 1.0e6, slice.cycleSamples) || !intOption(args, "fft", 256, 2048, snapshots.fftSize) ||
        !intOption(args, "frames", 1, zed::kMaxFrames, snapshots.frames) || !numberOption(args, "seed", 0, 4294967295.0, seed))
        return kUsage;
    slice.maxFrames = maxFrames;
    slice.snapToZeroCrossings = args.has("snap");
    slice.reduction = args.has("average") ? zed::CycleReduction::Average : zed::CycleReduction::Pick;
    snapshots.seed = static_cast<std::uint64_t>(seed);
    const std::string phase = args.get("phase", "zero");
    if (phase != "original" && phase != "zero" && phase != "random") {
        std::fprintf(stderr, "error: --phase must be original, zero or random\n");
        return kUsage;
    }
    snapshots.phase = phase == "original" ? zed::SnapshotPhase::Original : phase == "zero" ? zed::SnapshotPhase::Zero : zed::SnapshotPhase::Random;
    if (args.has("note")) {
        note = zed::parseNoteName(args.get("note"));
        if (note < 0) {
            std::fprintf(stderr, "error: --note must be a note name such as A3\n");
            return kUsage;
        }
    }

    for (std::size_t c = 0; c < choices.size(); ++c) {
        Wavetable table;
        std::string error;
        zed::ImportReport report;
        bool ok = false;
        if (method == "image") {
            std::vector<std::uint8_t> bytes;
            zed::ImageData image;
            if (!readBytes(files[0], bytes))
                return kFailed;
            const std::string mode = args.get("image-mode", "amplitude");
            if (mode != "amplitude" && mode != "harmonics") {
                std::fprintf(stderr, "error: --image-mode must be amplitude or harmonics\n");
                return kUsage;
            }
            ok = zed::decodeImage(bytes, image, error) &&
                 zed::importImage(image, mode == "amplitude" ? zed::ImageMode::Amplitude : zed::ImageMode::Harmonics, maxFrames, table, error);
        } else if (method == "cycles") {
            std::vector<zed::AudioClip> cycles;
            for (const auto& file : files) {
                zed::WavData wav;
                if (!loadWav(file, wav))
                    return kFailed;
                cycles.push_back(zed::clipFromWav(wav, choices[c]));
            }
            ok = zed::importSingleCycles(cycles, table, error);
        } else {
            zed::WavData wav;
            if (!loadWav(files[0], wav))
                return kFailed;
            if (method == "auto") {
                const zed::ImportSuggestion suggestion = zed::suggestImport(wav);
                method = suggestion.method == "single_cycle" ? "cycles" : suggestion.method == "pitch_tracked" ? "pitch"
                         : suggestion.method == "fixed_cycles" ? "fixed" : suggestion.method;
                std::printf("method: %s. %s\n", method.c_str(), suggestion.reason.c_str());
            }
            const zed::AudioClip clip = zed::clipFromWav(wav, choices[c]);
            if (method == "table") {
                const int channel = choices[c] == zed::StereoChoice::Mid ? -1 : choices[c] == zed::StereoChoice::Left ? 0 : 1;
                ok = loadTable(files[0], table, frameSize, wav.channelCount() > 1 ? channel : -1);
                if (!ok)
                    return kFailed;
            } else if (method == "cycles") {
                const zed::AudioClip one[] = {clip};
                ok = zed::importSingleCycles(one, table, error);
            } else if (method == "pitch") {
                ok = zed::importPitchTracked(clip, slice, table, error, &report);
            } else if (method == "fixed") {
                if (note >= 0)
                    slice.cycleSamples = zed::cycleSamplesForNote(note, clip.sampleRate);
                ok = zed::importFixedCycles(clip, slice, table, error, &report);
            } else if (method == "spectral") {
                ok = zed::importSpectralSnapshots(clip, snapshots, table, error);
            } else {
                std::fprintf(stderr, "error: --method must be auto, table, cycles, pitch, fixed, spectral or image\n");
                return kUsage;
            }
        }
        if (!ok) {
            std::fprintf(stderr, "error: import: %s\n", error.c_str());
            return kFailed;
        }
        if (report.cyclesFound > 0)
            std::printf("found %d cycles of about %.2f samples (%.2f Hz); made %d frames\n", report.cyclesFound, report.cycleSamples,
                        report.detectedHz, report.framesMade);
        if (table.name.empty())
            table.name = std::filesystem::path(files[0]).stem().string();
        // Follow-ups the wizard offers.
        if (args.has("fade-edges")) {
            int n = 32;
            if (!intOption(args, "fade-edges", 1, zed::kFrameSize / 2, n))
                return kUsage;
            zed::processFrames(table, zed::allFrames(), zed::ProcessOp::FadeEdges, n);
        }
        if (args.has("normalize"))
            zed::processFrames(table, zed::allFrames(), zed::ProcessOp::NormalizeShared);
        if (args.has("trim-silent")) {
            double threshold = -60.0;
            if (!numberOption(args, "trim-silent", -200, 0, threshold))
                return kUsage;
            zed::trimSilentFrames(table, threshold);
        }
        Args target = args;
        if (split) {
            const std::filesystem::path out(args.get("out"));
            target.options["out"] = (out.parent_path() / (out.stem().string() + (c == 0 ? "_left" : "_right") + out.extension().string())).string();
        }
        if (!saveTable(target, table))
            return kFailed;
    }
    return kOk;
}

// --- resynthesis and export --------------------------------------------------------------------

int tableResynth(const Args& args) {
    const std::string op = args.get("op");
    Wavetable out;
    if (op == "bake-warp") {
        Wavetable table;
        zed::osc::WarpMode mode = zed::osc::WarpMode::Off;
        double position = 0.0;
        int frames = zed::kMaxFrames;
        if (!args.has("in") || !warpModeOf(args.get("mode"), mode) || !numberOption(args, "position", 0, 1, position) ||
            !intOption(args, "frames", 1, zed::kMaxFrames, frames))
            return kUsage;
        if (!loadTable(args.get("in"), table))
            return kFailed;
        out = zed::bakeWarp(table, position, mode, frames);
    } else if (op == "bake-patch") {
        zed::Patch patch;
        zed::BakePatchOptions options;
        double seed = 1;
        if (!numberOption(args, "bars", 0.01, 64, options.bars) || !numberOption(args, "tempo", 20, 999, options.bpm) ||
            !intOption(args, "frames", 1, zed::kMaxFrames, options.frames) || !numberOption(args, "seed", 0, 4294967295.0, seed))
            return kUsage;
        options.seed = static_cast<std::uint64_t>(seed);
        if (args.has("note")) {
            double number = 0;
            options.note = toNumber(args.get("note"), number) ? static_cast<int>(number) : zed::parseNoteName(args.get("note"));
            if (options.note < 0 || options.note > 127) {
                std::fprintf(stderr, "error: --note must be a MIDI number or a note name such as C3\n");
                return kUsage;
            }
        }
        const std::string patchFile = args.get("patch", "init");
        std::string presetDir;
        if (patchFile != "init") {
            std::vector<std::uint8_t> bytes;
            if (!readBytes(patchFile, bytes))
                return kFailed;
            auto loaded = zed::loadPatch({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            if (!loaded.ok()) {
                std::fprintf(stderr, "error: %s is not a usable patch (%d error(s)); check it with validate-patch\n", patchFile.c_str(),
                             loaded.errors());
                return kFailed;
            }
            patch = std::move(loaded.patch);
            presetDir = std::filesystem::path(patchFile).parent_path().string();
        }
        zed::ContentLibrary library = zed::ContentLibrary::standard();
        if (args.has("content"))
            library.setFactoryDir(args.get("content"));
        for (int i = 0; i < zed::kNumOscs; ++i) {
            std::string error;
            options.tables[static_cast<std::size_t>(i)] = library.loadTable(patch.tables[static_cast<std::size_t>(i)], error, presetDir);
            if (options.tables[static_cast<std::size_t>(i)] == nullptr)
                std::fprintf(stderr, "warning: osc%d: %s; the built-in table is used instead\n", i + 1, error.c_str());
        }
        std::string error;
        options.texture = library.loadTexture(patch.textureSample, error, presetDir);
        if (options.texture == nullptr)
            std::fprintf(stderr, "warning: texture: %s; the built-in noise is used instead\n", error.c_str());
        if (!zed::bakePatch(patch, options, out, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return kFailed;
        }
    } else if (op == "shape") {
        std::vector<float> values;
        const std::string text = args.get("values");
        std::size_t pos = 0;
        while (pos <= text.size()) {
            const std::size_t comma = std::min(text.find(',', pos), text.size());
            std::string item = text.substr(pos, comma - pos);
            item.erase(std::remove(item.begin(), item.end(), ' '), item.end());
            double value = 0;
            if (!item.empty()) {
                if (!toNumber(item, value)) {
                    std::fprintf(stderr, "error: --values must be numbers separated by commas\n");
                    return kUsage;
                }
                values.push_back(static_cast<float>(value));
            }
            pos = comma + 1;
        }
        if (values.size() < 2) {
            std::fprintf(stderr, "error: --values needs two or more numbers, for example --values \"0,1,0.5,0\"\n");
            return kUsage;
        }
        out.frames.push_back(zed::shapeToFrame(values, args.has("unipolar")));
    } else {
        std::fprintf(stderr, "error: table resynth needs --op bake-warp, bake-patch or shape\n");
        return kUsage;
    }
    if (out.name.empty())
        out.name = args.get("name");
    return saveTable(args, out) ? kOk : kFailed;
}

int tableExport(const Args& args) {
    Wavetable table;
    zed::FrameSelection scope;
    if (!args.has("in") || !scopeOf(args, scope))
        return kUsage;
    if (!loadTable(args.get("in"), table))
        return kFailed;
    if (args.has("out-dir")) { // a folder of single-cycle files
        const std::filesystem::path dir(args.get("out-dir"));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto files = zed::exportSingleCycles(table, scope);
        const int first = std::max(scope.first, 0);
        for (std::size_t i = 0; i < files.size(); ++i) {
            char name[64];
            std::snprintf(name, sizeof name, "%s_%03d.wav", table.name.empty() ? "frame" : "cycle", first + static_cast<int>(i));
            if (!writeBytes((dir / name).string(), files[i]))
                return kFailed;
        }
        std::printf("wrote %zu single-cycle file(s) to %s\n", files.size(), dir.string().c_str());
        return kOk;
    }
    int generated = 0;
    if (!intOption(args, "generated", 1, zed::kMaxFrames, generated))
        return kUsage;
    if (generated > 0) // the full set of positions instead of the key frames
        zed::bakeMorph(table, generated);
    else if (!zed::cropFrames(table, scope)) {
        std::fprintf(stderr, "error: --scope selects no frames of this table\n");
        return kFailed;
    }
    return saveTable(args, table) ? kOk : kFailed;
}

bool loadRecipe(const std::string& path, nlohmann::json& recipe) {
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, bytes))
        return false;
    recipe = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (recipe.is_discarded()) {
        std::fprintf(stderr, "error: %s is not valid JSON\n", path.c_str());
        return false;
    }
    return true;
}

int tableBuild(const Args& args) {
    if (args.positional.size() != 2) {
        std::fprintf(stderr, "error: table build needs one recipe file\n");
        return kUsage;
    }
    nlohmann::json recipe;
    Wavetable table;
    std::string error;
    if (!loadRecipe(args.positional[1], recipe))
        return kFailed;
    if (!zed::buildTableRecipe(recipe, table, error)) {
        std::fprintf(stderr, "error: %s: %s\n", args.positional[1].c_str(), error.c_str());
        return kFailed;
    }
    return saveTable(args, table) ? kOk : kFailed;
}

} // namespace

int cmdTable(const Args& args) {
    const std::string sub = args.positional.empty() ? "" : args.positional[0];
    if (sub == "info") return tableInfo(args);
    if (sub == "new") return tableNew(args);
    if (sub == "formula") return tableFormula(args);
    if (sub == "process") return tableProcess(args);
    if (sub == "harmonic") return tableHarmonic(args);
    if (sub == "frames") return tableFrames(args);
    if (sub == "morph") return tableMorph(args);
    if (sub == "sort") return tableSort(args);
    if (sub == "import") return tableImport(args);
    if (sub == "resynth") return tableResynth(args);
    if (sub == "export") return tableExport(args);
    if (sub == "build") return tableBuild(args);
    std::fprintf(stderr, "error: table needs a subcommand: info, new, formula, process, harmonic, frames, morph, sort, import, "
                         "resynth, export or build (see zederia-cli help)\n");
    return kUsage;
}

int cmdTexture(const Args& args) {
    if (args.positional.size() != 2 || args.positional[0] != "build" || !args.has("out")) {
        std::fprintf(stderr, "error: usage: zederia-cli texture build RECIPE.json --out FILE.wav\n");
        return kUsage;
    }
    nlohmann::json recipe;
    zed::WavData wav;
    std::string error;
    if (!loadRecipe(args.positional[1], recipe))
        return kFailed;
    if (!zed::buildTextureRecipe(recipe, wav, error)) {
        std::fprintf(stderr, "error: %s: %s\n", args.positional[1].c_str(), error.c_str());
        return kFailed;
    }
    if (!writeBytes(args.get("out"), zed::encodeWavData(wav, zed::WavSampleFormat::Pcm24)))
        return kFailed;
    std::printf("wrote %s: %.2f s, %g Hz, %s\n", args.get("out").c_str(), static_cast<double>(wav.frames()) / wav.sampleRate,
                wav.sampleRate, wav.hasLoop ? "loops" : "one-shot");
    return kOk;
}

int cmdContent(const Args& args) {
    if (args.positional.size() != 1 || args.positional[0] != "build" || !args.has("in") || !args.has("out")) {
        std::fprintf(stderr, "error: usage: zederia-cli content build --in RECIPES_DIR --out FACTORY_DIR\n");
        return kUsage;
    }
    const zed::ContentBuild result = zed::buildContentFolder(args.get("in"), args.get("out"));
    for (const auto& file : result.written)
        std::printf("  %s\n", file.c_str());
    for (const auto& error : result.errors)
        std::fprintf(stderr, "error: %s\n", error.c_str());
    std::printf("built %d wavetable(s) and %d texture(s) into %s\n", result.tables, result.textures, args.get("out").c_str());
    return result.errors.empty() ? kOk : kFailed;
}

} // namespace cli
