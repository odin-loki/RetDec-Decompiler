// fuse_ui_tier_b: Tier B widgets — overlay layer, popup menu / combo list, menu bar, modal dialog, tooltip,
// virtualised list view, tree view, tab widget, scroll area. Optional argv[1]: write a PNG of the demo screen.
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
bool scopeContains(const UiContext& ui, NodeHandle scope, NodeHandle n) {
    for (; ui.alive(n); n = ui.parent(n))
        if (n == scope) return true;
    return false;
}
} // namespace

int main(int argc, char** argv) {
    UiContext ui({.viewport = {640, 420}, .font = &font()});
    w::install(ui);
    const NodeHandle root = ui.root();
    ui.setCss(root, "padding", "0px");
    const NodeHandle bar = w::menuBar(ui, root, "menubar");
    w::menuBarMenu(ui, bar, "File", "New\nOpen\n-\nQuit", "file");
    w::menuBarMenu(ui, bar, "Edit", "Undo\nRedo", "edit");
    const NodeHandle body = w::panel(ui, root, "body");
    ui.addClass(body, "row");
    ui.setCss(body, "padding", "10px");
    ui.setCss(body, "align-items", "start");

    std::string items;
    for (int i = 0; i < 10000; ++i) items += (i ? "\n" : "") + std::string("Item ") + std::to_string(i);
    const NodeHandle list = w::listView(ui, body, items, -1, "list");
    ui.setCss(list, "height", "200px");
    const NodeHandle tree = w::treeView(ui, body, "Scene\n\tCamera\n\tLights\n\t\tSun\n\t\tFill\n\tMeshes\n\t\tTerrain\nAssets", "tree");
    const NodeHandle tabs = w::tabWidget(ui, body, "General\nRender\nAudio", "tabs");
    w::label(ui, w::tabPage(ui, tabs, 0), "General page", "page0");
    w::label(ui, w::tabPage(ui, tabs, 1), "Render page", "page1");
    w::button(ui, w::tabPage(ui, tabs, 0), "Hover me", "tipbtn");
    const NodeHandle sa = w::scrollArea(ui, body, "scroll");
    ui.setCss(sa, "height", "80px");
    for (int i = 0; i < 12; ++i) w::label(ui, sa, "row " + std::to_string(i));
    ui.update();

    // --- overlay layer stays out of layout -----------------------------------------------------------------------
    const Rect listBefore = ui.rect(list);
    const NodeHandle layer = w::overlayLayer(ui);
    ui.update();
    UI_CHECK(ui.rect(list).x == listBefore.x && ui.rect(list).y == listBefore.y && ui.rect(list).w == listBefore.w);
    UI_CHECK_RECT(ui.rect(layer), 0, 0, 640, 420);
    UI_CHECK(ui.hitTest({5.f, 300.f}) != layer);

    UiTestDriver drv(ui);
    // --- list view: click, keys, wheel, virtualised paint ---------------------------------------------------------
    int changed = 0;
    ui.connect(list, SignalKind::ValueChanged, [&changed](UiContext&, NodeHandle, const SignalArgs&) { ++changed; });
    const Rect lc = ui.contentRect(list);
    drv.clickAt({lc.x + 10.f, lc.y + 2.f});
    UI_CHECK_EQ(ui.value(list), 0.f);
    drv.key(Key::Down);
    drv.key(Key::Down);
    UI_CHECK_EQ(ui.value(list), 2.f);
    drv.key(Key::End);
    UI_CHECK_EQ(ui.value(list), 9999.f);
    UI_CHECK(w::listFirstVisible(ui, list) > 9980u); // scrolled to keep the selection visible
    drv.key(Key::Home);
    UI_CHECK_EQ(w::listFirstVisible(ui, list), 0u);
    ui.wheel(ctr(ui.rect(list)), {0.f, -2.f});
    ui.update();
    UI_CHECK_EQ(w::listFirstVisible(ui, list), 6u);
    UI_CHECK(changed >= 4);
    UI_CHECK(w::listItem(ui, list, 1234) == "Item 1234");
    {
        u32 glyphRuns = 0;
        for (const auto& c : ui.drawList().cmds()) glyphRuns += c.node == list.index && c.op == DrawOp::Glyph;
        UI_CHECK(glyphRuns > 0 && glyphRuns < 200); // only visible rows (~9 rows x <10 glyphs), not 10000
    }

    // --- tree view -----------------------------------------------------------------------------------------------
    UI_CHECK_EQ(w::treeVisibleRows(ui, tree), 8u);
    ui.setFocus(tree, FocusReason::Keyboard);
    drv.key(Key::Down); // Scene
    UI_CHECK_EQ(ui.value(tree), 0.f);
    drv.key(Key::Left); // collapse Scene
    UI_CHECK(!w::treeExpanded(ui, tree, 0));
    UI_CHECK_EQ(w::treeVisibleRows(ui, tree), 2u);
    drv.key(Key::Down); // Assets
    UI_CHECK_EQ(ui.value(tree), 7.f);
    drv.key(Key::Up);
    drv.key(Key::Right); // expand
    UI_CHECK_EQ(w::treeVisibleRows(ui, tree), 8u);
    drv.key(Key::Down); drv.key(Key::Down); // Camera, Lights
    w::setTreeExpanded(ui, tree, 2, false);
    UI_CHECK_EQ(w::treeVisibleRows(ui, tree), 6u);
    drv.key(Key::Down); // Meshes (Sun / Fill hidden)
    UI_CHECK_EQ(ui.value(tree), 5.f);
    drv.key(Key::Left); // collapse Meshes
    drv.key(Key::Left); // to parent Scene
    UI_CHECK_EQ(ui.value(tree), 0.f);
    w::setTreeExpanded(ui, tree, 2, true);
    w::setTreeExpanded(ui, tree, 5, true);
    ui.update();
    {   // click the arrow of "Scene" collapses, clicking the label selects
        const Rect tc = ui.contentRect(tree);
        drv.clickAt({tc.x + 8.f, tc.y + 5.f});
        UI_CHECK(!w::treeExpanded(ui, tree, 0));
        drv.clickAt({tc.x + 8.f, tc.y + 5.f});
        UI_CHECK(w::treeExpanded(ui, tree, 0));
    }

    // --- tab widget ----------------------------------------------------------------------------------------------
    const NodeHandle tb = w::tabWidgetBar(ui, tabs);
    UI_CHECK(ui.computed(w::tabPage(ui, tabs, 0)).visible && !ui.computed(w::tabPage(ui, tabs, 1)).visible);
    ui.setFocus(tb, FocusReason::Keyboard);
    drv.key(Key::Right);
    UI_CHECK_EQ(ui.value(tb), 1.f);
    UI_CHECK(!ui.computed(w::tabPage(ui, tabs, 0)).visible && ui.computed(w::tabPage(ui, tabs, 1)).visible);
    UI_CHECK(ui.rect(ui.find("page1")).w > 0.f);
    {
        const Rect r = ui.contentRect(tb);
        drv.clickAt({r.x + 4.f, r.y + r.h * 0.5f});
        UI_CHECK_EQ(ui.value(tb), 0.f);
    }

    // --- scroll area ---------------------------------------------------------------------------------------------
    UI_CHECK(ui.scrollExtent(sa).h > ui.contentRect(sa).h);
    ui.wheel(ctr(ui.rect(sa)), {0.f, -1.f});
    ui.update();
    UI_CHECK(ui.scroll(sa).y > 0.f);

    // --- menu bar + popup menu -----------------------------------------------------------------------------------
    const NodeHandle file = ui.find("file");
    int submitted = -1;
    std::string picked;
    ui.connect(file, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { submitted = int(a.value); picked = a.text; });
    drv.click("file");
    NodeHandle scope = ui.firstChild(layer);
    UI_CHECK(w::popupOpen(ui, scope));
    UI_CHECK(ui.text(ui.focused()) == "New");
    UI_CHECK(ui.rect(ui.firstChild(scope)).y >= ui.rect(file).bottom()); // drops below the title
    drv.key(Key::Up); // wraps to Quit (separator skipped)
    UI_CHECK(ui.text(ui.focused()) == "Quit");
    drv.key(Key::Up);
    UI_CHECK(ui.text(ui.focused()) == "Open");
    // focus is trapped: Tab stays inside the popup
    ui.focusNext(false);
    UI_CHECK(scopeContains(ui, scope, ui.focused()));
    drv.key(Key::Escape);
    UI_CHECK(ui.focused() == file);
    ui.update();
    UI_CHECK(!ui.alive(scope));
    // reopen, click outside: closes without picking
    drv.click("file");
    scope = ui.firstChild(layer);
    drv.clickAt({600.f, 400.f});
    UI_CHECK(!w::popupOpen(ui, scope));
    UI_CHECK_EQ(submitted, -1);
    // reopen, click an item
    drv.click("file");
    scope = ui.firstChild(layer);
    ui.update();
    const NodeHandle open = ui.nextSibling(ui.firstChild(ui.firstChild(scope)));
    UI_CHECK(ui.text(open) == "Open");
    const Rect orc = ui.rect(open);
    drv.clickAt({orc.x + orc.w * 0.5f, orc.y + orc.h * 0.5f});
    UI_CHECK_EQ(submitted, 1);
    UI_CHECK(picked == "Open");
    ui.update();
    UI_CHECK(!ui.alive(ui.firstChild(layer)) || ui.classOf(ui.firstChild(layer)) != ui.registry().find("popup") ||
             !w::popupOpen(ui, ui.firstChild(layer)));

    // --- modal dialog --------------------------------------------------------------------------------------------
    const NodeHandle dlg = w::dialog(ui, "Delete asset?", "confirm");
    const NodeHandle ok = w::button(ui, w::dialogBody(ui, dlg), "Delete", "dlg.ok");
    const NodeHandle cancel = w::button(ui, w::dialogBody(ui, dlg), "Cancel", "dlg.cancel");
    ui.update();
    UI_CHECK(!w::popupOpen(ui, ui.parent(dlg)));
    ui.setFocus(list, FocusReason::Keyboard);
    w::openDialog(ui, dlg);
    ui.update();
    UI_CHECK(w::popupOpen(ui, ui.parent(dlg)));
    UI_CHECK(ui.focused() == ok);
    ui.focusNext(false);
    UI_CHECK(ui.focused() == cancel);
    ui.focusNext(false);
    UI_CHECK(ui.focused() == ok); // wraps inside the dialog
    {   // centred
        const Rect d = ui.rect(dlg);
        UI_CHECK(std::abs((d.x + d.w * 0.5f) - 320.f) <= 1.f && std::abs((d.y + d.h * 0.5f) - 210.f) <= 1.f);
    }
    drv.clickAt({5.f, 400.f}); // outside click does not close a modal dialog
    UI_CHECK(w::popupOpen(ui, ui.parent(dlg)));
    UI_CHECK(ui.hitTest({5.f, 400.f}) == ui.parent(dlg)); // the scrim blocks the content below
    std::string dump;
    ui.dumpA11y(dump);
    UI_CHECK(dump.find("Delete asset?") != std::string::npos);
    if (argc > 1) {
        Image img;
        render(ui, img);
        stbi_write_png(argv[1], img.width, img.height, 4, img.pixels.data(), img.width * 4);
    }
    drv.key(Key::Escape);
    ui.update();
    UI_CHECK(!w::popupOpen(ui, ui.parent(dlg)) && ui.alive(dlg)); // hidden, reusable
    UI_CHECK(ui.focused() == list);

    // --- tooltip -------------------------------------------------------------------------------------------------
    const NodeHandle tip = w::setTooltip(ui, ui.find("tipbtn"), "Does nothing", 0.25f);
    ui.update();
    drv.hover("tipbtn");
    UI_CHECK(!ui.computed(tip).visible);
    for (int i = 0; i < 20; ++i) ui.update(1.f / 60.f);
    UI_CHECK(ui.computed(tip).visible);
    UI_CHECK(ui.rect(tip).y >= ui.rect(ui.find("tipbtn")).bottom());
    drv.clickAt({630.f, 410.f}); // pointer leaves
    UI_CHECK(!ui.computed(tip).visible);

    // --- combo box popup on a fresh context keeps Tier A behaviour when Tier B is absent ------------------------
    {
        UiContext a({.viewport = {200, 100}, .font = &font()});
        w::registerTierA(a);
        const NodeHandle cb = w::comboBox(a, a.root(), "A\nB", 0, "cb");
        a.update();
        UiTestDriver d(a);
        d.click("cb");
        UI_CHECK(w::comboText(a, cb) == "B");
    }
    return finish("fuse_ui_tier_b");
}
