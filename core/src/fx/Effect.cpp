#include "FxImpl.h"
#include "zederia/params/Registry.h"

#include <string_view>
#include <vector>

namespace zed::fx {
namespace {

struct Entry {
    std::string_view id;
    std::unique_ptr<Effect> (*make)();
    const char* const* params;
    const std::size_t* count;
};

#define ZED_ENTRY(id, Name) {id, &make##Name, k##Name##Params, &k##Name##ParamCount}
const Entry kEntries[] = {
    ZED_ENTRY("drive", Drive),     ZED_ENTRY("compressor", Compressor), ZED_ENTRY("eq", Eq),
    ZED_ENTRY("filter", FilterFx), ZED_ENTRY("chorus", Chorus),         ZED_ENTRY("flanger", Flanger),
    ZED_ENTRY("phaser", Phaser),   ZED_ENTRY("delay", Delay),           ZED_ENTRY("reverb", Reverb),
    ZED_ENTRY("swarm", Swarm),     ZED_ENTRY("widener", Widener),       ZED_ENTRY("utility", Utility),
    ZED_ENTRY("limiter", Limiter),
};
#undef ZED_ENTRY

const Entry* entryFor(int type) {
    const auto types = Registry::get().effectTypes();
    if (type < 0 || type >= static_cast<int>(types.size()))
        return nullptr;
    for (const Entry& e : kEntries)
        if (e.id == types[static_cast<std::size_t>(type)].id)
            return &e;
    return nullptr;
}

// Built before main(), so the audio thread only ever reads it.
const std::vector<double> kDivisionBeats = [] {
    std::vector<double> beats;
    for (const std::string& token : tempoDivisions())
        beats.push_back(std::max(divisionInBeats(token), 1.0e-3));
    return beats;
}();

} // namespace

std::unique_ptr<Effect> makeEffect(int type, double sampleRate) {
    const Entry* e = entryFor(type);
    if (e == nullptr)
        return nullptr;
    auto effect = e->make();
    effect->prepare(sampleRate);
    return effect;
}

std::span<const char* const> implementedParams(int type) {
    const Entry* e = entryFor(type);
    return e != nullptr ? std::span<const char* const>(e->params, *e->count) : std::span<const char* const>();
}

double divisionBeats(int index) noexcept {
    return index >= 0 && index < static_cast<int>(kDivisionBeats.size()) ? kDivisionBeats[static_cast<std::size_t>(index)] : 1.0;
}

bool effectResponse(int type, std::span<const double> values, double sampleRate, double hz, std::complex<double>& out) {
    const Entry* e = entryFor(type);
    if (e == nullptr || values.size() < *e->count)
        return false;
    if (e->id == "eq")
        return eqResponse(values.data(), sampleRate, hz, out);
    if (e->id == "filter")
        return filterFxResponse(values.data(), sampleRate, hz, out);
    return false;
}

} // namespace zed::fx
