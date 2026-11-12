// fuse_app native window gate (plan UI5): a real OS window (X11 under Xvfb, Win32 under Wine / Windows) presented by
// the Vulkan swapchain presenter, read back from the window system and compared with the CPU reference raster.
//   * GPU draw list path and CPU upload path, two windows at once, incremental (damage) frames
//   * the presented pixels match the CPU raster: max channel error <= 2/255 and < 0.5 % of pixels differ at all
//   * idle: no frames are presented while nothing changes (Expose / Configure noise must not cause a redraw loop)
// Exit 77 (skip) without a display or a Vulkan device that can present. --out=<dir> writes the captured windows.
#include "fuse/app/native.hpp"
#include "fuse/ui/doc/document.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace fuse::ui;
namespace app = fuse::app;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {
constexpr const char* kDoc = R"({ "fuseui": 1, "root": { "class": "panel", "id": "p", "children": [
    { "class": "label", "id": "title", "text": "Native window {n}" },
    { "class": "button", "id": "b", "text": "Press" },
    { "class": "checkbox", "id": "c", "text": "Option" },
    { "class": "slider", "id": "s" },
    { "class": "lineedit", "id": "e", "text": "Type here" } ] } })";

struct Diff {
    int maxErr = 0;
    double fraction = 0.0;
};
Diff compare(const Image& a, const Image& b) {
    Diff d;
    if (a.width != b.width || a.height != b.height) return {255, 1.0};
    std::size_t differ = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        int e = 0;
        for (int c = 0; c < 3; ++c) e = std::max(e, std::abs(int((a.pixels[i] >> (8 * c)) & 0xFF) - int((b.pixels[i] >> (8 * c)) & 0xFF)));
        d.maxErr = std::max(d.maxErr, e);
        differ += e != 0;
    }
    d.fraction = double(differ) / double(a.pixels.size());
    return d;
}
void presentAtLeast(app::Application& a, app::AppWindow& w, u64 frames) {
    for (int i = 0; i < 400 && w.framesPresented() < frames; ++i) a.step(0.01f);
}
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_native", .argc = argc, .argv = argv, .loadSettings = false}};
    const std::string out(a.args().get("out"));
    app::NativePlatform native(a);
    if (!native.available()) {
        std::printf("fuse_app_native_window: skipped (%.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 77;
    }
    fuse::ui::doc::Model model;
    model.set("n", 1.0);
    auto& gpuWin = a.createWindow({.title = "fuse_app gate (GPU draw)", .width = 320, .height = 240});
    CHECK(gpuWin.loadText(kDoc, &model));
    if (native.show(gpuWin) != app::PresenterKind::Vulkan) {
        std::printf("fuse_app_native_window: skipped, no Vulkan presenter (%.*s)\n", int(native.lastError().size()),
                    native.lastError().data());
        return 77;
    }
    std::printf("fuse_app_native_window: Vulkan presenter on %.*s, GPU draw list: %s\n", int(native.gpuName().size()),
                native.gpuName().data(), native.gpuDraws(gpuWin) ? "yes" : "no (CPU upload)");
    presentAtLeast(a, gpuWin, 1);
    CHECK(gpuWin.framesPresented() >= 1);

    auto check = [&](app::AppWindow& w, const char* name, int maxErr, double maxFrac) {
        for (int i = 0; i < 10; ++i) a.step(0.005f); // let Expose / Map settle
        w.requestRedraw();
        presentAtLeast(a, w, w.framesPresented() + 1);
        Image shown;
        CHECK(native.capture(w, shown));
        const Image& ref = w.rasterize({}); // CPU reference of the same draw list
        const Diff d = compare(shown, ref);
        std::printf("  %s: %dx%d, max channel error %d, %.3f %% pixels differ\n", name, shown.width, shown.height, d.maxErr,
                    d.fraction * 100.0);
        CHECK(d.maxErr <= maxErr && d.fraction < maxFrac);
        if (!out.empty()) app::writePng(shown, out + "/" + name + ".png");
    };
    check(gpuWin, "native_gpu", 2, native.gpuDraws(gpuWin) ? 0.005 : 1e-9);

    // incremental frame: a model change repaints a small region; the window shows the new text
    model.set("n", 2.0);
    gpuWin.ui().invalidate(gpuWin.ui().root());
    const u64 before = gpuWin.framesPresented();
    presentAtLeast(a, gpuWin, before + 1);
    CHECK(gpuWin.framesPresented() == before + 1);
    check(gpuWin, "native_gpu_update", 2, native.gpuDraws(gpuWin) ? 0.005 : 1e-9);

    // idle: 40 loop iterations with nothing changing present nothing
    const u64 idle0 = gpuWin.framesPresented();
    for (int i = 0; i < 40; ++i) a.step(0.005f);
    CHECK(gpuWin.framesPresented() == idle0);

    // a second window at the same time; then close the first one
    auto& w2 = a.createWindow({.title = "fuse_app gate (second)", .width = 200, .height = 120});
    widgets::button(w2.ui(), w2.ui().root(), "Second window", "b2");
    CHECK(native.show(w2) == app::PresenterKind::Vulkan);
    presentAtLeast(a, w2, 1);
    check(w2, "native_second", 2, native.gpuDraws(w2) ? 0.005 : 1e-9);
    gpuWin.requestClose();
    a.step(0.f);
    CHECK(a.windowCount() == 1 && native.nativeWindow(w2) != 0);
    w2.requestClose();
    a.step(0.f);
    CHECK(a.windowCount() == 0);

    // CPU upload path through its own platform object: must match the CPU raster exactly
    {
        app::Application b{{.name = "gate_native_cpu", .loadSettings = false}};
        app::NativePlatform cpu(b, {.drawMode = app::VkDrawMode::CpuUpload});
        auto& w = b.createWindow({.title = "fuse_app gate (CPU upload)", .width = 300, .height = 200});
        CHECK(w.loadText(kDoc, &model));
        CHECK(cpu.show(w) == app::PresenterKind::Vulkan && !cpu.gpuDraws(w));
        for (int i = 0; i < 400 && w.framesPresented() < 1; ++i) b.step(0.01f);
        for (int i = 0; i < 10; ++i) b.step(0.005f);
        w.requestRedraw();
        const u64 f0 = w.framesPresented();
        for (int i = 0; i < 400 && w.framesPresented() <= f0; ++i) b.step(0.01f);
        Image shown;
        CHECK(cpu.capture(w, shown));
        const Diff d = compare(shown, w.image());
        std::printf("  native_cpu_upload: %dx%d, max channel error %d, %.3f %% pixels differ\n", shown.width, shown.height,
                    d.maxErr, d.fraction * 100.0);
        CHECK(d.maxErr == 0);
        if (!out.empty()) app::writePng(shown, out + "/native_cpu_upload.png");
    }
    std::printf("fuse_app_native_window: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
