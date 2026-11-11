// The parameter registry: the single source of truth for every parameter (plan 5.5, Appendix A).
// Host parameters, the patch schema, interface bindings, the agent schema and docs/PARAMETERS.md
// are generated from it. It is append-only: ids and their order never change.
#pragma once

#include "zederia/params/ParamDef.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zed {

inline constexpr int kNumOscs = 3;
inline constexpr int kNumFilters = 2;
inline constexpr int kNumEnvs = 4;
inline constexpr int kNumLfos = 8;
inline constexpr int kNumFlux = 2;
inline constexpr int kNumMacros = 8;
inline constexpr int kNumModRoutes = 64;
inline constexpr int kNumFxLanes = 3;         // A, B, Master
inline constexpr int kNumFxSlots = 8;         // per lane
inline constexpr int kNumFxSlotParams = 16;   // generic parameters p1..p16 per slot
inline constexpr int kMaxPolyphony = 64;

// How a module's parameters are laid out in the patch document.
enum class ModuleKind : std::uint8_t { Plain, Macro, ModRoute, FxRack, FxSlot };

struct ModuleInfo {
    std::string id;          // "osc1", "mod.12", "fx.a3"
    std::string name;        // "Osc 1"
    std::string description;
    ModuleKind kind = ModuleKind::Plain;
    int firstParam = 0, numParams = 0;
};

// An effect that can be loaded into a rack slot. Its parameters map, in order, onto the slot's
// generic parameters p1..p16; in the patch document they appear under their own names.
struct EffectType {
    std::string id;   // "chorus"
    std::string name; // "Chorus"
    std::string description;
    std::vector<ParamDef> params; // at most kNumFxSlotParams; append-only per type
};

struct ModSourceDef {
    std::string id; // "lfo1", "modwheel"
    std::string name;
    Scope scope = Scope::Global;
    std::string description;
};

class Registry {
public:
    static const Registry& get();

    std::span<const ParamDef> params() const { return params_; }
    int size() const { return static_cast<int>(params_.size()); }
    const ParamDef& at(int index) const { return params_[static_cast<std::size_t>(index)]; }
    int indexOf(std::string_view id) const; // -1 when unknown
    const ParamDef* find(std::string_view id) const;
    const ParamDef* findByHostId(std::uint32_t hostId) const;

    std::span<const ModuleInfo> modules() const { return modules_; }

    std::span<const EffectType> effectTypes() const { return effects_; }
    int effectIndex(std::string_view id) const; // -1 when unknown

    std::span<const ModSourceDef> modSources() const { return sources_; }
    // True for a listed source or a MIDI controller written as "cc0" .. "cc127".
    bool isModSource(std::string_view id) const;

    // Index of a rack slot's generic parameters. lane: 0 = A, 1 = B, 2 = Master; slot: 0..7.
    int fxSlotOn(int lane, int slot) const { return fxFirst_ + (lane * kNumFxSlots + slot) * fxStride_; }
    int fxSlotMix(int lane, int slot) const { return fxSlotOn(lane, slot) + 1; }
    int fxSlotParam(int lane, int slot, int k) const { return fxSlotOn(lane, slot) + 2 + k; }
    // Reverse lookups. fxSlotOf: field 0 = on, 1 = mix, 2 + k = generic parameter k; false when the
    // index is not a slot parameter. fxSlotParamAt: true only for the generic parameters p1..p16.
    bool fxSlotOf(int index, int& lane, int& slot, int& field) const;
    bool fxSlotParamAt(int index, int& lane, int& slot, int& k) const;
    int modRouteOf(int index) const { // route number, or -1 when index is not a route amount
        return index >= modFirst_ && index < modFirst_ + kNumModRoutes ? index - modFirst_ : -1;
    }
    int modAmount(int route) const { return modFirst_ + route; }
    int macroValue(int macro) const { return macroFirst_ + macro * 2; }
    int macroBipolar(int macro) const { return macroFirst_ + macro * 2 + 1; }

private:
    Registry();
    std::vector<ParamDef> params_;
    std::vector<ModuleInfo> modules_;
    std::vector<EffectType> effects_;
    std::vector<ModSourceDef> sources_;
    std::unordered_map<std::string_view, int> byId_;
    std::unordered_map<std::uint32_t, int> byHostId_;
    int fxFirst_ = 0, fxStride_ = 0, modFirst_ = 0, macroFirst_ = 0;
};

// The names of the three rack lanes: ids use "a", "b", "m"; the patch document uses "a", "b", "master".
std::string_view fxLaneIdPrefix(int lane);
std::string_view fxLaneKey(int lane);

// Tempo divisions shared by every synced rate ("8/1" .. "1/128", with D for dotted and T for triplet).
const std::vector<std::string>& tempoDivisions();
// Length of a division in quarter notes; 0 when the token is unknown.
double divisionInBeats(std::string_view token);

} // namespace zed
