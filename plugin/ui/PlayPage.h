// The PLAY page (plan 3.9): the arpeggiator with its step lane and twelve patterns, chord memory,
// scale lock and strum, and the clip player with a piano-roll view, MIDI file import and export.
#pragma once

#include "Shell.h"

namespace zui {

class StepLane;
class ChordEditor;
class PianoRoll;

class PlayPage final : public Page, public juce::FileDragAndDropTarget {
public:
    explicit PlayPage(Context& ctx);
    ~PlayPage() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool reveal(const juce::String& module) override;
    void structureChanged() override;
    void applySimpleView() override;

    int clipSlot() const;  // 0..7
    int patternSlot() const; // 0..11
    PianoRoll& pianoRoll() { return *roll_; }
    StepLane& stepLane() { return *lane_; }
    bool importClipFile(const juce::File& file, juce::String& error);
    bool exportClipFile(const juce::File& file);
    void mouseDrag(const juce::MouseEvent& e) override; // dragging the clip out to the host
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    Parts parts_;
    std::unique_ptr<Card> arp_, chords_, clips_;
    std::unique_ptr<StepLane> lane_;
    std::unique_ptr<ChordEditor> chordEditor_;
    std::unique_ptr<PianoRoll> roll_;
    std::vector<FlowItem> arpItems_, chordItems_, clipItems_;
    TabStrip* clipTabs_ = nullptr;
    Caption* clipInfo_ = nullptr;
    std::unique_ptr<juce::Component> ticker_;
    int seenSlot_ = -1;
    juce::Component* dragButton_ = nullptr;
    bool draggingOut_ = false;
};

} // namespace zui
