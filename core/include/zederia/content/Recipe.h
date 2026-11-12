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
// burst {at, decay_ms, color, lowpass, highpass}, tone {at, hz, decay_ms, drop_octaves}.
// Every layer has "level".
#pragma once

#include "zederia/audio/WavFile.h"
#include "zederia/table/Wavetable.h"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace zed {

// "table", "texture" or "" when the document is neither.
std::string recipeKind(const nlohmann::json& recipe);
// Name and category of a recipe ("" when absent).
std::string recipeName(const nlohmann::json& recipe);
std::string recipeCategory(const nlohmann::json& recipe);

// Both return false with a message that names the step or layer and the problem.
bool buildTableRecipe(const nlohmann::json& recipe, Wavetable& out, std::string& error);
bool buildTextureRecipe(const nlohmann::json& recipe, WavData& out, std::string& error);

// Builds every recipe found under `recipesDir` (searched through its subfolders) into
// `outDir`/Wavetables/<Category>/<Name>.wav and `outDir`/Textures/<Name>.wav.
struct ContentBuild {
    int tables = 0, textures = 0;
    std::vector<std::string> written; // files, relative to outDir
    std::vector<std::string> errors;  // one line per recipe that failed
};
ContentBuild buildContentFolder(const std::string& recipesDir, const std::string& outDir);

} // namespace zed
