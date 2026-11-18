// The tool catalogue (plan 4.4). Every tool has an input schema, an output schema that its
// structured result validates against, annotations (read-only, destructive, idempotent) and a
// handler. Handlers throw ToolError with paths and fixes for anything the agent can correct.
#pragma once

#include "Bridge.h"

#include "zederia/patch/Patch.h"

#include <functional>
#include <string>
#include <vector>

namespace zmcp {

struct ToolOutput {
    std::string summary;           // a sentence or two for the model
    J structured = J::object();    // validates against the tool's output schema
    std::vector<J> content;        // more content blocks: resource links, images
};

struct ToolDef {
    std::string name, title, description;
    J input, output;
    bool readOnly = false, destructive = false, idempotent = false;
    std::function<ToolOutput(const J& args, Call& call)> run;
};

// All tools in the catalogue's order (deterministic, plan 4.3).
std::vector<ToolDef> buildTools(Bridge& bridge);
void addCoreTools(std::vector<ToolDef>& tools, Bridge& bridge);
void addLibraryTools(std::vector<ToolDef>& tools, Bridge& bridge);
void addAudioTools(std::vector<ToolDef>& tools, Bridge& bridge);
void addMusicTools(std::vector<ToolDef>& tools, Bridge& bridge);

// --- Helpers shared by the tool files ------------------------------------------------------------------
J schema(const char* json); // parses a schema written as JSON text
std::string argString(const J& args, const char* key, const std::string& fallback = {});
double argNumber(const J& args, const char* key, double fallback);
int argInt(const J& args, const char* key, int fallback);
bool argBool(const J& args, const char* key, bool fallback);
// A MIDI note given as a number or a name ("C4" is 60).
int noteArg(const J& value, const std::string& path);
// Velocity as 0..1 or 1..127.
double velocityArg(const J& value, double fallback);
// The text added to every description of a tool that returns text from files.
extern const char* kUntrustedNote;
// Schemas reused by several tools.
J targetSchema();
J notesSchema(); // [{note, start, length, velocity}]
// A clip from write_clip-style arguments (name, bars, beats_per_bar, tempo, key, root, notes).
zed::Clip clipFromArgs(const J& args);
J clipToJson(const zed::Clip& clip);
std::string displayName(const zed::Patch& patch);

} // namespace zmcp
