// Feeds fuse::platform v2 events (platform/events_v2.hpp, UI1) into a UiContext. Header-only so fuse_ui_core keeps
// no link dependency on fuse_core; include it where both are available (apps, the editor shell, game HUD glue).
#pragma once

#include "fuse/ui/ui.hpp"

#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/events_v2.hpp>
#include <fuse/platform/text_services.hpp>
#include <fuse/platform/window.hpp>

namespace fuse::ui {

constexpr Key toUiKey(fuse::platform::KeyV2 k) {
    using K2 = fuse::platform::KeyV2;
    switch (k) {
    case K2::Tab: return Key::Tab;
    case K2::Enter: case K2::NumpadEnter: return Key::Enter;
    case K2::Escape: return Key::Escape;
    case K2::Space: return Key::Space;
    case K2::Backspace: return Key::Backspace;
    case K2::Delete: return Key::Delete;
    case K2::Left: return Key::Left;
    case K2::Right: return Key::Right;
    case K2::Up: return Key::Up;
    case K2::Down: return Key::Down;
    case K2::Home: return Key::Home;
    case K2::End: return Key::End;
    case K2::PageUp: return Key::PageUp;
    case K2::PageDown: return Key::PageDown;
    case K2::A: return Key::A;
    case K2::C: return Key::C;
    case K2::V: return Key::V;
    case K2::X: return Key::X;
    case K2::Y: return Key::Y;
    case K2::Z: return Key::Z;
    case K2::F2: return Key::F2;
    case K2::W: return Key::W;
    case K2::S: return Key::S;
    case K2::D: return Key::D;
    case K2::Q: return Key::Q;
    case K2::E: return Key::E;
    case K2::N: return Key::N;
    case K2::O: return Key::O;
    case K2::F5: return Key::F5;
    case K2::F10: return Key::F10;
    case K2::Num1: return Key::Num1;
    case K2::Num2: return Key::Num2;
    case K2::Num3: return Key::Num3;
    case K2::Num4: return Key::Num4;
    case K2::COUNT: return Key::None;
    default: return Key::Other;
    }
}

constexpr u8 toUiMods(u8 m) {
    using namespace fuse::platform;
    return u8(((m & kKeyModShift) ? kModShift : 0) | ((m & kKeyModCtrl) ? kModCtrl : 0) | ((m & kKeyModAlt) ? kModAlt : 0) |
              ((m & kKeyModSuper) ? kModSuper : 0));
}

/// Routes one event; returns true when the UI consumed it (the game should ignore it). `dpiScale` divides pointer
/// coordinates (physical pixels → UI units). Pass the pump the event came from so IME pre-edit longer than the
/// event's inline text (EventPump::composition()) and its caret reach the UI.
inline bool feed(UiContext& ui, const fuse::platform::EventV2& e, float dpiScale = 1.f,
                 const fuse::platform::EventPump* pump = nullptr) {
    using T = fuse::platform::EventV2Type;
    const Vec2 p{e.x / dpiScale, e.y / dpiScale};
    const MouseButton b = e.mouseButton == 2 ? MouseButton::Right : (e.mouseButton == 3 ? MouseButton::Middle : MouseButton::Left);
    switch (e.type) {
    case T::MouseMove: return ui.pointerMove(p);
    case T::MouseButtonDown: return ui.pointerDown(p, b);
    case T::MouseButtonUp: return ui.pointerUp(p, b);
    case T::Wheel: return ui.wheel(e.x == 0.f && e.y == 0.f ? ui.pointer() : p, {e.wheelX, e.wheelY}); // (0,0): legacy
    case T::KeyDown: return ui.keyDown(toUiKey(e.key), toUiMods(e.mods));
    case T::KeyUp: return ui.keyUp(toUiKey(e.key), toUiMods(e.mods));
    case T::TextInput: return ui.textInput(e.textView());
    case T::TextComposition:
        if (pump != nullptr) return ui.textComposition(pump->composition(), pump->compositionCursor());
        return ui.textComposition(e.textView(), i32(e.textView().size()));
    case T::TouchDown: return ui.pointerDown(p);
    case T::TouchMove: return ui.pointerMove(p);
    case T::TouchUp: return ui.pointerUp(p);
    case T::WindowResized: ui.setViewport({float(e.width) / dpiScale, float(e.height) / dpiScale}); return false;
    default: return false;
    }
}

/// v1 convenience: feed a PlatformEvent (MouseWheel, TextInput, TextComposition included) from `pump`.
inline bool feed(UiContext& ui, const fuse::platform::PlatformEvent& e, const fuse::platform::EventPump& pump,
                 float dpiScale = 1.f) {
    return feed(ui, fuse::platform::fromLegacy(e), dpiScale, &pump);
}

/// Routes the context's clipboard to the platform clipboard (Win32 CF_UNICODETEXT; process-local elsewhere).
inline void usePlatformClipboard(UiContext& ui) {
    ClipboardHooks h;
    h.set = [](void*, std::string_view s) { return fuse::platform::setClipboardText(s); };
    h.get = [](void*, std::string& out) { return fuse::platform::clipboardText(out); };
    ui.setClipboard(h);
}

/// Call once per frame after the UI update: opens / closes the window's text-input session (IME, virtual keyboard)
/// as text fields gain / lose focus, and keeps the IME candidate window at the caret (`dpiScale` multiplies UI units
/// back to client pixels).
inline void syncTextInput(UiContext& ui, fuse::platform::Window& window, float dpiScale = 1.f) {
    const bool want = ui.wantsTextInput();
    if (want != fuse::platform::textInputActive(window)) {
        if (want) fuse::platform::beginTextInput(window);
        else fuse::platform::endTextInput(window);
    }
    if (want) {
        const Rect c = ui.textCaret();
        fuse::platform::setTextInputArea(window, i32(c.x * dpiScale), i32(c.y * dpiScale), i32(c.w * dpiScale) + 1,
                                         i32(c.h * dpiScale));
    }
}

} // namespace fuse::ui
