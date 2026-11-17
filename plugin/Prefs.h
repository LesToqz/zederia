// Preferences (plan 3.12): one settings file per user, %APPDATA%\Zederia\settings.json, shared by
// every instance. Read once, written whenever something changes. Unknown or broken entries fall
// back to defaults; the file never stops the plug-in from opening.
#pragma once

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

struct Prefs {
    // Interface
    juce::String theme = "Graphite";
    int defaultScale = 100;      // percent of 1320 x 820, 70..300
    bool rotaryDrag = false;     // false: knobs follow vertical drags; true: they follow the angle around them
    int wheel = 1;               // 0 off, 1 normal, 2 fine
    bool doubleClickTypes = false; // false: double-click resets; true: it opens value entry
    bool tooltips = true;
    bool helpStrip = true;
    bool macroStrip = true;
    bool simpleView = false;
    int frameCap = 30;           // 15, 30 or 60 frames per second
    bool gpuRendering = false;   // false: software rendering (no GPU needed)
    // Content and sound
    juce::String author;
    juce::String userFolder;     // empty: Documents\Zederia
    juce::String defaultPreset;  // Init loads this file when set
    bool mpeDefault = false;     // new instances start with MPE on
    // Library
    juce::StringArray recent;    // newest first, at most 50
    juce::StringArray factoryFavourites; // factory presets cannot be written to: their favourite flag lives here
    // Agent link (milestone 6 fills these in; shown on the AGENT page)
    bool agentLink = false;

    static Prefs& get();         // loaded on first use
    // Tools and tests: read and write another settings file (a missing file means defaults).
    static void useFile(const juce::File& file);
    static juce::File file();
    void save() const;
    bool load(const juce::File& from);
    void noteRecent(const juce::String& presetPath);
    juce::File userContentFolder() const;
};
