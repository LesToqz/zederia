// What every part of the interface shares: the processor, the session, the preferences, the
// modulation focus (selected and hovered sources), the route list as the interface shows it, and
// services the editor provides (menus, the hover list, help, navigation).
#pragma once

#include "../PluginProcessor.h"
#include "../Prefs.h"
#include "../Session.h"
#include "Theme.h"

#include "zederia/params/ValueText.h"

#include <functional>
#include <vector>

namespace zui {

class ParamWidget;

class Context {
public:
    explicit Context(ZederiaProcessor& p);

    ZederiaProcessor& processor;
    Session& session;
    Prefs& prefs;

    // --- Routes ---------------------------------------------------------------------------------
    struct Route {
        int index = 0;           // route number: its amount is mod.(index+1).amount
        zed::ModRoute route;
        int dest = -1;           // registry index of the destination
        int amountParam = -1;    // registry index of the amount
        juce::Colour colour;
    };
    // Re-reads the routes when the patch changed; returns true when they did.
    bool refresh();
    const std::vector<Route>& routes() const { return routes_; }
    std::vector<const Route*> routesTo(int param) const;
    const Route* route(int index) const;
    int routeGeneration() const { return routeGeneration_; }
    double routeAmount(int route) const;
    bool routeAudible(const Route& r) const; // enabled, not muted, not silenced by another route's solo

    // Colour and name of a modulation source. Families share a colour (envelopes, LFOs, Flux,
    // macros, MIDI, random, audio, other); routes are also told apart by position and name.
    juce::Colour sourceColour(const juce::String& source) const;
    static juce::String sourceName(const juce::String& source);
    static int sourceIndex(const juce::String& source);

    // --- Modulation focus -------------------------------------------------------------------------
    juce::String selectedSource; // alt-drag on a control edits (or creates) this source's route
    juce::String hoveredSource;  // a chip under the pointer: its destinations light up
    int focusRoute = -1;         // the route a hover list or the MOD page points at

    // --- Services the editor provides -------------------------------------------------------------
    std::function<void(ParamWidget&)> openParamMenu;
    std::function<void(ParamWidget*)> hoverParam;  // null: the pointer left
    std::function<void(const juce::String& title, const juce::String& text)> showHelp;
    std::function<void(const juce::String& message)> notify;
    std::function<void(const juce::String& module)> reveal; // jump to a module's page and card
    std::function<void()> layoutChanged;                     // simple view or a card opened
    std::function<void(const juce::String& source)> showModulator; // the dock shows "env2", "lfo3" ...

    // Locked parameters, re-read when the session changes.
    bool isLocked(int param);
    zed::ValueContext valueContext() const;
    // The person's content folder: the instance's own when one was set, else the preferences' one.
    juce::File userFolder() const;
    bool simpleView() const { return prefs.simpleView; }
    int lastGeneration = -1;

private:
    std::vector<Route> routes_;
    std::vector<bool> locks_;
    int lockGeneration_ = -1;
    int routeGeneration_ = 0;
};

// The modulation sources as a menu, grouped (envelopes, LFOs, Flux, macros, MIDI, other).
void buildSourceMenu(juce::PopupMenu& menu, const std::function<void(const juce::String&)>& pick);

inline juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

} // namespace zui
