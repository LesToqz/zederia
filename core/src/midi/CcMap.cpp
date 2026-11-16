#include "zederia/midi/CcMap.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace zed {
namespace {

using J = nlohmann::ordered_json;

void issue(std::vector<Issue>& issues, Severity severity, std::string code, std::string path, std::string message,
           std::string fix) {
    issues.push_back({severity, std::move(code), std::move(path), std::move(message), std::move(fix)});
}

} // namespace

void CcMap::bind(const CcBinding& binding) {
    const auto same = [&](const CcBinding& b) { return b.controller == binding.controller && b.channel == binding.channel; };
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(), same), bindings.end());
    if (bindings.size() < kMaxBindings)
        bindings.push_back(binding);
}

void CcMap::unbindParam(std::string_view param) {
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&](const CcBinding& b) { return b.param == param; }),
                   bindings.end());
}

std::string saveCcMap(const CcMap& map) {
    J doc = J::object();
    doc["format"] = kCcMapFormat;
    doc["version"] = 1;
    J list = J::array();
    for (const CcBinding& b : map.bindings) {
        J o = J::object();
        o["cc"] = b.controller;
        if (b.channel >= 0)
            o["channel"] = b.channel + 1; // written as 1..16, as players count them
        o["param"] = b.param;
        o["min"] = b.min;
        o["max"] = b.max;
        list.push_back(std::move(o));
    }
    doc["bindings"] = std::move(list);
    return doc.dump(2) + "\n";
}

bool loadCcMap(std::string_view text, CcMap& out, std::vector<Issue>& issues) {
    const std::size_t before = issues.size();
    J doc;
    try {
        doc = J::parse(text);
    } catch (const J::exception&) {
        issue(issues, Severity::Error, "bad_json", "", "The text is not valid JSON.", "Look for a missing comma, quote or bracket.");
        return false;
    }
    if (!doc.is_object() || !doc.contains("format") || !doc["format"].is_string() ||
        doc["format"].get<std::string>() != kCcMapFormat) {
        issue(issues, Severity::Error, "bad_format", "/format", "This is not a controller map.",
              "Set \"format\" to \"zederia.ccmap\", or open a file that is one.");
        return false;
    }
    if (!doc.contains("bindings") || !doc["bindings"].is_array()) {
        issue(issues, Severity::Error, "missing_field", "/bindings", "The map has no \"bindings\" list.",
              "Add \"bindings\": [ { \"cc\": 74, \"param\": \"filter1.cutoff\" } ].");
        return false;
    }
    const auto& reg = Registry::get();
    CcMap map;
    const J& list = doc["bindings"];
    for (std::size_t i = 0; i < list.size() && i < CcMap::kMaxBindings; ++i) {
        const std::string path = "/bindings/" + std::to_string(i);
        const J& b = list[i];
        if (!b.is_object() || !b.contains("cc") || !b["cc"].is_number_integer() || !b.contains("param") || !b["param"].is_string()) {
            issue(issues, Severity::Error, "wrong_type", path, "A binding needs a whole-number \"cc\" and a \"param\" id.",
                  "Write it as { \"cc\": 74, \"param\": \"filter1.cutoff\" }.");
            continue;
        }
        CcBinding binding;
        binding.controller = b["cc"].get<int>();
        binding.param = b["param"].get<std::string>();
        if (binding.controller < 0 || binding.controller > 127) {
            issue(issues, Severity::Error, "out_of_range", path + "/cc", "Controller numbers go from 0 to 127.",
                  "Use a controller number from 0 to 127.");
            continue;
        }
        const ParamDef* def = reg.find(binding.param);
        if (def == nullptr || !def->automatable) {
            issue(issues, Severity::Warning, "unknown_param", path + "/param",
                  "\"" + binding.param + "\" is not a parameter a controller can move; the binding is skipped.",
                  "Use the id of an automatable parameter (see docs/PARAMETERS.md).");
            continue;
        }
        if (b.contains("channel")) {
            if (!b["channel"].is_number_integer() || b["channel"].get<int>() < 1 || b["channel"].get<int>() > 16) {
                issue(issues, Severity::Error, "out_of_range", path + "/channel", "Channels go from 1 to 16.",
                      "Use a channel from 1 to 16, or leave \"channel\" out for any channel.");
                continue;
            }
            binding.channel = b["channel"].get<int>() - 1;
        }
        const auto range = [&](const char* key, double& value) {
            if (b.contains(key) && b[key].is_number())
                value = std::clamp(b[key].get<double>(), 0.0, 1.0);
        };
        range("min", binding.min);
        range("max", binding.max);
        map.bind(binding);
    }
    for (std::size_t i = before; i < issues.size(); ++i)
        if (issues[i].severity == Severity::Error)
            return false;
    out = std::move(map);
    return true;
}

CcTable CcTable::build(const CcMap& map) {
    CcTable table;
    for (auto& channel : table.param)
        channel.fill(-1);
    for (auto& channel : table.min)
        channel.fill(0.0f);
    for (auto& channel : table.max)
        channel.fill(1.0f);
    const auto& reg = Registry::get();
    for (const CcBinding& b : map.bindings) {
        const int index = reg.indexOf(b.param);
        if (index < 0 || b.controller < 0 || b.controller > 127)
            continue;
        for (int ch = 0; ch < 16; ++ch) {
            if (b.channel >= 0 && b.channel != ch)
                continue;
            table.param[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b.controller)] = static_cast<std::int16_t>(index);
            table.min[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b.controller)] = static_cast<float>(b.min);
            table.max[static_cast<std::size_t>(ch)][static_cast<std::size_t>(b.controller)] = static_cast<float>(b.max);
        }
    }
    // Channel-specific bindings win over "any channel" ones, whatever their order.
    for (const CcBinding& b : map.bindings) {
        const int index = reg.indexOf(b.param);
        if (index < 0 || b.channel < 0 || b.controller < 0 || b.controller > 127)
            continue;
        table.param[static_cast<std::size_t>(b.channel)][static_cast<std::size_t>(b.controller)] = static_cast<std::int16_t>(index);
        table.min[static_cast<std::size_t>(b.channel)][static_cast<std::size_t>(b.controller)] = static_cast<float>(b.min);
        table.max[static_cast<std::size_t>(b.channel)][static_cast<std::size_t>(b.controller)] = static_cast<float>(b.max);
    }
    return table;
}

} // namespace zed
