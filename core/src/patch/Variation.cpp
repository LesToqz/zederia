#include "zederia/patch/Variation.h"

#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace zed {
namespace {

bool endsWith(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}
bool startsWith(std::string_view s, std::string_view head) { return s.substr(0, head.size()) == head; }

bool isLocked(std::span<const bool> locked, int index) {
    return index >= 0 && static_cast<std::size_t>(index) < locked.size() && locked[static_cast<std::size_t>(index)];
}

// The part of the normalized range a random value may land in, so a random sound stays playable.
void playableRange(const ParamDef& def, double& low, double& high) {
    low = 0.0;
    high = 1.0;
    const auto clampTo = [&](double lo, double hi) {
        low = def.toNormalized(std::clamp(lo, def.min, def.max));
        high = def.toNormalized(std::clamp(hi, def.min, def.max));
        if (low > high)
            std::swap(low, high);
    };
    if (def.unit == Unit::Db)
        clampTo(-24.0, 0.0);
    else if (def.unit == Unit::Ms)
        clampTo(def.min, std::min(def.max, 2000.0));
    else if (def.unit == Unit::Hz && startsWith(def.id, "lfo"))
        clampTo(def.min, std::min(def.max, 20.0));
    else if (endsWith(def.id, "unison.voices"))
        clampTo(1.0, 8.0);
}

} // namespace

VariationGroup variationGroup(int index) {
    const auto& reg = Registry::get();
    const ParamDef& def = reg.at(index);
    const std::string_view id = def.id, module = def.module;
    if (endsWith(id, ".on") || endsWith(id, ".engine") || endsWith(id, ".audio_rate") ||
        (endsWith(id, ".keytrack") && def.type == ParamType::Bool))
        return VariationGroup::None;
    if (startsWith(module, "osc") || module == "sub" || module == "texture")
        return VariationGroup::Sources;
    if (startsWith(module, "filter") || id == "voice.filter_routing")
        return VariationGroup::Filters;
    if (startsWith(module, "env") || startsWith(module, "lfo") || startsWith(module, "flux") || reg.modRouteOf(index) >= 0)
        return VariationGroup::Modulators;
    int lane = 0, slot = 0, field = 0;
    if (reg.fxSlotOf(index, lane, slot, field))
        return field >= 1 ? VariationGroup::Effects : VariationGroup::None;
    if (id == "fx.filter1_balance" || id == "fx.filter2_balance" || id == "fx.direct_balance")
        return VariationGroup::Effects;
    if (id == "voice.drift" || startsWith(id, "voice.variance.") || id == "voice.warmth" || id == "voice.noise_floor" ||
        id == "global.glide" || id == "global.glide_curve")
        return VariationGroup::Character;
    if ((module == "arp" || module == "chord") && def.type != ParamType::Bool)
        return VariationGroup::Play;
    return VariationGroup::None;
}

void randomizePatch(Patch& patch, const VariationScope& scope, double amount, std::span<const bool> locked,
                    std::uint64_t seed) {
    const auto& reg = Registry::get();
    Rng rng(seed);
    amount = std::clamp(amount, 0.0, 1.0);
    for (int i = 0; i < reg.size(); ++i) {
        const double r1 = rng.uniform(), r2 = rng.uniform(); // drawn for every index: results do not shift when scope changes
        const VariationGroup group = variationGroup(i);
        const bool inScope = (group == VariationGroup::Sources && scope.sources) || (group == VariationGroup::Filters && scope.filters) ||
                             (group == VariationGroup::Modulators && scope.modulators) || (group == VariationGroup::Effects && scope.effects) ||
                             (group == VariationGroup::Character && scope.character) || (group == VariationGroup::Play && scope.play);
        if (!inScope || isLocked(locked, i) || !patch.inUse(i))
            continue;
        const ParamDef& def = patch.effectiveDef(i);
        const double current = patch.get(i);
        if (def.type == ParamType::Choice || def.type == ParamType::Bool) {
            if (r1 < amount)
                patch.set(i, def.min + std::floor(r2 * (def.max - def.min + 1.0)));
            continue;
        }
        double low = 0.0, high = 1.0;
        playableRange(def, low, high);
        const double target = low + r1 * (high - low);
        const double from = def.toNormalized(current);
        if (amount <= 0.0)
            continue; // nothing moves (and no rounding through the normalized scale)
        patch.set(i, def.fromNormalized(from + amount * (target - from)));
    }
}

void mutatePatch(Patch& patch, double amount, std::span<const bool> locked, std::uint64_t seed) {
    const auto& reg = Registry::get();
    Rng rng(seed);
    amount = std::clamp(amount, 0.0, 1.0);
    for (int i = 0; i < reg.size(); ++i) {
        const double r1 = rng.uniform(), r2 = rng.uniform();
        const VariationGroup group = variationGroup(i);
        if (group == VariationGroup::None || group == VariationGroup::Play || isLocked(locked, i) || !patch.inUse(i))
            continue;
        const ParamDef& def = patch.effectiveDef(i);
        if (def.type == ParamType::Choice || def.type == ParamType::Bool) {
            if (r1 < 0.04 * amount)
                patch.set(i, def.min + std::floor(r2 * (def.max - def.min + 1.0)));
            continue;
        }
        double low = 0.0, high = 1.0;
        playableRange(def, low, high);
        const double from = def.toNormalized(patch.get(i));
        const double moved = from + (r1 * 2.0 - 1.0) * 0.06 * amount;
        // Stay inside the playable range unless the value was already outside it.
        const double lo = std::min(low, from), hi = std::max(high, from);
        patch.set(i, def.fromNormalized(std::clamp(moved, lo, hi)));
    }
}

Patch blendPatches(const Patch& base, std::span<const Patch> parents, std::span<const bool> locked, std::uint64_t seed) {
    Patch out = base;
    if (parents.empty())
        return out;
    const auto& reg = Registry::get();
    Rng rng(seed);
    const auto pick = [&]() -> const Patch& {
        return parents[static_cast<std::size_t>(rng.next() % parents.size())];
    };
    for (const auto& module : reg.modules()) {
        if (module.kind != ModuleKind::Plain)
            continue;
        const std::string_view id = module.id;
        const bool sound = startsWith(id, "osc") || id == "sub" || id == "texture" || startsWith(id, "filter") ||
                           startsWith(id, "env") || startsWith(id, "lfo") || startsWith(id, "flux") || id == "voice";
        if (!sound)
            continue;
        const Patch& from = pick();
        for (int i = module.firstParam; i < module.firstParam + module.numParams; ++i)
            out.set(i, from.get(i));
        const auto number = [&](std::string_view prefix) { return std::atoi(std::string(id.substr(prefix.size())).c_str()) - 1; };
        if (startsWith(id, "osc")) {
            const auto k = static_cast<std::size_t>(number("osc"));
            out.tables[k] = from.tables[k];
            out.tableData[k] = from.tableData[k];
            out.warpCurves[k] = from.warpCurves[k];
        } else if (id == "texture") {
            out.textureSample = from.textureSample;
        } else if (startsWith(id, "env")) {
            const auto k = static_cast<std::size_t>(number("env"));
            out.envShapes[k] = from.envShapes[k];
            out.envSync[k] = from.envSync[k];
        } else if (startsWith(id, "lfo")) {
            const auto k = static_cast<std::size_t>(number("lfo"));
            out.lfoShapes[k] = from.lfoShapes[k];
        }
    }
    // The rack whole from one parent; routes and macros from one parent.
    const Patch& rackFrom = pick();
    out.copyRackFrom(rackFrom);
    const Patch& routesFrom = pick();
    while (!out.routes.empty())
        out.removeRoute(static_cast<int>(out.routes.size()) - 1);
    out.routes = routesFrom.routes;
    for (int r = 0; r < static_cast<int>(out.routes.size()); ++r)
        out.set(reg.modAmount(r), routesFrom.get(reg.modAmount(r)));
    for (int m = 0; m < kNumMacros; ++m) {
        out.set(reg.macroBipolar(m), routesFrom.get(reg.macroBipolar(m)));
        out.macroNames[static_cast<std::size_t>(m)] = routesFrom.macroNames[static_cast<std::size_t>(m)];
    }
    // Routes onto effect parameters only make sense with the rack they were made for.
    if (&rackFrom != &routesFrom) {
        for (int r = static_cast<int>(out.routes.size()) - 1; r >= 0; --r) {
            const int dest = reg.indexOf(out.routes[static_cast<std::size_t>(r)].dest);
            int lane = 0, slot = 0, field = 0;
            if (dest >= 0 && reg.fxSlotOf(dest, lane, slot, field))
                out.removeRoute(r);
        }
    }
    for (int i = 0; i < reg.size(); ++i)
        if (isLocked(locked, i))
            out.set(i, base.get(i));
    return out;
}

} // namespace zed
