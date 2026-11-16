// Standard MIDI Files for clips (plan 3.9, 5.6): a clip imports from a file dropped on the clip
// player and exports to a file dragged out to the host. Times in a clip are quarter notes.
#pragma once

#include "zederia/patch/Patch.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zed::midi {

// What a file says about its notes beyond the notes themselves.
struct FileInfo {
    int ticksPerQuarter = 480;
    double tempo = 120.0;  // the first tempo in the file
    int beatsPerBar = 4;   // quarter notes per bar, from the first time signature
    bool hasKey = false;
    int sharps = 0;        // key signature: -7 (seven flats) .. 7 (seven sharps)
    bool minor = false;
    double endBeats = 0.0; // the end of the last track, in quarter notes
    std::string name;      // the first track name, if any
};

// Reads the notes of a Standard MIDI File (format 0 or 1, every track and channel merged) into
// clip notes in quarter notes. False with a message when the data is not a readable MIDI file.
bool readNotes(std::span<const std::uint8_t> bytes, std::vector<ClipNote>& notes, FileInfo& info, std::string& error);

// A clip from a file: its notes, its length rounded up to whole bars (at least one), its tempo,
// and its key from the key signature. The root (the key that plays the clip untransposed) is the
// key's tonic near middle C, or middle C itself when the file has no key signature.
bool clipFromFile(std::span<const std::uint8_t> bytes, Clip& clip, std::string& error);

// A format 0 file holding the clip: tempo, time signature, key signature (when the clip's key
// names one), the notes, and the end of the track at the clip's length.
std::vector<std::uint8_t> clipToFile(const Clip& clip, int ticksPerQuarter = 960);

// "C major", "F# minor", "Bb major" and the like: the tonic's pitch class (0..11) and mode.
// False when the text does not name a key.
bool parseKey(std::string_view text, int& tonic, bool& minor);
// The key signature for a key: sharps (positive) or flats (negative).
int keySignature(int tonic, bool minor);
// The name of the key a signature stands for, "D major" or "B minor".
std::string keyName(int sharps, bool minor);

} // namespace zed::midi
