// The patch: one document that describes a whole sound (plan 4.5). It is the preset file, the
// plug-in state and the agent's working material. Parameter values live in registry order;
// everything that is not a parameter (text, references, routes, rack layout) is held beside them.
#pragma once

#include "zederia/params/Registry.h"
#include "zederia/patch/Shape.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

struct PatchMeta {
    std::string name = "Init";
    std::string author, pack, type, notes, created, modified;
    std::vector<std::string> tags, genres;
    int rating = 0; // 0..5
    bool operator==(const PatchMeta&) const = default;
};

enum class Polarity : std::uint8_t { Bi, Uni };
enum class ModCurve : std::uint8_t { Linear, Exp, Log, S, Drawn };

// One modulation route. Its amount is the registry parameter mod.N.amount.
struct ModRoute {
    std::string source;      // a modulation source id, or "ccN"
    std::string via;         // optional second source that scales the route
    std::string dest;        // registry id of a modulatable parameter
    bool viaInvert = false;
    Polarity polarity = Polarity::Bi;
    ModCurve curve = ModCurve::Linear;
    Shape curveShape;        // the drawn curve (ModCurve::Drawn): y from 0 to 1 over x from 0 to 1
    bool enabled = true, mute = false, solo = false;
    bool operator==(const ModRoute&) const = default;
};

// A microtuning stored with the patch: the pitch of each of the 128 keys as an equal-tempered
// note number (see tuning/Tuning.h). No keys means equal temperament.
struct PatchTuning {
    std::string name;
    std::vector<double> keys;
    bool empty() const { return keys.empty(); }
    bool operator==(const PatchTuning&) const = default;
};

// Names of the five envelope times that can follow the tempo, in the order of Patch::envSync.
inline constexpr const char* kEnvSyncKeys[5] = {"delay", "attack", "hold", "decay", "release"};

class Patch {
public:
    Patch(); // the init patch: every parameter at its default

    double get(int index) const { return values_[static_cast<std::size_t>(index)]; }
    double get(std::string_view id) const; // 0 when the id is unknown
    // Stores the value clamped to the parameter's range and returns what was stored.
    double set(int index, double value);
    bool set(std::string_view id, double value); // false when the id is unknown
    std::span<const double> values() const { return values_; }

    // The definition that governs a value. For a rack slot's generic parameters this is the
    // loaded effect's parameter (its name, range and unit); otherwise the registry entry.
    const ParamDef& effectiveDef(int index) const;
    // False for a value that belongs to something the patch does not have: a parameter of an
    // empty rack slot, a generic slot parameter the loaded effect does not use, or the amount of
    // a route that does not exist. Such values stay at their defaults; set() leaves them alone.
    bool inUse(int index) const;

    // Effects rack. lane: 0 = A, 1 = B, 2 = Master. Slots are dense: no gaps.
    int fxSlotCount(int lane) const { return static_cast<int>(fx_[static_cast<std::size_t>(lane)].size()); }
    int fxSlotType(int lane, int slot) const; // effect index, -1 when the slot is empty
    // Appends a slot holding the effect at its defaults; returns its position or -1 when the lane is full.
    int addFxSlot(int lane, int effectIndex);
    void clearFx(int lane);
    // Removes a route and moves the amounts of the routes after it down by one.
    void removeRoute(int route);

    PatchMeta meta;
    std::array<std::string, kNumOscs> tables;       // wavetable reference per oscillator; empty = built-in
    std::string textureSample;                      // sample reference for the texture player
    std::array<std::string, kNumMacros> macroNames; // empty = "Macro N"
    std::vector<ModRoute> routes;                   // at most kNumModRoutes; route i uses mod.(i+1).amount

    // Structure that is not a parameter (milestone 3). An empty shape means the built-in one.
    std::array<Shape, kNumLfos> lfoShapes;                     // y from -1 to 1; empty = a sine
    std::array<EnvShape, kNumEnvs> envShapes;                  // multi-segment mode (envelopes 2 to 4)
    // Envelope times given as note lengths ("1/8", "3/16", "1/4T"): delay, attack, hold, decay,
    // release. A non-empty entry replaces the time in milliseconds and follows the tempo.
    std::array<std::array<std::string, 5>, kNumEnvs> envSync;
    std::array<std::array<Shape, 2>, kNumOscs> warpCurves;     // drawn phase curves of warp A and B; y from 0 to 1
    PatchTuning tuning;                                        // empty = equal temperament

    bool operator==(const Patch&) const = default;

private:
    std::vector<double> values_;
    std::array<std::vector<int>, kNumFxLanes> fx_;
};

} // namespace zed
