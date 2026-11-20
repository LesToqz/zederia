// The editor (plan 3.13): a 1320 x 940 vector interface scaled freely from 70 % to 300 %, in fixed
// layers: the header (logo, preset bar, page tabs), the seven pages, the modulation dock (macros,
// envelopes, LFOs, Flux), the keyboard and the help strip; and the overlays (hover lists, the
// mod map, the command palette, preferences, history). Animation runs on one frame clock capped
// by the preferences (15, 30 or 60 per second) and stops while the window is hidden; rendering is
// in software unless the preferences allow the graphics card.
#pragma once

#include "PluginProcessor.h"
#include "ui/Dock.h"
#include "ui/ModOverlay.h"
#include "ui/Palette.h"
#include "ui/Shell.h"

#include <array>

class ZederiaEditor final : public juce::AudioProcessorEditor,
                            public juce::DragAndDropContainer,
                            private juce::Timer,
                            private juce::ChangeListener {
public:
    enum PageId { Synth, Fx, Mod, Table, Play, Library, Agent, Kit, NumPages };
    static juce::StringArray pageNames(); // the seven pages the tabs show (Kit is the component gallery)

    explicit ZederiaEditor(ZederiaProcessor& processor);
    ~ZederiaEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override { mouseMove(e); }
    void mouseDown(const juce::MouseEvent& e) override;
    // The keyboard belongs to the editor (the computer keys play) unless a text field or a part
    // with editing keys of its own holds it: a click on empty panel never gives it to a control.
    std::unique_ptr<juce::ComponentTraverser> createFocusTraverser() override;

    // --- Navigation -------------------------------------------------------------------------------
    void showPage(int page);
    int currentPage() const { return current_; }
    zui::Page& page(int page); // built on first use
    bool revealModule(const juce::String& module);
    bool revealParam(int index);
    void setScalePercent(int percent);
    float scale() const { return static_cast<float>(getWidth()) / static_cast<float>(zui::kBaseWidth); }

    // --- Overlays ---------------------------------------------------------------------------------
    void showPalette();
    void showPrefs();
    void toggleHistory();
    void setModMap(bool shown);
    bool modMapShown() const { return modMap_.isVisible(); }
    void openParamMenu(zui::ParamWidget& widget);
    void showMainMenu();
    void savePresetAs();
    // What the save dialog does: name, location inside the Presets folder ("User Presets/My Pack";
    // missing folders are made), type and comma-separated tags. False with a message on failure.
    bool savePresetTo(const juce::String& name, const juce::String& location, const juce::String& type, const juce::String& tags, juce::String& error,
                      juce::File* saved = nullptr);
    juce::String defaultSaveLocation() const; // the dialog's first location
    void closeOverlays();

    // --- Frame clock --------------------------------------------------------------------------------
    // One frame: refreshes what changed and repaints only that. Returns the areas it repainted
    // (editor coordinates); tests measure the interface's cost with it.
    std::vector<juce::Rectangle<int>> tickFrame();
    bool animating() const { return isTimerRunning() && getTimerInterval() < 200; }

    // --- For tests and tools --------------------------------------------------------------------------
    zui::Context& context() { return ctx_; }
    juce::Component& content();
    zui::TopBar& topBar() { return *topBar_; }
    zui::OutputPanel& output() { return *output_; }
    zui::HelpStrip& helpStrip() { return *help_; }
    zui::ModDock& dock() { return *dock_; }
    zui::KeyboardLayer& keyboardLayer() { return *keys_; }
    juce::Rectangle<int> pageArea() const; // where pages go, between the header and the dock
    zui::CommandPalette& palette() { return *palette_; }
    zui::ModHoverList& hoverList() { return *hoverList_; }
    zui::ModMapOverlay& modMap() { return modMap_; }
    void applyLayout();      // after preferences change
    void applyTheme();
    void saveUiState();
    void loadUiState();
    std::vector<zui::CommandPalette::Entry> paletteEntries();

private:
    class Root;
    void paintFrame(juce::Graphics& g);  // the chassis, and the header (cached) under the header's parts
    void paintHeader(juce::Graphics& g); // the header plate and the logo
    zui::CachedDrawing frameCache_;
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void tickTree(juce::Component& c, std::vector<juce::Rectangle<int>>& dirty);
    void updateHover();
    void applyRenderer();
    void fitToScreen();
    bool fitPending_ = false;
    bool focusPending_ = true; // the standalone application takes the keyboard when it first shows
    void takeKeysOnFirstShow();
    void stepPreset(int direction);

    ZederiaProcessor& processor_;
    zui::LookAndFeel lnf_;
    zui::Context ctx_;
    std::unique_ptr<Root> root_;
    std::unique_ptr<zui::TopBar> topBar_;
    std::unique_ptr<zui::OutputPanel> output_;
    std::unique_ptr<zui::TabStrip> tabs_;
    zui::Button simpleButton_{"Simple"}, mapButton_{"Mod map", zui::Glyph::Map}, findButton_{"Find", zui::Glyph::Search},
        prefsButton_{"", zui::Glyph::Gear};
    std::array<std::unique_ptr<zui::Page>, NumPages> pages_;
    int current_ = Synth;
    std::unique_ptr<zui::ModDock> dock_;
    std::unique_ptr<zui::KeyboardLayer> keys_;
    std::unique_ptr<zui::HelpStrip> help_;
    std::unique_ptr<zui::ModHoverList> hoverList_;
    zui::ModMapOverlay modMap_;
    std::unique_ptr<zui::CommandPalette> palette_;
    std::unique_ptr<zui::PrefsPanel> prefs_;
    std::unique_ptr<zui::HistoryPanel> history_;
    juce::Component::SafePointer<zui::ParamWidget> hoverCandidate_;
    double hoverSince_ = 0.0, hoverLeft_ = 0.0;
    int seenGeneration_ = -1, seenSession_ = -1, seenRestores_ = 0;
    bool layingOut_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZederiaEditor)
};
