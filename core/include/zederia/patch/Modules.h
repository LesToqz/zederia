// Copy and paste for every module and shape (plan 1.1 "Nothing is a dead end", 3.11), and the
// module presets of the library. A module travels as a small JSON document holding its section of
// a patch, written and read by the patch code itself, so the same names, units and checks apply:
//   { "format": "zederia.module", "version": 1, "class": "lfo", "module": "lfo3",
//     "content": { ...the "lfo3" section of a patch, with its shape... } }
// Like modules accept each other's content: an LFO pastes into any LFO, an oscillator (with its
// table and warp curves) into any oscillator, an envelope into any envelope.
#pragma once

#include "zederia/patch/PatchIO.h"

#include <string>
#include <string_view>
#include <vector>

namespace zed {

inline constexpr std::string_view kModuleFormat = "zederia.module";
inline constexpr std::string_view kShapeFormat = "zederia.shape";

// The class of a module id: "osc", "sub", "texture", "filter", "env", "lfo", "flux", "voice",
// "arp", "chord", "clip" (with a slot: "clip3"), "arp_pattern" (with a slot: "arp_pattern2"),
// "fx_slot" ("fx.a3"), "fx_rack" ("fx"); "" for anything else.
std::string moduleClass(std::string_view moduleId);
// A short name for the interface: "Oscillator", "LFO", "Effect slot" ...
std::string moduleClassName(std::string_view moduleClass);

// The module's section of the patch as a module document; "" when the id is not a module.
std::string copyModule(const Patch& patch, std::string_view moduleId);
// Replaces the module's values and structure with a module document of the same class. Nothing
// changes when the document has errors or is of another class.
bool pasteModule(std::string_view json, Patch& patch, std::string_view moduleId, std::vector<Issue>& issues);
// The class a module document holds ("" when it is not one).
std::string moduleDocumentClass(std::string_view json);

// --- Shapes ------------------------------------------------------------------------------------
// LFO shapes (y -1..1), and the curves whose y runs 0..1: warp curves, Drive's curves, a route's
// drawn curve. A shape pastes into any of them; y is mapped between the two ranges.
enum class ShapeKind { Lfo, Curve };
std::string copyShape(const Shape& shape, ShapeKind kind);
bool pasteShape(std::string_view json, ShapeKind kind, Shape& out, std::vector<Issue>& issues);
// A multi-segment envelope as a shape and back (for pasting between an envelope and an LFO or
// curve): points spread over the envelope's own times, levels 0..1.
std::string copyEnvShape(const EnvShape& shape);
bool pasteEnvShape(std::string_view json, EnvShape& out, std::vector<Issue>& issues);

} // namespace zed
