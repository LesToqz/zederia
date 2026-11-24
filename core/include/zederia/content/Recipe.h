// Recipes: factory wavetables and textures are not stored as audio but as short JSON documents
// that say how to make them (plan 3.2, 7, 9). Building a recipe is deterministic, so the origin
// of every factory sound is reproducible. The files live in content/.
//
// A table recipe:
//   { "kind": "table", "name": "Saw Morph", "category": "Basics", "frames": 4,
//     "morph": "spectral", "stepped": false,
//     "steps": [ { "op": "formula", "domain": "harmonics", "expr": "1/h" }, ... ] }
// Steps (each may carry "frames": [first, last] to limit its scope):
//   frames {count}                       resize the table (new frames are silent)
//   formula {domain, expr, phase, selected, seed, normalize}
//   process {name, n}                    a ProcessOp by name
//   harmonic {name, n, amount, seed}     a HarmonicOp by name
//   draw {shape, from, to, a, b}         a DrawShape by name
//   noise {from, to, amount, seed}
//   morph {mode}   bake_morph {frames}   sort {by, descending}   reverse   thin {keep_every}
//   crop   duplicate   delete   move {from, to}   split   join
//   pulse_width_sweep {frame, count, narrowest}   copy_spectrum {source}   copy_phases {source}
//   bake_warp {position, mode, count}    trim_silent {threshold_db}   stepped {value}
//
// A texture recipe:
//   { "kind": "texture", "name": "Pink Bed", "seconds": 3, "rate": 48000, "loop": true,
//     "seed": 7, "peak_db": -3, "layers": [ { "type": "noise", "color": "pink", "level": 1 } ] }
// Layer types: noise {color: white|pink|brown, lowpass, highpass, width}, crackle {density,
// decay_ms, lowpass}, wind {center_hz, sweep_octaves, rate_hz, q}, grit {hold_hz, bits},
// burst {at, decay_ms, color, lowpass, highpass}, tone {at, hz, decay_ms, drop_octaves},
// partials {at, hz, ratios: [...], levels: [...], decay_ms, decay_tilt, spread_cents} (struck
// partials, each decaying faster the higher it is), hum {hz, harmonics, tilt_db} (a steady tone; in
// a loop its pitch is nudged so whole cycles fit), chirp {from_hz, to_hz, at, duration, noise, q}
// (a sine, or a band of noise, gliding evenly in pitch). Every layer has "level", and may have
// "fade_in" and "fade_out" in seconds (from the start, towards the end of the texture).
//
// A clip recipe (built into a Standard MIDI File):
//   { "kind": "clip", "name": "Minor Drive", "tempo": 124, "bars": 8, "beats_per_bar": 4,
//     "key": "F minor",
//     "parts": [ { "chords": ["i", "VI", "III", "VII"], "beats_per_chord": 8, "centre": 60,
//                  "low": 53, "high": 76, "bass": true, "hits": [[0, 8]], "velocity": 0.7,
//                  "bass_velocity": 0.8 },
//                { "notes": "0:1.5:C5:0.8 1.5:0.5:Ab4", "repeat": 2, "every": 16 } ] }
// Times are in beats. A chord part repeats its chords until the clip is full; each chord is
// voice-led from the one before and played at every "hit" (start and length inside the chord).
// A notes part lists start:length:key[:velocity] (velocity 0..1 or 1..127), copied "repeat"
// times "every" beats. A factory clip must pass every check of the clip validator (notes in the
// piano's range, no overlaps on one key, nothing past the end, whole bars, inside the key unless
// "chromatic" is true).
//
// A content folder (content/) holds, besides the recipes: presets/**.zpreset, shapes/**.zshape
// and racks/**.zrack (effect presets in racks/Effects/**.zfx). They are checked and copied, line
// endings made LF, so the factory folder is the same from any checkout.
#pragma once

#include "zederia/audio/WavFile.h"
#include "zederia/patch/Patch.h"
#include "zederia/table/Wavetable.h"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace zed {

// "table", "texture", "clip" or "" when the document is none of them.
std::string recipeKind(const nlohmann::json& recipe);
// Name and category of a recipe ("" when absent).
std::string recipeName(const nlohmann::json& recipe);
std::string recipeCategory(const nlohmann::json& recipe);

// Both return false with a message that names the step or layer and the problem.
bool buildTableRecipe(const nlohmann::json& recipe, Wavetable& out, std::string& error);
// Applies a list of recipe steps (the same vocabulary as "steps" above) to an existing table, in
// order. On an error the table is left as it was and `error` names the step and the problem.
bool applyTableSteps(const nlohmann::json& steps, Wavetable& table, std::string& error);
bool buildTextureRecipe(const nlohmann::json& recipe, WavData& out, std::string& error);
bool buildClipRecipe(const nlohmann::json& recipe, Clip& out, std::string& error);

// Builds every recipe found under `recipesDir` (searched through its subfolders) into
// `outDir`/Wavetables/<Category>/<Name>.wav, `outDir`/Textures/<Name>.wav and
// `outDir`/Clips/<Name>.mid, and copies the presets, shapes and racks it holds to
// `outDir`/Presets, `outDir`/Shapes and `outDir`/Racks after checking each one.
struct ContentBuild {
    int tables = 0, textures = 0, clips = 0, presets = 0, shapes = 0, racks = 0;
    std::vector<std::string> written; // files, relative to outDir
    std::vector<std::string> errors;  // one line per recipe or file that failed
};
ContentBuild buildContentFolder(const std::string& recipesDir, const std::string& outDir);

} // namespace zed
