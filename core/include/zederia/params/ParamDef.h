// One parameter's definition: the unit of the registry (plan 5.5).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

enum class ParamType : std::uint8_t { Float, Int, Bool, Choice };

// Percent means the stored value is a fraction (0..1 or -1..1) that is shown multiplied by 100.
enum class Unit : std::uint8_t { None, Percent, Hz, Ms, Db, Semitones, Cents, Octaves, Degrees };

// How the normalized 0..1 host value maps onto the range.
//   Linear: min + n * (max - min)
//   Log:    min * (max / min)^n          (needs min > 0; frequencies)
//   Power:  min + (max - min) * n^exp    (times that start at zero)
enum class Skew : std::uint8_t { Linear, Log, Power };

// How the engine follows changes.
//   None:    stepped or structural; taken at the next control tick as is
//   Control: smoothed at the control rate (every 16 samples)
//   Sample:  smoothed at the control rate and interpolated per sample (pitch, cutoff, amplitude)
enum class Smoothing : std::uint8_t { None, Control, Sample };

// Whether the value exists once per instrument or once per voice. Routes are polyphonic when
// both ends are per-voice.
enum class Scope : std::uint8_t { Global, Voice };

// Levels at or below this are treated as silence.
inline constexpr double kSilenceDb = -72.0;

struct ParamDef {
    std::string id;          // "osc1.unison.detune": fixed for ever
    std::string name;        // "Osc 1 Unison Detune"
    std::string module;      // "osc1"
    std::string pointer;     // JSON pointer inside a patch: "/osc1/unison/detune"
    std::string description; // one sentence, shown in PARAMETERS.md and to agents
    ParamType type = ParamType::Float;
    double min = 0.0, max = 1.0, def = 0.0;
    Unit unit = Unit::None;
    Skew skew = Skew::Linear;
    double skewExp = 1.0;
    Smoothing smoothing = Smoothing::None;
    Scope scope = Scope::Global;
    bool automatable = true;  // exposed to the host
    bool modulatable = false; // valid destination for a modulation route
    std::vector<std::string> choices; // tokens for Choice parameters, in index order
    std::uint32_t hostId = 0; // stable hash of id, the host's parameter id
    int index = -1;           // position in the registry

    bool stepped() const { return type != ParamType::Float; }
    int steps() const { return stepped() ? static_cast<int>(max - min) + 1 : 0; }

    // Clamps into range; stepped types snap to whole numbers. NaN becomes the default.
    double clamp(double value) const;
    double toNormalized(double plain) const;
    double fromNormalized(double normalized) const;
    // Text for a plain value with its unit ("-6.0 dB", "25.0 %", "ladder_lp24", "on").
    std::string format(double plain) const;
    int choiceIndex(std::string_view token) const; // -1 when unknown

    // Fluent setters used while the registry is being declared.
    ParamDef& noAuto() { automatable = false; return *this; }
    ParamDef& mod() { modulatable = true; return *this; }
    ParamDef& noMod() { modulatable = false; return *this; }
    ParamDef& smooth(Smoothing s) { smoothing = s; return *this; }
    ParamDef& in(Scope s) { scope = s; return *this; }
    ParamDef& at(std::string jsonPointer) { pointer = std::move(jsonPointer); return *this; }
};

std::string_view unitLabel(Unit unit);      // "", "%", "Hz", "ms", "dB", "st", "cents", "oct", "deg"
std::string_view unitKeySuffix(Unit unit);  // "", "_hz", "_ms", "_db", "_st", "_cents", "_deg"
std::string_view typeLabel(ParamType type); // "float", "int", "bool", "choice"
std::string_view skewLabel(Skew skew);
std::string_view smoothingLabel(Smoothing smoothing);
std::string_view scopeLabel(Scope scope);

// The host derives its 32-bit parameter id from the string id with this function, so adding
// parameters never renumbers existing ones. Collisions are checked by the registry tests.
std::uint32_t hostIdFor(std::string_view id);

} // namespace zed
