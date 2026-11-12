// fuse_ui_platform_keys_v2 (UI1): KeyV2 / EventV2 back-compat with the v1 Key / PlatformEvent, names, virtual keys,
// the legacy wheel tag, and the UiContext bridge.
#include "test_util.hpp"

#include "fuse/platform/events_v2.hpp"
#include "fuse/ui/platform_bridge.hpp"

using namespace fuse::platform;
using fuse::ui::test::failures;
using fuse::u16;
using fuse::u32;

// input.cpp (the v1 key tables) is compiled in without the rest of fuse_core; InputState::update's pump is unused here.
bool fuse::platform::EventPump::pollEvent(PlatformEvent&) { return false; }
#if defined(_WIN32)
// Win32 text_services.cpp reads the HWND; these tests link only the input sources, not the window backend.
fuse::platform::NativeWindowHandle fuse::platform::Window::nativeHandle() const { return {}; }
#endif

int main() {
    // every v1 key round-trips unchanged
    for (u16 i = 0; i < u16(Key::COUNT); ++i) {
        UI_CHECK(toLegacy(fromLegacy(Key(i))) == Key(i));
        UI_CHECK(keyV2Name(KeyV2(i)) == keyName(Key(i)));
    }
    // new keys fold back where meaningful, else COUNT
    UI_CHECK(toLegacy(KeyV2::Numpad7) == Key::Num7);
    UI_CHECK(toLegacy(KeyV2::RightShift) == Key::Shift);
    UI_CHECK(toLegacy(KeyV2::NumpadEnter) == Key::Enter);
    UI_CHECK(toLegacy(KeyV2::VolumeUp) == Key::COUNT);
    // names are unique and round-trip
    for (u16 i = 0; i < u16(KeyV2::COUNT); ++i) {
        const auto n = keyV2Name(KeyV2(i));
        UI_CHECK(!n.empty());
        UI_CHECK(keyV2FromName(n) == KeyV2(i));
    }
    // virtual keys
    UI_CHECK(keyV2FromVirtualKey(0x67) == KeyV2::Numpad7);
    UI_CHECK(keyV2FromVirtualKey(0x7C) == KeyV2::F13);
    UI_CHECK(keyV2FromVirtualKey('Q') == KeyV2::Q);
    for (u16 i = 0; i < u16(KeyV2::COUNT); ++i) {
        const u32 vk = virtualKeyFromKeyV2(KeyV2(i));
        if (vk && KeyV2(i) != KeyV2::NumpadEnter && KeyV2(i) != KeyV2::Super && keyV2FromVirtualKey(vk) != KeyV2(i)) { std::fprintf(stderr, "vk %x: %s -> %s\n", vk, std::string(keyV2Name(KeyV2(i))).c_str(), std::string(keyV2Name(keyV2FromVirtualKey(vk))).c_str()); UI_CHECK(false); }
    }
    // retired legacy wheel tag (recorded streams) still decodes; v2 → v1 now yields a real MouseWheel
    PlatformEvent w;
    w.type = PlatformEventType::MouseMove;
    w.keyCode = 0xFFFFu;
    w.mouseY = -240;
    const EventV2 v = fromLegacy(w);
    UI_CHECK(v.type == EventV2Type::Wheel);
    UI_CHECK(v.wheelY == -2.f);
    const auto back = toLegacy(v);
    UI_CHECK(back && back->type == PlatformEventType::MouseWheel && back->wheelY == -240);
    EventV2 t;
    t.type = EventV2Type::TextInput;
    t.setText("\xC3\xA9");
    const auto tl = toLegacy(t);
    UI_CHECK(tl && tl->type == PlatformEventType::TextInput && std::string_view(tl->text) == "\xC3\xA9");
    EventV2 touch;
    touch.type = EventV2Type::TouchDown;
    UI_CHECK(!toLegacy(touch));
    UI_CHECK(t.textView() == "\xC3\xA9");
    // v1 key event → v2 → v1 keeps the code
    PlatformEvent k;
    k.type = PlatformEventType::KeyDown;
    k.keyCode = 0x25; // VK_LEFT
    const EventV2 kv = fromLegacy(k);
    UI_CHECK(kv.key == KeyV2::Left);
    UI_CHECK(toLegacy(kv)->keyCode == 0x25u);

    // bridge into the UI
    using namespace fuse::ui;
    UiContext ui({.viewport = {300, 100}, .font = &fuse::ui::test::font()});
    widgets::install(ui);
    const NodeHandle le = widgets::lineEdit(ui, ui.root(), "", "le");
    ui.update();
    ui.setFocus(le);
    EventV2 ti;
    ti.type = EventV2Type::TextInput;
    ti.setText("hi");
    UI_CHECK(feed(ui, ti));
    EventV2 kd;
    kd.type = EventV2Type::KeyDown;
    kd.key = KeyV2::Backspace;
    UI_CHECK(feed(ui, kd));
    UI_CHECK(ui.text(le) == "h");
    UI_CHECK(toUiKey(KeyV2::NumpadEnter) == fuse::ui::Key::Enter);
    return fuse::ui::test::finish("fuse_ui_platform_keys_v2");
}
