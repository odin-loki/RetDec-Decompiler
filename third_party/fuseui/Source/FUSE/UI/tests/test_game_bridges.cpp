// fuse_ui_game engine bridges (plan UI7, in tree only), one executable per gate (FUSE_UI_GAME_GATE):
//   ecs_surface — UiSurface as an ECS component: placement from Transform (scale sizes the panel), ray routing to the
//                 nearest panel not hidden by scene depth, always-on-top panels, ray click; CPU reference composite
//                 with a per-pixel depth test against a scene depth buffer (wall occludes, overlay shows through)
//   ui_audio    — @sounds theme clips loaded through fuse_audio (WAV on disk + in-memory clip), GameUi sound hook
//                 plays them, the Null backend capture contains the mixed one-shots
// `--out=<dir>` writes ecs_surface.png.
#include "test_util.hpp"

#if FUSE_UI_GATE_ECS
#include "fuse/ui/game/ecs_surface.hpp"
#include <fuse/ecs/components/transform.hpp>
#endif
#if FUSE_UI_GATE_AUDIO
#include "fuse/ui/game/ui_audio.hpp"
#include <fuse/audio/audio_registry.hpp>
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace g = fuse::ui::game;
namespace w = fuse::ui::widgets;

namespace {
const char* g_out = nullptr;
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }

#if FUSE_UI_GATE_ECS
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::Transform;

void setTransform(Transform& t, g::Vec3 p, f32 sx, f32 sy) {
    t.local_to_world = fuse::ecs::mat4::identity();
    t.local_to_world.data[0] = sx;
    t.local_to_world.data[5] = sy;
    t.local_to_world.data[12] = p.x;
    t.local_to_world.data[13] = p.y;
    t.local_to_world.data[14] = p.z;
}
struct Panel {
    UiContext ui;
    NodeHandle btn;
    int clicks = 0;
    g::UiSurface surf;
    Panel(Size vp, const char* bg, const char* text) : ui({.viewport = vp, .font = &font()}) {
        w::install(ui);
        g::install(ui);
        ui.setCss(ui.root(), "layout", "anchor");
        ui.setCss(ui.root(), "background", bg);
        btn = w::button(ui, ui.root(), text, "btn");
        ui.setCss(btn, "left", "20px");
        ui.setCss(btn, "top", "20px");
        ui.setCss(btn, "width", "80px");
        ui.setCss(btn, "height", "40px");
        ui.connect(btn, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
        surf.ui = &ui;
    }
};
// camera at (0,0,6) looking down -Z, 60 degree vertical fov, GL clip
g::Mat4 viewProj(f32 aspect) {
    const f32 f = 1.f / std::tan(0.5f * 1.0471976f), n = 0.1f, fa = 100.f;
    g::Mat4 p{};
    p[0] = f / aspect; p[5] = f; p[10] = (fa + n) / (n - fa); p[11] = -1.f; p[14] = 2.f * fa * n / (n - fa);
    g::Mat4 v{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -6.f, 1};
    g::Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
            for (int k = 0; k < 4; ++k) r[c * 4 + rr] += p[k * 4 + rr] * v[c * 4 + k];
    return r;
}

int ecsSurface() {
    Registry reg;
    reg.init(1024);
    Panel a({200, 120}, "#2A6FDB", "Back"), b({120, 80}, "#E0A030", "Near"), c({100, 60}, "#30C070", "Top");
    // A: 2 x 1.2 at z=0 (component 1x1, transform scale 2 x 1.2); B nearer, overlapping A's right side;
    // C behind the wall but always on top
    const EntityID ea = reg.create(), eb = reg.create(), ec = reg.create(), plain = reg.create();
    setTransform(reg.add<Transform>(ea), {0.f, 0.f, 0.f}, 2.f, 1.2f);
    reg.add<g::UiSurfaceComponent>(ea, {&a.surf, 1.f, 1.f, true, true, 1.f});
    setTransform(reg.add<Transform>(eb), {0.9f, -0.3f, 1.f}, 1.f, 1.f);
    reg.add<g::UiSurfaceComponent>(eb, {&b.surf, 1.2f, 0.8f, true, true, 1.f});
    setTransform(reg.add<Transform>(ec), {-1.6f, 1.2f, -1.f}, 1.f, 1.f);
    reg.add<g::UiSurfaceComponent>(ec, {&c.surf, 1.f, 0.6f, false, true, 0.9f});
    reg.add<Transform>(plain); // no surface: ignored
    g::UiSurfaceSystem sys;
    UI_CHECK_EQ(sys.update(reg, 0.f), 3u);
    UI_CHECK_EQ(sys.update(reg, 0.f), 0u); // idle
    UI_CHECK(std::fabs(a.surf.width - 2.f) < 1e-5f && std::fabs(a.surf.height - 1.2f) < 1e-5f);
    UI_CHECK(std::fabs(b.surf.center.z - 1.f) < 1e-6f);
    UI_CHECK(a.surf.texture.width == 200 && a.surf.texture.height == 120);

    const g::Vec3 eye{0.f, 0.f, 6.f};
    auto dirTo = [&](g::Vec3 p) { return g::Vec3{p.x - eye.x, p.y - eye.y, p.z - eye.z}; };
    // A's centre-left: only A
    auto h = sys.raycast(reg, eye, dirTo({-0.5f, 0.f, 0.f}));
    UI_CHECK(h && h->entity == ea && std::fabs(h->t - std::hypot(0.5f, 6.f)) < 1e-3f);
    // the overlap: B is nearer
    h = sys.raycast(reg, eye, dirTo({0.9f, -0.3f, 0.f}));
    UI_CHECK(h && h->entity == eb);
    // a wall at distance 3 along that ray hides both depth-tested panels
    UI_CHECK(!sys.raycast(reg, eye, dirTo({-0.5f, 0.f, 0.f}), 3.f));
    // C is always on top: it wins even with a wall in front
    h = sys.raycast(reg, eye, dirTo({-1.6f, 1.2f, -1.f}), 2.f);
    UI_CHECK(h && h->entity == ec);
    // moving the entity moves the panel
    setTransform(*reg.get<Transform>(eb), {0.9f, 5.f, 1.f}, 1.f, 1.f);
    h = sys.raycast(reg, eye, dirTo({0.9f, -0.3f, 0.f}));
    UI_CHECK(h && h->entity == ea);
    setTransform(*reg.get<Transform>(eb), {0.9f, -0.3f, 1.f}, 1.f, 1.f);
    // ray click on B's button: panel pixel (60, 40) = world (0.9 - 0.6 + 0.6, ...) -> local (0, 0) offset
    // B pixel (60, 40) is its centre; the button spans x 20..100, y 20..60 -> centre is on it
    const g::Vec3 onB = dirTo({0.9f, -0.3f, 1.f});
    sys.pointer(reg, eye, onB, false, false);
    sys.update(reg, 1.f / 60.f);
    UI_CHECK(b.ui.hovered() == b.btn);
    sys.pointer(reg, eye, onB, true, false);
    sys.update(reg, 1.f / 60.f);
    sys.pointer(reg, eye, onB, false, true);
    sys.update(reg, 1.f / 60.f);
    UI_CHECK_EQ(b.clicks, 1);
    UI_CHECK_EQ(a.clicks, 0);
    // press on B, release with the ray behind a wall: no click, hover cleared
    sys.pointer(reg, eye, onB, true, false);
    sys.pointer(reg, eye, onB, false, true, 1.f);
    sys.update(reg, 1.f / 60.f);
    UI_CHECK_EQ(b.clicks, 1);
    UI_CHECK(!b.ui.hovered().valid());
    sys.pointer(reg, eye, dirTo({0.f, 9.f, 0.f}), false, false);
    sys.update(reg, 0.5f);

    // composite: scene = dark floor colour, a wall at z = 0.5 over x < -0.4 (world), drawn grey
    const int W = 480, H = 270;
    Image color;
    color.width = W;
    color.height = H;
    color.pixels.assign(std::size_t(W) * H, 0xFF201A18u);
    std::vector<f32> depth(std::size_t(W) * H, 1e30f);
    const g::Mat4 vp = viewProj(f32(W) / f32(H));
    const g::Mat4 inv = g::inverse(vp);
    auto rayFor = [&](int x, int y) {
        const f32 nx = (f32(x) + 0.5f) / f32(W) * 2.f - 1.f, ny = 1.f - (f32(y) + 0.5f) / f32(H) * 2.f;
        const f32 v[4] = {nx, ny, 1.f, 1.f};
        f32 r[4];
        for (int i = 0; i < 4; ++i) r[i] = inv[i] * v[0] + inv[4 + i] * v[1] + inv[8 + i] * v[2] + inv[12 + i] * v[3];
        g::Vec3 d{r[0] / r[3] - eye.x, r[1] / r[3] - eye.y, r[2] / r[3] - eye.z};
        const f32 l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        return g::Vec3{d.x / l, d.y / l, d.z / l};
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const g::Vec3 d = rayFor(x, y);
            const f32 t = (0.5f - eye.z) / d.z;
            if (t > 0.f && eye.x + d.x * t < -0.4f && std::fabs(eye.y + d.y * t) < 1.5f) {
                depth[std::size_t(y) * W + x] = t;
                color.pixels[std::size_t(y) * W + x] = 0xFF707070u;
            }
        }
    const Image scene = color;
    const u64 written = sys.composite(reg, vp, eye, color, depth);
    UI_CHECK(written > 0);
    // inverse sanity: inv * vp == identity
    {
        f32 err = 0.f;
        for (int c2 = 0; c2 < 4; ++c2)
            for (int r = 0; r < 4; ++r) {
                f32 s = 0.f;
                for (int k = 0; k < 4; ++k) s += inv[k * 4 + r] * vp[c2 * 4 + k];
                err = std::max(err, std::fabs(s - (r == c2 ? 1.f : 0.f)));
            }
        UI_CHECK(err < 1e-4f);
    }
    auto project = [&](g::Vec3 p, int& px, int& py) {
        f32 r[4];
        for (int i = 0; i < 4; ++i) r[i] = vp[i] * p.x + vp[4 + i] * p.y + vp[8 + i] * p.z + vp[12 + i];
        px = int((r[0] / r[3] * 0.5f + 0.5f) * f32(W));
        py = int((0.5f - r[1] / r[3] * 0.5f) * f32(H));
    };
    auto px = [&](g::Vec3 p) { int x, y; project(p, x, y); return color.at(x, y); };
    auto sc = [&](g::Vec3 p) { int x, y; project(p, x, y); return scene.at(x, y); };
    // A visible right of the wall (blue background), hidden behind it (wall grey stays)
    const u32 aOpen = px({0.8f, 0.5f, 0.f});
    UI_CHECK(ch(aOpen, 2) > 0xB0 && ch(aOpen, 0) < 0x60);
    UI_CHECK(px({-0.8f, -0.4f, 0.f}) == sc({-0.8f, -0.4f, 0.f}));
    UI_CHECK(sc({-0.8f, -0.4f, 0.f}) == 0xFF707070u);
    // B over A where they overlap (orange)
    const u32 bPix = px({1.4f, -0.6f, 1.f});
    UI_CHECK(ch(bPix, 0) > 0xC0 && ch(bPix, 2) < 0x60);
    // C (always on top) visible although behind the wall; opacity 0.9 lets 10 % of the wall through
    const u32 cPix = px({-1.95f, 1.35f, -1.f});
    UI_CHECK(sc({-1.95f, 1.35f, -1.f}) == 0xFF707070u);
    UI_CHECK(ch(cPix, 1) > 0xA0 && ch(cPix, 0) < 0x50);
    // background untouched outside every panel and the wall
    UI_CHECK(px({2.5f, 1.6f, 0.f}) == 0xFF201A18u);
    if (g_out) {
        std::vector<u8> rgba(std::size_t(W) * H * 4);
        for (std::size_t i = 0; i < color.pixels.size(); ++i)
            for (int k = 0; k < 4; ++k) rgba[i * 4 + k] = ch(color.pixels[i], k);
        const std::string path = std::string(g_out) + "/ecs_surface.png";
        stbi_write_png(path.c_str(), W, H, 4, rgba.data(), W * 4);
        std::printf("wrote %s\n", path.c_str());
    }
    return finish("fuse_ui_ecs_surface");
}
#endif

#if FUSE_UI_GATE_AUDIO
namespace fa = fuse::audio;
fa::AudioClip tone(f32 hz, u32 frames, f32 amp) {
    fa::AudioClip c;
    c.sample_rate = 48000;
    c.channel_count = 1;
    c.samples.resize(frames);
    for (u32 i = 0; i < frames; ++i) c.samples[i] = amp * std::sin(6.2831853f * hz * f32(i) / 48000.f);
    c.duration = f32(frames) / 48000.f;
    return c;
}
bool writeWav(const std::string& path, f32 hz, u32 frames) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32le = [&](u32 v) { std::fwrite(&v, 4, 1, f); };
    auto u16le = [&](u16 v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32le(36 + frames * 2); std::fwrite("WAVEfmt ", 1, 8, f);
    u32le(16); u16le(1); u16le(1); u32le(48000); u32le(96000); u16le(2); u16le(16);
    std::fwrite("data", 1, 4, f); u32le(frames * 2);
    for (u32 i = 0; i < frames; ++i) {
        const auto s = int16_t(std::lround(12000.f * std::sin(6.2831853f * hz * f32(i) / 48000.f)));
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return true;
}
int uiAudio() {
    const std::string dir = "/tmp";
    UI_CHECK(writeWav(dir + "/fuse_ui_accept.wav", 880.f, 4800));
    const char* css = "@sounds { volume: 0.5; accept: fuse_ui_accept.wav; error: missing_clip.wav; }\n"
                      ".x { color: #fff; }";
    g::SoundTheme theme;
    std::vector<std::string> errs;
    const std::string rest = g::extractSoundTheme(css, theme, &errs);
    UI_CHECK(errs.empty());
    UI_CHECK(theme.clip(g::UiSound::Accept) == "fuse_ui_accept.wav");

    fa::AudioDesc desc;
    desc.frames_per_buf = 512;
    desc.cuda_reverb = false;
    desc.hrtf_enabled = false;
    desc.output = fa::AudioOutputRequest::Null;
    desc.capture_frames = 512 * 40;
    fa::AudioEngine engine;
    engine.init(desc);
    fa::AudioRegistry registry;
    g::UiAudio audio(engine);
    UI_CHECK_EQ(audio.load(theme, dir), 1u); // the missing clip fails, the WAV loads
    UI_CHECK(audio.has(g::UiSound::Accept));
    UI_CHECK(!audio.has(g::UiSound::Error));
    audio.setClip(g::UiSound::Move, tone(440.f, 2400, 0.4f));

    UiContext ui({.viewport = {320, 180}, .font = &font()});
    w::install(ui);
    g::install(ui);
    g::GameUi game(ui);
    audio.attach(game);
    // silence first
    for (int i = 0; i < 5; ++i) engine.update(registry, 1.f / 60.f);
    game.playSound(g::UiSound::Move);
    game.playSound(g::UiSound::Error); // no clip: ignored
    for (int i = 0; i < 10; ++i) engine.update(registry, 1.f / 60.f);
    game.playSound(g::UiSound::Accept);
    for (int i = 0; i < 20; ++i) engine.update(registry, 1.f / 60.f);
    UI_CHECK_EQ(audio.played(g::UiSound::Move), 1u);
    UI_CHECK_EQ(audio.played(g::UiSound::Accept), 1u);
    UI_CHECK_EQ(audio.played(g::UiSound::Error), 0u);
    const std::vector<float>& cap = engine.output_capture();
    UI_CHECK(cap.size() >= std::size_t(512) * 2 * 30);
    auto peak = [&](std::size_t b, std::size_t e) {
        f32 p = 0.f;
        for (std::size_t i = b * 2; i < std::min(e * 2, cap.size()); ++i) p = std::max(p, std::fabs(cap[i]));
        return p;
    };
    const f32 quiet = peak(0, 512 * 5), moveP = peak(512 * 5, 512 * 15), accP = peak(512 * 15, 512 * 35);
    std::printf("peaks: idle %.4f move %.4f accept %.4f\n", quiet, moveP, accP);
    UI_CHECK(quiet < 1e-4f);
    UI_CHECK(moveP > 0.05f);
    UI_CHECK(accP > 0.05f);
    std::remove((dir + "/fuse_ui_accept.wav").c_str());
    return finish("fuse_ui_ui_audio");
}
#endif
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
#if FUSE_UI_GATE_ECS
    return ecsSurface();
#elif FUSE_UI_GATE_AUDIO
    return uiAudio();
#else
    return 1;
#endif
}
