// Gate fuse_ui_single_thread_parity: the same script applied (a) directly on the owner thread and (b) as commands
// posted from worker threads produces identical layout dumps, draw-list hashes and rasterised images; repeated runs
// are bit-identical (determinism for replays).
#include "test_util.hpp"

#include <thread>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
struct Result {
    std::string layout, a11y;
    u64 drawHash = 0, imageHash = 0;
};
const char* kIds[] = {"exposure", "gain", "fps", "progress", "hscroll"};

Result run(bool threaded) {
    UiContext ui({.viewport = {488, 400}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    if (!threaded) {
        for (u64 i = 0; i < 5; ++i) {
            const NodeHandle n = ui.find(kIds[i]);
            ui.setValue(n, ui.rangeMin(n) + (ui.rangeMax(n) - ui.rangeMin(n)) * f32(i + 1) / 7.f);
        }
    } else {
        std::thread th[5];
        for (u64 i = 0; i < 5; ++i) {
            const NodeHandle n = ui.find(kIds[i]);
            th[i] = std::thread([&ui, n, i] {
                ui.post([](UiContext& u, NodeHandle h, u64 a, u64) {
                    u.setValue(h, u.rangeMin(h) + (u.rangeMax(h) - u.rangeMin(h)) * f32(a + 1) / 7.f);
                }, n, i);
            });
        }
        for (auto& t : th) t.join();
    }
    UiTestDriver drv(ui);
    drv.click("bloom");
    drv.click("q.ultra");
    ui.update();
    Result r;
    ui.dumpLayout(r.layout);
    ui.dumpA11y(r.a11y);
    r.drawHash = ui.drawList().hash();
    Image img;
    render(ui, img);
    r.imageHash = img.hash();
    return r;
}
} // namespace

int main() {
    const Result a = run(false), b = run(true), c = run(false);
    UI_CHECK(a.layout == b.layout);
    UI_CHECK(a.a11y == b.a11y);
    UI_CHECK_EQ(a.drawHash, b.drawHash);
    UI_CHECK_EQ(a.imageHash, b.imageHash);
    UI_CHECK_EQ(a.drawHash, c.drawHash);
    UI_CHECK_EQ(a.imageHash, c.imageHash);
    std::printf("draw hash %016llx image hash %016llx\n", (unsigned long long)a.drawHash, (unsigned long long)a.imageHash);
    return finish("fuse_ui_single_thread_parity");
}
