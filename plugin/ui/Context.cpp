#include "Context.h"

namespace zui {

Context::Context(ZederiaProcessor& p) : processor(p), session(p.session()), prefs(Prefs::get()) {}

bool Context::refresh() {
    const int generation = processor.stateGeneration();
    if (generation == lastGeneration)
        return false;
    lastGeneration = generation;
    const auto& reg = zed::Registry::get();
    std::vector<Route> next;
    const auto list = processor.routes();
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        Route r;
        r.index = i;
        r.route = list[static_cast<std::size_t>(i)];
        r.dest = reg.indexOf(r.route.dest);
        r.amountParam = reg.modAmount(i);
        r.colour = sourceColour(str(r.route.source));
        next.push_back(std::move(r));
    }
    bool same = next.size() == routes_.size();
    for (std::size_t i = 0; same && i < next.size(); ++i)
        same = next[i].route == routes_[i].route;
    routes_ = std::move(next);
    if (!same)
        ++routeGeneration_;
    return !same;
}

std::vector<const Context::Route*> Context::routesTo(int param) const {
    std::vector<const Route*> out;
    for (const auto& r : routes_)
        if (r.dest == param)
            out.push_back(&r);
    return out;
}

const Context::Route* Context::route(int index) const {
    return index >= 0 && index < static_cast<int>(routes_.size()) ? &routes_[static_cast<std::size_t>(index)] : nullptr;
}

double Context::routeAmount(int route) const {
    return processor.plainValue(zed::Registry::get().modAmount(route));
}

bool Context::routeAudible(const Route& r) const {
    if (!r.route.enabled || r.route.mute)
        return false;
    bool anySolo = false;
    for (const auto& other : routes_)
        anySolo = anySolo || (other.route.solo && other.route.enabled);
    return !anySolo || r.route.solo;
}

juce::Colour Context::sourceColour(const juce::String& s) const {
    const Theme& t = theme();
    if (s.startsWith("env")) return t.mod[0];
    if (s.startsWith("lfo")) return t.mod[1];
    if (s.startsWith("flux")) return t.mod[2];
    if (s.startsWith("macro")) return t.mod[3];
    if (s.startsWith("random") || s == "alternate") return t.mod[5];
    if (s.endsWith("_audio") || s == "follower") return t.mod[6];
    if (s == "velocity" || s == "release_velocity" || s == "note" || s.startsWith("cc") || s == "modwheel" || s == "pitchbend" ||
        s.contains("aftertouch") || s.startsWith("mpe") || s == "expression" || s == "breath" || s == "sustain")
        return t.mod[4];
    return t.mod[7];
}

juce::String Context::sourceName(const juce::String& id) {
    for (const auto& s : zed::Registry::get().modSources())
        if (id == str(s.id))
            return str(s.name);
    if (id.startsWith("cc"))
        return "CC " + id.substring(2);
    return id;
}

int Context::sourceIndex(const juce::String& source) { return zed::Engine::sourceIndex(source.toStdString()); }

zed::ValueContext Context::valueContext() const {
    zed::ValueContext c;
    c.bpm = processor.hostBpm();
    c.referenceHz = processor.plainValue(zed::Registry::get().indexOf("global.reference"));
    return c;
}

} // namespace zui

namespace zui {

bool Context::isLocked(int param) {
    if (lockGeneration_ != session.generation()) {
        lockGeneration_ = session.generation();
        locks_ = processor.lockMask();
    }
    return param >= 0 && param < static_cast<int>(locks_.size()) && locks_[static_cast<std::size_t>(param)];
}

} // namespace zui

namespace zui {

void buildSourceMenu(juce::PopupMenu& menu, const std::function<void(const juce::String&)>& pick) {
    juce::PopupMenu env, lfo, flux, macros, midi, other;
    for (const auto& s : zed::Registry::get().modSources()) {
        const juce::String id = str(s.id), name = str(s.name);
        const bool isMidi = id == "velocity" || id == "release_velocity" || id == "note" || id == "pitchbend" || id == "modwheel" ||
                            id.contains("aftertouch") || id.startsWith("mpe") || id == "expression" || id == "breath" || id == "sustain";
        juce::PopupMenu& target = id.startsWith("env") ? env : id.startsWith("lfo") ? lfo : id.startsWith("flux") ? flux : id.startsWith("macro") ? macros : isMidi ? midi : other;
        target.addItem(name, [pick, id] { pick(id); });
    }
    juce::PopupMenu controllers;
    for (int cc = 0; cc < 128; ++cc)
        controllers.addItem("CC " + juce::String(cc), [pick, cc] { pick("cc" + juce::String(cc)); });
    midi.addSubMenu("MIDI controller", controllers);
    menu.addSubMenu("Envelopes", env);
    menu.addSubMenu("LFOs", lfo);
    menu.addSubMenu("Flux", flux);
    menu.addSubMenu("Macros", macros);
    menu.addSubMenu("MIDI and notes", midi);
    menu.addSubMenu("Other", other);
}

} // namespace zui
