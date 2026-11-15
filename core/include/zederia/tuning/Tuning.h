// Microtuning (plan 3.6): a table that says what pitch each MIDI key plays, read from Scala scale
// (.scl) and keyboard-mapping (.kbm) files or from a .tun file.
//
// A key's pitch is stored as a fractional equal-tempered note number, where 69.0 is the
// instrument's reference A (global.reference, 440 Hz unless changed). In equal temperament key k
// simply holds k. Everything else in the engine (glide, bend, fine tune) keeps working in
// semitones on top of that.
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

struct Tuning {
    std::string name;             // shown to the user; "" for equal temperament
    std::array<double, 128> note; // pitch of each key as an equal-tempered note number

    Tuning();                     // equal temperament
    bool isEqual() const;
    // The pitch of a fractional key (a glide between two keys moves in a straight line between
    // their pitches; outside 0..127 it continues in equal steps).
    double pitchOf(double key) const noexcept;
    bool operator==(const Tuning&) const = default;
};

// A Scala scale: the pitch of each degree above the first, in cents; the last entry is the
// interval at which the scale repeats (usually the octave, 1200).
struct ScaleDef {
    std::string description;
    std::vector<double> cents;
};

// A Scala keyboard mapping.
struct KeyMap {
    int first = 0, last = 127;   // keys the mapping covers
    int middle = 60;             // key that plays the scale's first degree
    int reference = 60;          // key whose frequency is given
    double referenceHz = 261.6255653006; // equal-tempered middle C when A is 440
    int octaveDegree = 0;        // scale degree that one pass of the map spans; 0 = the scale's size
    std::vector<int> degrees;    // degree per key of the pattern; -1 = unmapped; empty = one key per degree
};

// All three parsers never throw; on bad input they return false with a message that names the
// line and says what was expected.
bool parseScl(std::string_view text, ScaleDef& out, std::string& error);
bool parseKbm(std::string_view text, KeyMap& out, std::string& error);
bool parseTun(std::string_view text, Tuning& out, std::string& error);

// The tuning a scale gives under a mapping. Unmapped keys keep the pitch of the mapped key below.
Tuning tuningFromScale(const ScaleDef& scale, const KeyMap& map, std::string name);

// Reads "file.scl", "file.scl" plus "file.kbm", or "file.tun" by extension. `kbmPath` may be empty.
bool loadTuningFiles(const std::string& path, const std::string& kbmPath, Tuning& out, std::string& error);

} // namespace zed
