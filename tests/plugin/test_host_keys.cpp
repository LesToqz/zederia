// Inside a host the keyboard goes back to the host's window (ui/HostKeys.h). Real windows, kept off
// the screen: a stand-in for the host's window and a plug-in window inside it, as the VST3 wrapper
// makes one.
#include "ui/HostKeys.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

TEST_CASE("host keys: the keyboard goes back to the host's window unless a text field is being typed in", "[editor][focus][host]") {
    juce::Component host; // the host's window
    host.setBounds(-4000, -4000, 400, 300);
    host.addToDesktop(0);
    host.setVisible(true);
    REQUIRE(host.getPeer() != nullptr);

    juce::Component plugin; // the plug-in's window inside it
    juce::TextEditor field("name");
    juce::TextEditor preview("preview");
    preview.setReadOnly(true);
    juce::Component knob;
    knob.setWantsKeyboardFocus(true);
    plugin.setBounds(0, 0, 300, 200);
    field.setBounds(10, 10, 120, 24);
    preview.setBounds(10, 40, 120, 24);
    knob.setBounds(10, 80, 40, 40);
    for (juce::Component* c : {static_cast<juce::Component*>(&field), static_cast<juce::Component*>(&preview), &knob})
        plugin.addAndMakeVisible(c);
    plugin.addToDesktop(0, host.getPeer()->getNativeHandle());
    plugin.setVisible(true);
    REQUIRE(plugin.getPeer() != nullptr);
    const auto pluginWindow = static_cast<HWND>(plugin.getPeer()->getNativeHandle());
    const auto hostWindow = static_cast<HWND>(host.getPeer()->getNativeHandle());
    REQUIRE(GetAncestor(pluginWindow, GA_ROOT) == hostWindow);

    // Typing a name: the plug-in keeps the keyboard.
    field.grabKeyboardFocus();
    REQUIRE(field.hasKeyboardFocus(false));
    CHECK(zui::typingIn(plugin));
    // A read-only text and a control are not typing.
    preview.grabKeyboardFocus();
    CHECK_FALSE(zui::typingIn(plugin));
    knob.grabKeyboardFocus();
    REQUIRE(knob.hasKeyboardFocus(false));
    CHECK(GetFocus() == pluginWindow);
    CHECK_FALSE(zui::typingIn(plugin));

    // Given back: no part has it, and the host's window has the keyboard (Space reaches the host).
    zui::giveKeysToHost(plugin);
    CHECK(juce::Component::getCurrentlyFocusedComponent() == nullptr);
    CHECK(GetFocus() == hostWindow);
    // Nothing to give back: nothing changes.
    zui::giveKeysToHost(plugin);
    CHECK(GetFocus() == hostWindow);

    plugin.removeFromDesktop();
    host.removeFromDesktop();
}

namespace {

// The keyboard hook's flags for a key going down and coming up (bit 31), as Windows sends them.
constexpr std::int64_t kPress = 0x00000001, kRelease = 0xC0000001;

// Runs what is waiting on the message thread (the steps are sent asynchronously).
void pump() {
    for (int round = 0; round < 5; ++round) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
}

} // namespace

TEST_CASE("host keys: Up and Down switch presets after a click in the plug-in, every other key stays the host's", "[editor][focus][host]") {
    // A host's window that is not one of the plug-in's own. (Keys posted in a test never reach the
    // keyboard hook, which Windows calls for real key presses only; real ones would go to the user's
    // window in front, so the hook's handling is called as Windows calls it.)
    const HWND hostWindow = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW, -4000, -4000, 400, 300, nullptr, nullptr,
                                            GetModuleHandleW(nullptr), nullptr);
    REQUIRE(hostWindow != nullptr);
    ShowWindow(hostWindow, SW_SHOWNA);

    juce::Component plugin;
    juce::TextEditor field("search");
    plugin.setBounds(0, 0, 300, 200);
    field.setBounds(10, 10, 120, 24);
    plugin.addAndMakeVisible(field);
    plugin.addToDesktop(0, hostWindow);
    plugin.setVisible(true);
    REQUIRE(plugin.getPeer() != nullptr);
    const auto pluginWindow = static_cast<HWND>(plugin.getPeer()->getNativeHandle());

    std::vector<int> steps;
    {
        zui::PresetKeys keys(plugin, [&](int direction) { steps.push_back(direction); });
        CHECK(zui::PresetKeys::hooked()); // the thread's keyboard and mouse hooks are in place
        SetFocus(hostWindow);             // the host has the keyboard, as inside FL Studio
        REQUIRE(GetFocus() == hostWindow);

        // Before any click in the plug-in, the arrows are the host's.
        CHECK_FALSE(zui::PresetKeys::hookKey(VK_DOWN, kPress));
        pump();
        CHECK(steps.empty());

        // A click in the plug-in: Up and Down are taken, press and release, and step once per press
        // (a held key repeats its press); Space, Left and Right are the host's.
        keys.clicked(pluginWindow);
        CHECK(keys.armed());
        CHECK(zui::PresetKeys::hookKey(VK_DOWN, kPress));
        CHECK(zui::PresetKeys::hookKey(VK_DOWN, kRelease));
        CHECK(zui::PresetKeys::hookKey(VK_UP, kPress));
        CHECK(zui::PresetKeys::hookKey(VK_UP, kPress | 0x40000000)); // auto-repeat
        CHECK(zui::PresetKeys::hookKey(VK_UP, kRelease));
        for (const int other : {VK_SPACE, VK_LEFT, VK_RIGHT, static_cast<int>('Z')}) {
            CHECK_FALSE(zui::PresetKeys::hookKey(other, kPress));
            CHECK_FALSE(zui::PresetKeys::hookKey(other, kRelease));
        }
        pump();
        CHECK(steps == std::vector<int>{1, -1, -1});

        // Typing in the plug-in's search field: its own window has the keyboard, nothing is taken.
        field.grabKeyboardFocus();
        REQUIRE(GetFocus() == pluginWindow);
        CHECK_FALSE(zui::PresetKeys::hookKey(VK_DOWN, kPress));
        CHECK_FALSE(zui::PresetKeys::hookKey(VK_DOWN, kRelease));
        SetFocus(hostWindow);

        // A click anywhere else in the host: the arrows are the host's again.
        keys.clicked(hostWindow);
        CHECK_FALSE(keys.armed());
        CHECK_FALSE(zui::PresetKeys::hookKey(VK_DOWN, kPress));
        pump();
        CHECK(steps.size() == 3);
    }
    // Gone with the editor: the hooks are removed and nothing is taken.
    CHECK_FALSE(zui::PresetKeys::hooked());
    CHECK_FALSE(zui::PresetKeys::hookKey(VK_DOWN, kPress));

    plugin.removeFromDesktop();
    DestroyWindow(hostWindow);
}
#endif
