// The command palette (Ctrl+K, plan 3.13: finds any parameter, action or preset) and the
// preferences panel (plan 3.12).
#pragma once

#include "Widgets.h"

namespace zui {

class CommandPalette final : public juce::Component, private juce::ListBoxModel {
public:
    struct Entry {
        juce::String kind;   // "Control", "Action", "Preset", "Page"
        juce::String name;
        juce::String detail; // where it lives, or what it does
        std::function<void()> run;
    };
    explicit CommandPalette(Context& ctx);
    void setEntries(std::vector<Entry> entries);
    void open();
    void setQuery(const juce::String& text);
    int matchCount() const { return static_cast<int>(matches_.size()); }
    const Entry& match(int i) const { return entries_[static_cast<std::size_t>(matches_[static_cast<std::size_t>(i)])]; }
    bool runMatch(int i);
    std::function<void()> onClose;
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    TextField search;
    juce::ListBox list;

private:
    int getNumRows() override { return matchCount(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override { runMatch(row); }
    void returnKeyPressed(int row) override { runMatch(row); }
    void filter();
    Context& ctx_;
    std::vector<Entry> entries_;
    std::vector<int> matches_;
};

class PrefsPanel final : public juce::Component {
public:
    explicit PrefsPanel(Context& ctx);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void refresh();
    std::function<void()> onClose;
    std::function<void()> onThemeChanged;
    std::function<void(int percent)> onScale;
    std::function<void()> onLayout; // macro strip, help strip, simple view

private:
    struct Row {
        juce::String label, help;
        std::unique_ptr<Button> value;
        std::function<juce::String()> current;
        std::function<void(Button&)> change;
    };
    void addRow(const juce::String& label, const juce::String& help, std::function<juce::String()> current, std::function<void(Button&)> change);
    Context& ctx_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::unique_ptr<ChoiceBox> offlineQuality;
    TextField author{"Your name, for new presets", "Default author"};
    Button close{"", Glyph::Close};
};

} // namespace zui
