// Inside a host the computer keyboard belongs to the host: Space starts its transport and its
// typing keyboard plays notes. Zederia's window takes the keyboard only while the user types in one
// of its text fields, and hands it back when that ends. Clicks on knobs, buttons, lists and
// presets leave it with the host. The Standalone keeps the keyboard for its shortcuts and computer
// keys, as before.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

namespace zui {

// Whether clicks leave the keyboard with the host. Set from the processor when an editor's context
// is made; widgets read it as they are made.
bool keysStayWithHost();
void setKeysStayWithHost(bool on);

// Whether the user is typing in a text field inside `root`: an editable one that has the focus.
bool typingIn(const juce::Component& root);

// Takes the keyboard focus from `root`'s parts and, when `root`'s window has the keyboard, gives it
// to the host's window around it (Windows; elsewhere only the first).
void giveKeysToHost(juce::Component& root);

// Inside a host, Up and Down switch presets while the user works in Zederia (from a click in one of
// its windows until a click anywhere else in the host), although the host keeps the keyboard: the
// two keys are taken before the host sees them, and every other key (Space) stays the host's.
// Hosts ignore the keys a plug-in passes on to them, so Zederia cannot hold the keyboard and pass
// Space along. Nothing is taken while a Zederia window has the keyboard (a text field being typed
// in, a menu) or with Ctrl, Alt or Shift held. Windows only; elsewhere it does nothing.
class PresetKeys {
public:
    PresetKeys(juce::Component& window, std::function<void(int direction)> step);
    ~PresetKeys();
    PresetKeys(const PresetKeys&) = delete;
    PresetKeys& operator=(const PresetKeys&) = delete;

    bool armed() const { return armed_; }
    // What the thread's input hooks call (public for the tests). A mouse button went down over
    // `nativeWindow`. A key went down or up: true when it is taken from the host (its step runs
    // asynchronously, on the message thread).
    void clicked(void* nativeWindow);
    bool key(int virtualKey, bool down);

    // For the tests: whether the thread's input hooks are in place, and a key message as the keyboard
    // hook gets it (its virtual key and its lParam flags); true when it is taken from the host.
    static bool hooked();
    static bool hookKey(int virtualKey, std::int64_t flags);

private:
    juce::Component::SafePointer<juce::Component> window_;
    std::shared_ptr<std::function<void(int)>> step_;
    bool armed_ = false;
    bool taken_[2] = {false, false}; // Up and Down: their release is taken when their press was
};

} // namespace zui
