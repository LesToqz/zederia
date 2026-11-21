// The editor's frame (plan 3.13): the top bar, the page tabs, the macro strip, the help strip, the
// history list, and the base class of every page.
#pragma once

#include "Displays.h"
#include "OutputMonitor.h"
#include "Widgets.h"

#include <nlohmann/json_fwd.hpp>

namespace zui {

// The editor is four fixed layers top to bottom: the header (logo, preset bar, page tabs), the page,
// the modulation dock and the keyboard; the help strip runs along the bottom edge.
inline constexpr int kBaseWidth = 1320, kBaseHeight = 940;
inline constexpr int kTopBarHeight = 40, kTabsHeight = 30, kMacroStripHeight = 50, kHelpStripHeight = 24;
inline constexpr int kHeaderHeight = 76, kLogoWidth = 150, kOutputWidth = 256;
inline constexpr int kChassisInset = 4;

// A page of the editor. Pages are built when first shown and kept.
class Page : public juce::Component {
public:
    explicit Page(Context& ctx) : ctx_(ctx) {}
    // Brings a module into view (the signal map, the command palette, "reveal" links).
    virtual bool reveal(const juce::String& module) { juce::ignoreUnused(module); return false; }
    // Shows or hides the advanced controls.
    virtual void applySimpleView() {}
    // What the page remembers between openings of the editor (selections, open cards).
    virtual void saveState(nlohmann::json& state) const { juce::ignoreUnused(state); }
    virtual void loadState(const nlohmann::json& state) { juce::ignoreUnused(state); }
    // Called when the patch changed structure (routes, rack, clips): rebuild what depends on it.
    virtual void structureChanged() {}

protected:
    Context& ctx_;
};

class TopBar final : public juce::Component {
public:
    TopBar(Context& ctx);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void refresh(); // names, history and A/B state
    juce::Rectangle<int> presetWindow() const; // the dark window behind the preset name, in the bar's coordinates
    bool tick();    // CPU and meters
    std::function<void()> onMenu, onPalette, onHistory, onSavePreset, onPresetBrowse;
    std::function<void(int)> onStepPreset;

    Button menu{"", Glyph::Menu}, prev{"", Glyph::Prev}, next{"", Glyph::Next}, save{"Save", Glyph::Save};
    Button presetName{"Init"};
    Button history{"", Glyph::History};
    Button abA{"A"}, abB{"B"}, abCopy{"A>B"};
    Caption cpuText{"CPU", kTextSmall, true};
    Caption agent{"Agent off", kTextSmall, true};
    std::unique_ptr<ChoiceBox> quality;
    std::unique_ptr<Toggle> fxOff;

private:
    Context& ctx_;
    double cpuShownAt_ = -1.0e9; // when the CPU readout last changed
};

// The output at the right end of the header, as tall as both header rows: the stereo meter and
// the Master level beside it, a full-size cap with its name and its value in dB.
class OutputPanel final : public juce::Component {
public:
    explicit OutputPanel(Context& ctx);
    void resized() override;
    OutputScope scope;
    Meter meter;
    std::unique_ptr<Knob> master;
};

// Engraved nameplate and the engine's current voice allocation.
class Nameplate final : public Widget {
public:
    explicit Nameplate(Context& ctx);
    void draw(juce::Graphics& g) override;
    bool tick() override;
    juce::RectangleList<int> dirtyArea() override { return getLocalBounds().removeFromBottom(28); }

private:
    Context& ctx_;
    int polyParam_, modeParam_, active_ = 0, polyphony_ = 16;
    CachedDrawing plate_;
};

// Eight macros, visible on every page (plan 3.7): knob, editable name, drag chip.
class MacroStrip final : public juce::Component {
public:
    explicit MacroStrip(Context& ctx);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void refreshNames();
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<DragChip>> chips;
    std::vector<std::unique_ptr<Button>> names;
    std::vector<std::unique_ptr<Segmented>> polarity; // 0..1 or -1..1

private:
    Context& ctx_;
};

// The help strip (plan 3.13): what is under the pointer, or a message; F1 pins it.
class HelpStrip final : public juce::Component {
public:
    HelpStrip();
    void paint(juce::Graphics& g) override;
    void show(const juce::String& title, const juce::String& text, const juce::String& value = {});
    void message(const juce::String& text); // stays a few seconds
    bool tick();
    bool pinned = false;
    bool messageOnly = false; // hidden in the preferences: shown only while a message lasts
    bool showingMessage() const { return message_.isNotEmpty(); }
    juce::String title() const { return title_; }
    juce::String text() const { return text_; }

private:
    juce::String title_, text_, value_, message_;
    double messageUntil_ = 0.0;
};

// The undo history as a list: click a step to go there (plan 3.11).
class HistoryPanel final : public juce::Component, private juce::ListBoxModel {
public:
    explicit HistoryPanel(Context& ctx);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void refresh();
    std::function<void()> onClose;
    juce::ListBox list;

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    Context& ctx_;
    Button close{"", Glyph::Close};
};

} // namespace zui
