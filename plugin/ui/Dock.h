// The fixed layers under every page (plan 3.13): the modulation dock and the keyboard.
//
// The dock holds the eight macros and the modulators (four envelopes, eight LFOs, two Flux): a
// chip for each to drag onto any control, and the selected one's display and controls. It stays
// on every page, so a modulator can be dragged onto an effect or a step as easily as onto a filter.
//
// The keyboard plays with the mouse and with the computer keyboard in two rows: Z S X D C V G B H N
// J M , L . ; / from C of the chosen octave, and Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P [ = ] from C an
// octave higher. Left and Right move the octave, Down and Up the velocity, so no note key changes
// them. It has pitch and modulation wheels and a panic button. A key that is let go always ends its note: releases are tracked by
// the keyboard itself, checked against the mouse buttons and the keys actually down every frame,
// and again by the processor against the keys the screen holds.
#pragma once

#include "CurveEditor.h"
#include "Displays.h"
#include "Shell.h"

#include <map>

namespace zui {

inline constexpr int kDockHeight = 194, kKeyboardHeight = 78;

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
    juce::String shownTitle() const; // the open modulator's name, as the card's header shows it

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
    // The note a computer key plays, in semitones above C of the keys' octave (the Q row starts
    // at 12), or -1 for a key that plays nothing. Takes the key or its shifted character.
    static int semitoneFor(int character);
    // The computer keys that play `semitone` above C of the octave: none, one, or two where the
    // rows overlap (C to E of the upper octave).
    static juce::StringArray lettersFor(int semitone);
    static constexpr int kSpan = 32; // the two rows reach from C to G two octaves up
    // Lets go of the computer keys that are no longer down, and of the mouse's key when no button
    // is (a release the host kept from the window). Tests replace how "down" is read.
    void checkHeld();
    std::function<bool(int key)> keyIsDown; // by the key's unshifted character; the real key state unless a test replaces it
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
    // What repaints when a key goes down or comes up: the key, its seams and, for a black key, the
    // shadow it casts at rest (the longer of its two).
    juce::Rectangle<int> paintArea(int note) const;
    bool shownDown(int note) const { return note >= 0 && note < 128 && shown_[static_cast<std::size_t>(note)]; }

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
    std::map<int, int> computerHeld_; // key (its unshifted character) -> note
    juce::RectangleList<int> dirty_;
};

// Pitch and modulation wheels beside the keys. A wheel hands its position to the processor, which
// plays the latest one; the pitch wheel springs back when let go, also when the host keeps the
// release from the window (checked every frame) and when the editor closes.
class Wheel final : public Widget {
public:
    Wheel(Context& ctx, bool pitch);
    ~Wheel() override;
    void draw(juce::Graphics& g) override;
    bool tick() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;
    float value() const { return value_; }
    bool held() const { return held_; }
    std::function<bool()> mouseIsDown = [] { return juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown(); };

private:
    void set(float v);
    void letGo();
    Context& ctx_;
    bool pitch_;
    float value_;
    bool held_ = false;
};

class KeyboardLayer final : public juce::Component {
public:
    explicit KeyboardLayer(Context& ctx);
    void paint(juce::Graphics& g) override;
    void resized() override;
    PianoKeys& piano() { return piano_; }
    Wheel& pitchWheel() { return *pitch_; }
    Wheel& modWheel() { return *mod_; }
    void refresh(); // octave and velocity as the preferences hold them
    void allNotesOff(); // stops every note and every sound (the command palette's "All notes off")

private:
    Context& ctx_;
    PianoKeys piano_;
    std::unique_ptr<Wheel> pitch_, mod_;
    Button octDown_{"", Glyph::Minus}, octUp_{"", Glyph::Plus}, velDown_{"", Glyph::Minus}, velUp_{"", Glyph::Plus};
    Caption octave_{"Oct 3", kTextSmall, false}, velocity_{"Vel 100", kTextSmall, false};
    CachedDrawing background_;
};

} // namespace zui
