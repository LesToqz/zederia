// Reading and writing patches as JSON (plan 4.5, 5.6).
//
// Rules: omitted fields take defaults; unknown fields are reported, not silently dropped; values
// are clamped and the clamp is reported; every issue carries a JSON-pointer path and a
// plain-language fix; save -> load -> save is byte-identical; no input can crash a load.
#pragma once

#include "zederia/patch/Patch.h"

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

inline constexpr std::string_view kPatchFormat = "zederia.patch";
inline constexpr int kPatchVersion = 1;
inline constexpr std::size_t kMaxPatchBytes = 64u * 1024u * 1024u;

enum class Severity : std::uint8_t { Warning, Error };

struct Issue {
    Severity severity = Severity::Error;
    std::string code;    // stable machine-readable kind: "unknown_field", "out_of_range", ...
    std::string path;    // JSON pointer into the document; "" is the document itself
    std::string message; // what is wrong
    std::string fix;     // what to do about it
};

struct LoadResult {
    Patch patch; // meaningful only when ok()
    std::vector<Issue> issues;
    bool ok() const;
    int errors() const;
};

LoadResult loadPatch(std::string_view jsonText);

struct SaveOptions {
    bool compact = false; // leave out values that equal their defaults
};
std::string savePatch(const Patch& patch, const SaveOptions& options = {});

// Rack presets (plan 3.8, 5.6): the whole rack, or one effect slot, as JSON.
inline constexpr std::string_view kRackFormat = "zederia.rack";
inline constexpr std::string_view kSlotFormat = "zederia.fx";
std::string saveRackPreset(const Patch& patch);
// Replaces the patch's rack with the preset's. Nothing changes when the preset has errors.
bool loadRackPreset(std::string_view jsonText, Patch& into, std::vector<Issue>& issues);
std::string saveSlotPreset(const Patch& patch, int lane, int slot);
// Loads one effect into slot `position` of `lane` (replacing what is there, or appending when
// `position` is the lane's slot count) and fills `moves` for Engine::setRack.
bool loadSlotPreset(std::string_view jsonText, Patch& into, int lane, int position, FxMoves* moves,
                    std::vector<Issue>& issues);

// One step of forward migration: rewrites a document of version `from` into version `from + 1`.
struct MigrationStep {
    int from = 0;
    std::function<void(nlohmann::ordered_json&)> apply;
};
// Brings `document` to `targetVersion` one step at a time and updates its "version" field.
// Returns false and adds an issue when the version is missing, invalid, newer than the target,
// or a step is missing.
bool migratePatch(nlohmann::ordered_json& document, std::span<const MigrationStep> steps, int targetVersion,
                  std::vector<Issue>& issues);

} // namespace zed
