#include "zederia/music/Theory.h"

#include "zederia/params/ValueText.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <set>

namespace zed::music {
namespace {

int mod12(int v) { return ((v % 12) + 12) % 12; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return std::string(s.substr(a, b - a));
}

// Reads the part of a chord symbol after the root (and before any slash bass).
bool parseQuality(std::string_view text, std::vector<int>& intervals, std::string& error) {
    std::string s;
    for (const char c : text)
        if (c != '(' && c != ')' && c != ',' && c != ' ')
            s.push_back(c);
    // Unicode signs some people type: ° (dim), ø (half-dim), Δ (maj).
    const auto replaceAll = [&](const std::string& from, const std::string& to) {
        for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
            s.replace(p, from.size(), to);
    };
    replaceAll("\xC2\xB0", "dim");
    replaceAll("\xC3\xB8", "hdim");
    replaceAll("\xCE\x94", "maj");
    int third = 4, fifth = 7, seventh = -1;
    bool majorSeventh = false, power = false, dim = false;
    std::set<int> extra;
    std::string_view r = s;
    const auto eat = [&](std::string_view token) {
        if (startsWith(r, token)) {
            r.remove_prefix(token.size());
            return true;
        }
        return false;
    };
    if (eat("maj") || eat("Maj") || eat("M")) {
        majorSeventh = true;
    } else if (eat("min") || eat("mi") || eat("m") || eat("-")) {
        third = 3;
        if (eat("maj") || eat("Maj") || eat("M"))
            majorSeventh = true;
    } else if (eat("hdim")) {
        third = 3;
        fifth = 6;
        seventh = 10;
    } else if (eat("dim") || eat("o")) {
        third = 3;
        fifth = 6;
        dim = true;
    } else if (eat("aug") || eat("+")) {
        fifth = 8;
    }
    bool sawNumber = false;
    while (!r.empty()) {
        if (eat("13")) {
            seventh = majorSeventh ? 11 : (seventh >= 0 ? seventh : 10);
            extra.insert(14);
            extra.insert(21);
            sawNumber = true;
        } else if (eat("11")) {
            seventh = majorSeventh ? 11 : (seventh >= 0 ? seventh : 10);
            extra.insert(14);
            extra.insert(17);
            sawNumber = true;
        } else if (eat("9")) {
            seventh = majorSeventh ? 11 : (seventh >= 0 ? seventh : 10);
            extra.insert(14);
            sawNumber = true;
        } else if (eat("7")) {
            seventh = majorSeventh ? 11 : dim ? 9 : (seventh >= 0 ? seventh : 10);
            sawNumber = true;
        } else if (eat("6/9") || eat("69")) {
            extra.insert(9);
            extra.insert(14);
            sawNumber = true;
        } else if (eat("6")) {
            extra.insert(9);
            sawNumber = true;
        } else if (eat("5") && !sawNumber && third == 4 && fifth == 7) {
            power = true;
        } else if (eat("sus2")) {
            third = 2;
        } else if (eat("sus4") || eat("sus")) {
            third = 5;
        } else if (eat("add9") || eat("add2")) {
            extra.insert(14);
        } else if (eat("add11") || eat("add4")) {
            extra.insert(17);
        } else if (eat("add13") || eat("add6")) {
            extra.insert(21);
        } else if (eat("b5") || eat("-5")) {
            fifth = 6;
        } else if (eat("#5") || eat("+5")) {
            fifth = 8;
        } else if (eat("b9") || eat("-9")) {
            extra.erase(14);
            extra.insert(13);
        } else if (eat("#9") || eat("+9")) {
            extra.erase(14);
            extra.insert(15);
        } else if (eat("#11") || eat("+11")) {
            extra.erase(17);
            extra.insert(18);
        } else if (eat("b13") || eat("-13")) {
            extra.erase(21);
            extra.insert(20);
        } else if (eat("maj") || eat("M")) { // "m(maj7)" after the minor sign was read
            majorSeventh = true;
        } else {
            error = "cannot read \"" + std::string(r) + "\" in the chord symbol";
            return false;
        }
    }
    intervals.clear();
    intervals.push_back(0);
    if (!power)
        intervals.push_back(third);
    intervals.push_back(fifth);
    if (seventh >= 0)
        intervals.push_back(seventh);
    for (const int e : extra)
        intervals.push_back(e);
    std::sort(intervals.begin(), intervals.end());
    intervals.erase(std::unique(intervals.begin(), intervals.end()), intervals.end());
    return true;
}

} // namespace

int pitchClass(std::string_view text, std::size_t* consumed) {
    if (consumed)
        *consumed = 0;
    if (text.empty())
        return -1;
    static constexpr int base[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (c < 'A' || c > 'G')
        return -1;
    int pc = base[c - 'A'];
    std::size_t i = 1;
    while (i < text.size()) {
        if (text[i] == '#') {
            ++pc;
            ++i;
        } else if (text[i] == 'b') {
            // "b" after a note letter is a flat unless it starts a word ("Cbass" is not a thing).
            --pc;
            ++i;
        } else if (i + 2 < text.size() + 1 && text.substr(i, 3) == "\xE2\x99\xAF") { // sharp sign
            ++pc;
            i += 3;
        } else if (text.substr(i, 3) == "\xE2\x99\xAD") { // flat sign
            --pc;
            i += 3;
        } else {
            break;
        }
    }
    if (consumed)
        *consumed = i;
    return mod12(pc);
}

std::string pitchClassName(int pc, bool flats) {
    static const char* sharpNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* flatNames[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    return (flats ? flatNames : sharpNames)[mod12(pc)];
}

bool prefersFlats(int tonic, bool minorLike) {
    // Keys written with flats: F Bb Eb Ab Db Gb major; D G C F Bb Eb minor.
    static constexpr bool majorFlats[12] = {false, true, false, true, false, true, true, false, true, false, true, false};
    static constexpr bool minorFlats[12] = {true, false, true, false, false, true, false, true, false, false, true, false};
    return (minorLike ? minorFlats : majorFlats)[mod12(tonic)];
}

const std::vector<ScaleDef>& scales() {
    static const std::vector<ScaleDef> all = {
        {"major", {0, 2, 4, 5, 7, 9, 11}, false},
        {"minor", {0, 2, 3, 5, 7, 8, 10}, true},
        {"harmonic_minor", {0, 2, 3, 5, 7, 8, 11}, true},
        {"melodic_minor", {0, 2, 3, 5, 7, 9, 11}, true},
        {"dorian", {0, 2, 3, 5, 7, 9, 10}, true},
        {"phrygian", {0, 1, 3, 5, 7, 8, 10}, true},
        {"lydian", {0, 2, 4, 6, 7, 9, 11}, false},
        {"mixolydian", {0, 2, 4, 5, 7, 9, 10}, false},
        {"locrian", {0, 1, 3, 5, 6, 8, 10}, true},
        {"major_pentatonic", {0, 2, 4, 7, 9}, false},
        {"minor_pentatonic", {0, 3, 5, 7, 10}, true},
        {"blues", {0, 3, 5, 6, 7, 10}, true},
        {"whole_tone", {0, 2, 4, 6, 8, 10}, false},
        {"chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, false},
    };
    return all;
}

const ScaleDef* findScale(std::string_view id) {
    std::string s = lower(trim(id));
    for (char& c : s)
        if (c == ' ' || c == '-')
            c = '_';
    if (s == "maj" || s == "ionian" || s == "major_scale" || s == "") s = "major";
    else if (s == "min" || s == "m" || s == "aeolian" || s == "natural_minor") s = "minor";
    else if (s == "dor") s = "dorian";
    else if (s == "phr") s = "phrygian";
    else if (s == "lyd") s = "lydian";
    else if (s == "mix" || s == "mixo") s = "mixolydian";
    else if (s == "loc") s = "locrian";
    else if (s == "pentatonic" || s == "maj_pent" || s == "major_pent") s = "major_pentatonic";
    else if (s == "min_pent" || s == "minor_pent") s = "minor_pentatonic";
    else if (s == "harmonic" || s == "harm_minor") s = "harmonic_minor";
    else if (s == "melodic" || s == "mel_minor") s = "melodic_minor";
    for (const auto& sc : scales())
        if (sc.id == s)
            return &sc;
    return nullptr;
}

bool Key::contains(int note) const {
    if (!scale)
        return true;
    const int pc = mod12(note - tonic);
    return std::find(scale->steps.begin(), scale->steps.end(), pc) != scale->steps.end();
}

std::string Key::name() const {
    if (!scale)
        return {};
    return pitchClassName(tonic, prefersFlats(tonic, scale->minorLike)) + " " + scale->id;
}

std::string Key::shortName() const {
    if (!scale)
        return {};
    std::string tag = scale->id == "major" ? "maj" : scale->id == "minor" ? "min" : scale->id.substr(0, 3);
    if (scale->id == "harmonic_minor") tag = "hmin";
    if (scale->id == "melodic_minor") tag = "mmin";
    if (scale->id == "major_pentatonic") tag = "pent";
    if (scale->id == "minor_pentatonic") tag = "mpent";
    return pitchClassName(tonic, prefersFlats(tonic, scale->minorLike)) + tag;
}

bool parseKey(std::string_view text, Key& out) {
    out = Key{};
    const std::string t = trim(text);
    std::size_t used = 0;
    const int pc = pitchClass(t, &used);
    if (pc < 0)
        return false;
    std::string rest = trim(std::string_view(t).substr(used));
    const ScaleDef* sc = nullptr;
    if (rest.empty() || rest == "M")
        sc = findScale("major");
    else if (rest == "m")
        sc = findScale("minor");
    else
        sc = findScale(rest);
    if (!sc)
        return false;
    out.tonic = pc;
    out.scale = sc;
    return true;
}

bool parseChord(std::string_view symbol, Chord& out, std::string& error) {
    out = Chord{};
    const std::string s = trim(symbol);
    out.symbol = s;
    std::size_t used = 0;
    const int root = pitchClass(s, &used);
    if (root < 0) {
        error = "a chord symbol starts with its root note (A to G), for example \"Fm7\" or \"Bb/D\"";
        return false;
    }
    out.root = root;
    std::string_view rest = std::string_view(s).substr(used);
    // A slash bass: the text after the last '/' when it is a note name (not the "6/9" chord).
    const std::size_t slash = rest.rfind('/');
    if (slash != std::string_view::npos && !(slash > 0 && rest[slash - 1] == '6' && slash + 1 < rest.size() && rest[slash + 1] == '9')) {
        std::size_t bassUsed = 0;
        const int bass = pitchClass(rest.substr(slash + 1), &bassUsed);
        if (bass < 0 || bassUsed != rest.size() - slash - 1) {
            error = "the part after \"/\" must be a bass note such as \"E\" or \"Bb\"";
            return false;
        }
        out.bass = bass;
        rest = rest.substr(0, slash);
    }
    if (!parseQuality(rest, out.intervals, error)) {
        error += " \"" + s + "\"; write qualities such as m, 7, maj7, m7b5, dim, sus4, add9";
        return false;
    }
    return true;
}

bool parseRoman(std::string_view numeral, const Key& key, Chord& out, std::string& error) {
    out = Chord{};
    if (!key.valid()) {
        error = "roman numerals need a key, for example \"F minor\"";
        return false;
    }
    std::string s = trim(numeral);
    out.symbol = s;
    std::size_t i = 0;
    int alter = 0;
    while (i < s.size() && (s[i] == 'b' || s[i] == '#')) {
        alter += s[i] == '#' ? 1 : -1;
        ++i;
    }
    static const char* numerals[7] = {"vii", "iii", "vi", "iv", "ii", "v", "i"};
    static constexpr int degreeOf[7] = {6, 2, 5, 3, 1, 4, 0};
    int degree = -1;
    bool upper = false;
    for (int n = 0; n < 7 && degree < 0; ++n) {
        const std::string pat = numerals[n];
        if (s.size() - i < pat.size())
            continue;
        const std::string part = s.substr(i, pat.size());
        if (lower(part) != pat)
            continue;
        const bool allUpper = std::all_of(part.begin(), part.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
        const bool allLower = std::all_of(part.begin(), part.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); });
        if (!allUpper && !allLower)
            continue;
        degree = degreeOf[n];
        upper = allUpper;
        i += pat.size();
    }
    if (degree < 0) {
        error = "\"" + s + "\" is not a roman numeral (I to VII, lower case for minor)";
        return false;
    }
    const auto& steps = key.scale->steps;
    const int stepIndex = steps.size() == 7 ? degree : -1;
    if (stepIndex < 0) {
        error = "roman numerals need a seven-note key (major, minor or a mode)";
        return false;
    }
    out.root = mod12(key.tonic + steps[static_cast<std::size_t>(stepIndex)] + alter);
    std::string rest = s.substr(i);
    std::string quality = upper ? "" : "m";
    // "°" / "o" / "dim" / "ø" / "+" replace the case's quality.
    if (startsWith(rest, "\xC2\xB0") || startsWith(rest, "o") || startsWith(rest, "dim") || startsWith(rest, "\xC3\xB8") ||
        startsWith(rest, "+") || startsWith(rest, "aug"))
        quality.clear();
    std::string slashPart;
    const std::size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        slashPart = rest.substr(slash + 1);
        rest = rest.substr(0, slash);
    }
    if (!parseQuality(quality + rest, out.intervals, error)) {
        error += " in \"" + s + "\"";
        return false;
    }
    if (!slashPart.empty()) {
        std::size_t used = 0;
        const int bass = pitchClass(slashPart, &used);
        if (bass < 0 || used != slashPart.size()) {
            error = "the part after \"/\" must be a bass note such as \"E\" or \"Bb\"";
            return false;
        }
        out.bass = bass;
    }
    return true;
}

bool parseChordOrRoman(std::string_view text, const Key* key, Chord& out, std::string& error) {
    const std::string s = trim(text);
    std::size_t i = 0;
    while (i < s.size() && (s[i] == 'b' || s[i] == '#'))
        ++i;
    const bool romanLike = i < s.size() && std::string("IViv").find(s[i]) != std::string::npos;
    if (key && key->valid() && romanLike) {
        std::string romanError;
        if (parseRoman(s, *key, out, romanError))
            return true;
        if (parseChord(s, out, error))
            return true;
        error = romanError;
        return false;
    }
    return parseChord(s, out, error);
}

std::vector<int> closeVoicing(const Chord& chord, int lowest) {
    int root = lowest + mod12(chord.root - lowest);
    std::vector<int> notes;
    for (const int iv : chord.intervals)
        notes.push_back(root + iv);
    return notes;
}

namespace {

// Total movement between two voicings: the voices matched in order, extra voices to the nearest.
int movement(const std::vector<int>& a, const std::vector<int>& b) {
    int total = 0;
    if (a.size() == b.size()) {
        for (std::size_t i = 0; i < a.size(); ++i)
            total += std::abs(a[i] - b[i]);
        return total;
    }
    for (const int n : b) {
        int best = INT_MAX;
        for (const int m : a)
            best = std::min(best, std::abs(n - m));
        total += best;
    }
    return total;
}

} // namespace

std::vector<Voicing> voiceLead(const std::vector<Chord>& chords, int centre, int low, int high, bool bass) {
    std::vector<Voicing> out;
    if (high < low + 12)
        high = low + 12;
    std::vector<int> previous;
    for (const auto& chord : chords) {
        // Pitch classes of the chord, at most five (the fifth goes first when there are more).
        std::vector<int> pcs;
        for (const int iv : chord.intervals)
            pcs.push_back(iv);
        if (pcs.size() > 5) {
            pcs.erase(std::remove(pcs.begin(), pcs.end(), 7), pcs.end());
            while (pcs.size() > 5)
                pcs.pop_back();
        }
        std::vector<std::vector<int>> candidates;
        const int count = static_cast<int>(pcs.size());
        for (int inversion = 0; inversion < count; ++inversion) {
            for (int base = low - 12; base <= high; ++base) {
                std::vector<int> v;
                int last = -1000;
                for (int k = 0; k < count; ++k) {
                    const int iv = pcs[static_cast<std::size_t>((inversion + k) % count)];
                    int note = base + mod12(chord.root + iv - base);
                    while (note <= last)
                        note += 12;
                    v.push_back(note);
                    last = note;
                }
                if (v.front() < low || v.back() > high || v.back() - v.front() > 19)
                    continue;
                if (mod12(v.front() - (chord.root + pcs[static_cast<std::size_t>(inversion)])) != 0)
                    continue;
                if (std::find(candidates.begin(), candidates.end(), v) == candidates.end())
                    candidates.push_back(v);
            }
        }
        Voicing voicing;
        if (candidates.empty()) {
            voicing.notes = closeVoicing(chord, std::max(low, centre - 6));
        } else if (previous.empty()) {
            int best = INT_MAX;
            for (const auto& c : candidates) {
                const int score = std::abs((c.front() + c.back()) / 2 - centre) + (c.front() % 12 == (chord.root % 12 + 12) % 12 ? 0 : 2);
                if (score < best) {
                    best = score;
                    voicing.notes = c;
                }
            }
        } else {
            int best = INT_MAX;
            for (const auto& c : candidates) {
                const int score = movement(previous, c) * 4 + std::abs((c.front() + c.back()) / 2 - centre);
                if (score < best) {
                    best = score;
                    voicing.notes = c;
                }
            }
        }
        if (bass) {
            const int pc = chord.bass >= 0 ? chord.bass : chord.root;
            int b = low - 12 + mod12(pc - (low - 12));
            while (b >= voicing.notes.front())
                b -= 12;
            voicing.bass = std::max(b, 0);
        }
        previous = voicing.notes;
        out.push_back(voicing);
    }
    return out;
}

int quantize(int note, const Key& key, int direction) {
    if (!key.valid() || key.contains(note))
        return note;
    for (int d = 1; d < 12; ++d) {
        if (direction >= 0 && key.contains(note + d))
            return std::clamp(note + d, 0, 127);
        if (direction <= 0 && key.contains(note - d))
            return std::clamp(note - d, 0, 127);
    }
    return note;
}

namespace {
// A beat or bar count as people write it: "7", "1.5", "0.125".
std::string beatsText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}
} // namespace

std::vector<ClipIssue> validateClip(const Clip& clip, const Key& key) {
    std::vector<ClipIssue> issues;
    const auto add = [&](bool error, std::string code, std::string path, std::string message, std::string fix) {
        issues.push_back({error, std::move(code), std::move(path), std::move(message), std::move(fix)});
    };
    if (!(clip.bars > 0.0) || clip.bars > kMaxClipBars)
        add(true, "bars", "/bars", "the clip must be longer than 0 and at most 64 bars", "set \"bars\" to a whole number from 1 to 64");
    else if (clip.lengthBeats() < kMinClipBeats)
        add(true, "too_short", "/bars", "the clip lasts " + beatsText(clip.lengthBeats()) + " beats; the clip player needs at least 1/16 of a beat",
            "set \"bars\" to a whole number from 1 to 64");
    else if (std::abs(clip.bars - std::round(clip.bars)) > 1e-9)
        add(false, "partial_bar", "/bars", "the clip does not end on a bar line (" + beatsText(clip.bars) + " bars), so it will not loop on the beat",
            "use a whole number of bars");
    if (clip.beatsPerBar < 1 || clip.beatsPerBar > 16)
        add(true, "beats_per_bar", "/beats_per_bar", "beats per bar must be from 1 to 16", "use 4 for 4/4, 3 for 3/4");
    if (!(clip.tempo >= 20.0 && clip.tempo <= 400.0))
        add(true, "tempo", "/tempo", "the tempo must be from 20 to 400 bpm", "set \"tempo\" in beats per minute");
    if (static_cast<int>(clip.notes.size()) > kMaxAgentClipNotes)
        add(true, "too_many_notes", "/notes", "the clip has " + std::to_string(clip.notes.size()) + " notes; at most 2048 are allowed",
            "split the material into several clips");
    if (clip.notes.empty())
        add(false, "empty", "/notes", "the clip has no notes", "add notes with start, length, key and velocity");
    const double end = clip.lengthBeats();
    std::vector<std::string> outside;
    for (std::size_t i = 0; i < clip.notes.size(); ++i) {
        const auto& n = clip.notes[i];
        const std::string p = "/notes/" + std::to_string(i);
        if (n.key < 0 || n.key > 127)
            add(true, "out_of_range", p + "/key", "key " + std::to_string(n.key) + " is outside 0..127", "use a MIDI note from 0 to 127 (60 is C4)");
        else if (n.key < 21 || n.key > 108)
            add(false, "extreme_register", p + "/key", noteName(n.key) + " is outside the piano's range (A0 to C8)",
                "move it by an octave unless the extreme register is intended");
        if (!(n.length > 0.0))
            add(true, "length", p + "/length", "a note must be longer than 0 beats", "give the note a length in beats, for example 0.5");
        if (!(n.start >= 0.0))
            add(true, "start", p + "/start", "a note cannot start before the clip", "start it at 0 or later");
        if (!(n.velocity > 0.0 && n.velocity <= 1.0))
            add(true, "velocity", p + "/velocity", "velocity must be above 0 and at most 1 (or 1..127)", "use 0.8 for a normal note");
        if (n.start >= end - 1e-9)
            add(true, "past_end", p + "/start", "the note starts at beat " + beatsText(n.start) + ", after the clip ends at beat " + beatsText(end),
                "move it inside the clip or make the clip longer");
        else if (n.start + n.length > end + 1e-9)
            add(false, "past_end", p + "/length", "the note runs past the end of the clip; it is cut at the loop point",
                "shorten it to end by beat " + beatsText(end));
        if (key.valid() && n.key >= 0 && n.key <= 127 && !key.contains(n.key))
            outside.push_back(noteName(n.key) + " (#" + std::to_string(i) + ", nearest in key " + noteName(quantize(n.key, key)) + ")");
        for (std::size_t j = i + 1; j < clip.notes.size(); ++j) {
            const auto& m = clip.notes[j];
            if (m.key == n.key && m.start < n.start + n.length - 1e-9 && n.start < m.start + m.length - 1e-9) {
                add(false, "overlap", "/notes/" + std::to_string(j),
                    "notes " + std::to_string(i) + " and " + std::to_string(j) + " overlap on " + noteName(n.key) + "; the second retriggers the first",
                    "shorten the earlier note so it ends where the later one starts");
                break;
            }
        }
    }
    if (!outside.empty()) {
        std::string list;
        for (std::size_t i = 0; i < outside.size() && i < 12; ++i)
            list += (i ? ", " : "") + outside[i];
        if (outside.size() > 12)
            list += ", ...";
        add(false, "outside_key", "/notes", std::to_string(outside.size()) + " note(s) are outside " + key.name() + ": " + list,
            "keep them as passing notes, or move them to the nearest note of the key (theory quantize)");
    }
    return issues;
}

Clip transposeClip(const Clip& clip, int semitones) {
    Clip out = clip;
    for (auto& n : out.notes) {
        int k = n.key + semitones;
        while (k > 127)
            k -= 12;
        while (k < 0)
            k += 12;
        n.key = k;
    }
    out.root = std::clamp(clip.root + semitones, 0, 127);
    return out;
}

int transposeBetween(int fromTonic, int toTonic) {
    int d = mod12(toTonic - fromTonic);
    if (d > 5)
        d -= 12;
    return d;
}

std::string compactName(std::string_view name) {
    std::string out;
    bool startWord = true;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && u < 128) {
            out.push_back(startWord ? static_cast<char>(std::toupper(u)) : c);
            startWord = false;
        } else {
            startWord = true;
        }
    }
    return out.empty() ? "Untitled" : out;
}

std::string loopFileName(std::string_view kind, std::string_view preset, std::string_view keyShort, double bpm,
                         std::string_view extension) {
    std::string kindPart;
    for (const char c : kind) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && u < 128)
            kindPart.push_back(static_cast<char>(std::tolower(u)));
        else if (!kindPart.empty() && kindPart.back() != '-')
            kindPart.push_back('-');
    }
    while (!kindPart.empty() && kindPart.back() == '-')
        kindPart.pop_back();
    std::string out = "ZED_" + (kindPart.empty() ? std::string("loop") : kindPart) + "_" + compactName(preset);
    std::string keyPart;
    for (const char c : keyShort)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '#')
            keyPart.push_back(c);
    if (!keyPart.empty())
        out += "_" + keyPart;
    if (bpm > 0.0) {
        const double whole = std::round(bpm);
        if (std::abs(bpm - whole) < 1e-6) {
            out += "_" + std::to_string(static_cast<int>(whole)) + "bpm";
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "_%.2fbpm", bpm);
            out += buf;
        }
    }
    return out + "." + std::string(extension);
}

std::string oneShotFileName(std::string_view preset, int note, std::string_view extension) {
    std::string n = noteName(std::clamp(note, 0, 127));
    for (char& c : n)
        if (c == '-')
            c = 'm';
    return "ZED_oneshot_" + compactName(preset) + "_" + n + "." + std::string(extension);
}

std::string multiSampleFileName(std::string_view preset, int note, int velocity127, std::string_view extension) {
    std::string n = noteName(std::clamp(note, 0, 127));
    for (char& c : n)
        if (c == '-')
            c = 'm';
    return "ZED_" + compactName(preset) + "_" + n + "_v" + std::to_string(std::clamp(velocity127, 1, 127)) + "." +
           std::string(extension);
}

} // namespace zed::music
