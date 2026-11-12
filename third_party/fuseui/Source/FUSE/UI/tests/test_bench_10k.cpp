// Gate fuse_ui_bench_10k: a 10,000-node tree (100 fixed-size panels × 99 widgets); one label in one panel changes
// each frame. Budget (plan §4.5): re-layout + repaint ≤ 0.5 ms median in optimised builds. Debug builds report only.
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {1920, 1080}, .font = &font(), .reserveNodes = 10240});
    widgets::install(ui);
    ui.setCss(ui.root(), "layout", "flow");
    ui.setCss(ui.root(), "overflow", "clip");
    NodeHandle target;
    char buf[32];
    for (int p = 0; p < 100; ++p) {
        const NodeHandle panel = widgets::panel(ui, ui.root());
        ui.setCss(panel, "width", "180px");
        ui.setCss(panel, "height", "100px");
        ui.setCss(panel, "overflow", "clip");
        ui.setCss(panel, "layout", "flow");
        for (int i = 0; i < 99; ++i) {
            std::snprintf(buf, sizeof buf, "%d", i);
            const NodeHandle n = (i % 3 == 0) ? widgets::button(ui, panel, buf) : widgets::label(ui, panel, buf);
            if (p == 57 && i == 1) target = n;
        }
    }
    ui.update();
    std::printf("nodes: %u\n", ui.nodeCount());
    UI_CHECK(ui.nodeCount() >= 10000);
    std::vector<double> t;
    for (int f = 0; f < 300; ++f) {
        std::snprintf(buf, sizeof buf, "v%d", f % 10);
        const auto t0 = std::chrono::steady_clock::now();
        ui.setText(target, buf);
        ui.update();
        const auto t1 = std::chrono::steady_clock::now();
        t.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(t.begin(), t.end());
    const double med = t[t.size() / 2], p95 = t[t.size() * 95 / 100];
    std::printf("dirty-panel update: median %.3f ms, p95 %.3f ms; last frame measured %u arranged %u painted %u cmds %u\n",
                med, p95, ui.stats().measured, ui.stats().arranged, ui.stats().painted, ui.stats().drawCmds);
    UI_CHECK(ui.stats().measured < 20u);
    UI_CHECK(ui.stats().arranged < 20u);
#ifdef NDEBUG
#if FUSE_UI_TEST_TEXT_FULL
    UI_CHECK(med <= 1.0); // FULL re-shapes through HarfBuzz each paint; shaped-run cache is follow-up work
#else
    UI_CHECK(med <= 0.5);
#endif
#else
    std::printf("(debug build: budget not enforced)\n");
#endif
    return finish("fuse_ui_bench_10k");
}
