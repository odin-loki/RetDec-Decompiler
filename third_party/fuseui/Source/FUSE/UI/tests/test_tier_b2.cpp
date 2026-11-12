// fuse_ui_tier_b2: rest of Tier B — scroll container scrollbars, cascading submenus, popup screen clamp, splitters,
// dock area, item models and the virtualised table view. Optional argv[1]: write a PNG of the demo screen.
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void drag(UiContext& ui, Vec2 a, Vec2 b) {
    ui.pointerMove(a);
    ui.pointerDown(a);
    ui.update(1.f / 60.f);
    ui.pointerMove(b);
    ui.update(1.f / 60.f);
    ui.pointerUp(b);
    ui.update(1.f / 60.f);
}
NodeHandle itemNamed(const UiContext& ui, NodeHandle scope, std::string_view label) {
    for (NodeHandle it = ui.firstChild(ui.firstChild(scope)); ui.alive(it); it = ui.nextSibling(it))
        if (ui.text(it).substr(0, ui.text(it).find('\n')) == label) return it;
    return {};
}
} // namespace

int main(int argc, char** argv) {
    UiContext ui({.viewport = {720, 460}, .font = &font()});
    w::install(ui);
    const NodeHandle root = ui.root();
    ui.setCss(root, "padding", "0px");
    const NodeHandle area = w::dockArea(ui, root, "dock");

    // --- dock area: empty sides are hidden ---------------------------------------------------------------------------
    ui.update();
    UI_CHECK(!ui.computed(w::dockRegion(ui, area, w::DockSide::Left)).visible);
    UI_CHECK(!ui.computed(w::dockRegion(ui, area, w::DockSide::Bottom)).visible);
    const NodeHandle centre = w::dockRegion(ui, area, w::DockSide::Center);
    UI_CHECK(ui.rect(centre).w > 700.f);

    const NodeHandle outline = w::dockWidget(ui, area, "Outline", w::DockSide::Left, "outline");
    const NodeHandle log = w::dockWidget(ui, area, "Log", w::DockSide::Bottom, "log");
    const NodeHandle props = w::dockWidget(ui, area, "Properties", w::DockSide::Right, "props");
    ui.update();
    UI_CHECK(ui.computed(w::dockRegion(ui, area, w::DockSide::Left)).visible);
    UI_CHECK(std::abs(ui.rect(w::dockRegion(ui, area, w::DockSide::Left)).w - 200.f) < 1.f);
    UI_CHECK(std::abs(ui.rect(w::dockRegion(ui, area, w::DockSide::Bottom)).h - 160.f) < 1.f);
    UI_CHECK(ui.rect(centre).w < 330.f && ui.rect(centre).w > 290.f);

    // scroll container with a draggable scrollbar inside the outline dock
    const NodeHandle sa = w::scrollArea(ui, w::dockBody(ui, outline), "scroll");
    ui.setCss(sa, "flex-grow", "1");
    for (int i = 0; i < 40; ++i) w::label(ui, sa, "node " + std::to_string(i));

    // table over a string model in the centre
    w::StringTableModel model("Name\tKind\tSize");
    for (int i = 0; i < 5000; ++i)
        model.addRow("asset_" + std::to_string(i) + "\t" + (i % 3 ? "mesh" : "texture") + "\t" + std::to_string((i * 7919) % 1000));
    const NodeHandle table = w::tableView(ui, centre, &model.model(), "table");
    ui.setCss(table, "flex-grow", "1");
    w::StringTableModel logModel("Line");
    for (int i = 0; i < 30; ++i) logModel.addRow("log line " + std::to_string(i));
    const NodeHandle logList = w::modelListView(ui, w::dockBody(ui, log), &logModel.model(), "loglist");
    ui.setCss(logList, "flex-grow", "1");
    w::label(ui, w::dockBody(ui, props), "Selection: none", "proplabel");
    ui.update();

    // --- scrollbar drag on an overflow: scroll container ----------------------------------------------------------
    {
        const Rect r = ui.rect(sa);
        UI_CHECK(ui.scrollExtent(sa).h > r.h);
        const Vec2 top{r.right() - 5.f, r.y + 6.f}, bottom{r.right() - 5.f, r.bottom() + 40.f};
        drag(ui, top, bottom);
        UI_CHECK(ui.scroll(sa).y > 0.f);
        UI_CHECK(std::abs(ui.scroll(sa).y - (ui.scrollExtent(sa).h - ui.contentRect(sa).h)) < 1.f);
        UI_CHECK(!ui.scrollDragging());
        drag(ui, {r.right() - 5.f, r.bottom() - 6.f}, {r.right() - 5.f, r.y - 50.f});
        UI_CHECK(ui.scroll(sa).y == 0.f);
        // a click on the track jumps there
        ui.pointerDown({r.right() - 5.f, r.y + r.h * 0.85f});
        ui.pointerUp({r.right() - 5.f, r.y + r.h * 0.85f});
        ui.update();
        UI_CHECK(ui.scroll(sa).y > 0.f);
    }

    // --- splitter: drag the left dock handle -------------------------------------------------------------------
    {
        const NodeHandle left = w::dockRegion(ui, area, w::DockSide::Left);
        const Rect lr = ui.rect(left);
        const Vec2 h{lr.right() + 3.f, lr.y + lr.h * 0.5f};
        drag(ui, h, {h.x + 60.f, h.y});
        UI_CHECK(std::abs(ui.rect(left).w - 260.f) < 2.f);
        drag(ui, {ui.rect(left).right() + 3.f, h.y}, {-100.f, h.y}); // clamps to the minimum
        UI_CHECK(ui.rect(left).w >= 24.f && ui.rect(left).w < 30.f);
        drag(ui, {ui.rect(left).right() + 3.f, h.y}, {lr.right() + 3.f, h.y});
        UI_CHECK(std::abs(ui.rect(left).w - 200.f) < 2.f);
        // a plain splitter elsewhere: no fixed pane yet, first pane becomes fixed
        UiContext s({.viewport = {300, 100}, .font = &font()});
        w::install(s);
        const NodeHandle sp = w::splitter(s, s.root(), false, "sp");
        s.setCss(sp, "flex-grow", "1");
        const NodeHandle a = w::splitterPane(s, sp), b = w::splitterPane(s, sp);
        s.update();
        const f32 w0 = s.rect(a).w;
        UI_CHECK(std::abs(s.rect(a).w - s.rect(b).w) < 2.f);
        f32 got = 0.f;
        s.connect(sp, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs& g) { got = g.value; });
        drag(s, {s.rect(a).right() + 3.f, 50.f}, {s.rect(a).right() + 43.f, 50.f});
        UI_CHECK(std::abs(s.rect(a).w - (w0 + 40.f)) < 2.f && got > 0.f);
        s.setFocus(s.nextSibling(a));
        s.keyDown(Key::Left);
        s.update();
        UI_CHECK(std::abs(s.rect(a).w - (w0 + 24.f)) < 2.f);
    }

    // --- table view: virtualised paint, selection, sorting, resize, scroll ------------------------------------------
    {
        UiTestDriver drv(ui);
        const Rect tr = ui.contentRect(table);
        drv.clickAt({tr.x + 30.f, tr.y + 60.f});
        const i32 sel = i32(ui.value(table));
        UI_CHECK(sel >= 1 && sel <= 2);
        drv.key(Key::End);
        UI_CHECK(i32(ui.value(table)) == 4999);
        UI_CHECK(w::tableFirstVisible(ui, table) > 4900);
        drv.key(Key::Home);
        UI_CHECK(ui.value(table) == 0.f && w::tableFirstVisible(ui, table) == 0);
        ui.wheel(ctr(tr), {0.f, -2.f});
        ui.update();
        UI_CHECK(w::tableFirstVisible(ui, table) == 6);
        ui.wheel(ctr(tr), {0.f, 10.f});
        ui.update();
        UI_CHECK(w::tableFirstVisible(ui, table) == 0);
        // only visible rows are painted: glyph runs for ~10 rows, not 5000
        UI_CHECK(ui.stats().drawCmds < 2000);
        // sorting by clicking the "Size" header
        i32 sortCol = -1;
        bool asc = false;
        ui.connect(table, SignalKind::Toggled, [&](UiContext& c, NodeHandle t, const SignalArgs& a) {
            sortCol = i32(a.value);
            asc = a.flag;
            model.sortBy(u32(a.value), a.flag);
            w::tableModelChanged(c, t);
        });
        const f32 w0 = w::tableColumnWidth(ui, table, 0), w1 = w::tableColumnWidth(ui, table, 1);
        drv.clickAt({tr.x + w0 + w1 + 20.f, tr.y + 6.f});
        UI_CHECK(sortCol == 2 && asc);
        UI_CHECK(model.cell(0, 2) == "0" && model.cell(4999, 2) == "999");
        drv.clickAt({tr.x + w0 + w1 + 20.f, tr.y + 6.f});
        UI_CHECK(!asc && model.cell(0, 2) == "999");
        drv.clickAt({tr.x + 20.f, tr.y + 6.f});
        UI_CHECK(sortCol == 0 && model.cell(0, 0) == "asset_0");
        // resize column 0 by dragging its header edge
        drag(ui, {tr.x + w0, tr.y + 6.f}, {tr.x + w0 + 30.f, tr.y + 6.f});
        UI_CHECK(std::abs(w::tableColumnWidth(ui, table, 0) - (w0 + 30.f)) < 1.f);
        UI_CHECK(sortCol == 0); // edge drag does not sort
        // scrollbar drag to the bottom
        drag(ui, {tr.right() - 4.f, tr.y + 32.f}, {tr.right() - 4.f, tr.bottom() + 100.f});
        UI_CHECK(w::tableFirstVisible(ui, table) > 4900);
        drag(ui, {tr.right() - 4.f, tr.bottom() - 6.f}, {tr.right() - 4.f, tr.y - 100.f});
        UI_CHECK(w::tableFirstVisible(ui, table) == 0);
        // model shrink re-clamps
        model.clear();
        for (int i = 0; i < 3; ++i) model.addRow("x\ty\tz");
        w::tableModelChanged(ui, table);
        ui.update();
        UI_CHECK(w::tableFirstVisible(ui, table) == 0 && ui.value(table) < 3.f);
        model.clear();
        for (int i = 0; i < 5000; ++i)
            model.addRow("asset_" + std::to_string(i) + "\t" + (i % 3 ? "mesh" : "texture") + "\t" + std::to_string((i * 7919) % 1000));
        w::tableModelChanged(ui, table);
        // steady state: scrolling a model-backed table does not allocate nodes
        const u32 nodes = ui.nodeCount();
        for (int i = 0; i < 20; ++i) { ui.wheel(ctr(tr), {0.f, -1.f}); ui.update(1.f / 60.f); }
        UI_CHECK(ui.nodeCount() == nodes);
    }

    // --- dock close / re-dock -------------------------------------------------------------------------------------
    {
        bool open = true;
        ui.connect(props, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& a) { open = a.flag; });
        const Rect title = ui.rect(ui.firstChild(props));
        UiTestDriver drv(ui);
        drv.clickAt({title.right() - title.h * 0.5f, title.y + title.h * 0.5f});
        UI_CHECK(!open);
        UI_CHECK(!ui.computed(w::dockRegion(ui, area, w::DockSide::Right)).visible);
        w::dockTo(ui, props, w::DockSide::Left);
        ui.update();
        UI_CHECK(open && ui.parent(props) == w::dockRegion(ui, area, w::DockSide::Left));
        UI_CHECK(ui.rect(props).y > ui.rect(outline).y); // stacked under the outline
    }

    // --- cascading submenus + popup clamp ------------------------------------------------------------------------
    {
        UiTestDriver drv(ui);
        const NodeHandle owner = ui.find("proplabel");
        f32 picked = -1.f;
        std::string pickedText;
        ui.connect(owner, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { picked = a.value; pickedText = a.text; });
        // 0 New, 1 Recent >, 2 a.txt, 3 More >, 4 old.txt, 5 b.txt, 6 Quit
        const NodeHandle scope = w::popupMenu(ui, owner, "New\nRecent\n\ta.txt\n\tMore\n\t\told.txt\n\tb.txt\nQuit", {650.f, 420.f});
        ui.update();
        const Rect m = ui.rect(ui.firstChild(scope));
        UI_CHECK(m.right() <= 720.f && m.bottom() <= 460.f && m.x >= 0.f && m.y >= 0.f); // clamped on screen
        const NodeHandle recent = itemNamed(ui, scope, "Recent");
        UI_CHECK(ui.alive(recent));
        drv.clickAt(ctr(ui.rect(recent)));
        NodeHandle sub = ui.parent(ui.parent(ui.focused()));
        UI_CHECK(sub != scope && w::popupOpen(ui, sub) && w::popupOpen(ui, scope));
        const Rect sm = ui.rect(ui.firstChild(sub));
        UI_CHECK(sm.right() <= 720.f && sm.bottom() <= 460.f);
        UI_CHECK(sm.right() <= m.x + 4.f); // no room on the right: opens to the left of the parent
        // Left closes just the submenu
        drv.key(Key::Left);
        ui.update();
        UI_CHECK(!w::popupOpen(ui, sub) && w::popupOpen(ui, scope) && ui.focused() == recent);
        drv.key(Key::Right);
        ui.update();
        sub = ui.parent(ui.parent(ui.focused()));
        const NodeHandle more = itemNamed(ui, sub, "More");
        drv.clickAt(ctr(ui.rect(more)));
        const NodeHandle sub2 = ui.parent(ui.parent(ui.focused()));
        UI_CHECK(sub2 != sub && w::popupOpen(ui, sub2));
        if (argc > 1) {
            Image img;
            render(ui, img);
            stbi_write_png(argv[1], img.width, img.height, 4, img.pixels.data(), img.width * 4);
        }
        drv.clickAt(ctr(ui.rect(itemNamed(ui, sub2, "old.txt"))));
        ui.update();
        UI_CHECK(picked == 4.f && pickedText == "old.txt");
        UI_CHECK(!w::popupOpen(ui, scope) && !w::popupOpen(ui, sub) && !w::popupOpen(ui, sub2));
        UI_CHECK(ui.focused() == owner || !ui.focused().valid());
        // plain top-level pick keeps its index
        w::popupMenu(ui, owner, "New\nRecent\n\ta.txt\nQuit", {10.f, 10.f});
        ui.update();
        drv.key(Key::End);
        drv.key(Key::Enter);
        ui.update();
        UI_CHECK(picked == 3.f && pickedText == "Quit");
    }
    return finish("fuse_ui_tier_b2");
}
