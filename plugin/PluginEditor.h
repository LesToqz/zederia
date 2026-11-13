// Temporary generic editor: a searchable list of every parameter, pickers for the wavetable of
// each oscillator and for the texture sample, an on-screen keyboard and a level meter. The real
// interface is milestone 5.
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
    // Slot 0..2 is an oscillator's table, slot 3 the texture sample; item 0 is the built-in one.
    int assetChoiceCount(int slot) const { return assetBox_[slot].getNumItems(); }
    void chooseAsset(int slot, int item) { assetBox_[slot].setSelectedItemIndex(item, juce::sendNotificationSync); }
    juce::String chosenAsset(int slot) const { return assetBox_[slot].getText(); }

private:
    class Row;
    class Meter;

    int getNumRows() override { return static_cast<int>(rows_.size()); }
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow(int row, bool selected, juce::Component* existing) override;
    void timerCallback() override;
    void rebuildRows();
    void refreshAssetBoxes();

    ZederiaProcessor& processor_;
    std::vector<int> rows_; // registry indices that match the search
    juce::Label title_, status_;
    juce::TextEditor search_;
    juce::Label assetLabel_[4];
    juce::ComboBox assetBox_[4];
    std::vector<std::string> assetRefs_[4]; // the reference behind each item of a box
    bool fillingBoxes_ = false;
    juce::ListBox list_;
    std::unique_ptr<Meter> meter_;
    juce::MidiKeyboardComponent keyboard_;
    int seenGeneration_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZederiaEditor)
};
