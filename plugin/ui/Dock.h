// The fixed layers under every page (plan 3.13): the modulation dock and the keyboard.
//
// The dock holds the eight macros and the modulators (four envelopes, eight LFOs, two Flux): a
// chip for each to drag onto any control, and the selected one's display and controls. It stays
// on every page, so a modulator can be dragged onto an effect or a step as easily as onto a filter.
//
// The keyboard plays with the mouse and with the computer keyboard (A W S E D F T G Y H U J K O L
// P ; play C to E; Z and X move the octave, C and V the velocity), with pitch and modulation
// wheels and a panic button. A key that is let go always ends its note: releases are tracked by
// the keyboard itself, checked against the mouse buttons and the keys actually down every frame,
// and again by the processor against the keys the screen holds.
#pragma once

#include "CurveEditor.h"
#include "Displays.h"
#include "Shell.h"

#include <map>

namespace zui {

inline constexpr int kDockHeight = 168, kKeyboardHeight = 78;

class MacroCard;
class ModulatorCard;

class ModDock final : public juce::Component {
public:
    explicit ModDock(Context& ctx);
    ~ModDock() override;
    void resized() override;
    void select(const juce::String& source); // "env2", "lfo3", "flux1"; macros: their card
    const juce::String& selected() const;
    void refreshNames();  // macro names
    void reloadShapes();  // the shapes on show follow the document (undo, presets, pasting)
    bool tickDock();      // a sync switch or an envelope's mode changed what shows
    void applySimpleView();
    ModulatorCard& modulators() { return *modulators_; }

private:
    std::unique_ptr<MacroCard> macros_;
    std::unique_ptr<ModulatorCard> modulators_;
};

// The keys. Notes go to the processor's keyboard state; each key counts what holds it (the mouse,
// one or more computer keys), so a note starts when the first lets go of it and ends with the last.
class PianoKeys final : public Widget {
public:
    explicit PianoKeys(Context& ctx);
    ~PianoKeys() override;
    void draw(juce::Graphics& g) override;
    bool tick() override;
    juce::RectangleList<int> dirtyArea() override { return dirty_; }
    bool hasVisibleLabel() const override { return false; }
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void visibilityChanged() override;

    // Computer keys: true when the key plays (or moves the octave or velocity).
    bool computerKeyDown(const juce::KeyPress& key);
    // Lets go of the computer keys that are no longer down, and of the mouse's key when no button
    // is (a release the host kept from the window). Tests replace how "down" is read.
    void checkHeld();
    std::function<bool(int keyCode)> keyIsDown = [](int code) { return juce::KeyPress::isKeyCurrentlyDown(code); };
    std::function<bool()> mouseIsDown = [] { return juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown(); };

    void releaseAll();
    int heldCount() const;
    bool held(int note) const { return note >= 0 && note < 128 && holds_[static_cast<std::size_t>(note)] > 0; }
    int octave() const;
    void setOctave(int octave);
    int velocity() const;
    void setVelocity(int velocity);
    std::function<void()> onSettings; // octave or velocity changed

    // Geometry, public for tests.
    int lowest = 24, highest = 96; // C1 .. C7
    int keyAt(juce::Point<float> p) const;
    juce::Rectangle<float> keyBounds(int note) const;
    static bool isBlack(int note);
    static const char* letters(); // the computer keys, from C upwards

private:
    void press(int note, float velocity);
    void release(int note);
    float velocityAt(int note, float y) const;
    int whiteCount() const;
    int whiteIndex(int note) const; // whites below the note within the range

    Context& ctx_;
    std::array<std::uint8_t, 128> holds_{};
    std::array<bool, 128> shown_{};
    int mouseNote_ = -1;
    std::map<int, int> computerHeld_; // key code -> note
    juce::RectangleList<int> dirty_;
};

class KeyboardLayer final : public juce::Component {
public:
    explicit KeyboardLayer(Context& ctx);
    void paint(juce::Graphics& g) override;
    void resized() override;
    PianoKeys& piano() { return piano_; }
    void refresh(); // octave, velocity and the computer-keys switch as the preferences hold them

private:
    Context& ctx_;
    PianoKeys piano_;
    std::unique_ptr<Widget> pitch_, mod_;
    Button octDown_{"", Glyph::Minus}, octUp_{"", Glyph::Plus}, velDown_{"", Glyph::Minus}, velUp_{"", Glyph::Plus};
    Caption octave_{"Oct 3", kTextSmall, false}, velocity_{"Vel 100", kTextSmall, false};
    Button keysButton_{"Keys", Glyph::Keyboard}, panic_{"Panic", Glyph::Stop};
    CachedDrawing background_;
};

} // namespace zui
