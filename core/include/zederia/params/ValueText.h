// Values as people type and read them (plan 3.13: "Typed values accept units: Hz, kHz, ms, s, st,
// cents, dB, %, note lengths, note names"). The interface's value entry, the command palette and
// the agent tools all use these, so a value means the same everywhere.
#pragma once

#include "zederia/params/ParamDef.h"

#include <string>
#include <string_view>

namespace zed {

struct ValueContext {
    double bpm = 120.0;          // turns note lengths into milliseconds or hertz
    double referenceHz = 440.0;  // turns note names into hertz
};

struct ParsedValue {
    double plain = 0.0;      // in the parameter's own unit, clamped to its range
    bool clamped = false;    // the typed value was outside the range
    // Set when the text was a note length ("1/8", "1/4T", "3/16", "1/2D"): the token as typed and
    // its length in quarter notes. A time that should stay linked to the tempo keeps the token.
    std::string noteLength;
    double beats = 0.0;
};

// Reads typed text for a parameter. Understands
//   numbers with units:  "2k", "2 kHz", "440hz", "250 ms", "1.5 s", "+7 st", "1 oct", "50 c",
//                        "-6 dB", "-inf", "25 %", "90 deg"
//   note lengths:        "1/8", "1/4T", "1/16D", "3/16" for times (ms) and rates (Hz)
//   note names:          "A4", "C#3", "Eb2" for frequencies (A4 = the reference pitch)
//   choices and switches by name: "ladder lp24", "on", "off"
// A bare number is in the unit the value is shown in (a percentage for % values). Returns false
// with a short message when the text means nothing for this parameter.
bool parseValueText(const ParamDef& def, std::string_view text, const ValueContext& context, ParsedValue& out,
                    std::string& error);

// The value as the interface shows it: "2.00 kHz", "440 Hz", "250 ms", "1.20 s", "-6.0 dB", "-inf dB",
// "25 %", "+7 st", "+12.0 c", "90 deg", or the choice's display name.
std::string formatValueText(const ParamDef& def, double plain);

// A choice token as words: "ladder_lp24" -> "Ladder LP 24", "up_down_repeat" -> "Up Down Repeat",
// "1/16T" stays "1/16T".
std::string choiceDisplayName(std::string_view token);

// Note names: 60 -> "C4" (middle C is C4), and back ("C#4", "Db4", "c4" all work). False when unreadable.
std::string noteName(int key);
bool parseNoteName(std::string_view text, double& note);

} // namespace zed
