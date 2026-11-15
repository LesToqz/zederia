#include "zederia/tuning/Tuning.h"

#include "zederia/render/OfflineRenderer.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace zed {
namespace {

constexpr std::size_t kMaxTuningBytes = 1u << 20;
constexpr int kMaxScaleSize = 4096;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// The lines of a Scala file that are not comments, with their line numbers.
struct Line {
    std::string_view text;
    int number = 0;
};
std::vector<Line> scalaLines(std::string_view text) {
    std::vector<Line> lines;
    int number = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++number;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!line.empty() && line.front() == '!')
            continue;
        lines.push_back({line, number});
    }
    return lines;
}

std::string_view firstToken(std::string_view s) {
    s = trim(s);
    const std::size_t end = s.find_first_of(" \t");
    return end == std::string_view::npos ? s : s.substr(0, end);
}

bool toDouble(std::string_view s, double& out) {
    if (!s.empty() && s.front() == '+')
        s.remove_prefix(1);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(out);
}

bool toInt(std::string_view s, int& out) {
    if (!s.empty() && s.front() == '+')
        s.remove_prefix(1);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::string where(int line) { return "line " + std::to_string(line) + ": "; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return out;
}

} // namespace

Tuning::Tuning() {
    for (int i = 0; i < 128; ++i)
        note[static_cast<std::size_t>(i)] = i;
}

bool Tuning::isEqual() const {
    for (int i = 0; i < 128; ++i)
        if (note[static_cast<std::size_t>(i)] != static_cast<double>(i))
            return false;
    return true;
}

double Tuning::pitchOf(double key) const noexcept {
    if (key <= 0.0)
        return note[0] + key;
    if (key >= 127.0)
        return note[127] + (key - 127.0);
    const int below = static_cast<int>(key);
    const double t = key - below;
    const double a = note[static_cast<std::size_t>(below)], b = note[static_cast<std::size_t>(below + 1)];
    return a + (b - a) * t;
}

bool parseScl(std::string_view text, ScaleDef& out, std::string& error) {
    out = ScaleDef{};
    if (text.size() > kMaxTuningBytes) {
        error = "the file is too large to be a scale";
        return false;
    }
    const auto lines = scalaLines(text);
    if (lines.size() < 2) {
        error = "a Scala scale needs a description line and a note count";
        return false;
    }
    out.description = std::string(trim(lines[0].text));
    int count = 0;
    if (!toInt(firstToken(lines[1].text), count) || count < 1 || count > kMaxScaleSize) {
        error = where(lines[1].number) + "expected the number of notes in the scale (1 to " + std::to_string(kMaxScaleSize) + ")";
        return false;
    }
    std::size_t next = 2;
    for (int i = 0; i < count; ++i) {
        while (next < lines.size() && trim(lines[next].text).empty())
            ++next;
        if (next >= lines.size()) {
            error = "the scale says it has " + std::to_string(count) + " notes but lists only " + std::to_string(i);
            return false;
        }
        const Line& line = lines[next++];
        const std::string_view token = firstToken(line.text);
        double cents = 0.0;
        if (token.find('.') != std::string_view::npos) { // a value with a point is in cents
            if (!toDouble(token, cents)) {
                error = where(line.number) + "\"" + std::string(token) + "\" is not a pitch in cents";
                return false;
            }
        } else { // otherwise a ratio such as 3/2, or a whole number such as 2
            const std::size_t slash = token.find('/');
            double numerator = 0.0, denominator = 1.0;
            const bool ok = slash == std::string_view::npos
                                ? toDouble(token, numerator)
                                : toDouble(token.substr(0, slash), numerator) && toDouble(token.substr(slash + 1), denominator);
            if (!ok || numerator <= 0.0 || denominator <= 0.0) {
                error = where(line.number) + "\"" + std::string(token) + "\" is not a ratio such as 3/2 or a pitch in cents such as 701.955";
                return false;
            }
            cents = 1200.0 * std::log2(numerator / denominator);
        }
        if (std::abs(cents) > 120000.0) {
            error = where(line.number) + "the pitch is out of range";
            return false;
        }
        out.cents.push_back(cents);
    }
    if (out.cents.back() <= 0.0) {
        error = "the last note of the scale is the interval at which it repeats and must be above the first";
        return false;
    }
    return true;
}

bool parseKbm(std::string_view text, KeyMap& out, std::string& error) {
    out = KeyMap{};
    if (text.size() > kMaxTuningBytes) {
        error = "the file is too large to be a keyboard mapping";
        return false;
    }
    std::vector<Line> lines;
    for (const Line& line : scalaLines(text))
        if (!trim(line.text).empty())
            lines.push_back(line);
    if (lines.size() < 7) {
        error = "a keyboard mapping needs seven header values: size, first key, last key, middle key, reference key, "
                "reference frequency and octave degree";
        return false;
    }
    int size = 0;
    const auto readInt = [&](std::size_t index, int low, int high, const char* what, int& value) {
        if (toInt(firstToken(lines[index].text), value) && value >= low && value <= high)
            return true;
        error = where(lines[index].number) + "expected " + what + " (" + std::to_string(low) + " to " + std::to_string(high) + ")";
        return false;
    };
    if (!readInt(0, 0, 128, "the size of the mapping", size) || !readInt(1, 0, 127, "the first key", out.first) ||
        !readInt(2, 0, 127, "the last key", out.last) || !readInt(3, 0, 127, "the middle key", out.middle) ||
        !readInt(4, 0, 127, "the reference key", out.reference))
        return false;
    if (!toDouble(firstToken(lines[5].text), out.referenceHz) || out.referenceHz < 1.0 || out.referenceHz > 20000.0) {
        error = where(lines[5].number) + "expected the reference frequency in Hz";
        return false;
    }
    if (!readInt(6, 0, kMaxScaleSize, "the octave degree", out.octaveDegree))
        return false;
    for (int i = 0; i < size; ++i) {
        const std::size_t index = 7 + static_cast<std::size_t>(i);
        int degree = -1;
        if (index < lines.size()) { // missing entries at the end are unmapped keys
            const std::string_view token = firstToken(lines[index].text);
            if (token != "x" && token != "X" && (!toInt(token, degree) || degree < 0 || degree > kMaxScaleSize)) {
                error = where(lines[index].number) + "expected a scale degree or x for an unmapped key";
                return false;
            }
        }
        out.degrees.push_back(degree);
    }
    return true;
}

Tuning tuningFromScale(const ScaleDef& scale, const KeyMap& map, std::string name) {
    Tuning tuning;
    tuning.name = std::move(name);
    const int size = static_cast<int>(scale.cents.size());
    if (size == 0)
        return tuning;
    const double period = scale.cents.back();
    const auto centsOfDegree = [&](int degree) {
        const int octave = degree >= 0 ? degree / size : -((-degree + size - 1) / size);
        const int step = degree - octave * size;
        return octave * period + (step == 0 ? 0.0 : scale.cents[static_cast<std::size_t>(step - 1)]);
    };
    const int mapSize = static_cast<int>(map.degrees.size());
    const int octaveDegree = map.octaveDegree > 0 ? map.octaveDegree : size;
    // Cents of a key above the scale's first degree; false when the key is unmapped.
    const auto centsOfKey = [&](int key, double& cents) {
        const int offset = key - map.middle;
        if (mapSize == 0) {
            cents = centsOfDegree(offset);
            return true;
        }
        const int pass = offset >= 0 ? offset / mapSize : -((-offset + mapSize - 1) / mapSize);
        const int entry = map.degrees[static_cast<std::size_t>(offset - pass * mapSize)];
        if (entry < 0)
            return false;
        cents = centsOfDegree(entry + pass * octaveDegree);
        return true;
    };
    double referenceCents = 0.0;
    if (!centsOfKey(map.reference, referenceCents))
        referenceCents = 0.0;
    // The reference key's pitch as a note number, with A = 440 at note 69.
    const double referenceNote = 69.0 + 12.0 * std::log2(map.referenceHz / 440.0);
    double last = referenceNote;
    bool any = false;
    for (int key = 0; key < 128; ++key) {
        double cents = 0.0;
        const bool inRange = key >= std::min(map.first, map.last) && key <= std::max(map.first, map.last);
        if (inRange && centsOfKey(key, cents)) {
            last = referenceNote + (cents - referenceCents) / 100.0;
            if (!any) // keys below the first mapped one take its pitch too
                for (int earlier = 0; earlier < key; ++earlier)
                    tuning.note[static_cast<std::size_t>(earlier)] = last;
            any = true;
        }
        tuning.note[static_cast<std::size_t>(key)] = std::clamp(last, -48.0, 200.0);
    }
    if (!any)
        return Tuning{};
    return tuning;
}

bool parseTun(std::string_view text, Tuning& out, std::string& error) {
    out = Tuning{};
    if (text.size() > kMaxTuningBytes) {
        error = "the file is too large to be a tuning";
        return false;
    }
    // Two sections can hold the table: [Tuning] in whole cents and [Exact Tuning] with fractions
    // and its own base frequency. The exact one wins when both are present.
    constexpr double kDefaultBase = 8.1757989156437; // MIDI note 0 when A is 440
    double plain[128], exact[128];
    bool havePlain[128] = {}, haveExact[128] = {};
    double baseHz = kDefaultBase;
    std::string section;
    int number = 0, found = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++number;
        if (line.empty() || line.front() == ';')
            continue;
        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            section = lower(line.substr(1, close == std::string_view::npos ? line.size() - 1 : close - 1));
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || (section != "tuning" && section != "exact tuning"))
            continue;
        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string_view value = trim(line.substr(equals + 1));
        double v = 0.0;
        if (key == "basefreq") {
            if (!toDouble(value, v) || v < 1.0 || v > 1000.0) {
                error = where(number) + "the base frequency must be a number of Hz";
                return false;
            }
            baseHz = v;
            continue;
        }
        if (key.rfind("note", 0) != 0)
            continue;
        int index = -1;
        if (!toInt(trim(std::string_view(key).substr(4)), index) || index < 0 || index > 127) {
            error = where(number) + "\"" + key + "\" does not name a key from note 0 to note 127";
            return false;
        }
        if (!toDouble(value, v) || std::abs(v) > 100000.0) {
            error = where(number) + "the pitch of note " + std::to_string(index) + " must be a number of cents";
            return false;
        }
        (section == "tuning" ? plain : exact)[index] = v;
        (section == "tuning" ? havePlain : haveExact)[index] = true;
        ++found;
    }
    if (found == 0) {
        error = "no [Tuning] or [Exact Tuning] section with note values was found";
        return false;
    }
    const double baseNote = 12.0 * std::log2(baseHz / kDefaultBase);
    for (int i = 0; i < 128; ++i) {
        if (haveExact[i])
            out.note[static_cast<std::size_t>(i)] = exact[i] / 100.0 + baseNote;
        else if (havePlain[i])
            out.note[static_cast<std::size_t>(i)] = plain[i] / 100.0;
    }
    return true;
}

bool loadTuningFiles(const std::string& path, const std::string& kbmPath, Tuning& out, std::string& error) {
    const auto readText = [&](const std::string& file, std::string& text) {
        std::vector<std::uint8_t> bytes;
        if (!readFile(file, bytes, error))
            return false;
        text.assign(bytes.begin(), bytes.end());
        return true;
    };
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    const bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string extension = hasExtension ? lower(path.substr(dot)) : std::string();
    const std::size_t stemStart = slash == std::string::npos ? 0 : slash + 1;
    const std::string stem = path.substr(stemStart, (hasExtension ? dot : path.size()) - stemStart);
    std::string text;
    if (!readText(path, text))
        return false;
    if (extension == ".tun") {
        if (!parseTun(text, out, error)) {
            error = path + ": " + error;
            return false;
        }
        out.name = stem;
        return true;
    }
    if (extension != ".scl") {
        error = path + ": tuning files end in .scl or .tun";
        return false;
    }
    ScaleDef scale;
    if (!parseScl(text, scale, error)) {
        error = path + ": " + error;
        return false;
    }
    KeyMap map;
    if (!kbmPath.empty()) {
        std::string mapText;
        if (!readText(kbmPath, mapText))
            return false;
        if (!parseKbm(mapText, map, error)) {
            error = kbmPath + ": " + error;
            return false;
        }
    }
    out = tuningFromScale(scale, map, stem);
    return true;
}

} // namespace zed
