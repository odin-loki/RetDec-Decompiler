// Gate fuse_ui_layout_conformance: Box (row / column, grow, shrink, justify, align, gap, padding, margin, min / max),
// Grid (px / % / fr tracks, spans, explicit placement), Stack, Anchor, Absolute, Flow, visibility, scrolling, the
// measure cache and layout boundaries.
#include "test_util.hpp"

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
NodeHandle box(UiContext& ui, NodeHandle p, std::string_view css, std::string_view id = {}) {
    const NodeHandle n = ui.create(ui.registry().find("panel"), p, id);
    std::string_view rest = css;
    while (!rest.empty()) {
        const auto semi = rest.find(';');
        std::string_view d = rest.substr(0, semi);
        rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
        const auto colon = d.find(':');
        if (colon == std::string_view::npos) continue;
        auto trim = [](std::string_view s) {
            while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
            while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
            return s;
        };
        if (!ui.setCss(n, trim(d.substr(0, colon)), trim(d.substr(colon + 1))))
            std::fprintf(stderr, "bad css: %.*s\n", int(d.size()), d.data());
    }
    return n;
}
} // namespace

int main() {
    // ---- Box row: fixed, grow, gap, padding, margin ----
    {
        UiContext ui({.viewport = {400, 300}});
        const NodeHandle row = box(ui, ui.root(), "flex-direction: row; padding: 10px; gap: 5px; height: 100px");
        const NodeHandle a = box(ui, row, "width: 50px");
        const NodeHandle b = box(ui, row, "flex-grow: 1");
        const NodeHandle c = box(ui, row, "flex-grow: 3; margin: 0px 4px");
        ui.update();
        UI_CHECK_RECT(ui.rect(row), 0, 0, 400, 100);
        // content 380 wide: 50 + 5 + b + 5 + 4 + c + 4 = 380 → b + c = 312 → b = 78, c = 234
        UI_CHECK_RECT(ui.rect(a), 10, 10, 50, 80);
        UI_CHECK_RECT(ui.rect(b), 65, 10, 78, 80);
        UI_CHECK_RECT(ui.rect(c), 152, 10, 234, 80);
    }
    // ---- Box column: justify, align, shrink, min/max ----
    {
        UiContext ui({.viewport = {200, 200}});
        const NodeHandle col = box(ui, ui.root(), "height: 200px; justify-content: center; align-items: center");
        const NodeHandle a = box(ui, col, "width: 40px; height: 20px");
        const NodeHandle b = box(ui, col, "width: 60px; height: 30px");
        ui.update();
        UI_CHECK_RECT(ui.rect(a), 80, 75, 40, 20);
        UI_CHECK_RECT(ui.rect(b), 70, 95, 60, 30);
        ui.setCss(col, "justify-content", "space-between");
        ui.setCss(col, "align-items", "end");
        ui.update();
        UI_CHECK_RECT(ui.rect(a), 160, 0, 40, 20);
        UI_CHECK_RECT(ui.rect(b), 140, 170, 60, 30);
        // shrink: two 150px items in 200px shrink proportionally to their basis
        UiContext u2({.viewport = {200, 50}});
        const NodeHandle r = box(u2, u2.root(), "flex-direction: row; height: 50px");
        const NodeHandle x = box(u2, r, "flex-basis: 150px");
        const NodeHandle y = box(u2, r, "flex-basis: 50px; flex-shrink: 0");
        const NodeHandle z = box(u2, r, "flex-basis: 100px; max-width: 30px");
        u2.update();
        UI_CHECK(u2.rect(x).w < 150.f);
        UI_CHECK_EQ(u2.rect(y).w, 50.f);
        UI_CHECK(u2.rect(z).w <= 30.f);
        // min-width wins over grow results that are smaller
        const NodeHandle m = box(u2, r, "min-width: 25px; flex-basis: 0px");
        u2.update();
        UI_CHECK(u2.rect(m).w >= 25.f);
    }
    // ---- Grid ----
    {
        UiContext ui({.viewport = {320, 200}});
        const NodeHandle g = box(ui, ui.root(), "layout: grid; grid-template-columns: 100px 1fr 2fr; gap: 10px");
        NodeHandle cells[5];
        for (int i = 0; i < 5; ++i) cells[i] = box(ui, g, "height: 20px");
        ui.setCss(cells[3], "grid-column-span", "2");
        ui.update();
        // fr: (320 - 100 - 20) / 3 = 66.67 → 1fr ≈ 67, 2fr ≈ 133
        UI_CHECK_RECT(ui.rect(cells[0]), 0, 0, 100, 20);
        UI_CHECK_RECT(ui.rect(cells[1]), 110, 0, 67, 20);
        UI_CHECK_RECT(ui.rect(cells[2]), 187, 0, 133, 20);
        UI_CHECK_RECT(ui.rect(cells[3]), 0, 30, 177, 20);
        UI_CHECK_RECT(ui.rect(cells[4]), 187, 30, 133, 20);
        UI_CHECK_RECT(ui.rect(g), 0, 0, 320, 50);
        // explicit placement
        ui.setCss(cells[4], "grid-column", "2");
        ui.setCss(cells[4], "grid-row", "3");
        ui.update();
        UI_CHECK_RECT(ui.rect(cells[4]), 110, 60, 67, 20);
    }
    // ---- Stack / Anchor / Absolute ----
    {
        UiContext ui({.viewport = {300, 300}});
        const NodeHandle st = box(ui, ui.root(), "layout: stack; height: 100px");
        const NodeHandle full = box(ui, st, "");
        const NodeHandle mid = box(ui, st, "width: 50px; height: 20px");
        const NodeHandle an = box(ui, ui.root(), "layout: anchor; height: 100px");
        const NodeHandle tr = box(ui, an, "right: 10px; top: 5px; width: 30px; height: 30px");
        const NodeHandle stretch = box(ui, an, "left: 10px; right: 10px; bottom: 0px; height: 10px");
        const NodeHandle ctr = box(ui, an, "center-x: true; center-y: true; width: 40px; height: 20px");
        const NodeHandle ab = box(ui, ui.root(), "layout: absolute; flex-grow: 1");
        const NodeHandle p = box(ui, ab, "left: 12px; top: 7px; width: 5px; height: 6px");
        ui.update();
        UI_CHECK_RECT(ui.rect(full), 0, 0, 300, 100);
        UI_CHECK_RECT(ui.rect(mid), 125, 40, 50, 20);
        UI_CHECK_RECT(ui.rect(tr), 260, 105, 30, 30);
        UI_CHECK_RECT(ui.rect(stretch), 10, 190, 280, 10);
        UI_CHECK_RECT(ui.rect(ctr), 130, 140, 40, 20);
        UI_CHECK_RECT(ui.rect(p), 12, 207, 5, 6);
    }
    // ---- Flow wrapping ----
    {
        UiContext ui({.viewport = {100, 100}});
        const NodeHandle f = box(ui, ui.root(), "layout: flow; gap: 4px");
        NodeHandle it[4];
        for (auto& n : it) n = box(ui, f, "width: 40px; height: 10px");
        ui.update();
        UI_CHECK_RECT(ui.rect(it[0]), 0, 0, 40, 10);
        UI_CHECK_RECT(ui.rect(it[1]), 44, 0, 40, 10);
        UI_CHECK_RECT(ui.rect(it[2]), 0, 14, 40, 10);
        UI_CHECK_RECT(ui.rect(f), 0, 0, 100, 24); // two rows: 10 + gap 4 + 10
    }
    // ---- display:none, scrolling, cache and boundaries ----
    {
        UiContext ui({.viewport = {100, 100}});
        const NodeHandle sc = box(ui, ui.root(), "height: 50px; overflow: scroll");
        NodeHandle items[10];
        for (auto& n : items) n = box(ui, sc, "height: 20px; flex-shrink: 0");
        ui.update();
        UI_CHECK_EQ(ui.scrollExtent(sc).h, 200.f);
        ui.scrollTo(sc, {0.f, 500.f});
        ui.update();
        UI_CHECK_EQ(ui.scroll(sc).y, 150.f);
        UI_CHECK_RECT(ui.rect(items[9]), 0, 30, 100, 20);
        UI_CHECK(ui.wheel({10, 10}, {0.f, 1.f})); // one notch up = 40px
        ui.update();
        UI_CHECK_EQ(ui.scroll(sc).y, 110.f);
        ui.setCss(items[0], "display", "none");
        ui.update();
        UI_CHECK_EQ(ui.scrollExtent(sc).h, 180.f);
        UI_CHECK(ui.rect(items[0]).w == 0.f && ui.rect(items[0]).h == 0.f);
        // nothing dirty: no layout work
        ui.update();
        UI_CHECK_EQ(ui.stats().arranged, 0u);
        UI_CHECK(!ui.stats().repainted);

        // a fixed-size panel is a layout boundary: changing inside it re-measures nothing outside
        UiContext u2({.viewport = {400, 400}});
        NodeHandle panels[20], leaves[20];
        for (int i = 0; i < 20; ++i) {
            panels[i] = box(u2, u2.root(), "width: 100px; height: 15px; flex-direction: row");
            leaves[i] = box(u2, panels[i], "width: 10px");
        }
        u2.update();
        u2.setCss(leaves[7], "width", "30px");
        u2.update();
        UI_CHECK_EQ(u2.rect(leaves[7]).w, 30.f);
        UI_CHECK(u2.stats().measured <= 2u);
        UI_CHECK(u2.stats().arranged <= 3u);
        std::string dump;
        u2.dumpLayout(dump);
        UI_CHECK(dump.find("panel 0 105 100 15") != std::string::npos);
    }
    return finish("fuse_ui_layout_conformance");
}
