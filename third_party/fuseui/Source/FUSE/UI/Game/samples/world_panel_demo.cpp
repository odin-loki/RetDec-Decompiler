// world_panel_demo (UI7): a UI panel rendered to a texture and placed in the world (UiSurface). The view is ray
// traced on the CPU — every pixel's camera ray is intersected with the panel (surfaceRaycast) and samples the
// panel texture — and a scripted "laser pointer" ray presses a button on the panel. --screenshot=PATH.
#include "fuse/app/app.hpp"
#include "fuse/text/text.hpp"
#include "fuse/ui/game/game_ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace fuse::ui;
namespace g = fuse::ui::game;

namespace {
g::Vec3 rayDir(int x, int y, int w, int h, f32 fovY) { // camera at eye looking -Z
    const f32 t = std::tan(fovY * 0.5f);
    const f32 nx = (2.f * (f32(x) + 0.5f) / f32(w) - 1.f) * t * f32(w) / f32(h);
    const f32 ny = (1.f - 2.f * (f32(y) + 0.5f) / f32(h)) * t;
    const f32 l = std::sqrt(nx * nx + ny * ny + 1.f);
    return {nx / l, ny / l, -1.f / l};
}
}

int main(int argc, char** argv) {
    std::string out = "world_panel_demo.png";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--screenshot=", 0) == 0) out = a.substr(13);
    }
    auto font = fuse::text::Font::bundled();
    if (!font) return 1;
    UiContext panel({.viewport = {320, 200}, .font = &*font});
    widgets::install(panel);
    panel.setCss(panel.root(), "padding", "14px");
    panel.setCss(panel.root(), "gap", "8px");
    widgets::label(panel, panel.root(), "Relay terminal", "title");
    panel.setCss(panel.find("title"), "font-size", "20px");
    widgets::progressBar(panel, panel.root(), 0.4f, "charge");
    const NodeHandle btn = widgets::button(panel, panel.root(), "Boost relay", "boost");
    widgets::checkBox(panel, panel.root(), "Broadcast", true, "bc");
    int clicks = 0;
    panel.connect(btn, SignalKind::Clicked, [&clicks](UiContext& u, NodeHandle, const SignalArgs&) {
        ++clicks;
        u.setValue(u.find("charge"), 0.9f);
    });

    // panel in the world: 1.6 x 1.0 m, 3 m ahead, yawed 25 degrees
    g::UiSurface s;
    s.ui = &panel;
    const f32 yaw = 25.f * 3.14159265f / 180.f;
    s.center = {0.3f, 1.6f, -3.f};
    s.right = {std::cos(yaw), 0.f, std::sin(yaw)};
    s.up = {0.f, 1.f, 0.f};
    s.width = 1.6f;
    s.height = 1.0f;
    g::surfaceRender(s, 0.f);

    const g::Vec3 eye{0.f, 1.6f, 0.f};
    // laser pointer at the button's centre: aim through the panel point (u,v) of the button
    const Rect br = panel.rect(btn);
    const f32 u = (br.x + br.w * 0.5f) / 320.f - 0.5f, v = 0.5f - (br.y + br.h * 0.5f) / 200.f;
    const g::Vec3 target{s.center.x + s.right.x * u * s.width, s.center.y + v * s.height, s.center.z + s.right.z * u * s.width};
    g::Vec3 d{target.x - eye.x, target.y - eye.y, target.z - eye.z};
    const f32 l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    d = {d.x / l, d.y / l, d.z / l};
    g::surfacePointer(s, eye, d, true, false);
    g::surfaceRender(s, 1.f / 60.f);
    g::surfacePointer(s, eye, d, false, true);
    g::surfaceRender(s, 1.f / 60.f);
    g::surfacePointer(s, eye, d, false, false); // keep hover on the button for the shot
    g::surfaceRender(s, 1.f / 60.f);

    const int W = 800, H = 450;
    Image img;
    img.resize(W, H, 0xFF000000u);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const g::Vec3 rd = rayDir(x, y, W, H, 1.0f);
            u32 px = rd.y > 0.f ? 0xFF5C4A2Eu : 0xFF3A5A3Bu; // sky / ground (ABGR)
            if (rd.y <= 0.f && (int(std::floor(eye.x + rd.x * -eye.y / rd.y)) + int(std::floor(rd.z * -eye.y / rd.y))) % 2 == 0) px = 0xFF325232u;
            if (auto p = g::surfaceRaycast(s, eye, rd)) {
                const int tx = std::clamp(int(p->x), 0, s.texture.width - 1), ty = std::clamp(int(p->y), 0, s.texture.height - 1);
                const u32 t = s.texture.at(tx, ty);
                const u32 bg = 0xFF201A16u; // panel backing
                const u32 a = t >> 24;
                auto ch = [&](int c) { return ((t >> (8 * c)) & 0xFF) + ((((bg >> (8 * c)) & 0xFF) * (255 - a)) / 255); };
                px = 0xFF000000u | (std::min(255u, ch(2)) << 16) | (std::min(255u, ch(1)) << 8) | std::min(255u, ch(0));
            }
            img.pixels[std::size_t(y) * W + x] = px;
        }
    // laser beam from the lower right to the hit point
    f32 t = 0.f;
    if (g::surfaceRaycast(s, eye, d, &t)) {
        for (int i = 0; i < 400; ++i) {
            const f32 k = f32(i) / 400.f;
            // project the hit point back to the screen: it lies along d; screen of d
            const f32 sx = (d.x / -d.z) / (std::tan(0.5f) * f32(W) / f32(H)), sy = (d.y / -d.z) / std::tan(0.5f);
            const f32 hx = (sx * 0.5f + 0.5f) * W, hy = (0.5f - sy * 0.5f) * H;
            const int x = int(W * 0.8f + (hx - W * 0.8f) * k), y = int(H + (hy - H) * k);
            if (x >= 0 && y >= 0 && x < W && y < H) img.pixels[std::size_t(y) * W + x] = 0xFF4040FFu;
            if (x + 1 < W && y >= 0 && y < H) img.pixels[std::size_t(y) * W + x + 1] = 0xFF4040FFu;
        }
    }
    std::printf("world_panel_demo: button clicks via ray = %d, panel renders = %llu\n", clicks, (unsigned long long)s.renders);
    return fuse::app::writePng(img, out) && clicks == 1 ? 0 : 1;
}
