// MIDI learn (plan 3.9): controllers bound to parameters, saved and loaded as JSON. A map belongs
// to the instrument, not to a patch: it survives preset changes and lives in the host project.
#pragma once

#include "zederia/patch/PatchIO.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

inline constexpr std::string_view kCcMapFormat = "zederia.ccmap";

struct CcBinding {
    int controller = 1;  // 0..127
    int channel = -1;    // 0..15, or -1 for any channel
    std::string param;   // registry id of an automatable parameter
    double min = 0.0;    // the controller's range maps onto this part of the parameter (normalized)
    double max = 1.0;
    bool operator==(const CcBinding&) const = default;
};

struct CcMap {
    std::vector<CcBinding> bindings; // at most kMaxBindings; a later binding of the same controller wins
    static constexpr std::size_t kMaxBindings = 512;
    // Adds or replaces the binding of (controller, channel).
    void bind(const CcBinding& binding);
    void unbindParam(std::string_view param);
    bool operator==(const CcMap&) const = default;
};

std::string saveCcMap(const CcMap& map);
// Nothing changes when the document has errors; unknown parameters are reported and skipped.
bool loadCcMap(std::string_view jsonText, CcMap& out, std::vector<Issue>& issues);

// The map as the audio thread reads it: for each channel and controller, a parameter and its range.
struct CcTable {
    std::array<std::array<std::int16_t, 128>, 16> param; // registry index, -1 = none
    std::array<std::array<float, 128>, 16> min, max;
    static CcTable build(const CcMap& map);
};

} // namespace zed
