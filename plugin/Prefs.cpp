#include "Prefs.h"

#include <nlohmann/json.hpp>

namespace {

juce::StringArray toArray(const nlohmann::json& v) {
    juce::StringArray out;
    if (v.is_array())
        for (const auto& s : v)
            if (s.is_string())
                out.add(juce::String::fromUTF8(s.get<std::string>().c_str()));
    return out;
}

nlohmann::json fromArray(const juce::StringArray& a) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& s : a)
        out.push_back(s.toStdString());
    return out;
}

} // namespace

Prefs& Prefs::get() {
    static Prefs prefs = [] {
        Prefs p;
        // Tests and tools can point the settings somewhere else.
        p.load(file());
        return p;
    }();
    return prefs;
}

namespace {
juce::File& overrideFile() {
    static juce::File f;
    return f;
}
} // namespace

void Prefs::useFile(const juce::File& f) {
    overrideFile() = f;
    Prefs& p = get();
    p = Prefs();
    p.load(f);
}

juce::File Prefs::file() {
    if (overrideFile() != juce::File())
        return overrideFile();
    const juce::String env = juce::SystemStats::getEnvironmentVariable("ZEDERIA_SETTINGS", {});
    if (env.isNotEmpty())
        return juce::File(env);
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Zederia").getChildFile("settings.json");
}

bool Prefs::load(const juce::File& from) {
    if (!from.existsAsFile())
        return false;
    try {
        const auto doc = nlohmann::json::parse(from.loadFileAsString().toStdString());
        if (!doc.is_object())
            return false;
        const auto text = [&](const char* key, juce::String& out) {
            if (doc.contains(key) && doc[key].is_string())
                out = juce::String::fromUTF8(doc[key].get<std::string>().c_str());
        };
        const auto flag = [&](const char* key, bool& out) {
            if (doc.contains(key) && doc[key].is_boolean())
                out = doc[key].get<bool>();
        };
        const auto number = [&](const char* key, int& out, int low, int high) {
            if (doc.contains(key) && doc[key].is_number())
                out = juce::jlimit(low, high, static_cast<int>(doc[key].get<double>()));
        };
        text("theme", theme);
        number("default_scale", defaultScale, 70, 300);
        flag("rotary_drag", rotaryDrag);
        number("wheel", wheel, 0, 2);
        flag("double_click_types", doubleClickTypes);
        flag("tooltips", tooltips);
        flag("help_strip", helpStrip);
        flag("macro_strip", macroStrip);
        flag("computer_keys", computerKeys);
        // An octave and velocity saved with the earlier one-row layout, whose letters moved them while
        // playing, are not kept: the keys start again from C3 at velocity 100.
        if (doc.contains("keys_layout") && doc["keys_layout"].is_number() && doc["keys_layout"].get<int>() == 2) {
            number("keys_octave", keysOctave, 0, 7);
            number("keys_velocity", keysVelocity, 1, 127);
        }
        flag("key_labels", keyLabels);
        flag("simple_view", simpleView);
        number("frame_cap", frameCap, 15, 60);
        frameCap = frameCap <= 20 ? 15 : frameCap <= 45 ? 30 : 60;
        flag("gpu_rendering", gpuRendering);
        text("author", author);
        text("user_folder", userFolder);
        text("default_preset", defaultPreset);
        flag("mpe_default", mpeDefault);
        if (doc.contains("recent")) recent = toArray(doc["recent"]);
        if (doc.contains("factory_favourites")) factoryFavourites = toArray(doc["factory_favourites"]);
        flag("agent_link", agentLink);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void Prefs::save() const {
    nlohmann::json doc = nlohmann::json::object();
    doc["format"] = "zederia.settings";
    doc["theme"] = theme.toStdString();
    doc["default_scale"] = defaultScale;
    doc["rotary_drag"] = rotaryDrag;
    doc["wheel"] = wheel;
    doc["double_click_types"] = doubleClickTypes;
    doc["tooltips"] = tooltips;
    doc["help_strip"] = helpStrip;
    doc["macro_strip"] = macroStrip;
    doc["computer_keys"] = computerKeys;
    doc["keys_layout"] = 2; // two rows of computer keys
    doc["keys_octave"] = keysOctave;
    doc["keys_velocity"] = keysVelocity;
    doc["key_labels"] = keyLabels;
    doc["simple_view"] = simpleView;
    doc["frame_cap"] = frameCap;
    doc["gpu_rendering"] = gpuRendering;
    doc["author"] = author.toStdString();
    doc["user_folder"] = userFolder.toStdString();
    doc["default_preset"] = defaultPreset.toStdString();
    doc["mpe_default"] = mpeDefault;
    doc["recent"] = fromArray(recent);
    doc["factory_favourites"] = fromArray(factoryFavourites);
    doc["agent_link"] = agentLink;
    const juce::File target = file();
    target.getParentDirectory().createDirectory();
    target.replaceWithText(juce::String::fromUTF8(doc.dump(2).c_str()));
}

void Prefs::noteRecent(const juce::String& presetPath) {
    recent.removeString(presetPath);
    recent.insert(0, presetPath);
    while (recent.size() > 50)
        recent.remove(recent.size() - 1);
}

juce::File Prefs::userContentFolder() const {
    if (userFolder.isNotEmpty())
        return juce::File(userFolder);
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Zederia");
}
