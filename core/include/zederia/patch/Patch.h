// The patch: one document that describes a whole sound (plan 4.5). It is the preset file, the
// plug-in state and the agent's working material. Parameter values live in registry order;
// everything that is not a parameter (text, references, routes, rack layout) is held beside them.
#pragma once

#include "zederia/params/Registry.h"
#include "zederia/patch/Shape.h"
#include "zederia/table/Wavetable.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

struct PatchMeta {
    std::string name = "Init";
    std::string author, pack, type, notes, created, modified;
    std::vector<std::string> tags, genres;
    int rating = 0; // 0..5
    bool favourite = false;
    bool operator==(const PatchMeta&) const = default;
};

// A wavetable kept inside the patch instead of being loaded by reference: a table edited in the
// workshop or imported and not yet saved as a file. Written into a preset as deflated base64
// data (plan 5.6). Two embedded tables are equal when their contents are.
struct EmbeddedTable {
    std::shared_ptr<const Wavetable> table; // null: the oscillator loads its reference
    explicit operator bool() const { return table != nullptr; }
    bool operator==(const EmbeddedTable& other) const {
        return table == other.table || (table != nullptr && other.table != nullptr && *table == *other.table);
    }
};

enum class Polarity : std::uint8_t { Bi, Uni };
enum class ModCurve : std::uint8_t { Linear, Exp, Log, S, Drawn };

// One modulation route. Its amount is the registry parameter mod.N.amount.
struct ModRoute {
    std::string source;      // a modulation source id, or "ccN"
    std::string via;         // optional second source that scales the route
    std::string dest;        // registry id of a modulatable parameter
    bool viaInvert = false;
    Polarity polarity = Polarity::Bi;
    ModCurve curve = ModCurve::Linear;
    Shape curveShape;        // the drawn curve (ModCurve::Drawn): y from 0 to 1 over x from 0 to 1
    bool enabled = true, mute = false, solo = false;
    bool operator==(const ModRoute&) const = default;
};

// A microtuning stored with the patch: the pitch of each of the 128 keys as an equal-tempered
// note number (see tuning/Tuning.h). No keys means equal temperament.
struct PatchTuning {
    std::string name;
    std::vector<double> keys;
    bool empty() const { return keys.empty(); }
    bool operator==(const PatchTuning&) const = default;
};

// Names of the five envelope times that can follow the tempo, in the order of Patch::envSync.
inline constexpr const char* kEnvSyncKeys[5] = {"delay", "attack", "hold", "decay", "release"};

// --- Play data (plan 3.9) -------------------------------------------------------------------------

inline constexpr int kNumArpPatterns = 12;
inline constexpr int kMaxArpSteps = 32;
inline constexpr int kNumClips = 8;
inline constexpr int kMaxClipNotes = 4096;
inline constexpr int kMaxChordNotes = 12;

enum class StepState : std::uint8_t { On, Tie, Rest };

// One step of the arpeggiator's step lane.
struct ArpStep {
    StepState state = StepState::On;
    double velocity = 1.0; // 0..1, scales the played velocity
    int transpose = 0;     // semitones, -24..24
    int ratchet = 1;       // 1..4 notes in the step
    bool operator==(const ArpStep&) const = default;
};

// A pattern slot. Steps it does not list are On at full velocity.
struct ArpPattern {
    std::vector<ArpStep> steps; // at most kMaxArpSteps
    bool empty() const { return steps.empty(); }
    ArpStep step(int i) const {
        return i >= 0 && i < static_cast<int>(steps.size()) ? steps[static_cast<std::size_t>(i)] : ArpStep{};
    }
    bool operator==(const ArpPattern&) const = default;
};

// One note of a clip. Times are in quarter notes from the start of the clip.
struct ClipNote {
    double start = 0.0;
    double length = 0.25;
    int key = 60;
    double velocity = 0.8; // 0..1
    bool operator==(const ClipNote&) const = default;
};

// A clip (plan 3.9): a note list with its length and the tempo and key it was written in.
struct Clip {
    std::string name;
    double bars = 1.0;
    int beatsPerBar = 4;  // quarter notes per bar
    double tempo = 120.0; // hint: the clip plays at the host's tempo
    std::string key;      // hint, for example "C minor"; may be empty
    int root = 60;        // the key that plays the clip untransposed when a key starts it
    std::vector<ClipNote> notes;
    bool empty() const { return notes.empty(); }
    double lengthBeats() const { return bars * beatsPerBar; }
    bool operator==(const Clip&) const = default;
};

// Rack edits: for each slot of the new layout (lane * kNumFxSlots + slot), the slot of the old
// layout whose effect continues there, or -1 for a new effect. The engine keeps a continuing
// effect's state (its tail) and fades only what has to move.
struct FxMoves {
    std::array<int, kNumFxLanes * kNumFxSlots> from;
    FxMoves() { from.fill(-1); }
};

class Patch {
public:
    Patch(); // the init patch: every parameter at its default

    double get(int index) const { return values_[static_cast<std::size_t>(index)]; }
    double get(std::string_view id) const; // 0 when the id is unknown
    // Stores the value clamped to the parameter's range and returns what was stored.
    double set(int index, double value);
    bool set(std::string_view id, double value); // false when the id is unknown
    std::span<const double> values() const { return values_; }

    // The definition that governs a value. For a rack slot's generic parameters this is the
    // loaded effect's parameter (its name, range and unit); otherwise the registry entry.
    const ParamDef& effectiveDef(int index) const;
    // False for a value that belongs to something the patch does not have: a parameter of an
    // empty rack slot, a generic slot parameter the loaded effect does not use, or the amount of
    // a route that does not exist. Such values stay at their defaults; set() leaves them alone.
    bool inUse(int index) const;

    // Effects rack. lane: 0 = A, 1 = B, 2 = Master. Slots are dense: no gaps.
    int fxSlotCount(int lane) const { return static_cast<int>(fx_[static_cast<std::size_t>(lane)].size()); }
    int fxSlotType(int lane, int slot) const; // effect index, -1 when the slot is empty
    // Appends a slot holding the effect at its defaults; returns its position or -1 when the lane is full.
    int addFxSlot(int lane, int effectIndex);
    void clearFx(int lane);
    // Removes a route and moves the amounts of the routes after it down by one.
    void removeRoute(int route);

    // Rack editing. Values, drawn curves and routes onto a slot's parameters move with its effect;
    // routes onto a removed effect are removed. Each returns false when the edit is not possible
    // and otherwise fills `moves` (when given) for Engine::setRack.
    bool insertFxSlot(int lane, int position, int effectIndex, FxMoves* moves = nullptr);
    bool removeFxSlot(int lane, int position, FxMoves* moves = nullptr);
    bool moveFxSlot(int fromLane, int fromPosition, int toLane, int toPosition, FxMoves* moves = nullptr);
    // Takes the whole rack (lanes, slot values, drawn curves and the rack's routing values) from
    // another patch, for "lock rack" across preset changes. Routes onto slot parameters are dropped.
    void copyRackFrom(const Patch& other);

    PatchMeta meta;
    std::array<std::string, kNumOscs> tables;       // wavetable reference per oscillator; empty = built-in
    // A table kept in the patch replaces the reference's file; the reference stays as the table's
    // origin (shown as its name, flagged as modified).
    std::array<EmbeddedTable, kNumOscs> tableData;
    std::string textureSample;                      // sample reference for the texture player
    std::array<std::string, kNumMacros> macroNames; // empty = "Macro N"
    std::vector<ModRoute> routes;                   // at most kNumModRoutes; route i uses mod.(i+1).amount

    // Structure that is not a parameter (milestone 3). An empty shape means the built-in one.
    std::array<Shape, kNumLfos> lfoShapes;                     // y from -1 to 1; empty = a sine
    std::array<EnvShape, kNumEnvs> envShapes;                  // multi-segment mode (envelopes 2 to 4)
    // Envelope times given as note lengths ("1/8", "3/16", "1/4T"): delay, attack, hold, decay,
    // release. A non-empty entry replaces the time in milliseconds and follows the tempo.
    std::array<std::array<std::string, 5>, kNumEnvs> envSync;
    std::array<std::array<Shape, 2>, kNumOscs> warpCurves;     // drawn phase curves of warp A and B; y from 0 to 1
    PatchTuning tuning;                                        // empty = equal temperament

    // Milestone 4: effects and play.
    std::array<std::array<std::array<Shape, 2>, kNumFxSlots>, kNumFxLanes> fxCurves; // Drive's curves A and B; y 0..1
    std::array<ArpPattern, kNumArpPatterns> arpPatterns;
    std::vector<int> chordNotes = {0, 4, 7}; // chord memory: semitones above the played key
    std::array<Clip, kNumClips> clips;

    bool operator==(const Patch&) const = default;

private:
    std::vector<double> values_;
    std::array<std::vector<int>, kNumFxLanes> fx_;
};

} // namespace zed
