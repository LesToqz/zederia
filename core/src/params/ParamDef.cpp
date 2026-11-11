#include "zederia/params/ParamDef.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace zed {

double ParamDef::clamp(double value) const {
    if (std::isnan(value))
        return def;
    value = std::clamp(value, min, max);
    if (stepped())
        value = std::clamp(std::round(value), min, max);
    return value;
}

double ParamDef::toNormalized(double plain) const {
    plain = clamp(plain);
    if (max <= min)
        return 0.0;
    switch (skew) {
    case Skew::Log:
        return std::clamp(std::log(plain / min) / std::log(max / min), 0.0, 1.0);
    case Skew::Power:
        return std::clamp(std::pow((plain - min) / (max - min), 1.0 / skewExp), 0.0, 1.0);
    case Skew::Linear:
        break;
    }
    return (plain - min) / (max - min);
}

double ParamDef::fromNormalized(double normalized) const {
    if (std::isnan(normalized))
        return def;
    const double n = std::clamp(normalized, 0.0, 1.0);
    double value = min + n * (max - min);
    if (skew == Skew::Log)
        value = min * std::pow(max / min, n);
    else if (skew == Skew::Power)
        value = min + (max - min) * std::pow(n, skewExp);
    return clamp(value);
}

int ParamDef::choiceIndex(std::string_view token) const {
    for (std::size_t i = 0; i < choices.size(); ++i)
        if (choices[i] == token)
            return static_cast<int>(i);
    return -1;
}

std::string ParamDef::format(double plain) const {
    const double v = clamp(plain);
    char buf[64];
    switch (type) {
    case ParamType::Bool:
        return v >= 0.5 ? "on" : "off";
    case ParamType::Choice: {
        const auto i = static_cast<std::size_t>(v - min);
        return i < choices.size() ? choices[i] : std::string("?");
    }
    case ParamType::Int:
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(v));
        break;
    case ParamType::Float:
        switch (unit) {
        case Unit::Percent:
            std::snprintf(buf, sizeof buf, "%.1f", v * 100.0);
            break;
        case Unit::Db:
            if (v <= kSilenceDb)
                return "-inf dB";
            std::snprintf(buf, sizeof buf, "%.1f", v);
            break;
        case Unit::Hz:
        case Unit::Ms:
            std::snprintf(buf, sizeof buf, std::abs(v) < 10.0 ? "%.2f" : std::abs(v) < 100.0 ? "%.1f" : "%.0f", v);
            break;
        case Unit::Cents:
        case Unit::Degrees:
            std::snprintf(buf, sizeof buf, "%.1f", v);
            break;
        default:
            std::snprintf(buf, sizeof buf, "%.2f", v);
            break;
        }
        break;
    }
    std::string text = buf;
    const auto label = unitLabel(unit);
    if (!label.empty()) {
        text += ' ';
        text += label;
    }
    return text;
}

std::string_view unitLabel(Unit unit) {
    switch (unit) {
    case Unit::Percent: return "%";
    case Unit::Hz: return "Hz";
    case Unit::Ms: return "ms";
    case Unit::Db: return "dB";
    case Unit::Semitones: return "st";
    case Unit::Cents: return "cents";
    case Unit::Octaves: return "oct";
    case Unit::Degrees: return "deg";
    case Unit::None: break;
    }
    return "";
}

std::string_view unitKeySuffix(Unit unit) {
    switch (unit) {
    case Unit::Hz: return "_hz";
    case Unit::Ms: return "_ms";
    case Unit::Db: return "_db";
    case Unit::Semitones: return "_st";
    case Unit::Cents: return "_cents";
    case Unit::Degrees: return "_deg";
    default: break;
    }
    return "";
}

std::string_view typeLabel(ParamType type) {
    switch (type) {
    case ParamType::Float: return "float";
    case ParamType::Int: return "int";
    case ParamType::Bool: return "bool";
    case ParamType::Choice: return "choice";
    }
    return "";
}

std::string_view skewLabel(Skew skew) {
    switch (skew) {
    case Skew::Linear: return "linear";
    case Skew::Log: return "log";
    case Skew::Power: return "power";
    }
    return "";
}

std::string_view smoothingLabel(Smoothing smoothing) {
    switch (smoothing) {
    case Smoothing::None: return "none";
    case Smoothing::Control: return "control";
    case Smoothing::Sample: return "sample";
    }
    return "";
}

std::string_view scopeLabel(Scope scope) { return scope == Scope::Voice ? "voice" : "global"; }

std::uint32_t hostIdFor(std::string_view id) {
    // The plug-in framework hashes a parameter's string id this way (multiplier 31) and clears
    // the top bit for hosts that reject negative ids. The plug-in asserts that both agree.
    std::uint32_t hash = 0;
    for (const char c : id)
        hash = 31u * hash + static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    return hash & 0x7fffffffu;
}

} // namespace zed
