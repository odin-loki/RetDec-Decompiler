// Gate fuse_ui_focus_order: Tab / Shift+Tab document order, positive tab indices first, skipping disabled / hidden /
// non-focusable, wrap-around, modal focus trap, focus-visible only for keyboard focus, spatial navigation, keyboard
// activation (Enter / Space → Clicked).
#include "test_util.hpp"

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {400, 300}, .font = &font()});
    widgets::install(ui);
    using namespace widgets;
    const NodeHandle row = panel(ui, ui.root(), "row");
    ui.addClass(row, "row");
    const NodeHandle a = button(ui, row, "A", "a");
    const NodeHandle b = button(ui, row, "B", "b");
    const NodeHandle c = button(ui, row, "C", "c");
    const NodeHandle row2 = panel(ui, ui.root(), "row2");
    ui.addClass(row2, "row");
    const NodeHandle d = lineEdit(ui, row2, "", "d");
    const NodeHandle e = checkBox(ui, row2, "E", false, "e");
    label(ui, row2, "not focusable", "lbl");
    ui.update();

    std::vector<NodeHandle> order;
    ui.tabOrder(order);
    UI_CHECK_EQ(order.size(), std::size_t(5));

    UI_CHECK(ui.keyDown(Key::Tab));
    UI_CHECK(ui.focused() == a);
    UI_CHECK(ui.focusVisible());
    UI_CHECK(ui.states(a) & kStateFocusVisible);
    ui.keyDown(Key::Tab);
    UI_CHECK(ui.focused() == b);
    ui.keyDown(Key::Tab, kModShift);
    UI_CHECK(ui.focused() == a);
    ui.keyDown(Key::Tab, kModShift);
    UI_CHECK(ui.focused() == e); // wraps

    // disabled and hidden are skipped
    ui.setDisabled(b, true);
    ui.setCss(c, "display", "none");
    ui.update();
    ui.setFocus(a, FocusReason::Keyboard);
    ui.keyDown(Key::Tab);
    UI_CHECK(ui.focused() == d);
    ui.setDisabled(b, false);
    ui.setCss(c, "display", "flex");
    ui.update();

    // positive tab index first
    ui.setTabIndex(e, 1);
    ui.tabOrder(order);
    UI_CHECK(order[0] == e);
    ui.setTabIndex(e, 0);

    // pointer focus is not focus-visible
    UiTestDriver drv(ui);
    drv.click("d");
    UI_CHECK(ui.focused() == d);
    UI_CHECK(!ui.focusVisible());
    UI_CHECK(!(ui.states(d) & kStateFocusVisible));
    // clicking a non-focusable area clears focus
    drv.clickAt({390, 290});
    UI_CHECK(!ui.focused().valid());

    // keyboard activation
    int clicks = 0;
    ui.connect(b, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    ui.setFocus(b, FocusReason::Keyboard);
    drv.key(Key::Enter);
    drv.key(Key::Space);
    UI_CHECK_EQ(clicks, 2);
    ui.setFocus(e, FocusReason::Keyboard);
    drv.key(Key::Space);
    UI_CHECK(ui.checked(e));

    // spatial navigation (gamepad / arrows)
    ui.setSpatialNavigation(true);
    ui.setFocus(a, FocusReason::Keyboard);
    UI_CHECK(ui.keyDown(Key::Right));
    UI_CHECK(ui.focused() == b);
    UI_CHECK(ui.navigate(NavDir::Down));
    UI_CHECK(ui.focused() == d || ui.focused() == e);
    ui.setSpatialNavigation(false);

    // modal trap
    const NodeHandle dlg = panel(ui, ui.root(), "dialog");
    const NodeHandle ok = button(ui, dlg, "OK", "ok");
    const NodeHandle no = button(ui, dlg, "No", "no");
    ui.update();
    ui.setModal(dlg);
    UI_CHECK(ui.focused() == ok);
    ui.keyDown(Key::Tab);
    UI_CHECK(ui.focused() == no);
    ui.keyDown(Key::Tab);
    UI_CHECK(ui.focused() == ok);
    ui.setFocus(a);
    UI_CHECK(ui.focused() == ok); // outside the modal: refused
    ui.setModal({});
    ui.setFocus(a);
    UI_CHECK(ui.focused() == a);
    return finish("fuse_ui_focus_order");
}
