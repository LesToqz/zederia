#include "zederia/patch/Modules.h"

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace zed {
namespace {

using J = nlohmann::ordered_json;

bool startsWith(std::string_view s, std::string_view head) { return s.substr(0, head.size()) == head; }

// "lfo3" -> 3 for prefix "lfo"; -1 when the rest is not a number in [1, limit].
int slotNumber(std::string_view id, std::string_view prefix, int limit) {
    if (!startsWith(id, prefix) || id.size() == prefix.size() || id.size() > prefix.size() + 2)
        return -1;
    int n = 0;
    for (const char c : id.substr(prefix.size())) {
        if (c < '0' || c > '9')
            return -1;
        n = n * 10 + (c - '0');
    }
    return n >= 1 && n <= limit ? n : -1;
}

std::string textField(const J& doc, const char* key) {
    const auto it = doc.find(key);
    return it != doc.end() && it->is_string() ? it->get<std::string>() : std::string();
}

J fullDocument(const Patch& patch) { return J::parse(savePatch(patch)); }

void issue(std::vector<Issue>& issues, std::string code, std::string message, std::string fix) {
    issues.push_back({Severity::Error, std::move(code), "", std::move(message), std::move(fix)});
}

bool parseDocument(std::string_view json, std::string_view format, J& out, std::vector<Issue>& issues) {
    try {
        out = J::parse(json);
    } catch (const std::exception&) {
        issue(issues, "not_json", "The clipboard does not hold a Zederia module or shape.", "Copy a module or a shape first.");
        return false;
    }
    if (!out.is_object() || !out.contains("format") || !out["format"].is_string() || out.value("format", std::string()) != format || !out.contains("content")) {
        issue(issues, "wrong_format", "The clipboard does not hold a Zederia " + std::string(format == kShapeFormat ? "shape" : "module") + ".",
              "Copy a module or a shape first.");
        return false;
    }
    return true;
}

// Loads a patch document holding only `content` at `pointer` and returns the patch, or false with
// the document's errors (their paths point into the content).
bool loadFragment(const J& content, const std::string& pointer, Patch& out, std::vector<Issue>& issues) {
    J doc = J::object();
    doc["format"] = kPatchFormat;
    doc["version"] = kPatchVersion;
    doc[J::json_pointer(pointer)] = content;
    LoadResult loaded = loadPatch(doc.dump());
    for (auto& i : loaded.issues)
        if (i.severity == Severity::Error)
            issues.push_back(i);
    if (!loaded.ok())
        return false;
    out = std::move(loaded.patch);
    return true;
}

J wrap(std::string_view format, std::string_view cls, std::string_view module, J content) {
    J doc = J::object();
    doc["format"] = format;
    doc["version"] = 1;
    doc["class"] = cls;
    if (!module.empty())
        doc["module"] = module;
    doc["content"] = std::move(content);
    return doc;
}

} // namespace

std::string moduleClass(std::string_view id) {
    if (slotNumber(id, "osc", kNumOscs) > 0) return "osc";
    if (id == "sub") return "sub";
    if (id == "texture") return "texture";
    if (slotNumber(id, "filter", kNumFilters) > 0) return "filter";
    if (slotNumber(id, "env", kNumEnvs) > 0) return "env";
    if (slotNumber(id, "lfo", kNumLfos) > 0) return "lfo";
    if (slotNumber(id, "flux", kNumFlux) > 0) return "flux";
    if (id == "voice") return "voice";
    if (id == "arp") return "arp";
    if (id == "chord") return "chord";
    if (slotNumber(id, "clip", kNumClips) > 0) return "clip";
    if (slotNumber(id, "arp_pattern", kNumArpPatterns) > 0) return "arp_pattern";
    if (id == "fx") return "fx_rack";
    if (id.size() >= 5 && startsWith(id, "fx.") && (id[3] == 'a' || id[3] == 'b' || id[3] == 'm') && slotNumber(id.substr(4), "", kNumFxSlots) > 0)
        return "fx_slot";
    return "";
}

std::string moduleClassName(std::string_view cls) {
    if (cls == "osc") return "Oscillator";
    if (cls == "sub") return "Sub";
    if (cls == "texture") return "Texture";
    if (cls == "filter") return "Filter";
    if (cls == "env") return "Envelope";
    if (cls == "lfo") return "LFO";
    if (cls == "flux") return "Flux";
    if (cls == "voice") return "Voice";
    if (cls == "arp") return "Arpeggiator";
    if (cls == "chord") return "Chords";
    if (cls == "clip") return "Clip";
    if (cls == "arp_pattern") return "Arpeggiator pattern";
    if (cls == "fx_rack") return "Effect rack";
    if (cls == "fx_slot") return "Effect slot";
    if (cls == "shape") return "Shape";
    if (cls == "env_shape") return "Envelope shape";
    return "Module";
}

std::string moduleDocumentClass(std::string_view json) {
    try {
        const J doc = J::parse(json);
        if (doc.is_object() && doc.contains("format") && (doc.value("format", std::string()) == kModuleFormat || doc.value("format", std::string()) == kShapeFormat) &&
            doc.contains("class") && doc["class"].is_string())
            return doc["class"].get<std::string>();
    } catch (const std::exception&) {
    }
    return "";
}

std::string copyModule(const Patch& patch, std::string_view id) {
    const std::string cls = moduleClass(id);
    if (cls.empty())
        return "";
    if (cls == "fx_rack")
        return wrap(kModuleFormat, cls, id, J::parse(saveRackPreset(patch))).dump(2);
    if (cls == "fx_slot") {
        const int lane = id[3] == 'a' ? 0 : id[3] == 'b' ? 1 : 2;
        const int slot = slotNumber(id.substr(4), "", kNumFxSlots) - 1;
        if (slot >= patch.fxSlotCount(lane))
            return "";
        return wrap(kModuleFormat, cls, id, J::parse(saveSlotPreset(patch, lane, slot))).dump(2);
    }
    const J doc = fullDocument(patch);
    J content = J::object();
    if (cls == "clip") {
        const auto k = static_cast<std::size_t>(slotNumber(id, "clip", kNumClips) - 1);
        const J pointer = doc.value(J::json_pointer("/clip/clips"), J::array());
        if (pointer.is_array() && k < pointer.size())
            content = pointer[k];
    } else if (cls == "arp_pattern") {
        const auto k = static_cast<std::size_t>(slotNumber(id, "arp_pattern", kNumArpPatterns) - 1);
        const J patterns = doc.value(J::json_pointer("/arp/patterns"), J::array());
        if (patterns.is_array() && k < patterns.size())
            content = patterns[k];
    } else if (doc.contains(std::string(id))) {
        content = doc[std::string(id)];
        if (cls == "arp")
            content.erase("patterns"); // patterns travel on their own
    }
    return wrap(kModuleFormat, cls, id, std::move(content)).dump(2);
}

bool pasteModule(std::string_view json, Patch& patch, std::string_view id, std::vector<Issue>& issues) {
    const std::string cls = moduleClass(id);
    J doc;
    if (cls.empty() || !parseDocument(json, kModuleFormat, doc, issues))
        return false;
    if (textField(doc, "class") != cls) {
        issue(issues, "wrong_class",
              "The clipboard holds " + moduleClassName(textField(doc, "class")) + " settings, which do not fit " +
                  moduleClassName(cls) + ".",
              "Paste into a module of the same kind.");
        return false;
    }
    const J& content = doc["content"];
    if (cls == "fx_rack") {
        Patch edited = patch;
        if (!loadRackPreset(content.dump(), edited, issues))
            return false;
        patch = std::move(edited);
        return true;
    }
    if (cls == "fx_slot") {
        const int lane = id[3] == 'a' ? 0 : id[3] == 'b' ? 1 : 2;
        const int slot = std::min(slotNumber(id.substr(4), "", kNumFxSlots) - 1, patch.fxSlotCount(lane));
        Patch edited = patch;
        if (!loadSlotPreset(content.dump(), edited, lane, slot, nullptr, issues))
            return false;
        patch = std::move(edited);
        return true;
    }
    Patch src;
    const auto& reg = Registry::get();
    if (cls == "clip" || cls == "arp_pattern") {
        const bool clip = cls == "clip";
        const int k = slotNumber(id, clip ? "clip" : "arp_pattern", clip ? kNumClips : kNumArpPatterns) - 1;
        J list = J::array();
        for (int i = 0; i < k; ++i)
            list.push_back(J::object());
        list.push_back(content);
        if (!loadFragment(list, clip ? "/clip/clips" : "/arp/patterns", src, issues))
            return false;
        if (clip)
            patch.clips[static_cast<std::size_t>(k)] = src.clips[static_cast<std::size_t>(k)];
        else
            patch.arpPatterns[static_cast<std::size_t>(k)] = src.arpPatterns[static_cast<std::size_t>(k)];
        return true;
    }
    J cleaned = content;
    if (cls == "env" && id == "env1") { // the amp envelope has no multi-segment mode
        cleaned.erase("mode");
        cleaned.erase("multi");
    }
    if (!loadFragment(cleaned, "/" + std::string(id), src, issues))
        return false;
    for (const auto& module : reg.modules())
        if (module.id == id)
            for (int i = module.firstParam; i < module.firstParam + module.numParams; ++i)
                patch.set(i, src.get(i));
    if (cls == "osc") {
        const auto k = static_cast<std::size_t>(slotNumber(id, "osc", kNumOscs) - 1);
        patch.tables[k] = src.tables[k];
        patch.tableData[k] = src.tableData[k];
        patch.warpCurves[k] = src.warpCurves[k];
    } else if (cls == "texture") {
        patch.textureSample = src.textureSample;
    } else if (cls == "env") {
        const auto k = static_cast<std::size_t>(slotNumber(id, "env", kNumEnvs) - 1);
        patch.envSync[k] = src.envSync[k];
        if (k > 0)
            patch.envShapes[k] = src.envShapes[k];
    } else if (cls == "lfo") {
        const auto k = static_cast<std::size_t>(slotNumber(id, "lfo", kNumLfos) - 1);
        patch.lfoShapes[k] = src.lfoShapes[k];
    } else if (cls == "chord") {
        patch.chordNotes = src.chordNotes;
    }
    return true;
}

// --- Shapes ------------------------------------------------------------------------------------

std::string copyShape(const Shape& shape, ShapeKind kind) {
    Patch carrier;
    if (kind == ShapeKind::Lfo)
        carrier.lfoShapes[0] = shape;
    else
        carrier.warpCurves[0][0] = shape;
    const J doc = fullDocument(carrier);
    const J content = kind == ShapeKind::Lfo ? doc[J::json_pointer("/lfo1/shape")] : doc[J::json_pointer("/osc1/warp_a/curve")];
    J out = wrap(kShapeFormat, "shape", "", content);
    out["range"] = kind == ShapeKind::Lfo ? "bipolar" : "unipolar";
    return out.dump(2);
}

std::string copyEnvShape(const EnvShape& shape) {
    Patch carrier;
    carrier.envShapes[1] = shape;
    const J doc = fullDocument(carrier);
    return wrap(kShapeFormat, "env_shape", "", doc.value(J::json_pointer("/env2/multi"), J::object())).dump(2);
}

bool pasteShape(std::string_view json, ShapeKind kind, Shape& out, std::vector<Issue>& issues) {
    J doc;
    if (!parseDocument(json, kShapeFormat, doc, issues))
        return false;
    const std::string cls = textField(doc, "class");
    Shape shape;
    bool bipolar = true;
    if (cls == "env_shape") { // an envelope becomes a shape over its own time, levels 0..1
        EnvShape env;
        if (!pasteEnvShape(json, env, issues))
            return false;
        double total = 0.0;
        for (const auto& p : env.points)
            total += std::max(p.timeMs, 0.0);
        double t = 0.0;
        for (const auto& p : env.points) {
            t += std::max(p.timeMs, 0.0);
            shape.points.push_back({total > 0.0 ? t / total : 0.0, p.level, p.curve});
        }
        bipolar = false;
    } else if (cls == "shape") {
        bipolar = textField(doc, "range") != "unipolar";
        Patch src;
        if (!loadFragment(doc["content"], bipolar ? "/lfo1/shape" : "/osc1/warp_a/curve", src, issues))
            return false;
        shape = bipolar ? src.lfoShapes[0] : src.warpCurves[0][0];
    } else {
        issue(issues, "wrong_class", "The clipboard does not hold a shape.", "Copy a shape first.");
        return false;
    }
    const bool wantBipolar = kind == ShapeKind::Lfo;
    for (auto& p : shape.points) {
        if (bipolar && !wantBipolar)
            p.y = (p.y + 1.0) * 0.5;
        else if (!bipolar && wantBipolar)
            p.y = p.y * 2.0 - 1.0;
    }
    out = std::move(shape);
    return true;
}

bool pasteEnvShape(std::string_view json, EnvShape& out, std::vector<Issue>& issues) {
    J doc;
    if (!parseDocument(json, kShapeFormat, doc, issues))
        return false;
    const std::string cls = textField(doc, "class");
    if (cls == "shape") { // a shape becomes an envelope lasting one second, levels 0..1
        Shape shape;
        if (!pasteShape(json, ShapeKind::Curve, shape, issues))
            return false;
        EnvShape env;
        double lastX = 0.0;
        for (const auto& p : shape.points) {
            if (static_cast<int>(env.points.size()) >= kMaxEnvPoints)
                break;
            EnvPoint e;
            e.timeMs = std::max(0.0, p.x - lastX) * 1000.0;
            e.level = std::clamp(p.y, 0.0, 1.0);
            e.curve = p.curve;
            lastX = p.x;
            env.points.push_back(e);
        }
        env.sustainPoint = env.points.empty() ? -1 : static_cast<int>(env.points.size()) - 1;
        out = std::move(env);
        return true;
    }
    if (cls != "env_shape") {
        issue(issues, "wrong_class", "The clipboard does not hold a shape.", "Copy a shape first.");
        return false;
    }
    Patch src;
    if (!loadFragment(doc["content"], "/env2/multi", src, issues))
        return false;
    out = src.envShapes[1];
    return true;
}

} // namespace zed
