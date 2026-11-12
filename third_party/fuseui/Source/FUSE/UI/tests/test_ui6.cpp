// UI6 gates (plan §5, §9), one binary per gate (FUSE_UI6_GATE):
//   dock_layout_roundtrip  dock save -> restore into a fresh area reproduces sides, order, open state and sizes
//   modelview_1m_rows      1,000,000-row table scrolled continuously: frame CPU <= 4 ms, flat memory, only visible rows read
//   tier_c                 sort / filter proxy, cell delegates, virtualised grid view
//   tier_d                 property grid, colour picker, calendar, chart, markdown view
// Optional --out=<dir>: PNG of the screen (tier_c.png / tier_d.png / dock_layout.png).
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#ifndef FUSE_UI6_GATE
#define FUSE_UI6_GATE "tier_c"
#endif

namespace {
std::atomic<unsigned long long> g_allocs{0}, g_bytes{0};
}
void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
std::string g_out;
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void click(UiContext& ui, Vec2 p) {
    ui.pointerMove(p);
    ui.pointerDown(p);
    ui.pointerUp(p);
    ui.update(1.f / 60.f);
}
void drag(UiContext& ui, Vec2 a, Vec2 b) {
    ui.pointerMove(a);
    ui.pointerDown(a);
    ui.update(1.f / 60.f);
    ui.pointerMove(b);
    ui.update(1.f / 60.f);
    ui.pointerUp(b);
    ui.update(1.f / 60.f);
}
void key(UiContext& ui, Key k) {
    ui.keyDown(k);
    ui.keyUp(k);
    ui.update(1.f / 60.f);
}
void shot(UiContext& ui, const char* name) {
    if (g_out.empty()) return;
    ui.update();
    Image img;
    render(ui, img);
    const std::string path = g_out + "/" + name;
    stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    std::printf("wrote %s\n", path.c_str());
}
bool near(f32 a, f32 b, f32 eps = 1.f) { return std::abs(a - b) <= eps; }

// ---- dock layout roundtrip ------------------------------------------------------------------------------------------
struct DockApp {
    NodeHandle area;
    NodeHandle docks[6];
};
const char* kDockIds[] = {"outline", "assets", "log", "console", "props", "history"};
DockApp buildDocks(UiContext& ui, NodeHandle parent, std::string_view areaId) {
    DockApp a;
    a.area = w::dockArea(ui, parent, areaId);
    ui.setCss(a.area, "flex-grow", "1");
    const w::DockSide sides[] = {w::DockSide::Left, w::DockSide::Left, w::DockSide::Bottom, w::DockSide::Bottom, w::DockSide::Right, w::DockSide::Right};
    for (int i = 0; i < 6; ++i) {
        std::string id = std::string(areaId) + "." + kDockIds[i];
        a.docks[i] = w::dockWidget(ui, a.area, kDockIds[i], sides[i], id);
        w::label(ui, w::dockBody(ui, a.docks[i]), kDockIds[i]);
    }
    w::label(ui, w::dockRegion(ui, a.area, w::DockSide::Center), "viewport");
    return a;
}
std::string withoutPrefix(std::string s, std::string_view prefix) { // ids differ only by the area prefix
    for (std::size_t p; (p = s.find(prefix)) != std::string::npos;) s.erase(p, prefix.size());
    return s;
}
int dockLayoutRoundtrip() {
    UiContext ui({.viewport = {1400, 560}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    ui.setCss(ui.root(), "flex-direction", "row");
    const NodeHandle hostA = w::panel(ui, ui.root());
    ui.setCss(hostA, "flex-grow", "1");
    ui.setCss(hostA, "padding", "0px");
    DockApp a = buildDocks(ui, hostA, "A");
    ui.update();
    const std::string fresh = w::dockSaveLayout(ui, a.area);

    // rearrange: move, reorder, close, resize via the splitter handles (keyboard)
    w::dockTo(ui, a.docks[0], w::DockSide::Right);  // outline -> right (after props, history)
    w::dockTo(ui, a.docks[5], w::DockSide::Left);   // history -> left (after assets)
    w::dockClose(ui, a.docks[3]);                   // console closed
    w::dockTo(ui, a.docks[4], w::DockSide::Bottom); // props -> bottom
    ui.update();
    const NodeHandle row = ui.firstChild(a.area);
    const NodeHandle leftHandle = ui.nextSibling(ui.firstChild(row));
    ui.setFocus(leftHandle);
    for (int i = 0; i < 3; ++i) key(ui, Key::Right); // left region +48 px
    const NodeHandle bottomHandle = ui.nextSibling(row);
    ui.setFocus(bottomHandle);
    for (int i = 0; i < 2; ++i) key(ui, Key::Up); // bottom region +32 px
    ui.update();
    const std::string saved = w::dockSaveLayout(ui, a.area);
    std::printf("saved layout:\n%s", saved.c_str());
    UI_CHECK(saved != fresh);
    UI_CHECK(saved.find("size left 248") != std::string::npos);
    UI_CHECK(saved.find("size bottom 192") != std::string::npos);
    UI_CHECK(saved.find("dock A.console bottom 1 closed") != std::string::npos);

    // restore into a second, freshly built area (default arrangement): identical blob and identical rects
    const NodeHandle hostB = w::panel(ui, ui.root());
    ui.setCss(hostB, "flex-grow", "1");
    ui.setCss(hostB, "padding", "0px");
    DockApp b = buildDocks(ui, hostB, "B");
    ui.update();
    UI_CHECK(withoutPrefix(w::dockSaveLayout(ui, b.area), "B.") == withoutPrefix(fresh, "A."));
    std::string blob = saved;
    for (std::size_t p; (p = blob.find("A.")) != std::string::npos;) blob.replace(p, 2, "B.");
    UI_CHECK(w::dockRestoreLayout(ui, b.area, blob));
    ui.update();
    const std::string again = w::dockSaveLayout(ui, b.area);
    UI_CHECK_EQ(again, blob);
    const Rect ra = ui.rect(a.area), rb = ui.rect(b.area);
    for (int i = 0; i < 6; ++i) {
        UI_CHECK_EQ(ui.computed(a.docks[i]).visible, ui.computed(b.docks[i]).visible);
        if (!ui.computed(a.docks[i]).visible) continue;
        const Rect x = ui.rect(a.docks[i]), y = ui.rect(b.docks[i]);
        UI_CHECK(near(x.x - ra.x, y.x - rb.x) && near(x.y - ra.y, y.y - rb.y) && near(x.w, y.w) && near(x.h, y.h));
    }
    for (w::DockSide s : {w::DockSide::Left, w::DockSide::Right, w::DockSide::Bottom, w::DockSide::Center}) {
        const Rect x = ui.rect(w::dockRegion(ui, a.area, s)), y = ui.rect(w::dockRegion(ui, b.area, s));
        UI_CHECK(near(x.w, y.w) && near(x.h, y.h));
    }
    // restoring the fresh layout returns to the defaults (re-opens the closed dock)
    std::string freshB = fresh;
    for (std::size_t p; (p = freshB.find("A.")) != std::string::npos;) freshB.replace(p, 2, "B.");
    UI_CHECK(w::dockRestoreLayout(ui, b.area, freshB));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), freshB);
    UI_CHECK(ui.computed(b.docks[3]).visible);
    // malformed blobs change nothing; unknown ids are ignored
    UI_CHECK(w::dockRestoreLayout(ui, b.area, blob));
    ui.update();
    for (const char* bad : {"", "fusedock 2\n", "nonsense\n", "fusedock 1\nsize left -5\n", "fusedock 1\nsize middle 10\n",
                            "fusedock 1\ndock B.log up 0 open\n", "fusedock 1\ndock B.log left x open\n", "fusedock 1\ndock B.log left 0 maybe\n"}) {
        UI_CHECK(!w::dockRestoreLayout(ui, b.area, bad));
        ui.update();
        UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), blob);
    }
    UI_CHECK(w::dockRestoreLayout(ui, b.area, "fusedock 1\ndock B.nothere left 0 open\n"));
    ui.update();
    UI_CHECK_EQ(w::dockSaveLayout(ui, b.area), blob);
    shot(ui, "dock_layout.png");
    return finish("fuse_ui_dock_layout_roundtrip");
}

// ---- 1M-row model / view ----------------------------------------------------------------------------------------------
struct BigModel {
    mutable u64 reads = 0;
    w::ItemModel model;
    BigModel() {
        model.user = this;
        model.columns = 4;
        model.rows = [](const void*) { return 1000000u; };
        model.data = [](const void* u, u32 r, u32 c, std::string& scratch) -> std::string_view {
            ++static_cast<const BigModel*>(u)->reads;
            char b[32];
            char* e = b;
            switch (c) {
            case 0: std::memcpy(b, "entity_", 7); e = std::to_chars(b + 7, b + sizeof b, r).ptr; break;
            case 1: { static constexpr std::string_view k[] = {"mesh", "light", "camera", "probe", "decal"}; return k[(r * 2654435761u >> 7) % 5]; }
            case 2: e = std::to_chars(b, b + sizeof b, (r * 7919u) % 100000u).ptr; break;
            default: e = std::to_chars(b, b + sizeof b, (r % 997u) * 0.25f).ptr; break;
            }
            scratch.assign(b, e);
            return scratch;
        };
        model.header = [](const void*, u32 c) -> std::string_view {
            static constexpr std::string_view h[] = {"Name", "Kind", "Triangles", "Cost (ms)"};
            return h[c];
        };
    }
};
int modelView1M() {
    UiContext ui({.viewport = {960, 600}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "padding", "0px");
    BigModel big;
    const NodeHandle t = w::tableView(ui, ui.root(), &big.model, "big");
    ui.setCss(t, "flex-grow", "1");
    w::setTableColumnWidth(ui, t, 0, 240.f);
    ui.update();
    ui.setFocus(t);
    const u32 nodes = ui.nodeCount();
    const Vec2 mid = ctr(ui.rect(t));
    ui.pointerMove(mid);
    using clock = std::chrono::steady_clock;
    // warm-up: every glyph the cells use, scratch buffers, draw-list capacity
    for (int i = 0; i < 120; ++i) { ui.wheel(mid, {0.f, -3.f}); ui.update(1.f / 60.f); }
    const unsigned long long a0 = g_allocs.load(), b0 = g_bytes.load();
    big.reads = 0;
    std::vector<double> ms;
    const int frames = 900;
    ms.reserve(frames);
    u32 prevFirst = w::tableFirstVisible(ui, t);
    bool monotonic = true;
    for (int i = 0; i < frames; ++i) {
        const auto t0 = clock::now();
        if (i == frames / 3) key(ui, Key::End); // jump to the end, then scroll back up continuously
        else ui.wheel(mid, {0.f, i < frames / 3 ? -40.f : 25.f});
        ui.update(1.f / 60.f);
        ms.push_back(std::chrono::duration<double, std::milli>(clock::now() - t0).count());
        const u32 f = w::tableFirstVisible(ui, t);
        if (i < frames / 3 && f < prevFirst) monotonic = false;
        prevFirst = f;
    }
    const unsigned long long allocs = g_allocs.load() - a0, bytes = g_bytes.load() - b0;
    std::vector<double> s = ms;
    std::sort(s.begin(), s.end());
    const double med = s[s.size() / 2], p95 = s[s.size() * 95 / 100], mx = s.back();
    const u32 vis = u32((ui.contentRect(t).h) / 18.f) + 2; // rows are >= 18 px at 14 px text
    std::printf("1M rows: %d frames, frame CPU median %.3f ms, p95 %.3f ms, max %.3f ms; %llu allocs (%llu bytes) in steady "
                "scroll; %.1f cell reads/frame; nodes %u\n", frames, med, p95, mx, allocs, bytes, double(big.reads) / frames, ui.nodeCount());
    UI_CHECK(monotonic);
    UI_CHECK(ui.stats().drawCmds > vis * 2); // the frames really painted the visible cells
    UI_CHECK(med <= 4.0);
    UI_CHECK(p95 <= 8.0);
    UI_CHECK(bytes <= 64 * 1024);          // flat memory: nothing grows with rows scrolled
    UI_CHECK_EQ(ui.nodeCount(), nodes);    // no per-row nodes
    UI_CHECK(double(big.reads) / frames <= double(vis * 4 + 8)); // only visible cells are read
    // End reached the last row; selection at the end and Home back
    key(ui, Key::End);
    UI_CHECK_EQ(i32(ui.value(t)), 999999);
    UI_CHECK(w::tableFirstVisible(ui, t) > 999900u);
    key(ui, Key::Home);
    UI_CHECK_EQ(w::tableFirstVisible(ui, t), 0u);
    // scrollbar drag to the middle lands near row 500k
    {
        const Rect r = ui.contentRect(t);
        drag(ui, {r.right() - 4.f, r.y + 40.f}, {r.right() - 4.f, r.y + 40.f + (r.h - 40.f) * 0.5f});
        const u32 f = w::tableFirstVisible(ui, t);
        UI_CHECK(f > 400000u && f < 600000u);
    }
    // a filter proxy over the 1M rows (one pass), then the view over it
    {
        w::SortFilterProxy proxy(&big.model);
        const auto t0 = clock::now();
        proxy.setFilter(1, "probe");
        const double filt = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        std::printf("proxy filter over 1M rows: %u matches in %.1f ms\n", proxy.rows(), filt);
        UI_CHECK(proxy.rows() > 150000u && proxy.rows() < 250000u);
        std::string sc;
        bool ok = true;
        for (u32 r = 0; r < proxy.rows(); r += 997) ok &= proxy.model().data(proxy.model().user, r, 1, sc) == "probe";
        UI_CHECK(ok);
        const NodeHandle t2 = w::tableView(ui, ui.root(), &proxy.model(), "filtered");
        ui.update();
        key(ui, Key::Tab);
        ui.setFocus(t2);
        key(ui, Key::End);
        UI_CHECK_EQ(u32(ui.value(t2)), proxy.rows() - 1);
        ui.destroy(t2);
        ui.update();
    }
    shot(ui, "modelview_1m.png");
    return finish("fuse_ui_modelview_1m_rows");
}

// ---- Tier C ----------------------------------------------------------------------------------------------------------------
struct BarDelegate { // paints the cost column as a bar plus the number
    static void paint(const void*, const PaintContext& pc, DrawList& dl, Rect cell, u32, u32, std::string_view text, bool selected) {
        double v = 0.0;
        std::from_chars(text.data(), text.data() + text.size(), v);
        const f32 f = std::clamp(f32(v) / 250.f, 0.f, 1.f);
        dl.rect({cell.x, cell.y + 5.f, cell.w * f, cell.h - 10.f}, pc.tint(selected ? Color::rgb(0xFFFFFF, 0.6f) : Color::rgb(0x3D8BFD, 0.7f)), 2.f);
        pc.ui.paintString(dl, text, cell, pc.tint(pc.style.color), pc.style.fontSize * 0.85f, TextAlignment::End, pc.node.index);
        ++*const_cast<int*>(&calls);
    }
    static inline int calls = 0;
};
int tierC() {
    UiContext ui({.viewport = {900, 520}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "gap", "10px");
    w::StringTableModel m("Name\tKind\tCost");
    const char* kinds[] = {"mesh", "texture", "audio", "script"};
    for (int i = 0; i < 400; ++i)
        m.addRow("asset_" + std::to_string(i) + "\t" + kinds[i % 4] + "\t" + std::to_string((i * 37) % 251));
    // proxy: identity costs no map; filter + sort
    w::SortFilterProxy proxy(&m.model());
    UI_CHECK_EQ(proxy.rows(), 400u);
    UI_CHECK_EQ(proxy.sourceRow(17), 17u);
    proxy.setFilter(1, "TEXT");
    UI_CHECK_EQ(proxy.rows(), 100u);
    std::string sc;
    UI_CHECK(proxy.model().data(proxy.model().user, 0, 0, sc) == "asset_1");
    proxy.sortBy(2, false);
    double prev = 1e9;
    bool sorted = true;
    for (u32 r = 0; r < proxy.rows(); ++r) {
        double v;
        const std::string_view d = proxy.model().data(proxy.model().user, r, 2, sc);
        std::from_chars(d.data(), d.data() + d.size(), v);
        sorted &= v <= prev;
        prev = v;
    }
    UI_CHECK(sorted);
    UI_CHECK(proxy.model().header && proxy.model().header(proxy.model().user, 2) == "Cost");
    proxy.sortBy(0, true);
    UI_CHECK(proxy.model().data(proxy.model().user, 0, 0, sc) == "asset_1"); // string order: asset_1 < asset_101 < asset_105 ...
    proxy.setFilter(0, "");
    proxy.sortBy(-1, true);
    UI_CHECK_EQ(proxy.rows(), 400u);
    UI_CHECK_EQ(proxy.sourceRow(399), 399u);
    proxy.setFilter(1, "mesh");
    proxy.sortBy(2, false);

    const NodeHandle left = w::panel(ui, ui.root());
    ui.setCss(left, "width", "430px");
    w::label(ui, left, "Proxy (kind = mesh, header sorts) + bar delegate");
    const NodeHandle table = w::tableView(ui, left, &proxy.model(), "table");
    ui.setCss(table, "flex-grow", "1");
    w::setTableColumnWidth(ui, table, 2, 150.f);
    static const w::CellDelegate bar{nullptr, &BarDelegate::paint};
    w::setTableDelegate(ui, table, 2, &bar);
    // re-sorting through the header toggles the proxy
    ui.connect(table, SignalKind::Toggled, [&](UiContext& u, NodeHandle n, const SignalArgs& a) {
        proxy.sortBy(i32(a.value), a.flag);
        w::tableModelChanged(u, n);
    });
    const NodeHandle right = w::panel(ui, ui.root());
    ui.setCss(right, "flex-grow", "1");
    w::label(ui, right, "Grid view (400 items, virtualised)");
    const NodeHandle grid = w::gridView(ui, right, &m.model(), 100.f, 80.f, "grid");
    ui.setCss(grid, "flex-grow", "1");
    ui.update();
    UI_CHECK(BarDelegate::calls > 0);
    UI_CHECK(ui.computed(table).visible);
    // header click sorts through the proxy (ascending on first click)
    {
        const Rect c = ui.contentRect(table);
        click(ui, {c.x + 240.f + 60.f, c.y + 8.f}); // third column (name 120 + kind 120)
        double first = 0, second = 0;
        const std::string_view d0 = proxy.model().data(proxy.model().user, 0, 2, sc);
        std::from_chars(d0.data(), d0.data() + d0.size(), first);
        const std::string_view d1 = proxy.model().data(proxy.model().user, 1, 2, sc);
        std::from_chars(d1.data(), d1.data() + d1.size(), second);
        UI_CHECK(first <= second);
    }
    // grid: columns from the width, 2D keyboard movement, click selection, scroll to keep selection visible
    const u32 nc = w::gridColumns(ui, grid);
    std::printf("grid columns: %u\n", nc);
    UI_CHECK(nc >= 3 && nc <= 5);
    ui.setFocus(grid);
    key(ui, Key::Right);
    UI_CHECK_EQ(i32(ui.value(grid)), 0);
    key(ui, Key::Right);
    key(ui, Key::Down);
    UI_CHECK_EQ(u32(ui.value(grid)), 1u + nc);
    key(ui, Key::End);
    UI_CHECK_EQ(i32(ui.value(grid)), 399);
    UI_CHECK(w::gridFirstVisibleRow(ui, grid) > 0u);
    key(ui, Key::Home);
    UI_CHECK_EQ(w::gridFirstVisibleRow(ui, grid), 0u);
    const Rect gc = ui.contentRect(grid);
    click(ui, {gc.x + 100.f * 2.f + 50.f, gc.y + 80.f + 40.f});
    UI_CHECK_EQ(u32(ui.value(grid)), nc + 2u);
    int submitted = -1;
    ui.connect(grid, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { submitted = int(a.value); });
    key(ui, Key::Enter);
    UI_CHECK_EQ(submitted, int(nc + 2u));
    ui.wheel(ctr(gc), {0.f, -2.f});
    ui.update();
    UI_CHECK_EQ(w::gridFirstVisibleRow(ui, grid), 2u);
    // table row select on the filtered view
    ui.setFocus(table);
    key(ui, Key::Down);
    key(ui, Key::Down);
    UI_CHECK_EQ(i32(ui.value(table)), 1);
    shot(ui, "tier_c.png");
    return finish("fuse_ui_tier_c");
}

// ---- Tier D ----------------------------------------------------------------------------------------------------------------
int tierD() {
    UiContext ui({.viewport = {1000, 600}, .font = &font()});
    w::install(ui);
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "gap", "10px");
    const NodeHandle col1 = w::panel(ui, ui.root());
    ui.setCss(col1, "width", "300px");
    const NodeHandle col2 = w::panel(ui, ui.root());
    ui.setCss(col2, "width", "260px");
    const NodeHandle col3 = w::panel(ui, ui.root());
    ui.setCss(col3, "flex-grow", "1");

    // property grid
    const NodeHandle pg = w::propertyGrid(ui, col1, "props");
    w::propertyRow(ui, pg, {.name = "Transform", .kind = w::PropKind::Group});
    w::propertyRow(ui, pg, {.name = "Name", .kind = w::PropKind::Text}, "Lantern");
    const NodeHandle scale = w::propertyRow(ui, pg, {.name = "Scale", .kind = w::PropKind::Number, .lo = 0.f, .hi = 10.f, .step = 0.5f}, "1.5");
    w::propertyRow(ui, pg, {.name = "Light", .kind = w::PropKind::Group});
    const NodeHandle shadows = w::propertyRow(ui, pg, {.name = "Shadows", .kind = w::PropKind::Bool}, "true");
    w::propertyRow(ui, pg, {.name = "Mode", .kind = w::PropKind::Enum, .options = "Point\nSpot\nArea"}, "1");
    w::propertyRow(ui, pg, {.name = "Tint", .kind = w::PropKind::Color}, "#FFB347FF");
    UI_CHECK_EQ(w::propertyCount(ui, pg), 7u);
    UI_CHECK(w::propertyValue(ui, pg, 1) == "Lantern");
    UI_CHECK(w::propertyValue(ui, pg, 2) == "1.5");
    UI_CHECK(w::propertyValue(ui, pg, 4) == "true");
    UI_CHECK(w::propertyValue(ui, pg, 5) == "1");
    UI_CHECK(w::propertyValue(ui, pg, 6) == "#FFB347FF");
    std::vector<std::string> changes;
    ui.connect(pg, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) {
        changes.push_back(std::to_string(int(a.value)) + ":" + std::string(a.text));
    });
    ui.update();
    click(ui, ctr(ui.rect(shadows)));
    UI_CHECK(w::propertyValue(ui, pg, 4) == "false");
    ui.setFocus(scale);
    key(ui, Key::Up);
    UI_CHECK(w::propertyValue(ui, pg, 2) == "2");
    UI_CHECK(changes.size() == 2 && changes[0] == "4:Shadows" && changes[1] == "2:Scale");
    // folding a group hides its rows only
    click(ui, ctr(ui.rect(ui.firstChild(pg))));
    UI_CHECK(!ui.computed(ui.nextSibling(ui.firstChild(pg))).visible);
    UI_CHECK(ui.computed(shadows).visible);
    w::setPropertyGroupOpen(ui, pg, 0, true);
    ui.update();
    UI_CHECK(ui.computed(ui.nextSibling(ui.firstChild(pg))).visible);

    // markdown view under the grid
    const NodeHandle md = w::markdownView(ui, col1,
        "# Lantern\nA **portable** light with `soft` shadows.\nSecond line of the paragraph.\n\n## Notes\n- flickers at night\n- 3 charges\n"
        "1. place\n2. light\n> Tip: hold to aim\n```\nlantern.intensity = 4\n```\n", "md");
    u32 nmd = 0;
    std::string classes;
    for (NodeHandle c = ui.firstChild(md); ui.alive(c); c = ui.nextSibling(c), ++nmd)
        for (const char* k : {"md-h1", "md-h2", "md-p", "md-li", "md-quote", "md-code"}) if (ui.hasClass(c, k)) { classes += k; classes += ' '; }
    UI_CHECK_EQ(nmd, 9u);
    UI_CHECK_EQ(classes, std::string("md-h1 md-p md-h2 md-li md-li md-li md-li md-quote md-code "));
    UI_CHECK(ui.text(ui.nextSibling(ui.firstChild(md))) == "A portable light with soft shadows. Second line of the paragraph.");
    w::setMarkdown(ui, md, "### Small\nonly");
    nmd = 0;
    for (NodeHandle c = ui.firstChild(md); ui.alive(c); c = ui.nextSibling(c)) ++nmd;
    UI_CHECK_EQ(nmd, 2u);
    w::setMarkdown(ui, md, "# Lantern\nPortable light with **soft** shadows.\n- flickers at night\n- 3 charges\n```\nlantern.intensity = 4\n```");

    // colour picker
    const NodeHandle cp = w::colorPicker(ui, col2, Color::rgb(0xFF0000), "picker");
    UI_CHECK(ui.text(cp) == "#FF0000FF");
    std::string lastHex;
    ui.connect(cp, SignalKind::TextChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { lastHex = a.text; });
    ui.update();
    const Rect pr = ui.contentRect(cp);
    const Rect sq{pr.x, pr.y, pr.w - 26.f, pr.h - 22.f};
    click(ui, {sq.x + 1.f, sq.y + 1.f}); // top-left = white
    UI_CHECK(lastHex.size() == 9 && lastHex[0] == '#' && lastHex.ends_with("FF")); // near white, opaque
    { const Color c = w::colorPickerColor(ui, cp); UI_CHECK(c.r > 0.97f && c.g > 0.97f && c.b > 0.97f); }
    click(ui, {sq.x + sq.w - 1.f, sq.bottom() - 1.f}); // bottom-right = black
    { const Color c = w::colorPickerColor(ui, cp); UI_CHECK(c.r < 0.03f && c.g < 0.03f && c.b < 0.03f); }
    // hue bar at 1/3 down = green; square top-right = full saturation
    click(ui, {pr.right() - 8.f, pr.y + sq.h / 3.f});
    click(ui, {sq.right() - 1.f, sq.y + 1.f});
    { const Color c = w::colorPickerColor(ui, cp); UI_CHECK(c.g > 0.95f && c.r < 0.08f && c.b < 0.08f); }
    // alpha bar
    click(ui, {pr.x + (pr.w - 26.f) * 0.5f, pr.bottom() - 7.f});
    { const Color c = w::colorPickerColor(ui, cp); UI_CHECK(std::abs(c.a - 0.5f) < 0.03f); }
    w::setColorPickerColor(ui, cp, Color::rgb(0x3D8BFD));
    { const Color c = w::colorPickerColor(ui, cp); UI_CHECK(std::abs(c.r - 0x3D / 255.f) < 0.01f && std::abs(c.b - 0xFD / 255.f) < 0.01f); }
    UI_CHECK(ui.text(cp) == "#3D8BFDFF");
    ui.setFocus(cp);
    key(ui, Key::Down);
    UI_CHECK(w::colorPickerColor(ui, cp).b < 0xFD / 255.f - 0.02f);

    // calendar: civil date maths, keys, clicks, month paging
    UI_CHECK_EQ(w::daysFromCivil(1970, 1, 1), 0);
    UI_CHECK_EQ(w::daysFromCivil(2000, 3, 1), 11017);
    UI_CHECK_EQ(w::daysFromCivil(2026, 10, 29), 20755);
    { int y, m, d; w::civilFromDays(20755, y, m, d); UI_CHECK(y == 2026 && m == 10 && d == 29); }
    { int y, m, d; w::civilFromDays(w::daysFromCivil(2024, 2, 29) + 1, y, m, d); UI_CHECK(y == 2024 && m == 3 && d == 1); }
    const NodeHandle cal = w::calendar(ui, col2, 2026, 10, 29, "cal");
    ui.update();
    int y, mo, d;
    ui.setFocus(cal);
    key(ui, Key::Right);
    key(ui, Key::Right);
    key(ui, Key::Right);
    w::calendarDate(ui, cal, y, mo, d);
    UI_CHECK(y == 2026 && mo == 11 && d == 1);
    key(ui, Key::PageDown);
    key(ui, Key::PageDown);
    w::calendarDate(ui, cal, y, mo, d);
    UI_CHECK(y == 2027 && mo == 1 && d == 1);
    key(ui, Key::Up);
    w::calendarDate(ui, cal, y, mo, d);
    UI_CHECK(y == 2026 && mo == 12 && d == 25);
    // the grid starts on Monday: Dec 2026 begins on a Tuesday, so the first cell is Mon Nov 30; click cell (row 2, Wed)
    {
        const Rect c = ui.contentRect(cal);
        const f32 hh = std::ceil(font().metrics(ui.computed(cal).fontSize).lineHeight()) + 8.f, cw = c.w / 7.f, ch = (c.h - hh) / 7.f;
        click(ui, {c.x + cw * 2.5f, c.y + hh + ch + ch * 2.5f});
        w::calendarDate(ui, cal, y, mo, d);
        UI_CHECK(y == 2026 && mo == 12 && d == 16);
        click(ui, {c.x + hh * 0.5f, c.y + hh * 0.5f}); // previous month (display only)
        w::calendarDate(ui, cal, y, mo, d);
        UI_CHECK(d == 16 && mo == 12);
        key(ui, Key::Left);
        w::calendarDate(ui, cal, y, mo, d);
        UI_CHECK(mo == 12 && d == 15);
    }
    { // back to the report date for the screenshot
        w::setMarkdown(ui, md, "# Lantern\nPortable light with **soft** shadows.\n- flickers at night\n- 3 charges\n```\nlantern.intensity = 4\n```");
        ui.setValue(cal, f32(w::daysFromCivil(2026, 10, 29)));
        key(ui, Key::Right);
        key(ui, Key::Left);
    }

    // charts
    const NodeHandle lc = w::chart(ui, col3, w::ChartKind::Line, "frametime");
    ui.setCss(lc, "height", "170px");
    std::vector<f32> cpu, gpu;
    for (int i = 0; i < 60; ++i) {
        cpu.push_back(6.f + 2.f * std::sin(f32(i) * 0.3f) + (i == 40 ? 6.f : 0.f));
        gpu.push_back(9.f + 1.5f * std::cos(f32(i) * 0.21f));
    }
    w::chartSetSeries(ui, lc, 0, "CPU ms", cpu, Color::rgb(0x3D8BFD));
    w::chartSetSeries(ui, lc, 1, "GPU ms", gpu, Color::rgb(0xFFB347));
    const NodeHandle bc = w::chart(ui, col3, w::ChartKind::Bar, "passes");
    ui.setCss(bc, "height", "150px");
    const f32 passA[] = {1.2f, 3.4f, 0.8f, 2.6f, 1.9f}, passB[] = {1.0f, 2.9f, 1.1f, 2.2f, 1.4f};
    w::chartSetSeries(ui, bc, 0, "Before", passA, Color::rgb(0x9A9CAB));
    w::chartSetSeries(ui, bc, 1, "After", passB, Color::rgb(0x4CC38A));
    const NodeHandle ac = w::chart(ui, col3, w::ChartKind::Area, "memory");
    ui.setCss(ac, "height", "140px");
    std::vector<f32> mem;
    for (int i = 0; i < 40; ++i) mem.push_back(200.f + f32(i * i) * 0.4f);
    w::chartSetSeries(ui, ac, 0, "VRAM MB", mem, Color::rgb(0xC77DFF));
    const NodeHandle sc = w::chart(ui, col3, w::ChartKind::Scatter, "scatter");
    ui.setCss(sc, "flex-grow", "1");
    std::vector<f32> pts;
    for (int i = 0; i < 50; ++i) pts.push_back(f32((i * 37) % 23) + f32(i) * 0.3f);
    w::chartSetSeries(ui, sc, 0, "draws", pts, Color::rgb(0xFF6B6B));
    ui.update();
    UI_CHECK_EQ(w::chartSeriesCount(ui, lc), 2u);
    const Rect lr = ui.contentRect(lc);
    const Rect plot{lr.x + 40.f, 0.f, lr.w - 46.f, 0.f};
    ui.pointerMove({plot.x + plot.w * 40.f / 59.f, lr.y + lr.h * 0.6f});
    ui.update();
    UI_CHECK_EQ(i32(ui.value(lc)), 40);
    ui.pointerMove({lr.x + 2.f, lr.y + 2.f});
    ui.update();
    UI_CHECK_EQ(i32(ui.value(lc)), -1);
    ui.pointerMove({plot.x + plot.w * 40.f / 59.f, lr.y + lr.h * 0.6f});
    ui.update();
    // classes registered once; a11y roles
    UI_CHECK(ui.registry().find("calendar") != 0xFFFFFFFFu && ui.registry().find("chart") != 0xFFFFFFFFu);
    UI_CHECK(ui.role(lc) == A11yRole::Image && ui.role(pg) == A11yRole::List);
    shot(ui, "tier_d.png");
    return finish("fuse_ui_tier_d");
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
    const std::string g = FUSE_UI6_GATE;
    if (g == "dock_layout_roundtrip") return dockLayoutRoundtrip();
    if (g == "modelview_1m_rows") return modelView1M();
    if (g == "tier_c") return tierC();
    if (g == "tier_d") return tierD();
    std::fprintf(stderr, "unknown gate %s\n", g.c_str());
    return 2;
}
