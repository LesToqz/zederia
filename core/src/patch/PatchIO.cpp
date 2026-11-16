#include "zederia/patch/PatchIO.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>

namespace zed {
namespace {

using J = nlohmann::ordered_json;

constexpr std::size_t kMaxNameLength = 128;
constexpr std::size_t kMaxNotesLength = 8192;
constexpr std::size_t kMaxRefLength = 1024;
constexpr std::size_t kMaxTags = 64;
constexpr std::size_t kMaxTagLength = 64;

// --- Small helpers ----------------------------------------------------------------------------

std::string escapeKey(std::string_view key) { // JSON pointer escaping (RFC 6901)
    std::string out;
    for (const char c : key) {
        if (c == '~')
            out += "~0";
        else if (c == '/')
            out += "~1";
        else
            out += c;
    }
    return out;
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

std::string inQuotes(std::string_view s) {
    // Keys and tokens can come from an untrusted file: keep them short and printable in messages.
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size() && i < 48; ++i)
        out += (static_cast<unsigned char>(s[i]) < 0x20 || s[i] == '"') ? '?' : s[i];
    if (s.size() > 48)
        out += "...";
    return out + "\"";
}

const char* kindOf(const J& v) {
    if (v.is_null()) return "null";
    if (v.is_boolean()) return "true/false";
    if (v.is_number()) return "a number";
    if (v.is_string()) return "text";
    if (v.is_array()) return "a list";
    return "an object";
}

std::size_t editDistance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

// The candidate closest to `key`, or "" when nothing is close enough to be a likely typo.
template <typename Range>
std::string closest(std::string_view key, const Range& candidates) {
    if (key.size() > 64)
        return {};
    std::string best;
    std::size_t bestDistance = std::max<std::size_t>(2, key.size() / 3) + 1;
    for (const auto& c : candidates) {
        const std::size_t d = editDistance(key, c);
        if (d < bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best;
}

std::string didYouMean(const std::string& suggestion) {
    return suggestion.empty() ? std::string() : "Did you mean " + inQuotes(suggestion) + "? ";
}

std::string listOf(const std::vector<std::string>& items, std::size_t limit = 12) {
    std::string out;
    for (std::size_t i = 0; i < items.size() && i < limit; ++i)
        out += (i ? ", " : "") + items[i];
    if (items.size() > limit)
        out += ", ... (" + std::to_string(items.size()) + " in total, listed in docs/PARAMETERS.md)";
    return out;
}

std::string rangeOf(const ParamDef& d) {
    std::string out = num(d.min) + " to " + num(d.max);
    if (d.unit == Unit::Percent)
        out += " (a fraction: 0.5 means 50 %)";
    else if (!unitLabel(d.unit).empty())
        out += " " + std::string(unitLabel(d.unit));
    return out;
}

void truncateUtf8(std::string& s, std::size_t limit) {
    if (s.size() <= limit)
        return;
    std::size_t n = limit;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n; // do not cut a multi-byte character in half
    s.resize(n);
}

struct Reporter {
    std::vector<Issue>& issues;
    void error(std::string code, std::string path, std::string message, std::string fix) {
        issues.push_back({Severity::Error, std::move(code), std::move(path), std::move(message), std::move(fix)});
    }
    void warn(std::string code, std::string path, std::string message, std::string fix) {
        issues.push_back({Severity::Warning, std::move(code), std::move(path), std::move(message), std::move(fix)});
    }
    void unknownField(const std::string& path, std::string_view key, const std::string& suggestion) {
        warn("unknown_field", path, "Unknown field " + inQuotes(key) + "; it is ignored.",
             didYouMean(suggestion) + "Remove it or correct its name.");
    }
};

// --- Reading values ---------------------------------------------------------------------------

void readParam(const J& v, int index, Patch& patch, const std::string& path, Reporter& r) {
    const ParamDef& d = patch.effectiveDef(index);
    double value = 0.0;
    switch (d.type) {
    case ParamType::Bool:
        if (!v.is_boolean()) {
            r.error("wrong_type", path, d.name + " must be true or false, but this is " + kindOf(v) + ".",
                    "Use true or false.");
            return;
        }
        value = v.get<bool>() ? 1.0 : 0.0;
        break;
    case ParamType::Choice: {
        if (!v.is_string()) {
            r.error("wrong_type", path, d.name + " must be one of its named options, but this is " + kindOf(v) + ".",
                    "Use one of: " + listOf(d.choices) + ".");
            return;
        }
        const auto& token = v.get_ref<const std::string&>();
        const int i = d.choiceIndex(token);
        if (i < 0) {
            r.error("unknown_choice", path, inQuotes(token) + " is not an option of " + d.name + ".",
                    didYouMean(closest(token, d.choices)) + "Valid options: " + listOf(d.choices) + ".");
            return;
        }
        value = i;
        break;
    }
    case ParamType::Int:
    case ParamType::Float:
        if (!v.is_number()) {
            r.error("wrong_type", path, d.name + " must be a number, but this is " + kindOf(v) + ".",
                    "Use a number from " + rangeOf(d) + ".");
            return;
        }
        value = v.get<double>();
        if (!std::isfinite(value)) {
            r.error("wrong_type", path, d.name + " must be a finite number.", "Use a number from " + rangeOf(d) + ".");
            return;
        }
        break;
    }
    const double stored = patch.set(index, value);
    if (stored == value)
        return;
    if (d.type == ParamType::Int && value >= d.min && value <= d.max)
        r.warn("rounded", path, d.name + " takes whole numbers; " + num(value) + " was rounded to " + num(stored) + ".",
               "Use a whole number from " + rangeOf(d) + ".");
    else
        r.warn("out_of_range", path,
               num(value) + " is outside the range of " + d.name + "; " + num(stored) + " is used instead.",
               "Use a value from " + rangeOf(d) + ".");
}

bool readString(const J& v, const std::string& path, const char* what, std::size_t limit, std::string& out,
                Reporter& r) {
    if (!v.is_string()) {
        r.error("wrong_type", path, std::string(what) + " must be text, but this is " + kindOf(v) + ".",
                "Put the value in double quotes.");
        return false;
    }
    out = v.get_ref<const std::string&>();
    if (out.size() > limit) {
        truncateUtf8(out, limit);
        r.warn("too_long", path, std::string(what) + " is longer than " + std::to_string(limit) + " bytes and was cut.",
               "Shorten the text.");
    }
    return true;
}

bool readBool(const J& v, const std::string& path, const char* what, bool& out, Reporter& r) {
    if (!v.is_boolean()) {
        r.error("wrong_type", path, std::string(what) + " must be true or false, but this is " + kindOf(v) + ".",
                "Use true or false.");
        return false;
    }
    out = v.get<bool>();
    return true;
}

bool requireObject(const J& v, const std::string& path, const char* what, Reporter& r) {
    if (v.is_object())
        return true;
    r.error("wrong_type", path, std::string(what) + " must be an object with named fields, but this is " + kindOf(v) + ".",
            "Write it as { \"field\": value, ... }.");
    return false;
}

bool requireArray(const J& v, const std::string& path, const char* what, std::size_t limit, Reporter& r) {
    if (!v.is_array()) {
        r.error("wrong_type", path, std::string(what) + " must be a list, but this is " + kindOf(v) + ".",
                "Write it as [ ... ].");
        return false;
    }
    if (v.size() > limit)
        r.error("too_many_items", path,
                std::string(what) + " has " + std::to_string(v.size()) + " entries; the most allowed is " +
                    std::to_string(limit) + ".",
                "Remove entries until there are at most " + std::to_string(limit) + ".");
    return true;
}

void readStringList(const J& v, const std::string& path, const char* what, std::vector<std::string>& out, Reporter& r) {
    if (!requireArray(v, path, what, kMaxTags, r))
        return;
    out.clear();
    for (std::size_t i = 0; i < v.size() && i < kMaxTags; ++i) {
        std::string s;
        if (readString(v[i], path + "/" + std::to_string(i), "Each entry", kMaxTagLength, s, r))
            out.push_back(std::move(s));
    }
}

void readRef(const J& v, const std::string& path, const char* what, std::string& out, Reporter& r) {
    if (!requireObject(v, path, what, r))
        return;
    bool found = false;
    for (const auto& [key, child] : v.items()) {
        if (key == "ref")
            found = readString(child, path + "/ref", "A reference", kMaxRefLength, out, r) || found;
        else
            r.unknownField(path + "/" + escapeKey(key), key, "ref");
    }
    if (!found && v.find("ref") == v.end())
        r.error("missing_field", path, std::string(what) + " has no \"ref\".",
                "Add \"ref\" with the name of the item, for example { \"ref\": \"factory:Basics/Saw\" }.");
}

// A finite number inside [low, high]; values outside are clamped with a warning.
bool readNumber(const J& v, const std::string& path, const char* what, double low, double high, double& out, Reporter& r) {
    if (!v.is_number() || !std::isfinite(v.get<double>())) {
        r.error("wrong_type", path, std::string(what) + " must be a number, but this is " + kindOf(v) + ".",
                "Use a number from " + num(low) + " to " + num(high) + ".");
        return false;
    }
    const double raw = v.get<double>();
    out = std::clamp(raw, low, high);
    if (out != raw)
        r.warn("out_of_range", path, num(raw) + " is outside the range of " + what + "; " + num(out) + " is used instead.",
               "Use a value from " + num(low) + " to " + num(high) + ".");
    return true;
}

// A note length such as "1/8", "3/16" or "1/4T".
bool readNoteLength(const J& v, const std::string& path, std::string& out, Reporter& r) {
    std::string token;
    if (!readString(v, path, "A note length", kMaxTagLength, token, r))
        return false;
    if (!token.empty() && divisionInBeats(token) <= 0.0) {
        r.error("bad_note_length", path, inQuotes(token) + " is not a note length.",
                "Write a fraction of a whole note such as \"1/8\", \"3/16\" or \"1/4T\" (T for triplet, D for dotted), or "
                "leave the text empty to use the time in milliseconds.");
        return false;
    }
    out = std::move(token);
    return true;
}

// A drawn shape: { "points": [ { "x": 0, "y": 0, "curve": 0 }, ... ], "loop_start": 0 }.
void readShape(const J& v, const std::string& path, double yLow, double yHigh, Shape& out, Reporter& r) {
    if (!requireObject(v, path, "A shape", r))
        return;
    static const std::vector<std::string> known = {"points", "loop_start"};
    static const std::vector<std::string> pointKeys = {"x", "y", "curve"};
    Shape shape;
    for (const auto& [key, child] : v.items()) {
        const std::string childPath = path + "/" + escapeKey(key);
        if (key == "loop_start") {
            readNumber(child, childPath, "The loop start", 0.0, 1.0, shape.loopStart, r);
        } else if (key == "points") {
            if (!requireArray(child, childPath, "\"points\"", kMaxShapePoints, r))
                continue;
            double lastX = 0.0;
            for (std::size_t i = 0; i < child.size() && i < static_cast<std::size_t>(kMaxShapePoints); ++i) {
                const std::string pointPath = childPath + "/" + std::to_string(i);
                if (!requireObject(child[i], pointPath, "A point", r))
                    continue;
                ShapePoint p;
                for (const auto& [name, value] : child[i].items()) {
                    const std::string valuePath = pointPath + "/" + escapeKey(name);
                    if (name == "x") readNumber(value, valuePath, "A point's x", 0.0, 1.0, p.x, r);
                    else if (name == "y") readNumber(value, valuePath, "A point's y", yLow, yHigh, p.y, r);
                    else if (name == "curve") readNumber(value, valuePath, "A point's curve", -1.0, 1.0, p.curve, r);
                    else r.unknownField(valuePath, name, closest(name, pointKeys));
                }
                if (p.x < lastX) {
                    r.warn("out_of_order", pointPath + "/x",
                           "Points must run from left to right; this one was moved to x = " + num(lastX) + ".",
                           "List the points in order of x.");
                    p.x = lastX;
                }
                lastX = p.x;
                shape.points.push_back(p);
            }
        } else {
            r.unknownField(childPath, key, closest(key, known));
        }
    }
    out = std::move(shape);
}

// Envelope times that follow the tempo: { "attack": "1/8", "release": "1/4" }.
void readEnvSync(const J& v, const std::string& path, std::array<std::string, 5>& out, Reporter& r) {
    if (!requireObject(v, path, "\"sync\"", r))
        return;
    const std::vector<std::string> known(std::begin(kEnvSyncKeys), std::end(kEnvSyncKeys));
    for (const auto& [key, child] : v.items()) {
        const auto it = std::find(known.begin(), known.end(), key);
        const std::string childPath = path + "/" + escapeKey(key);
        if (it == known.end())
            r.unknownField(childPath, key, closest(key, known));
        else
            readNoteLength(child, childPath, out[static_cast<std::size_t>(it - known.begin())], r);
    }
}

// A multi-segment envelope: { "points": [ { "time_ms": 10, "level": 1, "curve": 0 }, ... ],
// "sustain_point": 2, "loop": [1, 3] }. A point may give "time" as a note length instead.
void readEnvShape(const J& v, const std::string& path, EnvShape& out, Reporter& r) {
    if (!requireObject(v, path, "\"multi\"", r))
        return;
    static const std::vector<std::string> known = {"points", "sustain_point", "loop"};
    static const std::vector<std::string> pointKeys = {"time_ms", "time", "level", "curve"};
    EnvShape shape;
    const J* loop = nullptr;
    const J* sustain = nullptr;
    for (const auto& [key, child] : v.items()) {
        const std::string childPath = path + "/" + escapeKey(key);
        if (key == "sustain_point") {
            sustain = &child;
        } else if (key == "loop") {
            loop = &child;
        } else if (key == "points") {
            if (!requireArray(child, childPath, "\"points\"", kMaxEnvPoints, r))
                continue;
            for (std::size_t i = 0; i < child.size() && i < static_cast<std::size_t>(kMaxEnvPoints); ++i) {
                const std::string pointPath = childPath + "/" + std::to_string(i);
                if (!requireObject(child[i], pointPath, "A point", r))
                    continue;
                EnvPoint p;
                for (const auto& [name, value] : child[i].items()) {
                    const std::string valuePath = pointPath + "/" + escapeKey(name);
                    if (name == "time_ms") readNumber(value, valuePath, "A segment time", 0.0, 30000.0, p.timeMs, r);
                    else if (name == "time") readNoteLength(value, valuePath, p.sync, r);
                    else if (name == "level") readNumber(value, valuePath, "A point's level", 0.0, 1.0, p.level, r);
                    else if (name == "curve") readNumber(value, valuePath, "A point's curve", -1.0, 1.0, p.curve, r);
                    else r.unknownField(valuePath, name, closest(name, pointKeys));
                }
                shape.points.push_back(std::move(p));
            }
        } else {
            r.unknownField(childPath, key, closest(key, known));
        }
    }
    const double last = static_cast<double>(shape.points.size()) - 1.0;
    const auto readIndex = [&](const J& value, const std::string& indexPath, const char* what, int& index) {
        double number = -1.0;
        if (!readNumber(value, indexPath, what, -1.0, std::max(last, -1.0), number, r))
            return;
        index = static_cast<int>(std::floor(number));
    };
    if (sustain != nullptr)
        readIndex(*sustain, path + "/sustain_point", "The sustain point", shape.sustainPoint);
    if (loop != nullptr) {
        if (!loop->is_array() || loop->size() != 2) {
            r.error("wrong_type", path + "/loop", "\"loop\" must be a list of two point numbers.",
                    "Write it as [first, last], for example [1, 3].");
        } else {
            readIndex((*loop)[0], path + "/loop/0", "The loop start", shape.loopStart);
            readIndex((*loop)[1], path + "/loop/1", "The loop end", shape.loopEnd);
            if (shape.loopStart < 0 || shape.loopEnd <= shape.loopStart) {
                if (shape.loopStart >= 0 || shape.loopEnd >= 0)
                    r.warn("bad_loop", path + "/loop", "The loop must end at a later point than it starts; it is ignored.",
                           "Give two different point numbers in rising order.");
                shape.loopStart = shape.loopEnd = -1;
            }
        }
    }
    out = std::move(shape);
}

// A whole number inside [low, high]; values outside are clamped and fractions rounded, with a warning.
bool readWhole(const J& v, const std::string& path, const char* what, int low, int high, int& out, Reporter& r) {
    double number = 0.0;
    if (!readNumber(v, path, what, low, high, number, r))
        return false;
    out = static_cast<int>(std::lround(number));
    if (static_cast<double>(out) != number)
        r.warn("rounded", path, std::string(what) + " takes whole numbers; " + num(number) + " was rounded to " +
                                    std::to_string(out) + ".",
               "Use a whole number from " + std::to_string(low) + " to " + std::to_string(high) + ".");
    return true;
}

// The arpeggiator's pattern slots: [ { "steps": [ { "state": "on", "velocity": 1, "transpose": 0,
// "ratchet": 1 }, ... ] }, ... ]. Steps that are not listed play at full velocity.
void readArpPatterns(const J& v, const std::string& path, std::array<ArpPattern, kNumArpPatterns>& out, Reporter& r) {
    if (!requireArray(v, path, "\"patterns\"", kNumArpPatterns, r))
        return;
    static const std::vector<std::string> patternKeys = {"steps"};
    static const std::vector<std::string> stepKeys = {"state", "velocity", "transpose", "ratchet"};
    static const std::vector<std::string> states = {"on", "tie", "rest"};
    std::array<ArpPattern, kNumArpPatterns> patterns;
    for (std::size_t i = 0; i < v.size() && i < static_cast<std::size_t>(kNumArpPatterns); ++i) {
        const std::string patternPath = path + "/" + std::to_string(i);
        if (!requireObject(v[i], patternPath, "A pattern", r))
            continue;
        for (const auto& [key, child] : v[i].items()) {
            const std::string stepsPath = patternPath + "/" + escapeKey(key);
            if (key != "steps") {
                r.unknownField(stepsPath, key, closest(key, patternKeys));
                continue;
            }
            if (!requireArray(child, stepsPath, "\"steps\"", kMaxArpSteps, r))
                continue;
            for (std::size_t n = 0; n < child.size() && n < static_cast<std::size_t>(kMaxArpSteps); ++n) {
                const std::string stepPath = stepsPath + "/" + std::to_string(n);
                ArpStep step;
                if (requireObject(child[n], stepPath, "A step", r)) {
                    for (const auto& [name, value] : child[n].items()) {
                        const std::string valuePath = stepPath + "/" + escapeKey(name);
                        if (name == "state") {
                            std::string token;
                            if (!readString(value, valuePath, "A step's state", kMaxTagLength, token, r))
                                continue;
                            const auto it = std::find(states.begin(), states.end(), token);
                            if (it == states.end())
                                r.error("unknown_choice", valuePath, inQuotes(token) + " is not a step state.",
                                        didYouMean(closest(token, states)) + "Use \"on\", \"tie\" or \"rest\".");
                            else
                                step.state = static_cast<StepState>(it - states.begin());
                        } else if (name == "velocity") {
                            readNumber(value, valuePath, "A step's velocity", 0.0, 1.0, step.velocity, r);
                        } else if (name == "transpose") {
                            readWhole(value, valuePath, "A step's transposition", -24, 24, step.transpose, r);
                        } else if (name == "ratchet") {
                            readWhole(value, valuePath, "A step's ratchet", 1, 4, step.ratchet, r);
                        } else {
                            r.unknownField(valuePath, name, closest(name, stepKeys));
                        }
                    }
                }
                patterns[i].steps.push_back(step);
            }
        }
    }
    out = std::move(patterns);
}

// Chord memory: semitones above the played key, for example [0, 4, 7].
void readChordNotes(const J& v, const std::string& path, std::vector<int>& out, Reporter& r) {
    if (!requireArray(v, path, "\"notes\"", kMaxChordNotes, r))
        return;
    std::vector<int> notes;
    for (std::size_t i = 0; i < v.size() && i < static_cast<std::size_t>(kMaxChordNotes); ++i) {
        int semis = 0;
        if (readWhole(v[i], path + "/" + std::to_string(i), "A chord note", -48, 48, semis, r))
            notes.push_back(semis);
    }
    out = std::move(notes);
}

// The clip slots: [ { "name": "...", "bars": 2, "beats_per_bar": 4, "tempo": 120, "key": "A minor",
// "root": 57, "notes": [ { "start": 0, "length": 0.5, "key": 57, "velocity": 0.8 }, ... ] }, ... ].
// Times are in quarter notes.
void readClips(const J& v, const std::string& path, std::array<Clip, kNumClips>& out, Reporter& r) {
    if (!requireArray(v, path, "\"clips\"", kNumClips, r))
        return;
    static const std::vector<std::string> clipKeys = {"name", "bars", "beats_per_bar", "tempo", "key", "root", "notes"};
    static const std::vector<std::string> noteKeys = {"start", "length", "key", "velocity"};
    std::array<Clip, kNumClips> clips;
    for (std::size_t i = 0; i < v.size() && i < static_cast<std::size_t>(kNumClips); ++i) {
        const std::string clipPath = path + "/" + std::to_string(i);
        if (!requireObject(v[i], clipPath, "A clip", r))
            continue;
        Clip& clip = clips[i];
        for (const auto& [key, child] : v[i].items()) {
            const std::string childPath = clipPath + "/" + escapeKey(key);
            if (key == "name") {
                readString(child, childPath, "A clip's name", kMaxNameLength, clip.name, r);
            } else if (key == "bars") {
                readNumber(child, childPath, "A clip's length in bars", 0.25, 256.0, clip.bars, r);
            } else if (key == "beats_per_bar") {
                readWhole(child, childPath, "A clip's beats per bar", 1, 16, clip.beatsPerBar, r);
            } else if (key == "tempo") {
                readNumber(child, childPath, "A clip's tempo", 20.0, 999.0, clip.tempo, r);
            } else if (key == "key") {
                readString(child, childPath, "A clip's key", kMaxTagLength, clip.key, r);
            } else if (key == "root") {
                readWhole(child, childPath, "A clip's root note", 0, 127, clip.root, r);
            } else if (key == "notes") {
                if (!requireArray(child, childPath, "\"notes\"", kMaxClipNotes, r))
                    continue;
                for (std::size_t n = 0; n < child.size() && n < static_cast<std::size_t>(kMaxClipNotes); ++n) {
                    const std::string notePath = childPath + "/" + std::to_string(n);
                    if (!requireObject(child[n], notePath, "A note", r))
                        continue;
                    ClipNote note;
                    for (const auto& [name, value] : child[n].items()) {
                        const std::string valuePath = notePath + "/" + escapeKey(name);
                        if (name == "start") readNumber(value, valuePath, "A note's start", 0.0, 4096.0, note.start, r);
                        else if (name == "length") readNumber(value, valuePath, "A note's length", 1.0 / 128.0, 1024.0, note.length, r);
                        else if (name == "key") readWhole(value, valuePath, "A note's key", 0, 127, note.key, r);
                        else if (name == "velocity") readNumber(value, valuePath, "A note's velocity", 0.0, 1.0, note.velocity, r);
                        else r.unknownField(valuePath, name, closest(name, noteKeys));
                    }
                    clip.notes.push_back(note);
                }
            } else {
                r.unknownField(childPath, key, closest(key, clipKeys));
            }
        }
    }
    out = std::move(clips);
}

// A microtuning: { "name": "...", "keys": [128 note numbers] }.
void readTuning(const J& v, PatchTuning& out, Reporter& r) {
    if (!requireObject(v, "/tuning", "\"tuning\"", r))
        return;
    static const std::vector<std::string> known = {"name", "keys"};
    PatchTuning tuning;
    for (const auto& [key, child] : v.items()) {
        const std::string path = "/tuning/" + escapeKey(key);
        if (key == "name") {
            readString(child, path, "The tuning's name", kMaxNameLength, tuning.name, r);
        } else if (key == "keys") {
            if (!child.is_array() || (child.size() != 128 && !child.empty())) {
                r.error("wrong_type", path, "\"keys\" must list the pitch of all 128 keys.",
                        "Give 128 note numbers (60 is middle C in equal temperament), or leave the list empty for "
                        "equal temperament.");
                continue;
            }
            for (std::size_t i = 0; i < child.size(); ++i) {
                double note = static_cast<double>(i);
                readNumber(child[i], path + "/" + std::to_string(i), "A key's pitch", -48.0, 200.0, note, r);
                tuning.keys.push_back(note);
            }
        } else {
            r.unknownField(path, key, closest(key, known));
        }
    }
    out = std::move(tuning);
}

// --- Schema tree for the plain modules ---------------------------------------------------------

// Things inside a module's object that are not parameters.
enum class Extra : std::uint8_t { None, LfoShape, EnvSync, EnvMulti, WarpCurve, ArpPatterns, ChordNotes, Clips };

struct Node {
    std::map<std::string, Node, std::less<>> children;
    int param = -1;
    int tableOsc = -1;
    bool sample = false;
    bool isArray = false;
    Extra extra = Extra::None;
    int extraA = 0, extraB = 0;
};

void markArrays(Node& node) {
    bool numeric = !node.children.empty();
    for (auto& [key, child] : node.children) {
        numeric = numeric && !key.empty() && std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; });
        markArrays(child);
    }
    node.isArray = numeric;
}

const Node& schema() {
    static const Node root = [] {
        Node tree;
        const auto& reg = Registry::get();
        for (const auto& module : reg.modules()) {
            if (module.kind != ModuleKind::Plain)
                continue;
            for (int i = module.firstParam; i < module.firstParam + module.numParams; ++i) {
                const std::string& pointer = reg.at(i).pointer;
                Node* node = &tree;
                std::size_t pos = 1;
                while (pos <= pointer.size()) {
                    const std::size_t next = std::min(pointer.find('/', pos), pointer.size());
                    node = &node->children[pointer.substr(pos, next - pos)];
                    pos = next + 1;
                }
                node->param = i;
            }
        }
        for (int i = 0; i < kNumOscs; ++i)
            tree.children["osc" + std::to_string(i + 1)].children["table"].tableOsc = i;
        tree.children["texture"].children["sample"].sample = true;
        markArrays(tree);
        // Added after the arrays are marked: these are objects whose contents have their own readers.
        const auto extra = [](Node& node, Extra kind, int a, int b = 0) {
            node.extra = kind;
            node.extraA = a;
            node.extraB = b;
        };
        for (int i = 0; i < kNumLfos; ++i)
            extra(tree.children["lfo" + std::to_string(i + 1)].children["shape"], Extra::LfoShape, i);
        for (int i = 0; i < kNumEnvs; ++i) {
            Node& env = tree.children["env" + std::to_string(i + 1)];
            extra(env.children["sync"], Extra::EnvSync, i);
            if (i > 0)
                extra(env.children["multi"], Extra::EnvMulti, i);
        }
        for (int i = 0; i < kNumOscs; ++i)
            for (int slot = 0; slot < 2; ++slot)
                extra(tree.children["osc" + std::to_string(i + 1)].children[slot == 0 ? "warp_a" : "warp_b"].children["curve"],
                      Extra::WarpCurve, i, slot);
        extra(tree.children["arp"].children["patterns"], Extra::ArpPatterns, 0);
        extra(tree.children["chord"].children["notes"], Extra::ChordNotes, 0);
        extra(tree.children["clip"].children["clips"], Extra::Clips, 0);
        return tree;
    }();
    return root;
}

std::vector<std::string> keysOf(const Node& node) {
    std::vector<std::string> keys;
    for (const auto& [key, child] : node.children)
        keys.push_back(key);
    return keys;
}

void walk(const J& v, const Node& node, const std::string& path, Patch& patch, Reporter& r) {
    if (node.param >= 0)
        return readParam(v, node.param, patch, path, r);
    if (node.tableOsc >= 0)
        return readRef(v, path, "A wavetable reference", patch.tables[static_cast<std::size_t>(node.tableOsc)], r);
    if (node.sample)
        return readRef(v, path, "A sample reference", patch.textureSample, r);
    switch (node.extra) {
    case Extra::LfoShape: return readShape(v, path, -1.0, 1.0, patch.lfoShapes[static_cast<std::size_t>(node.extraA)], r);
    case Extra::EnvSync: return readEnvSync(v, path, patch.envSync[static_cast<std::size_t>(node.extraA)], r);
    case Extra::EnvMulti: return readEnvShape(v, path, patch.envShapes[static_cast<std::size_t>(node.extraA)], r);
    case Extra::WarpCurve:
        return readShape(v, path, 0.0, 1.0,
                         patch.warpCurves[static_cast<std::size_t>(node.extraA)][static_cast<std::size_t>(node.extraB)], r);
    case Extra::ArpPatterns: return readArpPatterns(v, path, patch.arpPatterns, r);
    case Extra::ChordNotes: return readChordNotes(v, path, patch.chordNotes, r);
    case Extra::Clips: return readClips(v, path, patch.clips, r);
    case Extra::None: break;
    }
    if (node.isArray) {
        if (!requireArray(v, path, "This field", node.children.size(), r))
            return;
        for (std::size_t i = 0; i < v.size() && i < node.children.size(); ++i)
            walk(v[i], node.children.find(std::to_string(i))->second, path + "/" + std::to_string(i), patch, r);
        return;
    }
    if (!requireObject(v, path, "This section", r))
        return;
    for (const auto& [key, child] : v.items()) {
        const auto it = node.children.find(key);
        const std::string childPath = path + "/" + escapeKey(key);
        if (it == node.children.end())
            r.unknownField(childPath, key, closest(key, keysOf(node)));
        else
            walk(child, it->second, childPath, patch, r);
    }
}

// --- Sections with their own layout -------------------------------------------------------------

void readMeta(const J& v, PatchMeta& meta, Reporter& r) {
    if (!requireObject(v, "/meta", "\"meta\"", r))
        return;
    static const std::vector<std::string> known = {"name", "author", "pack", "type", "tags", "genres",
                                                   "rating", "notes", "created", "modified"};
    for (const auto& [key, child] : v.items()) {
        const std::string path = "/meta/" + escapeKey(key);
        if (key == "name") readString(child, path, "The name", kMaxNameLength, meta.name, r);
        else if (key == "author") readString(child, path, "The author", kMaxNameLength, meta.author, r);
        else if (key == "pack") readString(child, path, "The pack name", kMaxNameLength, meta.pack, r);
        else if (key == "type") readString(child, path, "The type", kMaxTagLength, meta.type, r);
        else if (key == "notes") readString(child, path, "The notes", kMaxNotesLength, meta.notes, r);
        else if (key == "created") readString(child, path, "The creation date", kMaxTagLength, meta.created, r);
        else if (key == "modified") readString(child, path, "The modification date", kMaxTagLength, meta.modified, r);
        else if (key == "tags") readStringList(child, path, "\"tags\"", meta.tags, r);
        else if (key == "genres") readStringList(child, path, "\"genres\"", meta.genres, r);
        else if (key == "rating") {
            if (!child.is_number() || !std::isfinite(child.get<double>())) {
                r.error("wrong_type", path, std::string("The rating must be a number, but this is ") + kindOf(child) + ".",
                        "Use a whole number from 0 to 5.");
            } else {
                const double raw = child.get<double>();
                meta.rating = static_cast<int>(std::clamp(std::round(raw), 0.0, 5.0));
                if (meta.rating != raw)
                    r.warn("out_of_range", path, num(raw) + " is not a valid rating; " + std::to_string(meta.rating) +
                                                     " is used instead.", "Use a whole number from 0 to 5.");
            }
        } else
            r.unknownField(path, key, closest(key, known));
    }
}

void readRoutes(const J& v, Patch& patch, Reporter& r) {
    const auto& reg = Registry::get();
    if (!requireArray(v, "/mod", "\"mod\"", kNumModRoutes, r))
        return;
    static const std::vector<std::string> known = {"source", "via",   "via_invert",  "dest",    "amount", "polarity",
                                                   "curve",  "curve_shape", "enabled", "mute",   "solo"};
    static const std::vector<std::string> polarities = {"bi", "uni"};
    static const std::vector<std::string> curves = {"linear", "exp", "log", "s", "drawn"};
    const auto sourceNames = [&] {
        std::vector<std::string> names;
        for (const auto& s : reg.modSources())
            names.push_back(s.id);
        return names;
    };
    const auto readSource = [&](const J& value, const std::string& path, std::string& out) {
        std::string id;
        if (!readString(value, path, "A modulation source", kMaxTagLength, id, r))
            return;
        if (!reg.isModSource(id)) {
            r.error("unknown_source", path, inQuotes(id) + " is not a modulation source.",
                    didYouMean(closest(id, sourceNames())) +
                        "Use a source such as env1..env4, lfo1..lfo8, flux1, macro1..macro8, velocity, modwheel "
                        "or cc0..cc127 (all are listed in docs/PARAMETERS.md).");
            return;
        }
        out = std::move(id);
    };
    const auto readToken = [&](const J& value, const std::string& path, const char* what,
                               const std::vector<std::string>& options, int& out) {
        std::string token;
        if (!readString(value, path, what, kMaxTagLength, token, r))
            return;
        const auto it = std::find(options.begin(), options.end(), token);
        if (it == options.end())
            r.error("unknown_choice", path, inQuotes(token) + " is not valid here.",
                    didYouMean(closest(token, options)) + "Valid options: " + listOf(options) + ".");
        else
            out = static_cast<int>(it - options.begin());
    };

    for (std::size_t i = 0; i < v.size() && i < static_cast<std::size_t>(kNumModRoutes); ++i) {
        const std::string base = "/mod/" + std::to_string(i);
        ModRoute route;
        patch.routes.push_back(route); // keeps route numbers aligned with the document even on errors
        if (!requireObject(v[i], base, "A route", r))
            continue;
        for (const auto& [key, child] : v[i].items()) {
            const std::string path = base + "/" + escapeKey(key);
            if (key == "source") readSource(child, path, route.source);
            else if (key == "via") readSource(child, path, route.via);
            else if (key == "via_invert") readBool(child, path, "\"via_invert\"", route.viaInvert, r);
            else if (key == "enabled") readBool(child, path, "\"enabled\"", route.enabled, r);
            else if (key == "mute") readBool(child, path, "\"mute\"", route.mute, r);
            else if (key == "solo") readBool(child, path, "\"solo\"", route.solo, r);
            else if (key == "amount") readParam(child, reg.modAmount(static_cast<int>(i)), patch, path, r);
            else if (key == "polarity") {
                int p = static_cast<int>(route.polarity);
                readToken(child, path, "The polarity", polarities, p);
                route.polarity = static_cast<Polarity>(p);
            } else if (key == "curve") {
                int c = static_cast<int>(route.curve);
                readToken(child, path, "The curve", curves, c);
                route.curve = static_cast<ModCurve>(c);
            } else if (key == "curve_shape") {
                readShape(child, path, 0.0, 1.0, route.curveShape, r);
            } else if (key == "dest") {
                std::string id;
                if (!readString(child, path, "A destination", kMaxNameLength, id, r))
                    continue;
                const ParamDef* d = reg.find(id);
                if (d == nullptr) {
                    std::vector<std::string_view> ids;
                    for (const auto& p : reg.params())
                        if (p.modulatable)
                            ids.push_back(p.id);
                    r.error("unknown_dest", path, inQuotes(id) + " is not a parameter.",
                            didYouMean(closest(id, ids)) +
                                "Use the id of a continuous parameter, such as \"filter1.cutoff\" (ids are listed in "
                                "docs/PARAMETERS.md).");
                } else if (!d->modulatable) {
                    r.error("not_modulatable", path, d->name + " (" + id + ") cannot be modulated.",
                            "Choose a continuous parameter; switches and menus are not destinations.");
                } else
                    route.dest = std::move(id);
            } else
                r.unknownField(path, key, closest(key, known));
        }
        if (v[i].find("source") == v[i].end())
            r.error("missing_field", base, "This route has no \"source\".", "Add \"source\", for example \"lfo1\".");
        if (v[i].find("dest") == v[i].end())
            r.error("missing_field", base, "This route has no \"dest\".", "Add \"dest\", for example \"filter1.cutoff\".");
        patch.routes.back() = std::move(route);
    }
}

void readMacros(const J& v, Patch& patch, Reporter& r) {
    const auto& reg = Registry::get();
    if (!requireArray(v, "/macros", "\"macros\"", kNumMacros, r))
        return;
    static const std::vector<std::string> known = {"name", "value", "bipolar"};
    for (std::size_t i = 0; i < v.size() && i < static_cast<std::size_t>(kNumMacros); ++i) {
        const std::string base = "/macros/" + std::to_string(i);
        if (!requireObject(v[i], base, "A macro", r))
            continue;
        for (const auto& [key, child] : v[i].items()) {
            const std::string path = base + "/" + escapeKey(key);
            if (key == "name") readString(child, path, "A macro name", kMaxTagLength, patch.macroNames[i], r);
            else if (key == "value") readParam(child, reg.macroValue(static_cast<int>(i)), patch, path, r);
            else if (key == "bipolar") readParam(child, reg.macroBipolar(static_cast<int>(i)), patch, path, r);
            else r.unknownField(path, key, closest(key, known));
        }
    }
}

void readFxLane(const J& v, int lane, Patch& patch, Reporter& r) {
    const auto& reg = Registry::get();
    const std::string lanePath = "/fx/" + std::string(fxLaneKey(lane));
    if (!requireArray(v, lanePath, "An effects lane", kNumFxSlots, r))
        return;
    std::vector<std::string> effectNames;
    for (const auto& e : reg.effectTypes())
        effectNames.push_back(e.id);
    for (std::size_t i = 0; i < v.size() && i < static_cast<std::size_t>(kNumFxSlots); ++i) {
        const std::string base = lanePath + "/" + std::to_string(i);
        if (!requireObject(v[i], base, "An effect slot", r))
            continue;
        const auto typeIt = v[i].find("type");
        if (typeIt == v[i].end()) {
            r.error("missing_field", base, "This effect slot has no \"type\".",
                    "Add \"type\" with one of: " + listOf(effectNames, 20) + ".");
            continue;
        }
        std::string typeName;
        if (!readString(*typeIt, base + "/type", "An effect type", kMaxTagLength, typeName, r))
            continue;
        const int type = reg.effectIndex(typeName);
        if (type < 0) {
            r.error("unknown_choice", base + "/type", inQuotes(typeName) + " is not an effect.",
                    didYouMean(closest(typeName, effectNames)) + "Valid effects: " + listOf(effectNames, 20) + ".");
            continue;
        }
        const int slot = patch.addFxSlot(lane, type);
        if (slot < 0)
            continue;
        const auto& params = reg.effectTypes()[static_cast<std::size_t>(type)].params;
        for (const auto& [key, child] : v[i].items()) {
            const std::string path = base + "/" + escapeKey(key);
            if (key == "type")
                continue;
            if (key == "on") {
                readParam(child, reg.fxSlotOn(lane, slot), patch, path, r);
                continue;
            }
            if (key == "mix") {
                readParam(child, reg.fxSlotMix(lane, slot), patch, path, r);
                continue;
            }
            if (key == "curve_a" || key == "curve_b") { // Drive's drawn transfer curves
                readShape(child, path, 0.0, 1.0,
                          patch.fxCurves[static_cast<std::size_t>(lane)][static_cast<std::size_t>(slot)][key == "curve_a" ? 0u : 1u], r);
                continue;
            }
            const auto it = std::find_if(params.begin(), params.end(), [&](const ParamDef& p) { return p.pointer == key; });
            if (it == params.end()) {
                std::vector<std::string> keys = {"type", "on", "mix", "curve_a", "curve_b"};
                for (const auto& p : params)
                    keys.push_back(p.pointer);
                r.unknownField(path, key, closest(key, keys));
            } else
                readParam(child, reg.fxSlotParam(lane, slot, it->index), patch, path, r);
        }
    }
}

void readFx(const J& v, Patch& patch, Reporter& r) {
    const auto& reg = Registry::get();
    if (!requireObject(v, "/fx", "\"fx\"", r))
        return;
    const auto rack = std::find_if(reg.modules().begin(), reg.modules().end(),
                                   [](const ModuleInfo& m) { return m.kind == ModuleKind::FxRack; });
    for (const auto& [key, child] : v.items()) {
        bool handled = false;
        for (int lane = 0; lane < kNumFxLanes && !handled; ++lane) {
            if (key == fxLaneKey(lane)) {
                readFxLane(child, lane, patch, r);
                handled = true;
            }
        }
        std::vector<std::string> keys = {"a", "b", "master"};
        for (int i = rack->firstParam; i < rack->firstParam + rack->numParams && !handled; ++i) {
            const std::string name = reg.at(i).pointer.substr(4); // after "/fx/"
            keys.push_back(name);
            if (key == name) {
                readParam(child, i, patch, "/fx/" + key, r);
                handled = true;
            }
        }
        if (!handled)
            r.unknownField("/fx/" + escapeKey(key), key, closest(key, keys));
    }
}

// Migration steps for released format versions. Version 1 is the first, so there are none yet.
std::span<const MigrationStep> builtinMigrations() { return {}; }

void loadDocument(J& doc, LoadResult& out, Reporter& r) {
    if (!doc.is_object()) {
        r.error("wrong_type", "", std::string("A patch must be a JSON object, but this is ") + kindOf(doc) + ".",
                "Wrap the patch in { } with \"format\": \"zederia.patch\" and \"version\": 1.");
        return;
    }
    const auto format = doc.find("format");
    if (format == doc.end()) {
        r.error("missing_field", "/format", "The document does not say that it is a Zederia patch.",
                "Add \"format\": \"zederia.patch\".");
        return;
    }
    if (!format->is_string() || format->get_ref<const std::string&>() != kPatchFormat) {
        r.error("bad_format", "/format", "This is not a Zederia patch.",
                "Set \"format\" to \"zederia.patch\", or open a file that is one.");
        return;
    }
    if (!migratePatch(doc, builtinMigrations(), kPatchVersion, out.issues))
        return;

    std::vector<std::string> topKeys = keysOf(schema());
    for (const char* k : {"format", "version", "meta", "mod", "fx", "macros", "tuning"})
        topKeys.emplace_back(k);
    for (const auto& [key, child] : doc.items()) {
        if (key == "format" || key == "version") continue;
        if (key == "meta") readMeta(child, out.patch.meta, r);
        else if (key == "tuning") readTuning(child, out.patch.tuning, r);
        else if (key == "mod") readRoutes(child, out.patch, r);
        else if (key == "fx") readFx(child, out.patch, r);
        else if (key == "macros") readMacros(child, out.patch, r);
        else {
            const auto it = schema().children.find(key);
            const std::string path = "/" + escapeKey(key);
            if (it == schema().children.end())
                r.unknownField(path, key, closest(key, topKeys));
            else
                walk(child, it->second, path, out.patch, r);
        }
    }
}

// --- Writing ------------------------------------------------------------------------------------

J valueToJson(const ParamDef& d, double value) {
    switch (d.type) {
    case ParamType::Bool: return value >= 0.5;
    case ParamType::Choice: return d.choices[static_cast<std::size_t>(value)];
    case ParamType::Int: return static_cast<std::int64_t>(value);
    case ParamType::Float: break;
    }
    return value;
}

J shapeToJson(const Shape& shape, bool withLoop) {
    J o = J::object();
    J points = J::array();
    for (const ShapePoint& p : shape.points)
        points.push_back({{"x", p.x}, {"y", p.y}, {"curve", p.curve}});
    o["points"] = std::move(points);
    if (withLoop && shape.loopStart != 0.0)
        o["loop_start"] = shape.loopStart;
    return o;
}

// One rack slot: its type, switch, mix, the effect's parameters by name and any drawn curves.
J slotToJson(const Patch& patch, int lane, int slot, bool full) {
    const auto& reg = Registry::get();
    const auto& effect = reg.effectTypes()[static_cast<std::size_t>(patch.fxSlotType(lane, slot))];
    J o = J::object();
    o["type"] = effect.id;
    const int on = reg.fxSlotOn(lane, slot), mix = reg.fxSlotMix(lane, slot);
    if (full || patch.get(on) != reg.at(on).def) o["on"] = patch.get(on) >= 0.5;
    if (full || patch.get(mix) != reg.at(mix).def) o["mix"] = patch.get(mix);
    for (const auto& p : effect.params) {
        const double value = patch.get(reg.fxSlotParam(lane, slot, p.index));
        if (full || value != p.def)
            o[p.pointer] = valueToJson(p, value);
    }
    const auto& curves = patch.fxCurves[static_cast<std::size_t>(lane)][static_cast<std::size_t>(slot)];
    if (!curves[0].empty())
        o["curve_a"] = shapeToJson(curves[0], false);
    if (!curves[1].empty())
        o["curve_b"] = shapeToJson(curves[1], false);
    return o;
}

// The "fx" section: the rack's routing values and the three lanes.
J fxToJson(const Patch& patch, bool full) {
    const auto& reg = Registry::get();
    J fx = J::object();
    for (const auto& module : reg.modules())
        if (module.kind == ModuleKind::FxRack)
            for (int i = module.firstParam; i < module.firstParam + module.numParams; ++i)
                if (full || patch.get(i) != reg.at(i).def)
                    fx[reg.at(i).pointer.substr(4)] = valueToJson(reg.at(i), patch.get(i));
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        if (!full && patch.fxSlotCount(lane) == 0)
            continue;
        J slots = J::array();
        for (int slot = 0; slot < patch.fxSlotCount(lane); ++slot)
            slots.push_back(slotToJson(patch, lane, slot, full));
        fx[std::string(fxLaneKey(lane))] = std::move(slots);
    }
    return fx;
}

// Opens a rack or effect preset document: parses it and checks its format and version.
bool openPreset(std::string_view text, std::string_view format, J& doc, Reporter& r) {
    if (text.size() > kMaxPatchBytes) {
        r.error("too_large", "", "The document is larger than a preset can be.", "Check that this is a preset file.");
        return false;
    }
    try {
        doc = J::parse(text);
    } catch (const J::exception&) {
        r.error("bad_json", "", "The text is not valid JSON.", "Look for a missing comma, quote or bracket.");
        return false;
    }
    if (!doc.is_object() || doc.find("format") == doc.end() || !doc["format"].is_string() ||
        doc["format"].get_ref<const std::string&>() != format) {
        r.error("bad_format", "/format", "This is not a " + std::string(format) + " document.",
                "Set \"format\" to \"" + std::string(format) + "\", or open a file that is one.");
        return false;
    }
    const auto version = doc.find("version");
    if (version == doc.end() || !version->is_number_integer() || version->get<int>() != 1) {
        r.error("bad_version", "/version", "This preset's version is missing or not one this build reads.",
                "Set \"version\" to 1.");
        return false;
    }
    return true;
}

bool endsWithIndex(const std::string& pointer) {
    const auto slash = pointer.rfind('/');
    return slash != std::string::npos && slash + 1 < pointer.size() &&
           std::all_of(pointer.begin() + static_cast<std::ptrdiff_t>(slash) + 1, pointer.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

bool LoadResult::ok() const { return errors() == 0; }

int LoadResult::errors() const {
    return static_cast<int>(
        std::count_if(issues.begin(), issues.end(), [](const Issue& i) { return i.severity == Severity::Error; }));
}

bool migratePatch(nlohmann::ordered_json& doc, std::span<const MigrationStep> steps, int targetVersion,
                  std::vector<Issue>& issues) {
    Reporter r{issues};
    if (!doc.is_object()) {
        r.error("wrong_type", "", "A patch must be a JSON object.", "Wrap the patch in { }.");
        return false;
    }
    const auto it = doc.find("version");
    if (it == doc.end()) {
        r.error("missing_field", "/version", "The patch has no version number.",
                "Add \"version\": " + std::to_string(targetVersion) + ".");
        return false;
    }
    if (!it->is_number_integer() || it->get<double>() < 1 || it->get<double>() > 1000000) {
        r.error("bad_version", "/version", "The version must be a whole number of 1 or more.",
                "Set \"version\" to " + std::to_string(targetVersion) + ".");
        return false;
    }
    int version = static_cast<int>(it->get<double>());
    if (version > targetVersion) {
        r.error("version_too_new", "/version",
                "This patch is format version " + std::to_string(version) + ", but this build of Zederia reads up to version " +
                    std::to_string(targetVersion) + ".",
                "Open it with a newer Zederia.");
        return false;
    }
    while (version < targetVersion) {
        const auto step = std::find_if(steps.begin(), steps.end(), [&](const MigrationStep& s) { return s.from == version; });
        if (step == steps.end() || !step->apply) {
            r.error("migration_missing", "/version",
                    "No migration is available from format version " + std::to_string(version) + ".",
                    "Re-save the patch from the Zederia version that wrote it.");
            return false;
        }
        step->apply(doc);
        ++version;
        doc["version"] = version;
    }
    return true;
}

LoadResult loadPatch(std::string_view text) {
    LoadResult out;
    Reporter r{out.issues};
    try {
        if (text.size() > kMaxPatchBytes) {
            r.error("too_large", "", "The document is larger than a patch can be.", "Check that this is a patch file.");
            return out;
        }
        J doc;
        try {
            doc = J::parse(text);
        } catch (const J::parse_error& e) {
            r.error("bad_json", "", "The text is not valid JSON (problem near byte " + std::to_string(e.byte) + ").",
                    "Look for a missing comma, quote or bracket near that position.");
            return out;
        } catch (const J::exception&) {
            r.error("bad_json", "", "The text is not valid JSON (a number is too large or the nesting is broken).",
                    "Check the numbers and brackets.");
            return out;
        }
        loadDocument(doc, out, r);
    } catch (const std::exception&) {
        // Nothing above is expected to throw; this keeps the promise that a load never crashes.
        r.error("internal", "", "The patch could not be read.", "Check that the file is a complete Zederia patch.");
    }
    return out;
}

std::string savePatch(const Patch& patch, const SaveOptions& options) {
    const auto& reg = Registry::get();
    const bool full = !options.compact;
    J doc = J::object();
    doc["format"] = kPatchFormat;
    doc["version"] = kPatchVersion;

    const PatchMeta defaults;
    J meta = J::object();
    const auto putText = [&](const char* key, const std::string& value, const std::string& def, bool always) {
        if ((full && always) || value != def)
            meta[key] = value;
    };
    putText("name", patch.meta.name, defaults.name, true);
    putText("author", patch.meta.author, defaults.author, true);
    putText("pack", patch.meta.pack, defaults.pack, false);
    putText("type", patch.meta.type, defaults.type, true);
    if (full || !patch.meta.tags.empty()) meta["tags"] = patch.meta.tags;
    if (!patch.meta.genres.empty()) meta["genres"] = patch.meta.genres;
    if (patch.meta.rating != 0) meta["rating"] = patch.meta.rating;
    putText("notes", patch.meta.notes, defaults.notes, false);
    putText("created", patch.meta.created, defaults.created, false);
    putText("modified", patch.meta.modified, defaults.modified, false);
    if (!meta.empty())
        doc["meta"] = std::move(meta);

    const auto putParam = [&](J& target, int index) {
        const ParamDef& d = reg.at(index);
        if (full || patch.get(index) != d.def || endsWithIndex(d.pointer))
            target[J::json_pointer(d.pointer)] = valueToJson(d, patch.get(index));
    };
    for (const auto& module : reg.modules()) {
        if (module.kind != ModuleKind::Plain)
            continue;
        for (int i = module.firstParam; i < module.firstParam + module.numParams; ++i) {
            putParam(doc, i);
            const std::string& id = reg.at(i).id;
            for (int osc = 0; osc < kNumOscs; ++osc) {
                const auto& ref = patch.tables[static_cast<std::size_t>(osc)];
                if (!ref.empty() && id == "osc" + std::to_string(osc + 1) + ".engine")
                    doc["osc" + std::to_string(osc + 1)]["table"]["ref"] = ref;
            }
            if (!patch.textureSample.empty() && id == "texture.mode")
                doc["texture"]["sample"]["ref"] = patch.textureSample;
        }
    }
    // Structure that lives inside the modules' objects: written only when there is something to say.
    for (int i = 0; i < kNumLfos; ++i)
        if (!patch.lfoShapes[static_cast<std::size_t>(i)].empty())
            doc["lfo" + std::to_string(i + 1)]["shape"] = shapeToJson(patch.lfoShapes[static_cast<std::size_t>(i)], true);
    for (int i = 0; i < kNumEnvs; ++i) {
        const std::string env = "env" + std::to_string(i + 1);
        J sync = J::object();
        for (std::size_t k = 0; k < 5; ++k)
            if (!patch.envSync[static_cast<std::size_t>(i)][k].empty())
                sync[kEnvSyncKeys[k]] = patch.envSync[static_cast<std::size_t>(i)][k];
        if (!sync.empty())
            doc[env]["sync"] = std::move(sync);
        const EnvShape& shape = patch.envShapes[static_cast<std::size_t>(i)];
        if (i > 0 && !shape.points.empty()) {
            J multi = J::object();
            J points = J::array();
            for (const EnvPoint& p : shape.points) {
                J o = J::object();
                if (p.sync.empty())
                    o["time_ms"] = p.timeMs;
                else
                    o["time"] = p.sync;
                o["level"] = p.level;
                if (full || p.curve != 0.0) o["curve"] = p.curve;
                points.push_back(std::move(o));
            }
            multi["points"] = std::move(points);
            if (shape.sustainPoint >= 0) multi["sustain_point"] = shape.sustainPoint;
            if (shape.loopStart >= 0 && shape.loopEnd > shape.loopStart) multi["loop"] = {shape.loopStart, shape.loopEnd};
            doc[env]["multi"] = std::move(multi);
        }
    }
    for (int i = 0; i < kNumOscs; ++i)
        for (std::size_t slot = 0; slot < 2; ++slot)
            if (!patch.warpCurves[static_cast<std::size_t>(i)][slot].empty())
                doc["osc" + std::to_string(i + 1)][slot == 0 ? "warp_a" : "warp_b"]["curve"] =
                    shapeToJson(patch.warpCurves[static_cast<std::size_t>(i)][slot], false);

    // Play data: patterns, chord memory and clips.
    const bool anyPattern = std::any_of(patch.arpPatterns.begin(), patch.arpPatterns.end(),
                                        [](const ArpPattern& p) { return !p.empty(); });
    if (anyPattern) {
        static const char* states[] = {"on", "tie", "rest"};
        J patterns = J::array();
        for (const ArpPattern& pattern : patch.arpPatterns) {
            J steps = J::array();
            for (const ArpStep& s : pattern.steps) {
                J o = J::object();
                o["state"] = states[static_cast<int>(s.state)];
                if (full || s.velocity != 1.0) o["velocity"] = s.velocity;
                if (full || s.transpose != 0) o["transpose"] = s.transpose;
                if (full || s.ratchet != 1) o["ratchet"] = s.ratchet;
                steps.push_back(std::move(o));
            }
            patterns.push_back({{"steps", std::move(steps)}});
        }
        doc["arp"]["patterns"] = std::move(patterns);
    }
    if (full || patch.chordNotes != Patch().chordNotes)
        doc["chord"]["notes"] = patch.chordNotes;
    const bool anyClip = std::any_of(patch.clips.begin(), patch.clips.end(), [](const Clip& c) { return !(c == Clip{}); });
    if (anyClip) {
        const Clip clipDefaults;
        J clips = J::array();
        for (const Clip& clip : patch.clips) {
            J o = J::object();
            if (!(clip == clipDefaults)) {
                if (full || !clip.name.empty()) o["name"] = clip.name;
                o["bars"] = clip.bars;
                if (full || clip.beatsPerBar != clipDefaults.beatsPerBar) o["beats_per_bar"] = clip.beatsPerBar;
                o["tempo"] = clip.tempo;
                if (full || !clip.key.empty()) o["key"] = clip.key;
                o["root"] = clip.root;
                J notes = J::array();
                for (const ClipNote& n : clip.notes)
                    notes.push_back({{"start", n.start}, {"length", n.length}, {"key", n.key}, {"velocity", n.velocity}});
                o["notes"] = std::move(notes);
            }
            clips.push_back(std::move(o));
        }
        doc["clip"]["clips"] = std::move(clips);
    }

    const std::size_t routeCount = std::min<std::size_t>(patch.routes.size(), kNumModRoutes);
    if (full || routeCount > 0) {
        static const char* polarities[] = {"bi", "uni"};
        static const char* curves[] = {"linear", "exp", "log", "s", "drawn"};
        J routes = J::array();
        for (std::size_t i = 0; i < routeCount; ++i) {
            const ModRoute& route = patch.routes[i];
            J o = J::object();
            o["source"] = route.source;
            if (!route.via.empty()) {
                o["via"] = route.via;
                if (full || route.viaInvert) o["via_invert"] = route.viaInvert;
            }
            o["dest"] = route.dest;
            o["amount"] = patch.get(reg.modAmount(static_cast<int>(i)));
            if (full || route.polarity != Polarity::Bi) o["polarity"] = polarities[static_cast<int>(route.polarity)];
            if (full || route.curve != ModCurve::Linear) o["curve"] = curves[static_cast<int>(route.curve)];
            if (!route.curveShape.empty()) o["curve_shape"] = shapeToJson(route.curveShape, false);
            if (full || !route.enabled) o["enabled"] = route.enabled;
            if (full || route.mute) o["mute"] = route.mute;
            if (full || route.solo) o["solo"] = route.solo;
            routes.push_back(std::move(o));
        }
        doc["mod"] = std::move(routes);
    }

    J fx = fxToJson(patch, full);
    if (!fx.empty())
        doc["fx"] = std::move(fx);

    int macroCount = 0;
    for (int i = 0; i < kNumMacros; ++i) {
        const bool used = !patch.macroNames[static_cast<std::size_t>(i)].empty() ||
                          patch.get(reg.macroValue(i)) != reg.at(reg.macroValue(i)).def ||
                          patch.get(reg.macroBipolar(i)) != reg.at(reg.macroBipolar(i)).def;
        if (used)
            macroCount = i + 1;
    }
    if (full || macroCount > 0) {
        J macros = J::array();
        for (int i = 0; i < macroCount; ++i) {
            J o = J::object();
            o["name"] = patch.macroNames[static_cast<std::size_t>(i)];
            o["value"] = patch.get(reg.macroValue(i));
            if (full || patch.get(reg.macroBipolar(i)) >= 0.5) o["bipolar"] = patch.get(reg.macroBipolar(i)) >= 0.5;
            macros.push_back(std::move(o));
        }
        doc["macros"] = std::move(macros);
    }

    if (!patch.tuning.empty() || !patch.tuning.name.empty()) {
        J tuning = J::object();
        tuning["name"] = patch.tuning.name;
        tuning["keys"] = patch.tuning.keys;
        doc["tuning"] = std::move(tuning);
    }

    return doc.dump(2, ' ', false, J::error_handler_t::replace) + "\n";
}

// --- Rack and effect presets ---------------------------------------------------------------------

std::string saveRackPreset(const Patch& patch) {
    J doc = J::object();
    doc["format"] = kRackFormat;
    doc["version"] = 1;
    doc["fx"] = fxToJson(patch, true);
    return doc.dump(2, ' ', false, J::error_handler_t::replace) + "\n";
}

bool loadRackPreset(std::string_view text, Patch& into, std::vector<Issue>& issues) {
    Reporter r{issues};
    const std::size_t before = issues.size();
    try {
        J doc;
        if (!openPreset(text, kRackFormat, doc, r))
            return false;
        Patch rack;
        for (const auto& [key, child] : doc.items()) {
            if (key == "format" || key == "version")
                continue;
            if (key == "fx")
                readFx(child, rack, r);
            else
                r.unknownField("/" + escapeKey(key), key, "fx");
        }
        for (std::size_t i = before; i < issues.size(); ++i)
            if (issues[i].severity == Severity::Error)
                return false;
        into.copyRackFrom(rack);
        return true;
    } catch (const std::exception&) {
        r.error("internal", "", "The rack preset could not be read.", "Check that the file is a complete rack preset.");
        return false;
    }
}

std::string saveSlotPreset(const Patch& patch, int lane, int slot) {
    J doc = J::object();
    doc["format"] = kSlotFormat;
    doc["version"] = 1;
    if (lane >= 0 && lane < kNumFxLanes && slot >= 0 && slot < patch.fxSlotCount(lane))
        doc["slot"] = slotToJson(patch, lane, slot, true);
    return doc.dump(2, ' ', false, J::error_handler_t::replace) + "\n";
}

bool loadSlotPreset(std::string_view text, Patch& into, int lane, int position, FxMoves* moves,
                    std::vector<Issue>& issues) {
    Reporter r{issues};
    const std::size_t before = issues.size();
    if (lane < 0 || lane >= kNumFxLanes || position < 0 || position > into.fxSlotCount(lane) ||
        (position == into.fxSlotCount(lane) && position >= kNumFxSlots)) {
        r.error("no_slot", "", "There is no such slot to load the effect into.", "Choose a slot of a lane that is not full.");
        return false;
    }
    try {
        J doc;
        if (!openPreset(text, kSlotFormat, doc, r))
            return false;
        const auto slot = doc.find("slot");
        if (slot == doc.end()) {
            r.error("missing_field", "/slot", "The preset has no \"slot\".", "Add \"slot\" with the effect's \"type\" and values.");
            return false;
        }
        Patch single;
        J lanes = J::array();
        lanes.push_back(*slot);
        readFxLane(lanes, 0, single, r);
        for (std::size_t i = before; i < issues.size(); ++i) { // paths as they are in this document
            std::string& path = issues[i].path;
            if (path.rfind("/fx/a/0", 0) == 0)
                path = "/slot" + path.substr(7);
        }
        for (std::size_t i = before; i < issues.size(); ++i)
            if (issues[i].severity == Severity::Error)
                return false;
        if (single.fxSlotCount(0) != 1)
            return false;
        // Replace what is there (its routes go with it), then insert the new effect at its place.
        FxMoves removal, insertion;
        const bool replacing = position < into.fxSlotCount(lane);
        if (replacing)
            into.removeFxSlot(lane, position, &removal);
        into.insertFxSlot(lane, position, single.fxSlotType(0, 0), &insertion);
        const auto& reg = Registry::get();
        for (int f = 0; f < 2 + kNumFxSlotParams; ++f)
            into.set(reg.fxSlotOn(lane, position) + f, single.get(reg.fxSlotOn(0, 0) + f));
        into.fxCurves[static_cast<std::size_t>(lane)][static_cast<std::size_t>(position)] = single.fxCurves[0][0];
        if (moves != nullptr) {
            FxMoves combined;
            for (std::size_t k = 0; k < combined.from.size(); ++k) {
                const int mid = insertion.from[k];
                combined.from[k] = mid < 0 ? -1 : replacing ? removal.from[static_cast<std::size_t>(mid)] : mid;
            }
            *moves = combined;
        }
        return true;
    } catch (const std::exception&) {
        r.error("internal", "", "The effect preset could not be read.", "Check that the file is a complete effect preset.");
        return false;
    }
}

} // namespace zed
