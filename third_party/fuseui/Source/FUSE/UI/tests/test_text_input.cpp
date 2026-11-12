// fuse_ui_text_input (UI1 UI-side gate): IME pre-edit shown inline and committed through TextInput, the text-input
// session follows focus (caret rectangle for the candidate window), clipboard cut / copy / paste, and platform events
// (MouseWheel, TextInput, TextComposition) reach the context through platform_bridge.
#include "fuse/ui/platform_bridge.hpp"
#include "test_util.hpp"

// Standalone build links input.cpp without the EventPump implementation (platform_window.cpp); InputState::applyPump
// is unused here.
bool fuse::platform::EventPump::pollEvent(PlatformEvent&) { return false; }
#if defined(_WIN32)
// Win32 text_services.cpp reads the HWND; these tests link only the input sources, not the window backend.
fuse::platform::NativeWindowHandle fuse::platform::Window::nativeHandle() const { return {}; }
#endif

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {488, 400}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    UiTestDriver drv(ui);
    const NodeHandle name = ui.find("name");
    UI_CHECK(!ui.wantsTextInput());

    // focus the field: session opens, caret reported inside the field
    drv.click("name");
    UI_CHECK(ui.focused() == name);
    UI_CHECK(ui.wantsTextInput());
    const Rect field = ui.rect(name), caret = ui.textCaret();
    UI_CHECK(caret.h > 0.f && caret.x >= field.x && caret.x <= field.x + field.w && caret.y >= field.y);
    ui.editBuffer(name).selectAll();
    drv.key(Key::Delete);
    UI_CHECK(ui.text(name).empty());

    // IME: pre-edit is displayed but not committed; the caret follows the pre-edit caret
    drv.type("ab");
    const f32 caretBefore = ui.textCaret().x;
    ui.textComposition("\xE3\x81\x8B\xE3\x82\x93", 6); // "かん", caret at end
    ui.update(1.f / 60.f);
    UI_CHECK(ui.text(name) == "ab");
    UI_CHECK(ui.composition() == "\xE3\x81\x8B\xE3\x82\x93");
    UI_CHECK(ui.textCaret().x > caretBefore);
    int glyphsComposing = 0;
    for (const DrawCmd& c : ui.drawList().cmds()) glyphsComposing += c.op == DrawOp::Glyph;
    ui.textComposition({}, 0);
    drv.type("\xE6\xBC\xA2"); // commit "漢"
    UI_CHECK(ui.text(name) == "ab\xE6\xBC\xA2");
    UI_CHECK(ui.composition().empty());
    int glyphsAfter = 0;
    for (const DrawCmd& c : ui.drawList().cmds()) glyphsAfter += c.op == DrawOp::Glyph;
    UI_CHECK(glyphsComposing > glyphsAfter); // two pre-edit glyphs were drawn, one committed glyph now
    // pre-edit is dropped when focus moves
    ui.textComposition("x", 1);
    drv.click("bloom");
    UI_CHECK(ui.composition().empty());
    UI_CHECK(!ui.wantsTextInput());

    // clipboard: copy, cut, paste (newlines flattened in a single-line field)
    drv.click("name");
    ui.editBuffer(name).selectAll();
    drv.key(Key::C, kModCtrl);
    std::string clip;
    UI_CHECK(ui.clipboardText(clip) && clip == "ab\xE6\xBC\xA2");
    drv.key(Key::X, kModCtrl);
    UI_CHECK(ui.text(name).empty());
    UI_CHECK(ui.setClipboardText("one\ntwo"));
    drv.key(Key::V, kModCtrl);
    UI_CHECK(ui.text(name) == "one two");
    drv.key(Key::V, kModCtrl);
    UI_CHECK(ui.text(name) == "one twoone two");
    // custom clipboard hooks
    static std::string hooked;
    ClipboardHooks h;
    h.set = [](void*, std::string_view s) { hooked.assign(s); return true; };
    h.get = [](void*, std::string& out) { out = "hooked"; return true; };
    ui.setClipboard(h);
    ui.editBuffer(name).selectAll();
    drv.key(Key::C, kModCtrl);
    UI_CHECK(hooked == "one twoone two");
    drv.key(Key::V, kModCtrl);
    UI_CHECK(ui.text(name) == "hooked");

    // platform events through the bridge
    using namespace fuse::platform;
    EventPump* noPump = nullptr;
    PlatformEvent t{};
    t.type = PlatformEventType::TextInput;
    std::memcpy(t.text, "!", 1);
    UI_CHECK(feed(ui, fromLegacy(t), 1.f, noPump));
    UI_CHECK(ui.text(name) == "hooked!");
    PlatformEvent c{};
    c.type = PlatformEventType::TextComposition;
    std::memcpy(c.text, "zz", 2);
    UI_CHECK(feed(ui, fromLegacy(c)));
    UI_CHECK(ui.composition() == "zz" && ui.compositionCaret() == 2);
    ui.textComposition({}, 0);
    // wheel over the spin box changes it; the pointer position comes with the event (no MouseMove jump)
    const NodeHandle s = ui.find("fps");
    const f32 before = ui.value(s);
    const Rect sr = ui.rect(s);
    PlatformEvent w{};
    w.type = PlatformEventType::MouseWheel;
    w.mouseX = i32(sr.x + sr.w * 0.5f);
    w.mouseY = i32(sr.y + sr.h * 0.5f);
    w.wheelY = 120;
    feed(ui, fromLegacy(w));
    ui.update(1.f / 60.f);
    UI_CHECK(ui.value(s) != before);
    return finish("fuse_ui_text_input");
}
