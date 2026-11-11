#include "zederia/patch/Patch.h"

namespace zed {

Patch::Patch() {
    const auto& reg = Registry::get();
    values_.resize(static_cast<std::size_t>(reg.size()));
    for (const auto& p : reg.params())
        values_[static_cast<std::size_t>(p.index)] = p.def;
}

double Patch::get(std::string_view id) const {
    const int i = Registry::get().indexOf(id);
    return i < 0 ? 0.0 : get(i);
}

const ParamDef& Patch::effectiveDef(int index) const {
    const auto& reg = Registry::get();
    int lane = 0, slot = 0, k = 0;
    if (reg.fxSlotParamAt(index, lane, slot, k)) {
        const int type = fxSlotType(lane, slot);
        if (type >= 0) {
            const auto& params = reg.effectTypes()[static_cast<std::size_t>(type)].params;
            if (k < static_cast<int>(params.size()))
                return params[static_cast<std::size_t>(k)];
        }
    }
    return reg.at(index);
}

bool Patch::inUse(int index) const {
    const auto& reg = Registry::get();
    int lane = 0, slot = 0, field = 0;
    if (reg.fxSlotOf(index, lane, slot, field)) {
        const int type = fxSlotType(lane, slot);
        if (type < 0)
            return false;
        return field < 2 || field - 2 < static_cast<int>(reg.effectTypes()[static_cast<std::size_t>(type)].params.size());
    }
    const int route = reg.modRouteOf(index);
    return route < 0 || route < static_cast<int>(routes.size());
}

double Patch::set(int index, double value) {
    const double stored = inUse(index) ? effectiveDef(index).clamp(value) : Registry::get().at(index).def;
    values_[static_cast<std::size_t>(index)] = stored;
    return stored;
}

bool Patch::set(std::string_view id, double value) {
    const int i = Registry::get().indexOf(id);
    if (i < 0)
        return false;
    set(i, value);
    return true;
}

int Patch::fxSlotType(int lane, int slot) const {
    const auto& slots = fx_[static_cast<std::size_t>(lane)];
    return slot >= 0 && slot < static_cast<int>(slots.size()) ? slots[static_cast<std::size_t>(slot)] : -1;
}

int Patch::addFxSlot(int lane, int effectIndex) {
    const auto& reg = Registry::get();
    auto& slots = fx_[static_cast<std::size_t>(lane)];
    if (static_cast<int>(slots.size()) >= kNumFxSlots || effectIndex < 0 ||
        effectIndex >= static_cast<int>(reg.effectTypes().size()))
        return -1;
    const int slot = static_cast<int>(slots.size());
    slots.push_back(effectIndex);
    const auto& params = reg.effectTypes()[static_cast<std::size_t>(effectIndex)].params;
    set(reg.fxSlotOn(lane, slot), reg.at(reg.fxSlotOn(lane, slot)).def);
    set(reg.fxSlotMix(lane, slot), reg.at(reg.fxSlotMix(lane, slot)).def);
    for (int k = 0; k < kNumFxSlotParams; ++k)
        values_[static_cast<std::size_t>(reg.fxSlotParam(lane, slot, k))] =
            k < static_cast<int>(params.size()) ? params[static_cast<std::size_t>(k)].def : 0.0;
    return slot;
}

void Patch::removeRoute(int route) {
    const auto& reg = Registry::get();
    const int count = static_cast<int>(routes.size());
    if (route < 0 || route >= count)
        return;
    for (int i = route; i + 1 < count; ++i)
        values_[static_cast<std::size_t>(reg.modAmount(i))] = values_[static_cast<std::size_t>(reg.modAmount(i + 1))];
    values_[static_cast<std::size_t>(reg.modAmount(count - 1))] = reg.at(reg.modAmount(count - 1)).def;
    routes.erase(routes.begin() + route);
}

void Patch::clearFx(int lane) {
    const auto& reg = Registry::get();
    fx_[static_cast<std::size_t>(lane)].clear();
    for (int slot = 0; slot < kNumFxSlots; ++slot) {
        values_[static_cast<std::size_t>(reg.fxSlotOn(lane, slot))] = reg.at(reg.fxSlotOn(lane, slot)).def;
        values_[static_cast<std::size_t>(reg.fxSlotMix(lane, slot))] = reg.at(reg.fxSlotMix(lane, slot)).def;
        for (int k = 0; k < kNumFxSlotParams; ++k)
            values_[static_cast<std::size_t>(reg.fxSlotParam(lane, slot, k))] = 0.0;
    }
}

} // namespace zed
