// Music helpers for agents (plan 4.7): the agent writes notes, these keep them honest. Chord
// symbols and roman numerals become pitches, scales and keys are named, notes snap to a key,
// progressions are voice-led, clips are checked, and rendered loops get their file names.
// Pure functions; nothing here touches files or the engine.
#pragma once

#include "zederia/patch/Patch.h"

#include <string>
#include <string_view>
#include <vector>

namespace zed::music {

// "C", "c#", "Db", "F#", "B" -> 0..11; -1 when the text does not start with a note letter.
// `consumed` receives how many characters the name used.
int pitchClass(std::string_view text, std::size_t* consumed = nullptr);
std::string pitchClassName(int pc, bool flats = false);
bool prefersFlats(int tonic, bool minorLike);

struct ScaleDef {
    std::string id;          // "minor"
    std::vector<int> steps;  // semitones above the tonic, ascending, starting at 0
    bool minorLike = false;  // a minor third above the tonic
};
const std::vector<ScaleDef>& scales();
const ScaleDef* findScale(std::string_view id); // accepts aliases: "min", "aeolian", "maj", "ionian"

struct Key {
    int tonic = 0;           // pitch class
    const ScaleDef* scale = nullptr;
    bool valid() const { return scale != nullptr; }
    bool contains(int note) const;
    std::string name() const;      // "F minor"
    std::string shortName() const; // "Fmin", "Cmaj", "Ddor"
};
// "F minor", "Fm", "F#min", "Bb major", "D dorian", "A harmonic_minor". False when unreadable.
bool parseKey(std::string_view text, Key& out);

struct Chord {
    std::string symbol;        // as written
    int root = 0;              // pitch class
    int bass = -1;             // pitch class of a slash bass, -1 when none
    std::vector<int> intervals; // semitones above the root, ascending, 0 first
};
// Chord symbols: C, Cm, C-, Cdim, C°, Caug, C+, Csus2, Csus4, C5, C6, Cm6, C6/9, C7, Cmaj7, CM7,
// CΔ7, Cm7, CmMaj7, Cdim7, Cm7b5, Cø7, C9, Cmaj9, Cm9, C11, Cm11, C13, Cmaj13, Cadd9, Cadd11,
// alterations b5 #5 b9 #9 #11 b13 (with or without brackets), and a slash bass (C/E).
bool parseChord(std::string_view symbol, Chord& out, std::string& error);
// Roman numerals in a key: I ii iii IV V vi vii°, i iv v VI VII, with b or # in front, and the
// same suffixes as chord symbols (V7, ii7, viiø7, bVIImaj7, IVsus2). Upper case is major, lower
// case minor; degrees count along the key's own scale.
bool parseRoman(std::string_view numeral, const Key& key, Chord& out, std::string& error);
// Either form: a roman numeral when a key is given and the text reads as one, else a symbol.
bool parseChordOrRoman(std::string_view text, const Key* key, Chord& out, std::string& error);

// The chord in close position with its root at or above `lowest`.
std::vector<int> closeVoicing(const Chord& chord, int lowest);

struct Voicing {
    std::vector<int> notes; // ascending
    int bass = -1;          // a separate bass note below, -1 when not asked for
};
// Voice-leads a progression: the first chord in close position near `centre`, every later chord
// in the inversion and octave (inside [low, high]) that moves the voices least. With `bass`, each
// chord also gets its root (or slash bass) in the octave below `low`.
std::vector<Voicing> voiceLead(const std::vector<Chord>& chords, int centre, int low, int high, bool bass);

// The nearest note of the key (ties go up; direction > 0 always up, < 0 always down).
int quantize(int note, const Key& key, int direction = 0);

// --- Clips ---------------------------------------------------------------------------------------

inline constexpr int kMaxAgentClipNotes = 2048;
inline constexpr double kMaxClipBars = 64.0;
inline constexpr double kMinClipBeats = 1.0 / 16.0; // the clip player plays nothing shorter

struct ClipIssue {
    bool error = false;  // errors make the clip unusable; warnings are notes for the agent
    std::string code;    // "out_of_range", "overlap", "outside_key", "past_end", ...
    std::string path;    // JSON pointer into the clip spec, "/notes/3"
    std::string message;
    std::string fix;
};
// Range, overlaps, key membership (when `key` is valid) and bar length.
std::vector<ClipIssue> validateClip(const Clip& clip, const Key& key);
// Moves every note by `semitones`, keeping keys inside 0..127 by octaves.
Clip transposeClip(const Clip& clip, int semitones);
// The smallest move from one tonic to another, -6..+5 semitones.
int transposeBetween(int fromTonic, int toTonic);

// --- File names (plan 4.7) -------------------------------------------------------------------------

// "Glass Pluck" -> "GlassPluck"; letters and digits only, each word capitalised.
std::string compactName(std::string_view name);
// ZED_<kind>_<Preset>_<Key>_<bpm>bpm.<ext>, for example ZED_chords_GlassPluck_Fmin_124bpm.wav.
// An empty key or a bpm of 0 leaves that part out.
std::string loopFileName(std::string_view kind, std::string_view preset, std::string_view keyShort, double bpm,
                         std::string_view extension);
// ZED_oneshot_<Preset>_<Note>.wav and ZED_<Preset>_<Note>_v<velocity>.wav for multi-samples.
std::string oneShotFileName(std::string_view preset, int note, std::string_view extension);
std::string multiSampleFileName(std::string_view preset, int note, int velocity127, std::string_view extension);

} // namespace zed::music
