// The content check (plan 7, milestone 7): every preset of a folder loads, plays a phrase suited
// to its type, and stays inside the limits a factory preset must keep. A preset fails on a load
// error, a table or sample it names that cannot be found, clipping, silence, voices that never end,
// or a realtime cost above the plan's budget (5.8). Loudness and peak margins are reported, and
// warned about when they stray from the agent guide's targets, but do not fail.
// Rendering reads files and builds tables, so nothing here is for the audio thread.
#pragma once

#include "zederia/content/Library.h"
#include "zederia/render/OfflineRenderer.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

struct PresetCheck;

struct PresetCheckOptions {
    double sampleRate = 48000.0;
    int blockSize = 512;
    // Plan 5.8: the heaviest scenario (8 voices x 3 oscillators x unison 16, two filters, six
    // effects) may cost 35 % of one core at Standard quality; no preset may cost more.
    double cpuBudgetPercent = 35.0;
    bool measureCpu = true;
    int cpuAttempts = 3;          // a reading over the budget is measured again; the lowest counts
    double silenceDbfs = -60.0;   // a phrase whose peak stays below this is silent
    double peakMarginDbfs = -1.0; // the agent guide's peak margin (a warning above it)
    double loadBudgetMs = 100.0;  // plan 5.8, preset load (a warning above it)
    double maxReleaseSeconds = 30.0; // voices still sounding this long after the last note-off are stuck
    int threads = 0;              // renders in parallel (0: what the machine has, at most 4); CPU is measured alone
    // Called after each preset with its result; done counts from 1.
    std::function<void(int done, int total, const PresetCheck& result)> progress;
};

struct PresetCheck {
    std::string path;     // the file
    std::string relative; // inside the folder that was checked, "Bass/Anchor Point.zpreset"
    std::string name, type;
    std::string phrase;   // the audition phrase that was played
    double loadMs = 0.0;  // reading, checking and applying the patch, its tables built
    double peakDbfs = -200.0;     // the phrase at velocity 100 (as the agent guide measures)
    double loudnessLufs = -200.0;
    double peakFullVelocityDbfs = -200.0; // the same phrase at velocity 127
    std::int64_t clippedSamples = 0;      // samples at full scale in either render
    double releaseSeconds = 0.0; // from the last note-off until the last voice ended
    double cpuPercent = 0.0;     // an 8-note chord at the realtime quality, one core
    std::vector<std::string> errors;   // the preset fails
    std::vector<std::string> warnings; // reported only
    bool ok() const { return errors.empty(); }
};

struct ContentCheckReport {
    std::vector<PresetCheck> presets;
    std::vector<std::string> problems; // a folder that cannot be read, no presets found
    int failures() const;
    bool ok() const { return problems.empty() && failures() == 0; }
};

// Every .zpreset under `presetsDir` (subfolders included), in name order.
ContentCheckReport checkPresets(const std::string& presetsDir, const ContentLibrary& library,
                                const PresetCheckOptions& options = {});
// One preset file.
PresetCheck checkPreset(const std::string& file, const ContentLibrary& library, const PresetCheckOptions& options = {});

// The phrase the check plays for a preset type ("bass", "808", "lead", "pluck", "keys", "pad",
// "strings", "brass", "synth", "sequence", "fx", "percussion" ...): notes in seconds at 120 bpm,
// velocity 100. `name` receives a short description.
std::vector<NoteSpec> auditionPhrase(std::string_view type, std::string& name);

// The agent guide's loudness target for a type (integrated LUFS on its phrase), and whether the
// peak decides instead (short, percussive types).
struct LoudnessTarget {
    double lufs = -14.0;
    double tolerance = 4.0;
    bool peakDecides = false;
};
LoudnessTarget loudnessTarget(std::string_view type);

nlohmann::json toJson(const PresetCheck& check);
nlohmann::json toJson(const ContentCheckReport& report);

} // namespace zed
