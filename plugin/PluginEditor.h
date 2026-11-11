// Temporary generic editor for milestone 1: a searchable list of every parameter, an on-screen
// keyboard and a level meter. The real interface is milestone 5.
#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

class ZederiaEditor final : public juce::AudioProcessorEditor, private juce::ListBoxModel, private juce::Timer {
public:
    explicit ZederiaEditor(ZederiaProcessor& processor);
    ~ZederiaEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // For tests.
    int visibleParameterCount() const { return static_cast<int>(rows_.size()); }
    void setSearchText(const juce::String& text);
    juce::MidiKeyboardComponent& keyboard() { return keyboard_; }

private:
    class Row;
    class Meter;

    int getNumRows() override { return static_cast<int>(rows_.size()); }
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow(int row, bool selected, juce::Component* existing) override;
    void timerCallback() override;
    void rebuildRows();

    ZederiaProcessor& processor_;
    std::vector<int> rows_; // registry indices that match the search
    juce::Label title_, status_;
    juce::TextEditor search_;
    juce::ListBox list_;
    std::unique_ptr<Meter> meter_;
    juce::MidiKeyboardComponent keyboard_;
    int seenGeneration_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZederiaEditor)
};
