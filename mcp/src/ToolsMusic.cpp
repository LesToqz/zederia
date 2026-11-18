                         tFlats = key.valid() && music::prefersFlats(key.tonic, key.scale->minorLike);
#include "Tools.h"

#include "zederia/music/Theory.h"
#include "zederia/params/Registry.h"
#include "zederia/params/ValueText.h"

#include <algorithm>

namespace zmcp {

using zed::Patch;
namespace music = zed::music;

namespace {

thread_local bool tFlats = false;

std::string keyNoteName(int n) { return music::pitchClassName(n % 12, tFlats) + std::to_string(n / 12 - 1); }

J noteNames(const std::vector<int>& notes) {
    J out = J::array();
    for (const int n : notes)
        out.push_back(keyNoteName(n));
    return out;
}

music::Key keyArg(const J& a, bool required) {
    music::Key k;
    if (!a.contains("key")) {
        if (required)
            fail("/key", "this action needs a key", "add \"key\": \"F minor\" (tonic and scale)");
        return k;
    }
    if (!music::parseKey(argString(a, "key"), k)) {
        std::string list;
        for (const auto& s : music::scales())
            list += (list.empty() ? "" : ", ") + s.id;
        fail("/key", "cannot read the key \"" + argString(a, "key") + "\"", "write a tonic and a scale, such as \"F minor\" or \"D dorian\"; scales: " + list);
    }
    return k;
}

J chordJson(const music::Chord& c, const std::vector<int>& notes, int bass) {
    J iv = J::array();
    for (const int i : c.intervals)
        iv.push_back(i);
    J o = {{"symbol", c.symbol}, {"root", music::pitchClassName(c.root, tFlats)}, {"intervals", iv}, {"notes", notes}, {"names", noteNames(notes)}};
    if (bass >= 0) {
        o["bass"] = bass;
        o["bass_name"] = keyNoteName(bass);
    }
    return o;
}

} // namespace

void addMusicTools(std::vector<ToolDef>& tools, Bridge& bridge) {
    // --- theory ---
    tools.push_back({"theory", "Music theory helper",
                     "Turns chord symbols (Fm7, Db/F, Csus4, G7b9) and roman numerals in a key (i iv VI v, bVII) into notes; lists a "
                     "key's scale; voice-leads a progression (each chord in the inversion that moves least, inside low..high, optionally "
                     "with a bass note) and returns ready clip notes; quantizes notes to a key; lists the scales. Notes are MIDI numbers "
                     "(60 = C4).",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["action"],"properties":{
                        "action":{"enum":["chord","scale","progression","quantize","scales"]},
                        "symbols":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"string"},"description":"chord symbols or roman numerals (with key)"},
                        "key":{"type":"string","description":"tonic and scale, such as \"F minor\""},
                        "notes":{"type":"array","maxItems":4096,"items":{"type":["integer","string"]},"description":"for quantize"},
                        "octave":{"type":"integer","minimum":-1,"maximum":8,"description":"chord and scale: the octave to start in (default 3; C4 = 60)"},
                        "low":{"type":"integer","minimum":0,"maximum":127,"description":"progression: lowest note of the voicings (default 53, F3)"},
                        "high":{"type":"integer","minimum":0,"maximum":127,"description":"progression: highest note (default 76, E5)"},
                        "centre":{"type":"integer","minimum":0,"maximum":127,"description":"progression: where the first chord sits (default 64)"},
                        "bass":{"type":"boolean","description":"progression: add each chord's root (or slash bass) an octave below"},
                        "beats_per_chord":{"type":"number","exclusiveMinimum":0,"maximum":64,"description":"progression: length of each chord in the returned clip notes (default 4)"},
                        "velocity":{"type":"number","minimum":0,"maximum":127},
                        "direction":{"enum":["nearest","up","down"],"description":"quantize: where notes outside the key go"}}})json"),
                     schema(R"json({"type":"object","required":["action"],"properties":{"action":{"type":"string"},"key":{"type":"string"},
                        "chords":{"type":"array","items":{"type":"object","required":["symbol","notes","names"]}},
                        "scale":{"type":"object"},"notes":{"type":"array"},"clip_notes":{"type":"array"},"scales":{"type":"array"},"length_beats":{"type":"number"}}})json"),
                     true, false, true, [](const J& a, Call&) {
                         const std::string action = argString(a, "action");
                         ToolOutput out;
                         out.structured["action"] = action;
                         const music::Key key = keyArg(a, action == "scale" || action == "quantize");
                         if (key.valid())
                             out.structured["key"] = key.name();
                         if (action == "scales") {
                             J list = J::array();
                             for (const auto& s : music::scales())
                                 list.push_back({{"id", s.id}, {"steps", s.steps}});
                             out.structured["scales"] = list;
                             out.summary = std::to_string(list.size()) + " scales.";
                             return out;
                         }
                         if (action == "scale") {
                             const int base = (argInt(a, "octave", 3) + 1) * 12 + key.tonic;
                             std::vector<int> notes;
                             for (const int s : key.scale->steps)
                                 notes.push_back(base + s);
                             notes.push_back(base + 12);
                             J pcs = J::array();
                             for (const int s : key.scale->steps)
                                 pcs.push_back(music::pitchClassName(key.tonic + s, music::prefersFlats(key.tonic, key.scale->minorLike)));
                             out.structured["scale"] = {{"name", key.name()}, {"pitch_classes", pcs}, {"notes", notes}, {"names", noteNames(notes)}};
                             std::string text;
                             for (const auto& p : pcs)
                                 text += (text.empty() ? "" : " ") + p.get<std::string>();
                             out.summary = key.name() + ": " + text + ".";
                             return out;
                         }
                         if (action == "quantize") {
                             if (!a.contains("notes"))
                                 fail("/notes", "quantize needs notes", "give MIDI numbers or names");
                             const std::string dir = argString(a, "direction", "nearest");
                             J list = J::array();
                             int moved = 0;
                             for (std::size_t i = 0; i < a["notes"].size(); ++i) {
                                 const int n = noteArg(a["notes"][i], "/notes/" + std::to_string(i));
                                 const int q = music::quantize(n, key, dir == "up" ? 1 : dir == "down" ? -1 : 0);
                                 moved += q != n ? 1 : 0;
                                 list.push_back({{"from", n}, {"to", q}, {"name", keyNoteName(q)}});
                             }
                             out.structured["notes"] = list;
                             out.summary = std::to_string(moved) + " of " + std::to_string(list.size()) + " note(s) moved into " + key.name() + ".";
                             return out;
                         }
                         if (!a.contains("symbols"))
                             fail("/symbols", "this action needs chord symbols", "give [\"Fm\", \"Db\", \"Ab\", \"Eb\"] or numerals with a key: [\"i\", \"VI\", \"III\", \"VII\"]");
                         std::vector<music::Chord> chords;
                         for (std::size_t i = 0; i < a["symbols"].size(); ++i) {
                             music::Chord c;
                             std::string error;
                             if (!music::parseChordOrRoman(a["symbols"][i].get<std::string>(), key.valid() ? &key : nullptr, c, error))
                                 fail("/symbols/" + std::to_string(i), error, "write symbols such as \"Fm7\", \"Bb/D\", \"Csus4\", or numerals with a key");
                             chords.push_back(c);
                         }
                         J list = J::array();
                         if (action == "chord") {
                             const int lowest = (argInt(a, "octave", 3) + 1) * 12;
                             for (const auto& c : chords) {
                                 auto notes = music::closeVoicing(c, lowest);
                                 int bass = -1;
                                 if (c.bass >= 0) {
                                     bass = lowest - 12 + ((c.bass - (lowest - 12)) % 12 + 12) % 12;
                                     notes.insert(notes.begin(), bass);
                                 }
                                 list.push_back(chordJson(c, notes, -1));
                             }
                             out.structured["chords"] = list;
                             out.summary = std::to_string(list.size()) + " chord(s).";
                             return out;
                         }
                         // progression
                         const int low = argInt(a, "low", 53), high = argInt(a, "high", 76);
                         if (high - low < 12)
                             fail("/high", "the range low..high must span at least an octave", "widen low and high");
                         const auto voicings = music::voiceLead(chords, argInt(a, "centre", 64), low, high, argBool(a, "bass", false));
                         const double beats = argNumber(a, "beats_per_chord", 4.0);
                         const double vel = velocityArg(a.value("velocity", J(90)), 0.7);
                         J clipNotes = J::array();
                         for (std::size_t i = 0; i < chords.size(); ++i) {
                             list.push_back(chordJson(chords[i], voicings[i].notes, voicings[i].bass));
                             for (const int n : voicings[i].notes)
                                 clipNotes.push_back({{"start", beats * static_cast<double>(i)}, {"length", beats}, {"key", n}, {"velocity", vel}});
                             if (voicings[i].bass >= 0)
                                 clipNotes.push_back({{"start", beats * static_cast<double>(i)}, {"length", beats}, {"key", voicings[i].bass}, {"velocity", vel}});
                         }
                         out.structured["chords"] = list;
                         out.structured["clip_notes"] = clipNotes;
                         out.structured["length_beats"] = beats * static_cast<double>(chords.size());
                         std::string text;
                         for (const auto& c : list) {
                             std::string names;
                             for (const auto& n : c["names"])
                                 names += (names.empty() ? "" : " ") + n.get<std::string>();
                             text += (text.empty() ? "" : " | ") + c["symbol"].get<std::string>() + ": " + names;
                         }
                         out.summary = text + ". clip_notes is ready for write_clip.";
                         return out;
                     }});

    // --- write_clip ---
    tools.push_back({"write_clip", "Write a clip",
                     "Creates or replaces a clip: notes with start and length in beats, key (MIDI number or name) and velocity (0..1 or "
                     "1..127), the clip's bars, beats per bar, tempo and key. Checks range, overlaps, key membership and bar length and "
                     "reports problems (errors stop the write; warnings are notes). Returns a clip id for render_audio and export_pack. "
                     "With target and slot (1..8) the clip also goes into that target's clip player; activate: true switches the clip "
                     "player on so a played key starts and transposes the clip.",
                     schema(R"json({"type":"object","additionalProperties":false,"required":["name","bars","tempo","notes"],"properties":{
                        "clip_id":{"type":"string","description":"replace this clip"},
                        "name":{"type":"string","minLength":1,"maxLength":60,"description":"used in file names, such as \"chords\" or \"melody\""},
                        "bars":{"type":"number","exclusiveMinimum":0,"maximum":64},
                        "beats_per_bar":{"type":"integer","minimum":1,"maximum":16},
                        "tempo":{"type":"number","minimum":20,"maximum":400},
                        "key":{"type":"string","description":"such as \"F minor\"; notes outside it are reported"},
                        "root":{"type":["integer","string"],"description":"the key that plays the clip untransposed (default: the key's tonic near C4)"},
                        "notes":{"type":"array","maxItems":2048,"items":{"type":"object","additionalProperties":false,"required":["start","length"],"properties":{
                            "start":{"type":"number","minimum":0},"length":{"type":"number","exclusiveMinimum":0},
                            "key":{"type":["integer","string"]},"note":{"type":["integer","string"],"description":"the same as key"},
                            "velocity":{"type":"number","minimum":0,"maximum":127}}}},
                        "target":{"type":"string"},"slot":{"type":"integer","minimum":1,"maximum":8},"activate":{"type":"boolean"}}})json"),
                     schema(R"json({"type":"object","required":["clip_id","name","notes","length_beats","issues"],"properties":{"clip_id":{"type":"string"},
                        "name":{"type":"string"},"notes":{"type":"integer"},"length_beats":{"type":"number"},"key":{"type":"string"},
                        "issues":{"type":"array","items":{"type":"object","required":["severity","code","path","message","fix"]}},"loaded_into":{"type":"object"}}})json"),
                     false, false, false, [&bridge](const J& a, Call& call) {
                         for (std::size_t i = 0; i < a["notes"].size(); ++i)
                             if (!a["notes"][i].contains("key") && !a["notes"][i].contains("note"))
                                 fail("/notes/" + std::to_string(i), "the note has no key", "add \"key\": 60 or a name such as \"F3\"");
                         zed::Clip clip = clipFromArgs(a);
                         music::Key key;
                         if (!clip.key.empty()) {
                             key = keyArg(a, false);
                             clip.key = key.name();
                             if (!a.contains("root"))
                                 clip.root = 60 + ((key.tonic - 60) % 12 + 12) % 12 - (key.tonic > 6 ? 12 : 0);
                         }
                         const auto issues = music::validateClip(clip, key);
                         J list = J::array();
                         std::vector<Problem> errors;
                         for (const auto& is : issues) {
                             list.push_back({{"severity", is.error ? "error" : "warning"}, {"code", is.code}, {"path", is.path}, {"message", is.message}, {"fix", is.fix}});
                             if (is.error)
                                 errors.push_back({is.path, is.message, is.fix});
                         }
                         if (!errors.empty())
                             throw ToolError{errors};
                         std::string id = argString(a, "clip_id", "");
                         if (!id.empty()) {
                             zed::Clip old;
                             if (!bridge.loadClip(id, old))
                                 fail("/clip_id", "no clip \"" + id + "\" to replace", "leave clip_id out to make a new clip");
                         }
                         id = bridge.saveClip(clip, id);
                         ToolOutput out;
                         out.structured = {{"clip_id", id}, {"name", clip.name}, {"notes", clip.notes.size()}, {"length_beats", clip.lengthBeats()},
                                           {"issues", list}};
                         if (key.valid())
                             out.structured["key"] = key.name();
                         if (a.contains("target")) {
                             const std::string t = argString(a, "target");
                             bridge.checkTarget(t);
                             const int slot = argInt(a, "slot", 1);
                             const bool activate = argBool(a, "activate", false);
                             bridge.edit(t, call, "Clip " + std::to_string(slot) + ": " + clip.name, [&](Patch& p) {
                                 p.clips[static_cast<std::size_t>(slot - 1)] = clip;
                                 if (activate) {
                                     p.set("clip.on", 1.0);
                                     p.set("clip.slot", static_cast<double>(slot));
                                 }
                             });
                             out.structured["loaded_into"] = {{"target", t}, {"slot", slot}, {"active", activate}};
                         } else if (a.contains("slot")) {
                             fail("/slot", "slot needs a target", "add the target whose clip player gets the clip");
                         }
                         int warnings = 0;
                         for (const auto& is : issues)
                             warnings += is.error ? 0 : 1;
                         out.summary = "Clip " + id + " \"" + clip.name + "\": " + std::to_string(clip.notes.size()) + " notes over " +
                                       std::to_string(static_cast<int>(clip.bars)) + " bar(s)" + (warnings ? ", " + std::to_string(warnings) + " warning(s)" : "") +
                                       (out.structured.contains("loaded_into") ? ", in clip slot " + std::to_string(argInt(a, "slot", 1)) : "") + ".";
                         return out;
                     }});

    // --- get_clip ---
    tools.push_back({"get_clip", "Read a clip", "Reads a clip by its id, or the clip in a target's clip player slot.",
                     schema(R"json({"type":"object","additionalProperties":false,"properties":{
                        "clip_id":{"type":"string"},"target":{"type":"string"},"slot":{"type":"integer","minimum":1,"maximum":8}}})json"),
                     schema(R"json({"type":"object","required":["clip"],"properties":{"clip_id":{"type":"string"},
                        "clip":{"type":"object","required":["name","bars","beats_per_bar","tempo","key","notes"]}}})json"),
                     true, false, true, [&bridge](const J& a, Call& call) {
                         zed::Clip c;
                         ToolOutput out;
                         if (a.contains("clip_id")) {
                             if (!bridge.loadClip(argString(a, "clip_id"), c))
                                 fail("/clip_id", "no clip \"" + argString(a, "clip_id") + "\"", "use an id from write_clip");
                             out.structured["clip_id"] = argString(a, "clip_id");
                         } else if (a.contains("target")) {
                             const std::string t = argString(a, "target");
                             bridge.checkTarget(t);
                             const int slot = argInt(a, "slot", 1);
                             c = bridge.patch(t, call).clips[static_cast<std::size_t>(slot - 1)];
                         } else {
                             fail("", "say which clip", "give clip_id, or target and slot");
                         }
                         out.structured["clip"] = clipToJson(c);
                         out.summary = clipToText(c) + ".";
                         return out;
                     }});
}

} // namespace zmcp
