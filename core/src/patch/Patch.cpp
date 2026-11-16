#include "zederia/patch/Patch.h"

#include <algorithm>

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
        fxCurves[static_cast<std::size_t>(lane)][static_cast<std::size_t>(slot)] = {};
    }
}

// --- Rack editing ------------------------------------------------------------------------------

namespace {

struct SlotRecord {
    int type = -1;
    std::array<double, 2 + kNumFxSlotParams> values{}; // on, mix, p1..p16
    std::array<Shape, 2> curves;
    int origin = -1; // lane * kNumFxSlots + slot before the edit; -1 for a new effect
};
using Records = std::array<std::vector<SlotRecord>, kNumFxLanes>;

Records recordsOf(const Patch& patch) {
    const auto& reg = Registry::get();
    Records out;
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        for (int slot = 0; slot < patch.fxSlotCount(lane); ++slot) {
            SlotRecord r;
            r.type = patch.fxSlotType(lane, slot);
            for (int f = 0; f < 2 + kNumFxSlotParams; ++f)
                r.values[static_cast<std::size_t>(f)] = patch.get(reg.fxSlotOn(lane, slot) + f);
            r.curves = patch.fxCurves[static_cast<std::size_t>(lane)][static_cast<std::size_t>(slot)];
            r.origin = lane * kNumFxSlots + slot;
            out[static_cast<std::size_t>(lane)].push_back(std::move(r));
        }
    }
    return out;
}

SlotRecord freshRecord(int type) {
    const auto& reg = Registry::get();
    SlotRecord r;
    r.type = type;
    r.values[0] = reg.at(reg.fxSlotOn(0, 0)).def;
    r.values[1] = reg.at(reg.fxSlotMix(0, 0)).def;
    const auto& params = reg.effectTypes()[static_cast<std::size_t>(type)].params;
    for (std::size_t k = 0; k < params.size(); ++k)
        r.values[2 + k] = params[k].def;
    return r;
}

} // namespace

static void writeRecords(Patch& patch, const Records& records, FxMoves* moves) {
    const auto& reg = Registry::get();
    std::array<int, kNumFxLanes * kNumFxSlots> to;
    to.fill(-1);
    FxMoves result;
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        patch.clearFx(lane);
        const auto& list = records[static_cast<std::size_t>(lane)];
        for (const SlotRecord& r : list) {
            const int slot = patch.addFxSlot(lane, r.type);
            for (int f = 0; f < 2 + kNumFxSlotParams; ++f)
                patch.set(reg.fxSlotOn(lane, slot) + f, r.values[static_cast<std::size_t>(f)]);
            patch.fxCurves[static_cast<std::size_t>(lane)][static_cast<std::size_t>(slot)] = r.curves;
            if (r.origin >= 0) {
                to[static_cast<std::size_t>(r.origin)] = lane * kNumFxSlots + slot;
                result.from[static_cast<std::size_t>(lane * kNumFxSlots + slot)] = r.origin;
            }
        }
    }
    // Routes follow their effect; routes onto an effect that is gone are removed.
    for (int i = static_cast<int>(patch.routes.size()) - 1; i >= 0; --i) {
        ModRoute& route = patch.routes[static_cast<std::size_t>(i)];
        const int index = reg.indexOf(route.dest);
        int lane = 0, slot = 0, field = 0;
        if (index < 0 || !reg.fxSlotOf(index, lane, slot, field))
            continue;
        const int target = to[static_cast<std::size_t>(lane * kNumFxSlots + slot)];
        if (target < 0) {
            patch.removeRoute(i);
            continue;
        }
        route.dest = reg.at(reg.fxSlotOn(target / kNumFxSlots, target % kNumFxSlots) + field).id;
    }
    if (moves != nullptr)
        *moves = result;
}

bool Patch::insertFxSlot(int lane, int position, int effectIndex, FxMoves* moves) {
    const auto& reg = Registry::get();
    if (lane < 0 || lane >= kNumFxLanes || effectIndex < 0 || effectIndex >= static_cast<int>(reg.effectTypes().size()))
        return false;
    Records records = recordsOf(*this);
    auto& list = records[static_cast<std::size_t>(lane)];
    if (static_cast<int>(list.size()) >= kNumFxSlots || position < 0 || position > static_cast<int>(list.size()))
        return false;
    list.insert(list.begin() + position, freshRecord(effectIndex));
    writeRecords(*this, records, moves);
    return true;
}

bool Patch::removeFxSlot(int lane, int position, FxMoves* moves) {
    if (lane < 0 || lane >= kNumFxLanes || position < 0 || position >= fxSlotCount(lane))
        return false;
    Records records = recordsOf(*this);
    auto& list = records[static_cast<std::size_t>(lane)];
    list.erase(list.begin() + position);
    writeRecords(*this, records, moves);
    return true;
}

bool Patch::moveFxSlot(int fromLane, int fromPosition, int toLane, int toPosition, FxMoves* moves) {
    if (fromLane < 0 || fromLane >= kNumFxLanes || toLane < 0 || toLane >= kNumFxLanes || fromPosition < 0 ||
        fromPosition >= fxSlotCount(fromLane))
        return false;
    if (toLane != fromLane && fxSlotCount(toLane) >= kNumFxSlots)
        return false;
    Records records = recordsOf(*this);
    auto& from = records[static_cast<std::size_t>(fromLane)];
    SlotRecord moving = from[static_cast<std::size_t>(fromPosition)];
    from.erase(from.begin() + fromPosition);
    auto& to = records[static_cast<std::size_t>(toLane)];
    to.insert(to.begin() + std::clamp(toPosition, 0, static_cast<int>(to.size())), std::move(moving));
    writeRecords(*this, records, moves);
    return true;
}

void Patch::copyRackFrom(const Patch& other) {
    const auto& reg = Registry::get();
    Records records = recordsOf(other);
    for (auto& lane : records)
        for (auto& r : lane)
            r.origin = -1;
    // Routes of this patch onto its own effects would land on the other rack's: drop them.
    for (int i = static_cast<int>(routes.size()) - 1; i >= 0; --i) {
        const int index = reg.indexOf(routes[static_cast<std::size_t>(i)].dest);
        int lane = 0, slot = 0, field = 0;
        if (index >= 0 && reg.fxSlotOf(index, lane, slot, field))
            removeRoute(i);
    }
    writeRecords(*this, records, nullptr);
    for (const auto& module : reg.modules())
        if (module.kind == ModuleKind::FxRack)
            for (int i = module.firstParam; i < module.firstParam + module.numParams; ++i)
                set(i, other.get(i));
}

} // namespace zed
