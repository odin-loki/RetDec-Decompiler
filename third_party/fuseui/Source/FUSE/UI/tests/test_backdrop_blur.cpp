// fuse_ui_backdrop_blur (plan UI4): DrawOp::Backdrop / `backdrop-blur` (CSS backdrop-filter: blur()).
//   * pixels under the rounded rect are blurred: a hard stripe edge becomes a smooth ramp, its mean colour is kept
//   * pixels outside the rect (and in its rounded corners) are untouched; a tint is blended over the blur
//   * the style property reaches the draw list (Backdrop emitted under the node's background)
// Optional --out=<png>.
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
int ch(u32 p, int k) { return int((p >> (8 * k)) & 0xFFu); }
void scene(DrawList& dl) {
    for (int i = 0; i < 16; ++i) // 20 px vertical stripes, red / blue
        dl.rect({f32(i * 20), 0.f, 20.f, 200.f}, i % 2 ? Color::rgb(0x2050E0) : Color::rgb(0xE04020));
    for (int i = 0; i < 8; ++i) dl.rect({10.f + f32(i) * 40.f, 150.f, 18.f, 18.f}, Color::rgb(0xF0F0F0), 9.f);
}
} // namespace

int main(int argc, char** argv) {
    const Rect card{60.f, 40.f, 200.f, 120.f};
    Image ref, img;
    {
        DrawList dl;
        scene(dl);
        ref.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, ref);
        dl.clear();
        scene(dl);
        dl.backdrop(card, 12.f, {16.f, 16.f, 16.f, 16.f});
        img.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, img);
    }
    // outside: identical; rounded corner pixel: identical
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 320; ++x)
            if (!card.contains({f32(x) + 0.5f, f32(y) + 0.5f})) UI_CHECK(img.at(x, y) == ref.at(x, y));
    UI_CHECK(img.at(61, 41) == ref.at(61, 41));
    // inside: across the red|blue edge at x = 140 the reference jumps, the blur ramps
    {
        int maxStep = 0, refStep = 0;
        for (int x = 120; x < 160; ++x) {
            maxStep = std::max(maxStep, std::abs(ch(img.at(x + 1, 100), 0) - ch(img.at(x, 100), 0)));
            refStep = std::max(refStep, std::abs(ch(ref.at(x + 1, 100), 0) - ch(ref.at(x, 100), 0)));
        }
        std::printf("  stripe edge: max step %d (unblurred %d)\n", maxStep, refStep);
        UI_CHECK(refStep > 150 && maxStep < 40);
        long sumA = 0, sumB = 0; // mean over two full stripe periods is preserved
        for (int x = 100; x < 180; ++x) { sumA += ch(img.at(x, 100), 0); sumB += ch(ref.at(x, 100), 0); }
        UI_CHECK(std::abs(sumA - sumB) < 80 * 6);
    }
    // tint over the blur
    {
        DrawList dl;
        scene(dl);
        dl.backdrop(card, 12.f, {16.f, 16.f, 16.f, 16.f}, Color::rgb(0xFFFFFF, 0.25f));
        Image t;
        t.resize(320, 200, 0xFF000000u);
        CpuRasterizer().render(dl, t);
        UI_CHECK(ch(t.at(140, 100), 1) > ch(img.at(140, 100), 1) + 30);
        const std::string out = argc > 1 && std::string(argv[1]).rfind("--out=", 0) == 0 ? std::string(argv[1]).substr(6) : "";
        if (!out.empty()) {
            Image both;
            both.resize(640, 200, 0xFF000000u);
            for (int y = 0; y < 200; ++y)
                for (int x = 0; x < 320; ++x) { both.pixels[std::size_t(y) * 640 + x] = ref.at(x, y); both.pixels[std::size_t(y) * 640 + 320 + x] = t.at(x, y); }
            stbi_write_png(out.c_str(), 640, 200, 4, both.pixels.data(), 640 * 4);
        }
    }
    // style plumbing: backdrop-blur on a class emits a Backdrop under the background
    {
        UiContext ui({.viewport = {320, 200}, .font = &font()});
        w::install(ui);
        auto sheet = StyleSheet::parse(".glass { backdrop-blur: 12px; background: rgba(255,255,255,0.2); }");
        UI_CHECK(sheet.has_value());
        if (sheet) ui.styles().addSheet(std::move(*sheet));
        const NodeHandle n = w::label(ui, ui.root(), "Frosted");
        ui.addClass(n, "glass");
        ui.update();
        UI_CHECK(ui.computed(n).backdropBlur == 12.f);
        int bi = -1, bg = -1;
        const auto& cmds = ui.drawList().cmds();
        for (int i = 0; i < int(cmds.size()); ++i) {
            if (cmds[std::size_t(i)].op == DrawOp::Backdrop && bi < 0) bi = i;
            if (cmds[std::size_t(i)].op == DrawOp::Rect && cmds[std::size_t(i)].node == n.index && bg < 0) bg = i;
        }
        UI_CHECK(bi >= 0 && bg > bi);
    }
    return finish("fuse_ui_backdrop_blur");
}
