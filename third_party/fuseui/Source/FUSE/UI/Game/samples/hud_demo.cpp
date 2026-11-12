// hud_demo (UI7): the game HUD over a placeholder scene — bars with damage trail, cooldown dials, compass, radial
// menu, world markers (on-screen, distance-faded, off-screen edge arrow), kill feed, floating text, subtitles,
// typewriter dialogue and device prompts. Writes --screenshot=PATH (default hud_demo.png).
#include "hud_scene.hpp"

#include "fuse/app/app.hpp"
#include "fuse/text/text.hpp"

#include <cstdio>
#include <string>

using namespace fuse::ui;
namespace g = fuse::ui::game;

int main(int argc, char** argv) {
    std::string out = "hud_demo.png";
    bool pad = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--screenshot=", 0) == 0) out = a.substr(13);
        if (a == "--gamepad") pad = true;
    }
    auto font = fuse::text::Font::bundled();
    if (!font) { std::fprintf(stderr, "hud_demo: bundled font missing\n"); return 1; }
    UiContext ui({.viewport = g::demo::kHudViewport, .font = &*font});
    widgets::install(ui);
    g::install(ui);
    auto scene = g::demo::buildHudScene(ui);
    if (pad) { // prompts swap to controller glyphs when the last-used device changes
        scene.game->noteDevice(g::InputDevice::Xbox);
        ui.update(0.f);
    }
    Image img;
    img.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(ui.drawList(), img);
    if (!fuse::app::writePng(img, out)) { std::fprintf(stderr, "hud_demo: cannot write %s\n", out.c_str()); return 1; }
    std::printf("hud_demo: wrote %s (%d draw commands)\n", out.c_str(), int(ui.drawList().size()));
    return 0;
}
