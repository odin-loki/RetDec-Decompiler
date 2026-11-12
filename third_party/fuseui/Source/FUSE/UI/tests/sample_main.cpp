// fuse_ui_sample: renders the Tier A sample tree (with hover / focus states) through the CPU reference rasteriser
// and writes a PNG. Usage: fuse_ui_sample [out.png]
#include "test_util.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace fuse::ui;
using namespace fuse::ui::test;

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "fuse-ui-sample.png";
    UiContext ui({.viewport = {488, 400}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.update();
    UiTestDriver drv(ui);
    drv.click("name");
    drv.key(Key::End);
    drv.hover("apply");
    Image img;
    render(ui, img);
    if (!stbi_write_png(out, img.width, img.height, 4, img.pixels.data(), img.width * 4)) {
        std::fprintf(stderr, "failed to write %s\n", out);
        return 1;
    }
    std::printf("wrote %s (%dx%d, %u nodes, %u draw cmds)\n", out, img.width, img.height, ui.nodeCount(), ui.stats().drawCmds);
    return 0;
}
