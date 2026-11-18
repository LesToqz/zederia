#include "Session.h"

#include "PluginProcessor.h"
#include "Prefs.h"
#include "zederia/params/ValueText.h"
#include "zederia/patch/Modules.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <nlohmann/json.hpp>

namespace {

juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

std::unique_ptr<bool[]> maskOf(const std::vector<bool>& v) {
    auto out = std::make_unique<bool[]>(v.size());
    for (std::size_t i = 0; i < v.size(); ++i)
        out[i] = v[i];
    return out;
}

// Sources whose output runs 0..1 start as unipolar routes; the others as bipolar.
bool unipolarSource(const juce::String& s) {
    return s.startsWith("env") || s.startsWith("macro") || s.startsWith("cc") || s == "velocity" || s == "release_velocity" ||
           s == "modwheel" || s == "aftertouch" || s == "poly_aftertouch" || s == "mpe_pressure" || s == "mpe_slide" ||
           s == "expression" || s == "breath" || s == "sustain" || s == "voice_index" || s == "constant" || s == "follower" ||
           s == "bar_phase";
}

juce::String sourceName(const juce::String& id) {
    for (const auto& s : zed::Registry::get().modSources())
        if (id == str(s.id))
            return str(s.name);
    if (id.startsWith("cc"))
        return "CC " + id.substring(2);
    return id;
}

} // namespace

Session::Session(ZederiaProcessor& processor) : p_(processor) {
    const zed::Patch start = p_.snapshot();
    history_.reset(start, "Start");
    presetState_ = start;
}

void Session::changed() {
    ++generation_;
    sendChangeMessage();
}

// --- History ---------------------------------------------------------------------------------------

void Session::resetHistory(const juce::String& label) {
    const zed::Patch state = p_.snapshot();
    history_.reset(state, label.toStdString());
    presetState_ = state;
    changed();
}

bool Session::undo() {
    history_.absorb(p_.snapshot());
    const zed::Patch* state = history_.undo();
    if (state == nullptr)
        return false;
    p_.restore(*state);
    lastMergeKey_.clear();
    changed();
    return true;
}

bool Session::redo() {
    history_.absorb(p_.snapshot());
    const zed::Patch* state = history_.redo();
    if (state == nullptr)
        return false;
    p_.restore(*state);
    lastMergeKey_.clear();
    changed();
    return true;
}

bool Session::jumpTo(int step) {
    if (step == history_.current())
        return false;
    history_.absorb(p_.snapshot());
    const zed::Patch* state = history_.jumpTo(step);
    if (state == nullptr)
        return false;
    p_.restore(*state);
    lastMergeKey_.clear();
    changed();
    return true;
}

void Session::commit(const juce::String& label, const juce::String& mergeKey) {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const zed::Patch state = p_.snapshot();
    const bool merge = mergeKey.isNotEmpty() && mergeKey == lastMergeKey_ && now - lastMergeTime_ < 1200.0 && history_.current() > 0;
    const bool added = merge ? history_.amend(label.toStdString(), state) : history_.push(label.toStdString(), state);
    lastMergeKey_ = mergeKey;
    lastMergeTime_ = now;
    if (added)
        changed();
}

void Session::apply(const zed::Patch& state, const juce::String& label, const juce::String& mergeKey) {
    p_.restore(state);
    commit(label, mergeKey);
}

bool Session::edit(const juce::String& label, const std::function<bool(zed::Patch&)>& change) {
    const zed::Patch before = p_.snapshot();
    history_.absorb(before);
    zed::Patch next = before;
    if (!change(next) || next == before)
        return false;
    apply(next, label);
    return true;
}

// --- Parameters ------------------------------------------------------------------------------------

juce::String Session::describeChange(int index, double from, double to) const {
    const auto& reg = zed::Registry::get();
    const zed::ParamDef& own = reg.at(index);
    const zed::ParamDef& def = p_.effectiveDef(index);
    juce::String name = str(own.name);
    if (&def != &own) { // an effect slot's parameter: "FX A1 Chorus Rate"
        for (const auto& m : reg.modules())
            if (m.id == own.module)
                name = str(m.name) + " " + str(def.name);
    }
    const int route = reg.modRouteOf(index);
    if (route >= 0) {
        const auto routes = p_.routes();
        if (route < static_cast<int>(routes.size())) {
            const auto& r = routes[static_cast<std::size_t>(route)];
            const zed::ParamDef* dest = reg.find(r.dest);
            name = "Depth " + sourceName(str(r.source)) + " > " + (dest != nullptr ? str(dest->name) : str(r.dest));
        }
    }
    return name + ": " + str(zed::formatValueText(def, from)) + juce::String::fromUTF8(" \xe2\x86\x92 ") + str(zed::formatValueText(def, to));
}

void Session::beginGesture(int index) {
    history_.absorb(p_.snapshot());
    gestureStart_[index] = p_.plainValue(index);
    p_.beginGesture(index);
}

void Session::setFromGesture(int index, double plain) {
    const auto& def = p_.effectiveDef(index);
    p_.setNormalizedFromUi(index, static_cast<float>(def.toNormalized(def.clamp(plain))));
}

void Session::endGesture(int index) {
    p_.endGesture(index);
    const auto it = gestureStart_.find(index);
    if (it == gestureStart_.end())
        return;
    const double from = it->second;
    gestureStart_.erase(it);
    const double to = p_.plainValue(index);
    if (from != to)
        commit(describeChange(index, from, to));
}

void Session::setValue(int index, double plain) {
    history_.absorb(p_.snapshot());
    const double from = p_.plainValue(index);
    p_.beginGesture(index);
    setFromGesture(index, plain);
    p_.endGesture(index);
    const double to = p_.plainValue(index);
    if (from != to)
        commit(describeChange(index, from, to));
}

void Session::nudgeValue(int index, double plain) {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const juce::String key = "nudge:" + juce::String(index);
    const bool continuing = key == lastMergeKey_ && now - lastMergeTime_ < 1200.0;
    if (!continuing) {
        history_.absorb(p_.snapshot());
        gestureStart_[-1 - index] = p_.plainValue(index);
    }
    p_.beginGesture(index);
    setFromGesture(index, plain);
    p_.endGesture(index);
    const auto it = gestureStart_.find(-1 - index);
    const double from = it != gestureStart_.end() ? it->second : plain;
    commit(describeChange(index, from, p_.plainValue(index)), key);
}

void Session::resetValue(int index) { setValue(index, p_.effectiveDef(index).def); }

// --- Routes ----------------------------------------------------------------------------------------

std::vector<int> Session::routesTo(int index) const {
    std::vector<int> out;
    const auto routes = p_.routes();
    const std::string& id = zed::Registry::get().at(index).id;
    for (int r = 0; r < static_cast<int>(routes.size()); ++r)
        if (routes[static_cast<std::size_t>(r)].dest == id)
            out.push_back(r);
    return out;
}

int Session::addRoute(const juce::String& source, int destIndex, double amount) {
    const auto& reg = zed::Registry::get();
    if (destIndex < 0 || destIndex >= reg.size() || !reg.at(destIndex).modulatable || !reg.isModSource(source.toStdString()))
        return -1;
    const auto routes = p_.routes();
    for (int r = 0; r < static_cast<int>(routes.size()); ++r)
        if (routes[static_cast<std::size_t>(r)].dest == reg.at(destIndex).id && str(routes[static_cast<std::size_t>(r)].source) == source)
            return r;
    if (static_cast<int>(routes.size()) >= zed::kNumModRoutes)
        return -1;
    const int index = static_cast<int>(routes.size());
    edit("Modulate " + str(reg.at(destIndex).name) + " by " + sourceName(source), [&](zed::Patch& p) {
        zed::ModRoute route;
        route.source = source.toStdString();
        route.dest = reg.at(destIndex).id;
        route.polarity = unipolarSource(source) ? zed::Polarity::Uni : zed::Polarity::Bi;
        p.routes.push_back(route);
        p.set(reg.modAmount(index), amount);
        return true;
    });
    return index;
}

namespace {
template <class F>
bool editRoute(Session& s, const ZederiaProcessor& proc, int route, const juce::String& label, F&& f) {
    const auto routes = proc.routes();
    if (route < 0 || route >= static_cast<int>(routes.size()))
        return false;
    return s.edit(label, [&](zed::Patch& p) {
        f(p.routes[static_cast<std::size_t>(route)], p);
        return true;
    });
}

juce::String routeName(const ZederiaProcessor& proc, int route) {
    const auto routes = proc.routes();
    if (route < 0 || route >= static_cast<int>(routes.size()))
        return "route";
    const auto& r = routes[static_cast<std::size_t>(route)];
    const auto* dest = zed::Registry::get().find(r.dest);
    return sourceName(str(r.source)) + " > " + (dest != nullptr ? str(dest->name) : str(r.dest));
}
} // namespace

void Session::removeRoute(int route) {
    const juce::String name = routeName(p_, route);
    edit("Remove " + name, [&](zed::Patch& p) {
        if (route < 0 || route >= static_cast<int>(p.routes.size()))
            return false;
        p.removeRoute(route);
        return true;
    });
}

void Session::flipPolarity(int route) {
    editRoute(*this, p_, route, "Flip polarity of " + routeName(p_, route), [](zed::ModRoute& r, zed::Patch&) {
        r.polarity = r.polarity == zed::Polarity::Bi ? zed::Polarity::Uni : zed::Polarity::Bi;
    });
}

void Session::setRouteMute(int route, bool mute) {
    editRoute(*this, p_, route, (mute ? "Mute " : "Unmute ") + routeName(p_, route), [&](zed::ModRoute& r, zed::Patch&) { r.mute = mute; });
}

void Session::setRouteSolo(int route, bool solo) {
    editRoute(*this, p_, route, (solo ? "Solo " : "Unsolo ") + routeName(p_, route), [&](zed::ModRoute& r, zed::Patch&) { r.solo = solo; });
}

void Session::setRouteEnabled(int route, bool enabled) {
    editRoute(*this, p_, route, (enabled ? "Enable " : "Disable ") + routeName(p_, route),
              [&](zed::ModRoute& r, zed::Patch&) { r.enabled = enabled; });
}

void Session::setRouteSource(int route, const juce::String& source) {
    if (!zed::Registry::get().isModSource(source.toStdString()))
        return;
    editRoute(*this, p_, route, "Source of " + routeName(p_, route) + " to " + sourceName(source),
              [&](zed::ModRoute& r, zed::Patch&) { r.source = source.toStdString(); });
}

void Session::setRouteVia(int route, const juce::String& via, bool invert) {
    if (via.isNotEmpty() && !zed::Registry::get().isModSource(via.toStdString()))
        return;
    editRoute(*this, p_, route, "Via of " + routeName(p_, route), [&](zed::ModRoute& r, zed::Patch&) {
        r.via = via.toStdString();
        r.viaInvert = invert;
    });
}

void Session::setRouteCurve(int route, int curve) {
    editRoute(*this, p_, route, "Curve of " + routeName(p_, route), [&](zed::ModRoute& r, zed::Patch&) {
        r.curve = static_cast<zed::ModCurve>(juce::jlimit(0, 4, curve));
        if (r.curve == zed::ModCurve::Drawn && r.curveShape.empty())
            r.curveShape.points = {{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}};
    });
}

void Session::setRouteCurveShape(int route, const zed::Shape& shape) {
    editRoute(*this, p_, route, "Draw curve of " + routeName(p_, route), [&](zed::ModRoute& r, zed::Patch&) {
        r.curve = zed::ModCurve::Drawn;
        r.curveShape = shape;
    });
}

void Session::setRouteDest(int route, int destIndex) {
    const auto& reg = zed::Registry::get();
    if (destIndex < 0 || destIndex >= reg.size() || !reg.at(destIndex).modulatable)
        return;
    editRoute(*this, p_, route, "Destination of " + routeName(p_, route), [&](zed::ModRoute& r, zed::Patch&) { r.dest = reg.at(destIndex).id; });
}

void Session::replaceSource(const std::vector<int>& routes, const juce::String& source) {
    if (routes.empty() || !zed::Registry::get().isModSource(source.toStdString()))
        return;
    edit("Replace source of " + juce::String(static_cast<int>(routes.size())) + " routes with " + sourceName(source), [&](zed::Patch& p) {
        for (const int r : routes)
            if (r >= 0 && r < static_cast<int>(p.routes.size()))
                p.routes[static_cast<std::size_t>(r)].source = source.toStdString();
        return true;
    });
}

// --- A/B -------------------------------------------------------------------------------------------

void Session::switchAB() {
    const zed::Patch current = p_.snapshot();
    history_.absorb(current);
    const zed::Patch other = abOther_ != nullptr ? *abOther_ : current;
    abOther_ = std::make_unique<zed::Patch>(current);
    abSide_ ^= 1;
    p_.restore(other);
    const zed::Patch now = p_.snapshot();
    if (!history_.push(abSide_ == 1 ? "Switch to B" : "Switch to A", now))
        changed(); // the two sides are equal: nothing to undo, but the side changed
    else
        changed();
}

void Session::copyToOtherSide() {
    abOther_ = std::make_unique<zed::Patch>(p_.snapshot());
    changed();
}

std::string Session::abStateJson() const {
    if (abOther_ == nullptr)
        return {};
    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    doc["side"] = abSide_ == 1 ? "b" : "a";
    zed::SaveOptions options;
    options.compact = true;
    doc["other"] = nlohmann::ordered_json::parse(zed::savePatch(*abOther_, options));
    return doc.dump();
}

void Session::setAbStateJson(const std::string& json) {
    try {
        const auto doc = nlohmann::ordered_json::parse(json);
        if (!doc.is_object() || !doc.contains("other"))
            return;
        zed::LoadResult loaded = zed::loadPatch(doc["other"].dump());
        if (!loaded.ok())
            return;
        abOther_ = std::make_unique<zed::Patch>(std::move(loaded.patch));
        abSide_ = doc.contains("side") && doc["side"] == "b" ? 1 : 0;
        changed();
    } catch (const std::exception&) {
    }
}

// --- Presets ---------------------------------------------------------------------------------------

bool Session::loadPresetFile(const juce::File& file, juce::String& error) {
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes)) {
        error = "Could not read " + file.getFullPathName();
        return false;
    }
    const zed::LoadResult loaded = zed::loadPatch(std::string_view(static_cast<const char*>(bytes.getData()), bytes.getSize()));
    if (!loaded.ok()) {
        for (const auto& issue : loaded.issues)
            if (issue.severity == zed::Severity::Error) {
                error = str(issue.message) + " " + str(issue.fix);
                break;
            }
        return false;
    }
    history_.absorb(p_.snapshot());
    p_.loadPresetPatch(loaded.patch);
    presetFile_ = file;
    presetState_ = p_.snapshot();
    commit("Load preset " + str(loaded.patch.meta.name));
    Prefs::get().noteRecent(file.getFullPathName());
    changed();
    return true;
}

bool Session::savePresetFile(const juce::File& file, const zed::PatchMeta& meta, juce::String& error) {
    zed::PatchMeta m = meta;
    const auto now = juce::Time::getCurrentTime().toISO8601(true).toStdString();
    if (m.created.empty())
        m.created = now;
    m.modified = now;
    p_.setMeta(m);
    const zed::Patch state = p_.snapshot();
    const std::string text = zed::savePatch(state);
    file.getParentDirectory().createDirectory();
    if (!file.replaceWithData(text.data(), text.size())) {
        error = "Could not write " + file.getFullPathName();
        return false;
    }
    presetFile_ = file;
    presetState_ = state;
    history_.absorb(state);
    Prefs::get().noteRecent(file.getFullPathName());
    changed();
    return true;
}

void Session::initPatch() {
    const juce::String path = Prefs::get().defaultPreset;
    if (path.isNotEmpty()) {
        juce::String error;
        if (loadPresetFile(juce::File(path), error))
            return;
    }
    history_.absorb(p_.snapshot());
    zed::Patch init;
    init.set(zed::Registry::get().indexOf("global.mpe"), Prefs::get().mpeDefault ? 1.0 : 0.0);
    p_.loadPresetPatch(init);
    presetFile_ = juce::File();
    presetState_ = p_.snapshot();
    commit("Init");
    changed();
}

juce::String Session::presetName() const { return str(p_.meta().name); }

bool Session::modified() const {
    zed::Patch now = p_.snapshot();
    now.meta = presetState_.meta;
    return !(now == presetState_);
}

// --- Variation -------------------------------------------------------------------------------------

void Session::randomize(const zed::VariationScope& scope, double amount) {
    const auto lockVector = p_.lockMask();
    const auto mask = maskOf(lockVector);
    const std::uint64_t seed = static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks()) ^ (seed_++ * 0x9E3779B97F4A7C15ull);
    edit("Randomize " + juce::String(juce::roundToInt(amount * 100.0)) + " %", [&](zed::Patch& p) {
        zed::randomizePatch(p, scope, amount, std::span<const bool>(mask.get(), lockVector.size()), seed);
        return true;
    });
}

void Session::mutate(double amount) {
    const auto lockVector = p_.lockMask();
    const auto mask = maskOf(lockVector);
    const std::uint64_t seed = static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks()) ^ (seed_++ * 0x9E3779B97F4A7C15ull);
    edit("Mutate", [&](zed::Patch& p) {
        zed::mutatePatch(p, amount, std::span<const bool>(mask.get(), lockVector.size()), seed);
        return true;
    });
}

bool Session::blend(const juce::Array<juce::File>& presets, juce::String& error) {
    std::vector<zed::Patch> parents;
    for (const auto& file : presets) {
        juce::MemoryBlock bytes;
        if (!file.loadFileAsData(bytes))
            continue;
        zed::LoadResult loaded = zed::loadPatch(std::string_view(static_cast<const char*>(bytes.getData()), bytes.getSize()));
        if (loaded.ok())
            parents.push_back(std::move(loaded.patch));
    }
    if (parents.size() < 2) {
        error = "Choose at least two presets that load.";
        return false;
    }
    const auto lockVector = p_.lockMask();
    const auto mask = maskOf(lockVector);
    const std::uint64_t seed = static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks()) ^ (seed_++ * 0x9E3779B97F4A7C15ull);
    return edit("Blend " + juce::String(static_cast<int>(parents.size())) + " presets", [&](zed::Patch& p) {
        zed::Patch child = zed::blendPatches(p, parents, std::span<const bool>(mask.get(), lockVector.size()), seed);
        child.meta = p.meta;
        child.meta.name = "Blend";
        p = std::move(child);
        return true;
    });
}

// --- Locks -----------------------------------------------------------------------------------------

void Session::setLocked(int index, bool locked) {
    p_.setParamLocked(index, locked);
    changed();
}

void Session::setModuleLocked(const juce::String& module, bool locked) {
    for (const auto& m : zed::Registry::get().modules())
        if (str(m.id) == module)
            for (int i = m.firstParam; i < m.firstParam + m.numParams; ++i)
                p_.setParamLocked(i, locked);
    changed();
}

bool Session::moduleLocked(const juce::String& module) const {
    for (const auto& m : zed::Registry::get().modules())
        if (str(m.id) == module) {
            for (int i = m.firstParam; i < m.firstParam + m.numParams; ++i)
                if (!p_.paramLocked(i))
                    return false;
            return m.numParams > 0;
        }
    return false;
}

// --- Copy and paste ----------------------------------------------------------------------------------

bool Session::copyText(const juce::String& text) {
    clipboard_ = text;
    juce::SystemClipboard::copyTextToClipboard(text);
    return true;
}

juce::String Session::clipboardText() const {
    const juce::String system = juce::SystemClipboard::getTextFromClipboard();
    if (system.isNotEmpty() && !zed::moduleDocumentClass(system.toStdString()).empty())
        return system;
    return clipboard_;
}

juce::String Session::clipboardClass() const { return str(zed::moduleDocumentClass(clipboardText().toStdString())); }

bool Session::copyModule(const juce::String& moduleId) {
    const std::string text = zed::copyModule(p_.snapshot(), moduleId.toStdString());
    if (text.empty())
        return false;
    return copyText(str(text));
}

bool Session::pasteModule(const juce::String& moduleId, juce::String& error) {
    const std::string text = clipboardText().toStdString();
    const juce::String cls = str(zed::moduleClass(moduleId.toStdString()));
    std::vector<zed::Issue> issues;
    const bool ok = edit("Paste " + str(zed::moduleClassName(cls.toStdString())) + " into " + moduleId,
                         [&](zed::Patch& p) { return zed::pasteModule(text, p, moduleId.toStdString(), issues); });
    if (!ok && !issues.empty())
        error = str(issues.front().message) + " " + str(issues.front().fix);
    return ok;
}

bool Session::applyAgentState(const juce::String& label, const juce::String& origin, const zed::Patch& state, bool asPreset) {
    const zed::Patch before = p_.snapshot();
    history_.absorb(before);
    if (asPreset)
        p_.loadPresetPatch(state);
    else
        p_.restore(state);
    const zed::Patch after = p_.snapshot();
    lastMergeKey_.clear();
    if (after == before)
        return false;
    history_.push(label.toStdString(), after, origin.toStdString());
    changed();
    return true;
}

bool Session::perform(const juce::String& label, const std::function<bool()>& action) {
    history_.absorb(p_.snapshot());
    if (!action())
        return false;
    commit(label);
    return true;
}
