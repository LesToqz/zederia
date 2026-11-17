#include "zederia/params/ValueText.h"

#include "zederia/util/Math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace zed {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

// Letters and digits only, lower case: "Ladder LP 24" and "ladder_lp24" compare equal.
std::string squash(std::string_view s) {
    std::string out;
    for (const char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '#' || c == '+' || c == '-')
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Reads a leading number ("-6", "+7.5", ".25", "2e3", "-inf"); returns the rest of the text.
bool leadingNumber(std::string_view s, double& value, std::string_view& rest) {
    const std::string text(s);
    const std::string low = lower(text);
    for (const char* inf : {"-inf", "-infinity", "-\xe2\x88\x9e"}) { // "-∞"
        if (low.rfind(inf, 0) == 0) {
            value = -HUGE_VAL;
            rest = s.substr(std::string_view(inf).size());
            return true;
        }
    }
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value))
        return false;
    rest = s.substr(static_cast<std::size_t>(end - text.c_str()));
    return true;
}

// "1/8", "1/4T", "1/16D", "3/16", "1/8." (dotted), "2 bars", "1 bar".
bool noteLength(std::string_view s, double& beats, std::string& token) {
    const std::string low = lower(trim(s));
    if (low.empty())
        return false;
    const auto slash = low.find('/');
    if (slash == std::string::npos) {
        // "2 bars", "1 bar", "4 beats"
        double count = 0.0;
        std::string_view rest;
        if (!leadingNumber(low, count, rest) || count <= 0.0)
            return false;
        const std::string unit = lower(trim(rest));
        if (unit == "bar" || unit == "bars")
            beats = count * 4.0;
        else if (unit == "beat" || unit == "beats")
            beats = count;
        else
            return false;
        token = std::string(trim(s));
        return true;
    }
    const std::string num = low.substr(0, slash);
    std::string den = low.substr(slash + 1);
    double factor = 1.0;
    char mark = 0;
    if (!den.empty() && (den.back() == 't' || den.back() == 'd' || den.back() == '.')) {
        mark = den.back();
        factor = mark == 't' ? 2.0 / 3.0 : 1.5;
        den.pop_back();
    }
    if (num.empty() || den.empty() || num.size() > 3 || den.size() > 3)
        return false;
    for (const char c : num + den)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    const int n = std::atoi(num.c_str()), d = std::atoi(den.c_str());
    if (n <= 0 || d <= 0)
        return false;
    beats = 4.0 * n / d * factor;
    token = num + "/" + den + (mark == 't' ? "T" : mark ? "D" : "");
    return true;
}

std::string fmt(const char* format, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

std::string unitHint(const ParamDef& def) {
    switch (def.unit) {
    case Unit::Hz: return "a frequency, for example 440 Hz, 2 kHz, A4 or 1/8";
    case Unit::Ms: return "a time, for example 250 ms, 1.5 s or 1/8";
    case Unit::Db: return "a level, for example -6 dB or -inf";
    case Unit::Percent: return "a percentage, for example 25 %";
    case Unit::Semitones: return "semitones, for example +7 st or 1 oct";
    case Unit::Cents: return "cents, for example 12 c or 0.5 st";
    case Unit::Octaves: return "octaves, for example 1 oct or 12 st";
    case Unit::Degrees: return "degrees, for example 90";
    case Unit::None: break;
    }
    return "a number from " + fmt("%g", def.min) + " to " + fmt("%g", def.max);
}

} // namespace

std::string noteName(int key) {
    static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int octave = static_cast<int>(std::floor(key / 12.0)) - 1;
    return std::string(names[((key % 12) + 12) % 12]) + std::to_string(octave);
}

bool parseNoteName(std::string_view text, double& note) {
    const std::string s = lower(trim(text));
    if (s.size() < 2)
        return false;
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7}; // a b c d e f g
    if (s[0] < 'a' || s[0] > 'g')
        return false;
    int pitch = base[s[0] - 'a'];
    std::size_t i = 1;
    if (s[i] == '#') {
        ++pitch;
        ++i;
    } else if (s[i] == 'b' && i + 1 < s.size() && (std::isdigit(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '-')) {
        --pitch;
        ++i;
    }
    if (i >= s.size())
        return false;
    const std::string octaveText = s.substr(i);
    for (std::size_t k = 0; k < octaveText.size(); ++k)
        if (!(std::isdigit(static_cast<unsigned char>(octaveText[k])) || (k == 0 && octaveText[k] == '-')))
            return false;
    const int octave = std::atoi(octaveText.c_str());
    if (octave < -2 || octave > 9)
        return false;
    note = (octave + 1) * 12 + pitch;
    return true;
}

std::string choiceDisplayName(std::string_view token) {
    if (token.find('/') != std::string_view::npos)
        return std::string(token); // tempo divisions
    std::string out;
    std::size_t start = 0;
    while (start <= token.size()) {
        const std::size_t end = std::min(token.find('_', start), token.size());
        std::string part(token.substr(start, end - start));
        start = end + 1;
        if (part.empty())
            continue;
        std::string word;
        if (part == "plus") word = "+";
        else if (part == "minus") word = "-";
        else if (part == "both") word = "+/-";
        else if (part == "sharp") {
            if (!out.empty() && out.back() == ' ')
                out.pop_back();
            out += "#";
            continue;
        } else if (part == "svf" || part == "pm" || part == "am" || part == "rm" || part == "mpe" || part == "dc" || part == "eq" ||
                   part == "lbh" || part == "lph" || part == "lnh" || part == "bpn")
            word = upper(part);
        else {
            // "lp24" -> "LP 24", "pulse25" -> "Pulse 25", "a" -> "A"
            std::size_t digits = part.size();
            while (digits > 0 && std::isdigit(static_cast<unsigned char>(part[digits - 1])))
                --digits;
            std::string letters = part.substr(0, digits), number = part.substr(digits);
            if (letters == "lp" || letters == "hp" || letters == "bp")
                letters = upper(letters);
            else if (!letters.empty())
                letters[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(letters[0])));
            word = letters + (!letters.empty() && !number.empty() ? " " : "") + number;
        }
        if (!out.empty())
            out += ' ';
        out += word;
    }
    return out;
}

bool parseValueText(const ParamDef& def, std::string_view input, const ValueContext& context, ParsedValue& out,
                    std::string& error) {
    out = ParsedValue{};
    const std::string_view text = trim(input);
    const std::string low = lower(text);
    if (text.empty()) {
        error = "Type " + unitHint(def) + ".";
        return false;
    }
    const auto finish = [&](double value) {
        const double clamped = def.clamp(value);
        out.clamped = std::isfinite(value) ? std::abs(clamped - value) > 1e-9 * std::max(1.0, std::abs(value)) : true;
        out.plain = clamped;
        return true;
    };

    if (def.type == ParamType::Bool) {
        if (low == "on" || low == "true" || low == "yes" || low == "1")
            return finish(1.0);
        if (low == "off" || low == "false" || low == "no" || low == "0")
            return finish(0.0);
        error = "Type on or off.";
        return false;
    }
    if (def.type == ParamType::Choice) {
        const std::string want = squash(text);
        for (std::size_t i = 0; i < def.choices.size(); ++i)
            if (squash(def.choices[i]) == want || squash(choiceDisplayName(def.choices[i])) == want)
                return finish(static_cast<double>(i));
        // A unique prefix is enough ("lad" finds nothing unique, "ladder lp24" does).
        int found = -1, matches = 0;
        for (std::size_t i = 0; i < def.choices.size(); ++i)
            if (squash(choiceDisplayName(def.choices[i])).rfind(want, 0) == 0 && !want.empty()) {
                found = static_cast<int>(i);
                ++matches;
            }
        if (matches == 1)
            return finish(found);
        error = "\"" + std::string(text) + "\" is not one of the options of " + def.name + ".";
        return false;
    }

    // Note lengths: times and rates.
    double beats = 0.0;
    std::string token;
    if ((def.unit == Unit::Ms || def.unit == Unit::Hz) && noteLength(text, beats, token)) {
        const double seconds = beats * 60.0 / std::max(1.0, context.bpm);
        out.noteLength = token;
        out.beats = beats;
        return finish(def.unit == Unit::Ms ? seconds * 1000.0 : 1.0 / seconds);
    }
    // Note names: frequencies.
    double note = 0.0;
    if (def.unit == Unit::Hz && parseNoteName(text, note))
        return finish(noteToHz(note, context.referenceHz));

    double value = 0.0;
    std::string_view restView;
    if (!leadingNumber(text, value, restView)) {
        error = "Type " + unitHint(def) + ".";
        return false;
    }
    const std::string unit = lower(trim(restView));
    const auto bad = [&] {
        error = "\"" + std::string(text) + "\" does not fit " + def.name + "; type " + unitHint(def) + ".";
        return false;
    };
    if (std::isinf(value)) { // "-inf": the bottom of the range (silence for levels)
        if (def.unit == Unit::Db || def.unit == Unit::None || def.unit == Unit::Percent)
            return finish(def.min);
        return bad();
    }

    switch (def.unit) {
    case Unit::Hz:
        if (unit.empty() || unit == "hz") return finish(value);
        if (unit == "k" || unit == "khz") return finish(value * 1000.0);
        if (unit == "ms") return value > 0 ? finish(1000.0 / value) : bad();
        if (unit == "s" || unit == "sec") return value > 0 ? finish(1.0 / value) : bad();
        return bad();
    case Unit::Ms:
        if (unit.empty() || unit == "ms") return finish(value);
        if (unit == "s" || unit == "sec" || unit == "secs" || unit == "seconds") return finish(value * 1000.0);
        if (unit == "min") return finish(value * 60000.0);
        if (unit == "hz") return value > 0 ? finish(1000.0 / value) : bad();
        return bad();
    case Unit::Db:
        if (unit.empty() || unit == "db") return finish(value);
        if (unit == "%") return value > 0 ? finish(gainToDb(value / 100.0)) : finish(def.min);
        if (unit == "x") return value > 0 ? finish(gainToDb(value)) : finish(def.min);
        return bad();
    case Unit::Percent:
        if (unit.empty() || unit == "%") return finish(value / 100.0);
        return bad();
    case Unit::Semitones:
        if (unit.empty() || unit == "st" || unit == "semi" || unit == "semitones") return finish(value);
        if (unit == "oct" || unit == "octave" || unit == "octaves") return finish(value * 12.0);
        if (unit == "c" || unit == "ct" || unit == "cent" || unit == "cents") return finish(value / 100.0);
        return bad();
    case Unit::Cents:
        if (unit.empty() || unit == "c" || unit == "ct" || unit == "cent" || unit == "cents") return finish(value);
        if (unit == "st" || unit == "semi" || unit == "semitones") return finish(value * 100.0);
        return bad();
    case Unit::Octaves:
        if (unit.empty() || unit == "oct" || unit == "octave" || unit == "octaves") return finish(value);
        if (unit == "st" || unit == "semi" || unit == "semitones") return finish(value / 12.0);
        return bad();
    case Unit::Degrees:
        if (unit.empty() || unit == "deg" || unit == "\xc2\xb0") return finish(value);
        return bad();
    case Unit::None:
        if (unit.empty()) return finish(def.type == ParamType::Int ? std::round(value) : value);
        if (unit == "%" && def.min >= -1.0 && def.max <= 1.0) return finish(value / 100.0);
        return bad();
    }
    return bad();
}

std::string formatValueText(const ParamDef& def, double plain) {
    switch (def.type) {
    case ParamType::Bool:
        return plain >= 0.5 ? "On" : "Off";
    case ParamType::Choice: {
        const int i = static_cast<int>(std::lround(plain - def.min));
        return i >= 0 && i < static_cast<int>(def.choices.size()) ? choiceDisplayName(def.choices[static_cast<std::size_t>(i)]) : "?";
    }
    case ParamType::Int:
        if (def.unit == Unit::Semitones || def.unit == Unit::Octaves)
            return (plain > 0 ? "+" : "") + std::to_string(static_cast<int>(std::lround(plain))) +
                   (def.unit == Unit::Semitones ? " st" : " oct");
        return std::to_string(static_cast<int>(std::lround(plain)));
    case ParamType::Float:
        break;
    }
    const double a = std::abs(plain);
    switch (def.unit) {
    case Unit::Hz:
        if (a >= 1000.0)
            return fmt(a >= 10000.0 ? "%.1f kHz" : "%.2f kHz", plain / 1000.0);
        return fmt(a >= 100.0 ? "%.0f Hz" : a >= 10.0 ? "%.1f Hz" : "%.2f Hz", plain);
    case Unit::Ms:
        if (a >= 1000.0)
            return fmt(a >= 10000.0 ? "%.1f s" : "%.2f s", plain / 1000.0);
        return fmt(a >= 100.0 ? "%.0f ms" : "%.1f ms", plain);
    case Unit::Db:
        return plain <= kSilenceDb + 1e-9 ? "-inf dB" : fmt("%.1f dB", std::abs(plain) < 0.05 ? 0.0 : plain);
    case Unit::Percent: {
        const double p = plain * 100.0;
        const std::string sign = def.min < 0.0 && p > 0.05 ? "+" : "";
        return sign + fmt(std::abs(p) >= 9.95 ? "%.0f %%" : "%.1f %%", std::abs(p) < 0.05 ? 0.0 : p);
    }
    case Unit::Semitones:
        if (std::abs(plain - std::round(plain)) < 0.005)
            return (std::round(plain) > 0 ? "+" : "") + fmt("%.0f st", std::round(plain));
        return fmt("%+.2f st", plain);
    case Unit::Cents:
        return fmt(std::abs(plain) < 0.05 ? "%.1f c" : "%+.1f c", std::abs(plain) < 0.05 ? 0.0 : plain);
    case Unit::Octaves:
        return fmt("%+.2f oct", plain);
    case Unit::Degrees:
        return fmt("%.0f\xc2\xb0", plain);
    case Unit::None:
        break;
    }
    return fmt(a >= 100.0 ? "%.0f" : a >= 10.0 ? "%.1f" : "%.2f", plain);
}

} // namespace zed
