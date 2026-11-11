// Offline rendering (plan 5.4): a patch plus a note list or a MIDI file becomes audio. It runs the
// same engine code as the plug-in and, for the same build, patch, events and seed, produces
// bit-identical output on every run.
#pragma once

#include "zederia/engine/MidiEvent.h"
#include "zederia/patch/Patch.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

// An event on the render timeline. `event.offset` is ignored; `sample` is the absolute position.
struct SeqEvent {
    std::int64_t sample = 0;
    MidiEvent event;
};

struct NoteSpec {
    int note = 60;
    double start = 0.0;    // seconds
    double duration = 1.0; // seconds
    double velocity = 0.8; // 0..1
};

// Parses "note:start:duration[:velocity]" entries separated by commas, for example
// "C3:0:0.5, 64:0.5:0.5:100". A note is a MIDI number or a name (C4 is 60); times are in seconds;
// velocity is 1..127 and defaults to 100. Returns false and sets `error` on bad input.
bool parseNoteList(std::string_view text, std::vector<NoteSpec>& out, std::string& error);
// Parses a note name such as "C4", "F#2" or "Bb-1"; returns -1 when it is not one.
int parseNoteName(std::string_view text);

std::vector<SeqEvent> sequenceFromNotes(std::span<const NoteSpec> notes, double sampleRate);

// Reads a Standard MIDI File (format 0 or 1) into timeline events, following its tempo changes.
// Returns false and sets `error` when the data is not a readable MIDI file.
bool sequenceFromMidiFile(std::span<const std::uint8_t> bytes, double sampleRate, std::vector<SeqEvent>& out,
                          std::string& error);

struct RenderSettings {
    double sampleRate = 48000.0;
    int blockSize = 512;
    std::uint64_t seed = 1;
    double tailSeconds = -1.0; // time rendered after the last event; negative = the engine's own tail
    double bpm = 120.0;
};

struct RenderResult {
    std::vector<float> left, right;
    double sampleRate = 48000.0;
    std::uint64_t hash() const; // of the sample data
    float peak() const;
};

RenderResult renderOffline(const Patch& patch, std::span<const SeqEvent> events, const RenderSettings& settings);

// --- WAV files ---------------------------------------------------------------------------------

enum class WavFormat { Pcm16, Pcm24, Float32 };

std::vector<std::uint8_t> encodeWav(std::span<const float> left, std::span<const float> right, double sampleRate,
                                    WavFormat format);
bool writeFile(const std::string& path, std::span<const std::uint8_t> bytes, std::string& error);
bool readFile(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error);

} // namespace zed
