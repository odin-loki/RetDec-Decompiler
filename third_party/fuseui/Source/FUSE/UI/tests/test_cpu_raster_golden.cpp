// Gate fuse_ui_cpu_raster_golden: primitive checks of the CPU reference rasteriser plus the sample widget tree
// against tests/golden/sample.png (per-channel tolerance; regenerate with FUSE_UI_UPDATE_GOLDEN=1).
#include "test_util.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdlib>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
}

int main(int argc, char** argv) {
    const std::string golden = argc > 1 ? argv[1]
#if FUSE_UI_TEST_TEXT_FULL
                                                : "golden/sample_full.png"; // FreeType coverage differs from stb_truetype
#else
                                                : "golden/sample.png";
#endif
    // ---- primitives ----
    {
        Image img;
        img.resize(32, 32, 0);
        DrawList dl;
        dl.rect({4, 4, 8, 8}, Color::rgb(0xFF0000));
        dl.pushClip({0, 0, 16, 32});
        dl.rect({12, 20, 10, 4}, Color::rgb(0x00FF00, 0.5f));
        dl.popClip();
        CpuRasterizer r;
        r.render(dl, img);
        UI_CHECK_EQ(img.at(4, 4), 0xFF0000FFu);
        UI_CHECK_EQ(img.at(11, 11), 0xFF0000FFu);
        UI_CHECK_EQ(img.at(12, 12), 0u);
        UI_CHECK_EQ(img.at(3, 4), 0u);
        UI_CHECK(ch(img.at(13, 21), 1) >= 126 && ch(img.at(13, 21), 1) <= 129); // premultiplied half green
        UI_CHECK(ch(img.at(13, 21), 3) >= 126 && ch(img.at(13, 21), 3) <= 129);
        UI_CHECK_EQ(img.at(17, 21), 0u); // clipped
        // rounded corner is antialiased: corner pixel partially covered
        Image img2;
        img2.resize(32, 32, 0);
        DrawList d2;
        d2.rect({0, 0, 32, 32}, Color::rgb(0xFFFFFF), 10.f);
        r.render(d2, img2);
        UI_CHECK_EQ(ch(img2.at(0, 0), 3), u8(0));
        UI_CHECK_EQ(ch(img2.at(16, 16), 3), u8(255));
        const u8 edge = ch(img2.at(1, 4), 3); // centre 10.1 px from the corner centre: partially covered
        UI_CHECK(edge > 0 && edge < 255);
    }
    // ---- sample tree golden ----
    UiContext ui({.viewport = {488, 400}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    Image img;
    render(ui, img);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden.c_str());
        return finish("fuse_ui_cpu_raster_golden");
    }
    int w = 0, h = 0, n = 0;
    unsigned char* ref = stbi_load(golden.c_str(), &w, &h, &n, 4);
    UI_CHECK(ref != nullptr);
    if (ref) {
        UI_CHECK_EQ(w, img.width);
        UI_CHECK_EQ(h, img.height);
        if (w == img.width && h == img.height) {
            int bad = 0, maxd = 0;
            for (int i = 0; i < w * h; ++i)
                for (int c = 0; c < 4; ++c) {
                    const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                    maxd = std::max(maxd, d);
                    if (d > 3) { ++bad; break; }
                }
            std::printf("golden diff: %d pixels > 3, max channel diff %d\n", bad, maxd);
            UI_CHECK(bad <= w * h / 1000); // ≤ 0.1% of pixels may differ (compiler / libm rounding)
        }
        stbi_image_free(ref);
    }
    return finish("fuse_ui_cpu_raster_golden");
}
