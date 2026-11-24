#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "HostKeys.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace zui {

namespace {
bool& stayWithHost() {
    static bool on = false;
    return on;
}

// The open editors inside a host; the hooks exist while there is one (all on the message thread).
std::vector<PresetKeys*>& watchers() {
    static std::vector<PresetKeys*> list;
    return list;
}

#ifdef _WIN32
HHOOK keyHook = nullptr, mouseHook = nullptr;

// Called as the host's message loop takes a key message from the thread's queue, before the host
// handles it: returning 1 drops it.
LRESULT CALLBACK onKey(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && PresetKeys::hookKey(static_cast<int>(wParam), static_cast<std::int64_t>(lParam)))
        return 1;
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Only watches: every click goes on to its window.
LRESULT CALLBACK onMouse(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
        case WM_NCMBUTTONDOWN:
        case WM_NCXBUTTONDOWN:
            for (auto* w : watchers())
                w->clicked(reinterpret_cast<const MOUSEHOOKSTRUCT*>(lParam)->hwnd);
            break;
        default:
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// One of this plug-in's own windows: the editor, a menu or a dialog it opened.
bool ownWindow(HWND h) {
    if (h == nullptr)
        return false;
    for (int i = juce::ComponentPeer::getNumPeers(); --i >= 0;)
        if (auto* peer = juce::ComponentPeer::getPeer(i); peer != nullptr) {
            const auto w = static_cast<HWND>(peer->getNativeHandle());
            if (w == h || IsChild(w, h))
                return true;
        }
    return false;
}
#endif
} // namespace

PresetKeys::PresetKeys(juce::Component& window, std::function<void(int)> step)
    : window_(&window), step_(std::make_shared<std::function<void(int)>>(std::move(step))) {
    watchers().push_back(this);
#ifdef _WIN32
    if (keyHook == nullptr)
        keyHook = SetWindowsHookExW(WH_KEYBOARD, onKey, nullptr, GetCurrentThreadId());
    if (mouseHook == nullptr)
        mouseHook = SetWindowsHookExW(WH_MOUSE, onMouse, nullptr, GetCurrentThreadId());
#endif
}

PresetKeys::~PresetKeys() {
    auto& list = watchers();
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
#ifdef _WIN32
    if (list.empty()) {
        if (keyHook != nullptr)
            UnhookWindowsHookEx(keyHook);
        if (mouseHook != nullptr)
            UnhookWindowsHookEx(mouseHook);
        keyHook = mouseHook = nullptr;
    }
#endif
}

bool PresetKeys::hooked() {
#ifdef _WIN32
    return keyHook != nullptr && mouseHook != nullptr;
#else
    return false;
#endif
}

bool PresetKeys::hookKey(int virtualKey, std::int64_t flags) {
    const bool down = (static_cast<std::uint64_t>(flags) & 0x80000000u) == 0; // bit 31: the key is going up
    for (auto* w : watchers())
        if (w->key(virtualKey, down))
            return true;
    return false;
}

void PresetKeys::clicked([[maybe_unused]] void* nativeWindow) {
#ifdef _WIN32
    armed_ = ownWindow(static_cast<HWND>(nativeWindow));
#endif
}

bool PresetKeys::key([[maybe_unused]] int virtualKey, [[maybe_unused]] bool down) {
#ifdef _WIN32
    const int which = virtualKey == VK_UP ? 0 : virtualKey == VK_DOWN ? 1 : -1;
    if (which < 0)
        return false;
    const auto i = static_cast<std::size_t>(which);
    if (!down) {
        const bool taken = taken_[i];
        taken_[i] = false;
        return taken;
    }
    if (!armed_ || window_ == nullptr || !window_->isShowing())
        return false;
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 || GetKeyState(VK_SHIFT) < 0)
        return false;
    if (ownWindow(GetFocus()))
        return false; // a Zederia window has the keyboard and reads its own keys
    taken_[i] = true;
    const std::weak_ptr<std::function<void(int)>> step = step_;
    const int direction = which == 0 ? -1 : 1;
    juce::MessageManager::callAsync([step, direction] {
        if (auto s = step.lock())
            (*s)(direction);
    });
    return true;
#else
    return false;
#endif
}

bool keysStayWithHost() { return stayWithHost(); }
void setKeysStayWithHost(bool on) { stayWithHost() = on; }

bool typingIn(const juce::Component& root) {
    auto* text = dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent());
    return text != nullptr && root.isParentOf(text) && text->isShowing() && text->isEnabled() && !text->isReadOnly();
}

void giveKeysToHost(juce::Component& root) {
    if (auto* focused = juce::Component::getCurrentlyFocusedComponent(); focused != nullptr && (focused == &root || root.isParentOf(focused)))
        focused->giveAwayKeyboardFocus();
#ifdef _WIN32
    // Unused keys would only be posted to the host's window, and some hosts (FL Studio) ignore
    // posted keys, so the window itself goes back to the host.
    auto* peer = root.getPeer();
    if (peer == nullptr)
        return;
    const auto own = static_cast<HWND>(peer->getNativeHandle());
    if (own == nullptr || GetFocus() != own)
        return;
    const HWND host = GetAncestor(own, GA_ROOT);
    if (host != nullptr && host != own)
        SetFocus(host);
#endif
}

} // namespace zui
