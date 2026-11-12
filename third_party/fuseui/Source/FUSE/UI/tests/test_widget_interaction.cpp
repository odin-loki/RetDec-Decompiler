// fuse_ui_widget_interaction (UI6 Tier A): each widget's pointer + keyboard behaviour and signals.
#include "test_util.hpp"

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {488, 400}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    UiTestDriver drv(ui);
    int toggled = 0, changed = 0, submitted = 0;
    auto count = [](int& c) { return [&c](UiContext&, NodeHandle, const SignalArgs&) { ++c; }; };
    ui.connect(ui.find("bloom"), SignalKind::Toggled, count(toggled));
    ui.connect(ui.find("exposure"), SignalKind::ValueChanged, count(changed));
    ui.connect(ui.find("name"), SignalKind::Submitted, count(submitted));

    // checkbox / switch toggle
    drv.click("bloom");
    UI_CHECK(ui.checked(ui.find("bloom")));
    drv.click("vsync");
    UI_CHECK(!ui.checked(ui.find("vsync")));
    UI_CHECK_EQ(toggled, 1);
    // radio exclusivity among siblings
    drv.click("q.ultra");
    UI_CHECK(ui.checked(ui.find("q.ultra")));
    UI_CHECK(!ui.checked(ui.find("q.high")));
    // slider: click at the left edge → min, keyboard steps
    const NodeHandle s = ui.find("exposure");
    const Rect sr = ui.rect(s);
    drv.clickAt({sr.x + 1, sr.y + sr.h / 2});
    UI_CHECK(ui.value(s) < 0.05f);
    ui.setFocus(s, FocusReason::Keyboard);
    drv.key(Key::End);
    UI_CHECK_EQ(ui.value(s), 1.f);
    drv.key(Key::Left);
    UI_CHECK(ui.value(s) < 1.f);
    UI_CHECK(changed >= 3);
    // spinbox clamps
    const NodeHandle sp = ui.find("fps");
    ui.setFocus(sp, FocusReason::Keyboard);
    drv.key(Key::Up);
    UI_CHECK_EQ(ui.value(sp), 145.f);
    ui.setValue(sp, 1000.f);
    UI_CHECK_EQ(ui.value(sp), 240.f);
    // combobox: click opens the Tier B popup list (current item focused), Down + Enter picks, focus returns
    const NodeHandle cb = ui.find("backend");
    drv.click("backend");
    UI_CHECK(ui.alive(ui.firstChild(widgets::overlayLayer(ui))));
    UI_CHECK(ui.text(ui.focused()) == "Vulkan");
    drv.key(Key::Down);
    UI_CHECK(ui.text(ui.focused()) == "CPU raster");
    drv.key(Key::Enter);
    UI_CHECK(widgets::comboText(ui, cb) == "CPU raster");
    ui.update();
    UI_CHECK(!ui.alive(ui.firstChild(widgets::overlayLayer(ui))));
    UI_CHECK(ui.focused() == cb);
    ui.setFocus(cb, FocusReason::Keyboard);
    drv.key(Key::Down);
    UI_CHECK(widgets::comboText(ui, cb) == "Null");
    // line edit: typing, editing keys, submit
    const NodeHandle le = ui.find("name");
    drv.click("name");
    drv.key(Key::End);
    drv.type("_2");
    UI_CHECK(ui.text(le) == "player_one_2");
    drv.key(Key::Backspace);
    drv.key(Key::Home);
    drv.type(">");
    UI_CHECK(ui.text(le) == ">player_one_");
    drv.key(Key::A, kModCtrl);
    drv.type("\xC3\xA9t\xC3\xA9");
    UI_CHECK(ui.text(le) == "\xC3\xA9t\xC3\xA9");
    drv.key(Key::Backspace);
    UI_CHECK(ui.text(le) == "\xC3\xA9t");
    drv.key(Key::Z, kModCtrl);
    UI_CHECK(ui.text(le) == "\xC3\xA9t\xC3\xA9");
    drv.key(Key::Enter);
    UI_CHECK_EQ(submitted, 1);
    // scrollbar via wheel
    const NodeHandle sb = ui.find("hscroll");
    const f32 before = ui.value(sb);
    ui.pointerMove({ui.rect(sb).x + 5, ui.rect(sb).y + 2});
    ui.wheel({ui.rect(sb).x + 5, ui.rect(sb).y + 2}, {0, -1});
    ui.update();
    UI_CHECK(ui.value(sb) != before);
    // disabled widgets ignore input
    ui.setDisabled(ui.find("shadows"), true);
    drv.click("shadows");
    UI_CHECK(ui.checked(ui.find("shadows")));
    return finish("fuse_ui_widget_interaction");
}
