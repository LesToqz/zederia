#include "Protocol.h"

#include "zederia/Version.h"
#include "zederia/params/ParamDocs.h"
#include "zederia/params/Registry.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace zmcp {

const std::vector<std::string>& legacyVersions() {
    static const std::vector<std::string> v = {"2025-11-25", "2025-06-18"};
    return v;
}

const std::vector<std::string>& supportedVersions() {
    static const std::vector<std::string> v = {"2026-07-28", "2025-11-25", "2025-06-18"};
    return v;
}

namespace {

constexpr int kListTtlMs = 300000;

bool isLegacyVersion(const std::string& v) {
    const auto& l = legacyVersions();
    return std::find(l.begin(), l.end(), v) != l.end();
}

std::string clientNameOf(const J& info) {
    if (!info.is_object())
        return "agent";
    std::string n = info.value("title", "");
    if (n.empty())
        n = info.value("name", "");
    // Shown in undo labels and the activity log: keep it short and printable.
    std::string clean;
    for (const char c : n)
        if (static_cast<unsigned char>(c) >= 32 && c != '\x7f')
            clean.push_back(c);
    if (clean.size() > 40)
        clean.resize(40);
    return clean.empty() ? "agent" : clean;
}

// The schema of the patch document, generated from the parameter registry: every value at its
// patch pointer with its type and range, plus the sections that are not parameters.
J patchSchema() {
    J root = {{"$schema", "https://json-schema.org/draft/2020-12/schema"},
              {"$id", "zederia://schema/patch"},
              {"title", "Zederia patch"},
              {"type", "object"},
              {"properties", J::object()}};
    root["properties"]["format"] = {{"const", "zederia.patch"}};
    root["properties"]["version"] = {{"type", "integer"}};
    root["properties"]["meta"] = {{"type", "object"},
                                  {"properties",
                                   {{"name", {{"type", "string"}}}, {"author", {{"type", "string"}}}, {"type", {{"type", "string"}}},
                                    {"pack", {{"type", "string"}}}, {"notes", {{"type", "string"}}}, {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                    {"genres", {{"type", "array"}, {"items", {{"type", "string"}}}}}, {"rating", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5}}}}}};
    for (const auto& p : zed::Registry::get().params()) {
        if (p.pointer.empty() || p.pointer.rfind("/mod/", 0) == 0 || p.pointer.rfind("/fx/", 0) == 0 || p.pointer.rfind("/macros/", 0) == 0)
            continue;
        J* node = &root;
        std::stringstream ss(p.pointer.substr(1));
        std::string part;
        std::vector<std::string> parts;
        while (std::getline(ss, part, '/'))
            parts.push_back(part);
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            J& props = (*node)["properties"];
            if (!props.contains(parts[i]))
                props[parts[i]] = {{"type", "object"}, {"properties", J::object()}};
            node = &props[parts[i]];
        }
        J leaf = {{"description", p.name + ": " + p.description}};
        switch (p.type) {
        case zed::ParamType::Bool: leaf["type"] = "boolean"; break;
        case zed::ParamType::Choice: leaf["enum"] = p.choices; break;
        case zed::ParamType::Int: leaf["type"] = "integer"; leaf["minimum"] = p.min; leaf["maximum"] = p.max; break;
        default: leaf["type"] = "number"; leaf["minimum"] = p.min; leaf["maximum"] = p.max; break;
        }
        (*node)["properties"][parts.back()] = leaf;
    }
    J effects = J::array();
    for (const auto& e : zed::Registry::get().effectTypes())
        effects.push_back(e.id);
    const J slot = {{"type", "object"},
                    {"required", {"type"}},
                    {"properties", {{"type", {{"enum", effects}}}, {"on", {{"type", "boolean"}}}, {"mix", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}},
                    {"description", "an effect and its parameters by name (describe_synth module effect:<type>)"}};
    root["properties"]["fx"] = {{"type", "object"},
                                {"properties", {{"a", {{"type", "array"}, {"maxItems", 8}, {"items", slot}}},
                                                {"b", {{"type", "array"}, {"maxItems", 8}, {"items", slot}}},
                                                {"master", {{"type", "array"}, {"maxItems", 8}, {"items", slot}}}}}};
    root["properties"]["mod"] = {{"type", "array"},
                                 {"maxItems", 64},
                                 {"items",
                                  {{"type", "object"},
                                   {"required", {"source", "dest"}},
                                   {"properties",
                                    {{"source", {{"type", "string"}}}, {"via", {{"type", "string"}}}, {"dest", {{"type", "string"}}},
                                     {"amount", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}}, {"polarity", {{"enum", {"bi", "uni"}}}},
                                     {"curve", {{"enum", {"linear", "exp", "log", "s", "drawn"}}}}, {"enabled", {{"type", "boolean"}}},
                                     {"mute", {{"type", "boolean"}}}, {"solo", {{"type", "boolean"}}}}}}}};
    root["properties"]["macros"] = {{"type", "array"}, {"maxItems", 8},
                                    {"items", {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}, {"value", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}}}}}}};
    return root;
}

J guidePromptText(const std::string& text) {
    return {{"role", "user"}, {"content", {{"type", "text"}, {"text", text}}}};
}

} // namespace

const ToolDef* Server::findTool(const std::string& name) const {
    for (const auto& t : tools_)
        if (t.name == name)
            return &t;
    return nullptr;
}

Server::Server(Bridge& bridge) : bridge_(bridge), tools_(buildTools(bridge)) {}

J Server::error(const J& id, int code, const std::string& message, J data) {
    J e = {{"code", code}, {"message", message}};
    if (!data.is_null())
        e["data"] = std::move(data);
    J r = {{"jsonrpc", "2.0"}, {"error", e}};
    r["id"] = id.is_null() ? J() : id;
    return r;
}

Server::Era Server::eraOf(const J& m) {
    if (!m.is_object())
        return Era::Unknown;
    if (m.value("method", "") == "initialize")
        return Era::Legacy;
    const auto params = m.find("params");
    if (params != m.end() && params->is_object()) {
        const auto meta = params->find("_meta");
        if (meta != params->end() && meta->is_object() && meta->contains("io.modelcontextprotocol/protocolVersion"))
            return Era::Modern;
    }
    return Era::Unknown;
}

J Server::serverInfo() const {
    return {{"name", "zederia-mcp"}, {"title", "Zederia"}, {"version", zed::kVersion},
            {"description", "Design sounds, wavetables, presets, clips and sample packs with the Zederia synthesizer."}};
}

std::string Server::instructions() const {
    return "Zederia is a wavetable synthesizer. Work on a target: open_session for a headless session (no host needed) or a "
           "live:... instance from list_targets (the user hears it). Read zederia://guide/agent first: it explains the parameters, "
           "the design loop (brief -> start preset or init -> edit -> render a short phrase -> read the analysis -> adjust -> save "
           "with tags), loudness and naming conventions. Use describe_synth for parameter ids and ranges. Every edit is one undo "
           "step labelled with your name. Audio comes back as file paths plus measurements, never inline.";
}

J Server::toolList() const {
    J list = J::array();
    for (const auto& t : tools_) {
        J d = {{"name", t.name}, {"title", t.title}, {"description", t.description}, {"inputSchema", t.input}, {"outputSchema", t.output},
               {"annotations",
                {{"title", t.title}, {"readOnlyHint", t.readOnly}, {"destructiveHint", t.destructive}, {"idempotentHint", t.idempotent},
                 {"openWorldHint", false}}}};
        list.push_back(std::move(d));
    }
    return list;
}

std::vector<std::string> Server::outputSchemaFailures() const {
    std::lock_guard<std::mutex> lock(failuresMutex_);
    return outputFailures_;
}

J Server::callTool(const std::string& name, const J& arguments, RequestInfo& info) {
    const ToolDef* tool = findTool(name);
    if (!tool)
        return nullptr;
    const J args = arguments.is_null() ? J::object() : arguments;
    const auto textResult = [](const std::string& text, bool isError) {
        return J{{"content", J::array({{{"type", "text"}, {"text", text}}})}, {"isError", isError}};
    };
    if (!args.is_object())
        return textResult("Invalid arguments: \"arguments\" must be an object.", true);
    const auto problems = validate(tool->input, args);
    if (!problems.empty()) {
        J details = problemsToJson(problems);
        std::string text = "Invalid arguments for " + name + ":";
        for (const auto& p : problems)
            text += "\n- at " + (p.path.empty() ? std::string("(the arguments)") : p.path) + ": " + p.problem + ". Fix: " + p.fix + ".";
        return {{"content", J::array({{{"type", "text"}, {"text", text}}, {{"type", "text"}, {"text", J{{"errors", details}}.dump()}}})}, {"isError", true}};
    }
    Call call;
    call.client = info.clientName;
    call.cancelled = info.cancelled;
    const J token = info.progressToken;
    double last = -1.0;
    std::mutex progressMutex;
    call.progress = [&, token](double fraction, const std::string& message) {
        if (token.is_null())
            return;
        std::lock_guard<std::mutex> lock(progressMutex);
        const double f = std::clamp(fraction, 0.0, 1.0);
        if (f <= last + 1e-9) // progress must increase
            return;
        last = f;
        info.notify({{"jsonrpc", "2.0"},
                     {"method", "notifications/progress"},
                     {"params", {{"progressToken", token}, {"progress", std::round(f * 1000.0) / 1000.0}, {"total", 1.0}, {"message", message}}}});
    };
    try {
        ToolOutput out = tool->run(args, call);
        if (info.cancelled())
            throw Cancelled{};
        const auto failures = validate(tool->output, out.structured, 5);
        if (!failures.empty()) {
            std::lock_guard<std::mutex> lock(failuresMutex_);
            for (const auto& f : failures) {
                outputFailures_.push_back(name + " " + f.path + ": " + f.problem);
                std::cerr << "zederia-mcp: output of " << name << " does not match its schema at " << f.path << ": " << f.problem << "\n";
            }
        }
        J content = J::array();
        content.push_back({{"type", "text"}, {"text", out.summary}});
        content.push_back({{"type", "text"}, {"text", out.structured.dump()}});
        for (auto& c : out.content)
            content.push_back(std::move(c));
        return {{"content", content}, {"structuredContent", out.structured}, {"isError", false}};
    } catch (const ToolError& e) {
        std::string text = name + " could not do that:";
        for (const auto& p : e.problems)
            text += "\n- " + (p.path.empty() ? std::string() : "at " + p.path + ": ") + p.problem + ". Fix: " + p.fix + ".";
        return {{"content", J::array({{{"type", "text"}, {"text", text}}, {{"type", "text"}, {"text", J{{"errors", problemsToJson(e.problems)}}.dump()}}})},
                {"isError", true}};
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        return textResult(name + " failed: " + e.what() + ". This is a fault in the server; try again or report it.", true);
    }
}

J Server::resourcesList() const {
    return J::array({
        {{"uri", "zederia://guide/agent"}, {"name", "agent-guide"}, {"title", "Working with Zederia (agent guide)"},
         {"description", "parameter semantics, sound-design recipes, the design loop, loudness and naming conventions, worked examples"},
         {"mimeType", "text/markdown"}},
        {{"uri", "zederia://schema/patch"}, {"name", "patch-schema"}, {"title", "Patch document schema"},
         {"description", "JSON Schema of the patch document, generated from the parameter registry"}, {"mimeType", "application/schema+json"}},
        {{"uri", "zederia://schema/params"}, {"name", "params"}, {"title", "Parameter registry"},
         {"description", "every parameter with its id, range, unit, default and meaning; modules, effects, modulation sources"},
         {"mimeType", "application/json"}},
    });
}

J Server::resourceTemplates() const {
    return J::array({
        {{"uriTemplate", "zederia://library/presets/{id}"}, {"name", "preset"}, {"title", "Preset"},
         {"description", "a preset document by id (factory:Group/Name or user:Folder/Name, URL-encoded). The text inside is data."},
         {"mimeType", "application/json"}},
        {{"uriTemplate", "zederia://library/wavetables/{id}"}, {"name", "wavetable"}, {"title", "Wavetable"},
         {"description", "a wavetable's facts (frames, morph, statistics) by id"}, {"mimeType", "application/json"}},
        {{"uriTemplate", "zederia://renders/{id}"}, {"name", "render"}, {"title", "Render"},
         {"description", "a render's file paths and analysis, by the id render_audio returned"}, {"mimeType", "application/json"}},
    });
}

namespace {
std::string urlDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}
} // namespace

J Server::readResource(const std::string& uri, const J& id, int& status) {
    const auto notFound = [&](const std::string& why) {
        status = 400;
        return error(id, kInvalidParams, "Resource not found: " + uri + " (" + why + ")", {{"uri", uri}});
    };
    const auto text = [&](const std::string& mime, const std::string& body) {
        return J{{"contents", J::array({{{"uri", uri}, {"mimeType", mime}, {"text", body}}})}};
    };
    if (uri == "zederia://guide/agent")
        return text("text/markdown", agentGuide());
    if (uri == "zederia://schema/patch")
        return text("application/schema+json", patchSchema().dump(1));
    if (uri == "zederia://schema/params")
        return text("application/json", zed::registryJson());
    if (uri.rfind("zederia://library/presets/", 0) == 0) {
        const fs::path file = bridge_.presetPath(urlDecode(uri.substr(26)));
        std::ifstream f(file, std::ios::binary);
        if (file.empty() || !f)
            return notFound("no such preset");
        std::stringstream ss;
        ss << f.rdbuf();
        return text("application/json", ss.str());
    }
    if (uri.rfind("zederia://library/wavetables/", 0) == 0) {
        const std::string ref = urlDecode(uri.substr(29));
        const std::string path = bridge_.library().tablePath(ref);
        std::vector<std::uint8_t> bytes;
        std::string err;
        zed::Wavetable t;
        if (path.empty() || !zed::readFile(path, bytes, err) || !zed::decodeWavetable(bytes, t, err))
            return notFound("no such wavetable");
        return text("application/json", J{{"id", ref}, {"name", t.name}, {"frames", t.size()}, {"morph", std::string(zed::morphModeName(t.morph))},
                                          {"stepped", t.stepped}, {"file", path}}.dump(1));
    }
    if (uri.rfind("zederia://renders/", 0) == 0) {
        const std::string rid = urlDecode(uri.substr(18));
        if (rid.empty() || rid.find_first_of("/\\:") != std::string::npos || rid.find("..") != std::string::npos)
            return notFound("not a render id");
        std::ifstream f(bridge_.userDir() / "Renders" / pathFromUtf8(rid + ".json"), std::ios::binary);
        if (!f)
            return notFound("no such render");
        std::stringstream ss;
        ss << f.rdbuf();
        return text("application/json", ss.str());
    }
    return notFound("unknown resource");
}

J Server::promptsList() const {
    const auto arg = [](const char* n, const char* d, bool req) { return J{{"name", n}, {"description", d}, {"required", req}}; };
    return J::array({
        {{"name", "design_sound"}, {"title", "Design a sound from a brief"},
         {"description", "the design loop: brief, start, edit, render, read the analysis, adjust, save with tags"},
         {"arguments", J::array({arg("brief", "what the sound should be, in words", true), arg("target", "a live target to work on (default: a new headless session)", false)})}},
        {{"name", "match_reference"}, {"title", "Match a reference sound"},
         {"description", "analyze a reference file, design towards it and compare after each change"},
         {"arguments", J::array({arg("path", "the reference WAV file", true), arg("note", "the note the reference plays, such as C3", false)})}},
        {{"name", "write_progression_loop"}, {"title", "Write a chord progression loop"},
         {"description", "theory for the progression, write_clip, render loops with a preset and the MIDI file"},
         {"arguments", J::array({arg("key", "such as F minor", true), arg("tempo", "bpm", false), arg("bars", "length in bars", false),
                                 arg("style", "genre or feel", false)})}},
        {{"name", "write_melody_loop"}, {"title", "Write a melody loop"},
         {"description", "a melody in a key over a given length, checked by write_clip, rendered as a loop with its MIDI file"},
         {"arguments", J::array({arg("key", "such as F minor", true), arg("tempo", "bpm", false), arg("bars", "length in bars", false),
                                 arg("chords", "the progression the melody sits over", false)})}},
        {{"name", "build_pack"}, {"title", "Build a preset pack"},
         {"description", "design, render, analyze and save a set of presets, tag them and export a sample pack"},
         {"arguments", J::array({arg("theme", "what the pack is, such as deep house basses", true), arg("count", "how many presets", false)})}},
    });
}

J Server::getPrompt(const J& params, const J& id, int& status) {
    const std::string name = params.value("name", "");
    const J args = params.value("arguments", J::object());
    const auto get = [&](const char* k, const char* def) { return args.contains(k) && args[k].is_string() ? args[k].get<std::string>() : std::string(def); };
    std::string text;
    if (name == "design_sound") {
        const std::string brief = get("brief", "");
        if (brief.empty()) {
            status = 400;
            return error(id, kInvalidParams, "design_sound needs the argument \"brief\"");
        }
        const std::string target = get("target", "");
        text = "Design a Zederia sound for this brief: \"" + brief + "\".\n\n1. Read zederia://guide/agent if you have not.\n2. " +
               (target.empty() ? std::string("open_session from init (or list_presets for a close starting point and open_session from it).")
                               : "Work on the live target " + target + " (the user hears it).") +
               "\n3. describe_synth for the modules you will use; set the sources, filter, envelopes and effects with apply_patch or set_params.\n"
               "4. render_audio a short phrase that suits the brief (for a pluck: four notes; for a pad: a held chord). Read the analysis: "
               "loudness, attack, decay, brightness (centroid), width, tags.\n5. Adjust and render again until the analysis matches the brief. "
               "Keep integrated loudness near -14 LUFS for one note and the peak below -1 dBFS.\n6. Give the macros useful names and ranges.\n"
               "7. save_preset with a name, a type and tags that describe the result. Report what you made and the file paths.";
    } else if (name == "match_reference") {
        const std::string path = get("path", "");
        if (path.empty()) {
            status = 400;
            return error(id, kInvalidParams, "match_reference needs the argument \"path\"");
        }
        text = "Match the reference sound " + path + ".\n\n1. analyze_audio on it (spectrogram: true if you can see images). Note its pitch, attack, "
               "decay, brightness, noisiness, width and movement.\n2. open_session and design towards it (" +
               (get("note", "").empty() ? std::string("play the reference's pitch") : "play " + get("note", "")) +
               ").\n3. After each change, render_audio a note of the same length and compare_audio (a = your render, b = the reference). "
               "Change one thing at a time and keep the change when the distance falls.\n4. Stop when the distance stops improving; "
               "save_preset and report the final comparison.";
    } else if (name == "write_progression_loop") {
        text = "Write a chord progression loop in " + get("key", "C minor") + " at " + get("tempo", "120") + " bpm, " + get("bars", "8") +
               " bars" + (get("style", "").empty() ? std::string() : ", in the style of " + get("style", "")) +
               ".\n\n1. theory action progression with roman numerals or chord symbols in the key (bass: true for a bass line); it returns "
               "clip_notes.\n2. write_clip with those notes, the bars, tempo and key; read its warnings.\n3. Pick a fitting preset "
               "(list_presets) or design one; render_audio with the clip (tail fold) for a loop WAV and its .mid.\n4. Report the files.";
    } else if (name == "write_melody_loop") {
        text = "Write a melody loop in " + get("key", "C minor") + " at " + get("tempo", "120") + " bpm, " + get("bars", "8") + " bars" +
               (get("chords", "").empty() ? std::string() : " over the chords " + get("chords", "")) +
               ".\n\n1. theory action scale for the key's notes (and chord for the progression's tones).\n2. Write the melody: phrases of two "
               "or four bars, chord tones on strong beats, a range of about an octave and a half, rests between phrases.\n3. write_clip and "
               "fix what it reports.\n4. render_audio with the clip and a lead or pluck preset for a loop WAV and its .mid. Report the files.";
    } else if (name == "build_pack") {
        text = "Build a pack of " + get("count", "10") + " presets: " + get("theme", "") +
               ".\n\nFor each: design with the loop in zederia://guide/agent (render, analyze, adjust), keep loudness consistent (about -14 LUFS "
               "per note), name it, save_preset with pack, type and tags. Then tag_presets to make the tags consistent, and export_pack with "
               "one_shots at three pitches. Report the folder and the files.";
    } else {
        status = 400;
        return error(id, kInvalidParams, "Unknown prompt: " + name);
    }
    return {{"description", name}, {"messages", J::array({guidePromptText(text)})}};
}

J Server::dispatch(const std::string& method, const J& params, RequestInfo& info, const J& id, int& status) {
    if (method == "tools/list") {
        if (params.contains("cursor") && !params["cursor"].is_null()) {
            status = 400;
            return error(id, kInvalidParams, "Invalid cursor: the tool list has one page");
        }
        J r = {{"tools", toolList()}};
        if (info.modern) {
            r["ttlMs"] = kListTtlMs;
            r["cacheScope"] = "public";
        }
        return r;
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        if (!findTool(name)) {
            status = 400;
            std::vector<std::string> names;
            for (const auto& t : tools_)
                names.push_back(t.name);
            const std::string near = closest(name, names);
            return error(id, kInvalidParams, "Unknown tool: " + name + (near.empty() ? "" : " (did you mean " + near + "?)"));
        }
        return callTool(name, params.value("arguments", J::object()), info);
    }
    if (method == "resources/list" || method == "resources/templates/list") {
        J r = method == "resources/list" ? J{{"resources", resourcesList()}} : J{{"resourceTemplates", resourceTemplates()}};
        if (info.modern) {
            r["ttlMs"] = kListTtlMs;
            r["cacheScope"] = "public";
        }
        return r;
    }
    if (method == "resources/read") {
        const std::string uri = params.value("uri", "");
        J r = readResource(uri, id, status);
        if (r.contains("error"))
            return r;
        if (info.modern) {
            const bool stable = uri == "zederia://guide/agent" || uri.rfind("zederia://schema/", 0) == 0;
            r["ttlMs"] = stable ? 3600000 : 0;
            r["cacheScope"] = stable ? "public" : "private";
        }
        return r;
    }
    if (method == "prompts/list") {
        J r = {{"prompts", promptsList()}};
        if (info.modern) {
            r["ttlMs"] = kListTtlMs;
            r["cacheScope"] = "public";
        }
        return r;
    }
    if (method == "prompts/get")
        return getPrompt(params, id, status);
    status = info.modern ? 404 : 400;
    return error(id, kMethodNotFound, "Method not found: " + method);
}

J Server::handle(const J& m, LegacyChannel* legacy, RequestInfo& info, int* httpStatus) {
    int status = 200;
    const auto finish = [&](J r) {
        if (httpStatus)
            *httpStatus = status;
        return r;
    };
    if (!m.is_object() || m.value("jsonrpc", "") != "2.0" || !m.contains("method") || !m["method"].is_string()) {
        status = 400;
        return finish(error(m.is_object() && m.contains("id") ? m["id"] : J(), kInvalidRequest, "Invalid request: expected a JSON-RPC 2.0 request or notification"));
    }
    const std::string method = m["method"].get<std::string>();
    const bool isRequest = m.contains("id") && !m["id"].is_null();
    const J id = isRequest ? m["id"] : J();
    if (isRequest && !m["id"].is_string() && !m["id"].is_number_integer()) {
        status = 400;
        return finish(error(J(), kInvalidRequest, "Invalid request: the id must be a string or an integer"));
    }
    const J params = m.contains("params") && m["params"].is_object() ? m["params"] : J::object();
    if (!isRequest) {
        if (method == "notifications/initialized" && legacy) {
            std::lock_guard<std::mutex> lock(legacy->mutex);
            legacy->initialized = !legacy->version.empty();
        }
        status = 202;
        return finish(nullptr); // notifications get no answer (cancellation is handled by the transport)
    }

    // The handshake generation: initialize selects a legacy revision for the channel.
    if (method == "initialize") {
        if (!legacy) {
            status = 400;
            return finish(error(id, kInvalidRequest, "initialize is not available on this channel"));
        }
        const std::string requested = params.value("protocolVersion", "");
        const std::string chosen = isLegacyVersion(requested) ? requested : legacyVersions().front();
        {
            std::lock_guard<std::mutex> lock(legacy->mutex);
            legacy->version = chosen;
            legacy->clientName = clientNameOf(params.value("clientInfo", J::object()));
            legacy->initialized = true; // requests may follow before notifications/initialized arrives
        }
        info.modern = false;
        return finish({{"jsonrpc", "2.0"},
                       {"id", id},
                       {"result",
                        {{"protocolVersion", chosen},
                         {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}, {"subscribe", false}}}, {"prompts", {{"listChanged", false}}}}},
                         {"serverInfo", serverInfo()},
                         {"instructions", instructions()}}}});
    }

    const J meta = params.value("_meta", J::object());
    const bool hasModernMeta = meta.is_object() && meta.contains("io.modelcontextprotocol/protocolVersion");
    if (hasModernMeta) {
        const J v = meta["io.modelcontextprotocol/protocolVersion"];
        const std::string version = v.is_string() ? v.get<std::string>() : "";
        bool legacyInitialized = false;
        if (legacy) {
            std::lock_guard<std::mutex> lock(legacy->mutex);
            legacyInitialized = legacy->initialized;
        }
        if (isLegacyVersion(version) && legacyInitialized) {
            // a handshake client that also tags its requests: serve it as negotiated
        } else if (version != kModernVersion) {
            status = 400;
            return finish(error(id, kUnsupportedVersion, "Unsupported protocol version",
                                {{"supported", supportedVersions()}, {"requested", version}}));
        } else {
            const auto caps = meta.find("io.modelcontextprotocol/clientCapabilities");
            if (caps == meta.end() || !caps->is_object()) {
                status = 400;
                return finish(error(id, kInvalidParams, "Invalid params: _meta must carry io.modelcontextprotocol/clientCapabilities (an object)"));
            }
            info.modern = true;
            info.version = version;
            info.clientName = clientNameOf(meta.value("io.modelcontextprotocol/clientInfo", J::object()));
        }
        if (meta.contains("io.modelcontextprotocol/logLevel")) {
            static const std::vector<std::string> levels = {"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
            const J lvl = meta["io.modelcontextprotocol/logLevel"];
            if (!lvl.is_string() || std::find(levels.begin(), levels.end(), lvl.get<std::string>()) == levels.end()) {
                status = 400;
                return finish(error(id, kInvalidParams, "Invalid params: unknown log level"));
            }
        }
    }
    if (!info.modern) {
        bool ok = false;
        if (legacy) {
            std::lock_guard<std::mutex> lock(legacy->mutex);
            ok = legacy->initialized;
            info.version = legacy->version;
            info.clientName = legacy->clientName;
        }
        if (!ok) {
            status = 400;
            return finish(error(id, kInvalidRequest,
                                "Server not initialized: send initialize (revision 2025-11-25 or 2025-06-18) first, or carry "
                                "io.modelcontextprotocol/protocolVersion \"2026-07-28\" and clientCapabilities in _meta",
                                {{"supported", supportedVersions()}}));
        }
    }
    if (meta.is_object() && meta.contains("progressToken") && (meta["progressToken"].is_string() || meta["progressToken"].is_number_integer()))
        info.progressToken = meta["progressToken"];

    J result;
    if (method == "server/discover") {
        if (!info.modern) {
            status = 400;
            return finish(error(id, kMethodNotFound, "Method not found: server/discover belongs to revision 2026-07-28"));
        }
        result = {{"supportedVersions", supportedVersions()},
                  {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}, {"subscribe", false}}}, {"prompts", {{"listChanged", false}}}}},
                  {"instructions", instructions()},
                  {"ttlMs", kListTtlMs},
                  {"cacheScope", "public"}};
    } else if (method == "ping") {
        if (info.modern) {
            status = 404;
            return finish(error(id, kMethodNotFound, "Method not found: ping is not part of revision 2026-07-28"));
        }
        result = J::object();
    } else {
        result = dispatch(method, params, info, id, status);
        if (result.contains("error") && result.contains("jsonrpc"))
            return finish(result);
    }
    if (info.cancelled())
        return finish(nullptr);
    if (info.modern) {
        result["resultType"] = "complete";
        result["_meta"] = {{"io.modelcontextprotocol/serverInfo", {{"name", "zederia-mcp"}, {"version", zed::kVersion}}}};
    }
    return finish({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

} // namespace zmcp
