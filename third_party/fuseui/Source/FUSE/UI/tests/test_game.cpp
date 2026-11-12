// fuse_ui_game gates (plan UI7), one executable per gate (FUSE_UI_GAME_GATE):
//   hud_golden          — the HUD demo scene against tests/golden/hud[_full].png (FUSE_UI_UPDATE_GOLDEN=1 rewrites)
//   gamepad_nav         — d-pad / stick spatial navigation with delay + repeat, A activates, B backs out,
//                         prompts swap glyphs with the last-used device, focus-move sound hook
//   world_panel_raycast — UiSurface ray -> panel pixel mapping, back-face / miss rejection, ray click on a button
//   input_arbitration   — pointer over interactive HUD -> UI, over the scene -> game, held press stays with the UI,
//                         keyboard goes to a focused text field, menu mode captures everything
//   pause_time_domain   — pausing freezes game-time HUD (trail, floating text, kill feed, typewriter) while
//                         real-time subtitles and UI animations continue; locale switching re-texts bound nodes
#include "test_util.hpp"

#include "../Game/samples/hud_scene.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace g = fuse::ui::game;
namespace w = fuse::ui::widgets;

namespace {
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
void setup(UiContext& ui) {
    w::install(ui);
    g::install(ui);
}

int hudGolden(const char* outDir) {
#if FUSE_UI_TEST_TEXT_FULL
    const std::string golden = "golden/hud_full.png";
#else
    const std::string golden = "golden/hud.png";
#endif
    UiContext ui({.viewport = g::demo::kHudViewport, .font = &font()});
    setup(ui);
    auto s = g::demo::buildHudScene(ui);
    g::GameUi& gu = *s.game;
    // structure: trail lingers above the value, markers placed / clamped, pools in use
    UI_CHECK(std::fabs(ui.value(s.health) - 0.55f) < 1e-5f);
    UI_CHECK(g::hudBarTrail(ui, s.health) > 0.55f && g::hudBarTrail(ui, s.health) < 1.f);
    UI_CHECK(!gu.markerOffscreen(s.objective));
    UI_CHECK(gu.markerOffscreen(s.behind));
    UI_CHECK(gu.floatTextActive() == 2);
    UI_CHECK(gu.feedLines() == 3);
    UI_CHECK(g::radialSelected(ui, s.radial) == 1);
    // the distant marker fades, the near one does not
    UI_CHECK(ui.computed(gu.markerNode(s.enemy)).opacity < ui.computed(gu.markerNode(s.objective)).opacity);
    // safe area: nothing of the HUD paints in the 16 px top inset except the scene (compass starts at y = 16)
    UI_CHECK(ui.rect(s.compass).y >= 16.f);
    // steady HUD: an idle frame repaints nothing once the animations settle
    Image img;
    render(ui, img);
    if (outDir) stbi_write_png((std::string(outDir) + "/hud_golden.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    if (std::getenv("FUSE_UI_UPDATE_GOLDEN")) {
        stbi_write_png(golden.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", golden.c_str());
        return finish("fuse_ui_game_hud_golden");
    }
    int gw = 0, gh = 0, n = 0;
    unsigned char* ref = stbi_load(golden.c_str(), &gw, &gh, &n, 4);
    UI_CHECK(ref != nullptr);
    if (ref) {
        UI_CHECK_EQ(gw, img.width);
        UI_CHECK_EQ(gh, img.height);
        if (gw == img.width && gh == img.height) {
            int bad = 0, maxd = 0;
            for (int i = 0; i < gw * gh; ++i)
                for (int c = 0; c < 4; ++c) {
                    const int d = std::abs(int(ref[i * 4 + c]) - int(ch(img.pixels[std::size_t(i)], c)));
                    maxd = std::max(maxd, d);
                    if (d > 3) { ++bad; break; }
                }
            std::printf("hud golden diff: %d pixels > 3, max channel diff %d\n", bad, maxd);
            UI_CHECK(bad <= gw * gh / 1000);
        }
        stbi_image_free(ref);
    }
    return finish("fuse_ui_game_hud_golden");
}

struct Sounds {
    int moves = 0, opens = 0, closes = 0;
    static void hook(void* u, g::UiSound s) {
        auto& me = *static_cast<Sounds*>(u);
        if (s == g::UiSound::Move) ++me.moves;
        if (s == g::UiSound::Open) ++me.opens;
        if (s == g::UiSound::Close) ++me.closes;
    }
};

int gamepadNav() {
    UiContext ui({.viewport = {640, 360}, .font = &font()});
    setup(ui);
    // 3x2 grid of menu buttons
    const NodeHandle grid = w::panel(ui, ui.root(), "grid");
    ui.setCss(grid, "gap", "10px");
    NodeHandle rows[2];
    for (int r = 0; r < 2; ++r) {
        rows[r] = w::panel(ui, grid, {});
        ui.setCss(rows[r], "flex-direction", "row");
        ui.setCss(rows[r], "gap", "10px");
        ui.setCss(rows[r], "padding", "0px");
    }
    NodeHandle b[6];
    int clicked = -1;
    for (int i = 0; i < 6; ++i) {
        b[i] = w::button(ui, rows[i / 3], std::string("Item ") + char('1' + i), std::string("b") + char('0' + i));
        ui.setCss(b[i], "width", "100px");
        ui.connect(b[i], SignalKind::Clicked, [&clicked, i](UiContext&, NodeHandle, const SignalArgs&) { clicked = i; });
    }
    g::GameUi gu(ui);
    Sounds snd;
    gu.setSoundHook(&Sounds::hook, &snd);
    gu.watchFocusSounds(true);
    ui.update();
    ui.setFocus(b[0]);
    gu.update(0.f);
    ui.update();
    g::GamepadNav nav(ui);
    const f32 dt = 1.f / 60.f;
    auto step = [&](g::GamepadState s, int frames) {
        int m = 0;
        for (int i = 0; i < frames; ++i) { m += nav.update(s, dt); gu.update(dt); ui.update(dt); }
        return m;
    };
    // tap right: one move
    UI_CHECK_EQ(step({.right = true}, 1), 1);
    step({}, 1);
    UI_CHECK(ui.focused() == b[1]);
    // tap down via the stick: b4
    step({.stick = {0.f, 0.9f}}, 1);
    step({}, 1);
    if (ui.focused() != b[4]) for (int i = 0; i < 6; ++i) { const Rect r = ui.rect(b[i]); std::fprintf(stderr, "b%d %g %g %g %g%s\n", i, double(r.x), double(r.y), double(r.w), double(r.h), ui.focused() == b[i] ? " *" : ""); }
    UI_CHECK(ui.focused() == b[4]);
    // hold left: one move, nothing during the 0.4 s delay, then repeats every 0.12 s (stops at the edge)
    UI_CHECK_EQ(step({.left = true}, 1), 1);
    UI_CHECK(ui.focused() == b[3]);
    UI_CHECK_EQ(step({.left = true}, 20), 0); // 0.35 s held
    step({}, 1);
    // hold up for 0.4 + 2 * 0.12 s: initial + 2 repeats attempted; only the first can move (row 0)
    const int movesUp = step({.up = true}, 1 + 24 + 15);
    UI_CHECK(ui.focused() == b[0]);
    UI_CHECK(movesUp == 1);
    step({}, 1);
    // hold right across the row with repeat: b0 -> b1 (tap) -> b2 (first repeat after 0.4 s)
    step({.right = true}, 1);
    UI_CHECK(ui.focused() == b[1]);
    step({.right = true}, 23);
    UI_CHECK(ui.focused() == b[1]);
    step({.right = true}, 2);
    UI_CHECK(ui.focused() == b[2]);
    step({}, 1);
    // A activates the focused button, B is Back
    step({.a = true}, 1);
    step({}, 1);
    UI_CHECK_EQ(clicked, 2);
    UI_CHECK_EQ(nav.accepts(), 1u);
    step({.b = true}, 1);
    step({}, 1);
    UI_CHECK_EQ(nav.backs(), 1u);
    UI_CHECK(snd.moves >= 4);
    // device prompts swap glyphs when the last-used device changes
    const NodeHandle p = gu.prompt(ui.root(), g::Action::Accept, "Select", "prompt");
    UI_CHECK(ui.text(p).starts_with("Enter\n"));
    gu.noteDevice(g::InputDevice::PlayStation);
    UI_CHECK(ui.text(p) == "Cross\nSelect");
    gu.noteDevice(g::InputDevice::Xbox);
    UI_CHECK(ui.text(p) == "A\nSelect");
    // radial menu by stick direction (up = first item, clockwise), dead zone keeps the pick, accept emits
    const NodeHandle rm = g::radialMenu(ui, ui.root(), "N\nE\nS\nW", "rm");
    UI_CHECK_EQ(g::radialSelect(ui, rm, {0.f, -1.f}), 0);
    UI_CHECK_EQ(g::radialSelect(ui, rm, {1.f, 0.1f}), 1);
    UI_CHECK_EQ(g::radialSelect(ui, rm, {0.f, 1.f}), 2);
    UI_CHECK_EQ(g::radialSelect(ui, rm, {-0.9f, 0.f}), 3);
    { const int k = g::radialSelect(ui, rm, {0.1f, 0.1f}); if (k != 3) std::fprintf(stderr, "dead zone pick %d value %g\n", k, double(ui.value(rm))); UI_CHECK_EQ(k, 3); }
    std::string picked;
    ui.connect(rm, SignalKind::Submitted, [&picked](UiContext&, NodeHandle, const SignalArgs& a) { picked = a.text; });
    UI_CHECK(g::radialAccept(ui, rm));
    UI_CHECK(picked == "W");
    return finish("fuse_ui_gamepad_nav");
}

int worldPanel() {
    UiContext panel({.viewport = {200, 100}, .font = &font()});
    setup(panel);
    panel.setCss(panel.root(), "padding", "0px");
    panel.setCss(panel.root(), "layout", "absolute");
    const NodeHandle btn = w::button(panel, panel.root(), "Press", "btn");
    panel.setCss(btn, "left", "120px");
    panel.setCss(btn, "top", "60px");
    panel.setCss(btn, "width", "60px");
    panel.setCss(btn, "height", "30px");
    int clicks = 0;
    panel.connect(btn, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    g::UiSurface s;
    s.ui = &panel;
    s.center = {0.f, 0.f, -5.f};
    s.width = 2.f;
    s.height = 1.f;
    UI_CHECK(g::surfaceRender(s, 0.f));
    UI_CHECK(s.texture.width == 200 && s.texture.height == 100);
    UI_CHECK(!g::surfaceRender(s, 0.f)); // idle: no re-render
    const g::Vec3 eye{0.f, 0.f, 0.f};
    f32 t = 0.f;
    // centre ray hits the panel centre at distance 5
    auto c = g::surfaceRaycast(s, eye, {0.f, 0.f, -1.f}, &t);
    UI_CHECK(c && std::fabs(c->x - 100.f) < 1e-3f && std::fabs(c->y - 50.f) < 1e-3f && std::fabs(t - 5.f) < 1e-4f);
    // a ray to the top-left corner region
    auto tl = g::surfaceRaycast(s, eye, {-0.9f / 5.f, 0.4f / 5.f, -1.f});
    UI_CHECK(tl && std::fabs(tl->x - 10.f) < 1e-2f && std::fabs(tl->y - 10.f) < 1e-2f);
    // misses: outside, behind the ray, back face
    UI_CHECK(!g::surfaceRaycast(s, eye, {0.5f, 0.f, -1.f}));
    UI_CHECK(!g::surfaceRaycast(s, eye, {0.f, 0.f, 1.f}));
    UI_CHECK(!g::surfaceRaycast(s, {0.f, 0.f, -10.f}, {0.f, 0.f, 1.f}));
    // rotated panel (yaw 90 degrees, facing +X): hit from the side
    g::UiSurface r = s;
    r.center = {-3.f, 0.f, 0.f};
    r.right = {0.f, 0.f, -1.f};
    auto side = g::surfaceRaycast(r, eye, {-1.f, 0.f, 0.f});
    UI_CHECK(side && std::fabs(side->x - 100.f) < 1e-3f);
    // ray click on the button: panel pixel (150, 75) -> world (0.5, -0.25, -5)
    const g::Vec3 d{0.5f / 5.f, -0.25f / 5.f, -1.f};
    UI_CHECK(g::surfacePointer(s, eye, d, false, false));
    g::surfaceRender(s, 1.f / 60.f);
    UI_CHECK(panel.hovered() == btn);
    UI_CHECK(g::surfacePointer(s, eye, d, true, false));
    UI_CHECK(g::surfaceRender(s, 1.f / 60.f)); // pressed state repaints the texture
    UI_CHECK(g::surfacePointer(s, eye, d, false, true));
    g::surfaceRender(s, 1.f / 60.f);
    UI_CHECK_EQ(clicks, 1);
    // pressing on the panel and releasing off it does not click
    g::surfacePointer(s, eye, d, true, false);
    g::surfacePointer(s, eye, {0.f, 3.f, -1.f}, false, true);
    UI_CHECK_EQ(clicks, 1);
    UI_CHECK(!panel.hovered().valid());
    return finish("fuse_ui_world_panel_raycast");
}

int inputArbitration() {
    UiContext ui({.viewport = {640, 360}, .font = &font()});
    setup(ui);
    ui.setCss(ui.root(), "layout", "anchor");
    g::GameUi gu(ui);
    const NodeHandle btn = w::button(ui, gu.safeArea(), "Map", "map");
    ui.setCss(btn, "left", "10px");
    ui.setCss(btn, "top", "10px");
    const NodeHandle field = w::lineEdit(ui, gu.safeArea(), "", "chat");
    ui.setCss(field, "left", "10px");
    ui.setCss(field, "top", "300px");
    ui.setCss(field, "width", "200px");
    gu.feed("kill feed line over the scene");
    gu.floatText({300.f, 150.f}, "-10", Color::rgb(0xFF0000));
    ui.update();
    g::InputArbiter arb(ui);
    const Rect br = ui.rect(btn);
    const Vec2 onBtn{br.x + 5.f, br.y + 5.f}, scene{320.f, 200.f};
    const Rect fr = ui.rect(ui.find("hud.killfeed"));
    const Vec2 onFeed{fr.x + 20.f, fr.y + 8.f};
    // pointer: interactive HUD -> UI; scene, floating text, kill feed (non-interactive HUD) -> game
    UI_CHECK(arb.pointerOwner(onBtn) == g::InputOwner::Ui);
    UI_CHECK(arb.pointerOwner(scene) == g::InputOwner::Game);
    UI_CHECK(arb.pointerOwner({305.f, 155.f}) == g::InputOwner::Game);
    UI_CHECK(arb.pointerOwner(onFeed) == g::InputOwner::Game);
    // a press that starts on the UI stays with the UI until release, even when dragged over the scene
    int clicks = 0;
    ui.connect(btn, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    arb.pointerMove(onBtn);
    UI_CHECK(arb.pointerDown(onBtn) == g::InputOwner::Ui);
    UI_CHECK(arb.pointerMove(scene) == g::InputOwner::Ui);
    UI_CHECK(arb.pointerUp(scene) == g::InputOwner::Ui);
    UI_CHECK_EQ(clicks, 0); // released off the button
    UI_CHECK(arb.pointerOwner(scene) == g::InputOwner::Game);
    arb.pointerMove(onBtn);
    arb.pointerDown(onBtn);
    UI_CHECK(arb.pointerUp(onBtn) == g::InputOwner::Ui);
    ui.update();
    UI_CHECK_EQ(clicks, 1);
    // a scene press goes to the game and does not touch UI focus
    UI_CHECK(arb.pointerDown(scene) == g::InputOwner::Game);
    UI_CHECK(arb.pointerUp(scene) == g::InputOwner::Game);
    // keyboard: game unless a text field is focused
    ui.setFocus({});
    ui.update();
    UI_CHECK(arb.keyboardOwner() == g::InputOwner::Game);
    UI_CHECK(arb.key(Key::Space, true) == g::InputOwner::Game);
    ui.setFocus(field);
    ui.update();
    UI_CHECK(arb.keyboardOwner() == g::InputOwner::Ui);
    ui.textInput("gg");
    ui.update();
    UI_CHECK(ui.text(field) == "gg" || ui.editBuffer(field).text() == "gg");
    ui.setFocus({});
    ui.update();
    // menu mode: everything to the UI, gamepad included
    UI_CHECK(arb.gamepadOwner() == g::InputOwner::Game);
    arb.setMenuOpen(true);
    UI_CHECK(arb.pointerOwner(scene) == g::InputOwner::Ui);
    UI_CHECK(arb.keyboardOwner() == g::InputOwner::Ui);
    UI_CHECK(arb.gamepadOwner() == g::InputOwner::Ui);
    arb.setMenuOpen(false);
    // a dialogue box is a blocker: clicks on it do not reach the game
    gu.say("Npc", "Hello", 100.f);
    ui.update();
    const Rect dr = ui.rect(ui.find("hud.dialogue"));
    UI_CHECK(arb.pointerOwner({dr.x + dr.w * 0.5f, dr.y + dr.h * 0.5f}) == g::InputOwner::Ui);
    return finish("fuse_ui_input_arbitration");
}

int pauseTimeDomain() {
    UiContext ui({.viewport = {640, 360}, .font = &font()});
    setup(ui);
    g::GameUi gu(ui);
    const NodeHandle bar = g::hudBar(ui, gu.safeArea(), 1.f, "hp");
    const NodeHandle title = w::label(ui, gu.safeArea(), "", "title");
    const NodeHandle fadeMe = w::label(ui, gu.safeArea(), "menu", "menu");
    ui.update();
    g::setHudBar(ui, bar, 0.4f);
    gu.floatText({100.f, 100.f}, "+5", Color::rgb(0x00FF00), 1.f);
    gu.feed("a", 2.f);
    gu.subtitle("", "real-time line", 1.f);
    gu.say("A", "0123456789", 10.f); // 10 chars / s of game time
    const f32 dt = 0.1f;
    auto run = [&](int n) { for (int i = 0; i < n; ++i) { gu.update(dt); ui.update(dt); } };
    run(3); // 0.3 s
    UI_CHECK_EQ(gu.dialogueVisible().size(), std::size_t(3));
    const f32 trail = g::hudBarTrail(ui, bar);
    UI_CHECK(trail == 1.f); // still in the 0.35 s hold
    // pause: game time stops, menus (UI animations) and subtitles keep going
    gu.clock().paused = true;
    ui.animate(fadeMe, UiContext::AnimProp::Opacity, 0.f, 0.5f);
    const double gt = gu.clock().gameTime;
    const std::string fbefore(ui.text(fadeMe));
    run(10); // 1 s real
    UI_CHECK(gu.clock().gameTime == gt);
    UI_CHECK(std::fabs(gu.clock().realTime - 1.3) < 1e-4);
    UI_CHECK_EQ(gu.dialogueVisible().size(), std::size_t(3));
    UI_CHECK(g::hudBarTrail(ui, bar) == trail);
    UI_CHECK_EQ(gu.floatTextActive(), 1u);
    UI_CHECK_EQ(gu.feedLines(), 1u);
    UI_CHECK(gu.subtitleText().empty()); // real-time subtitle expired during the pause
    UI_CHECK(!ui.animating());          // the menu fade ran in real time
    // resume: game-time elements continue from where they stopped
    gu.clock().paused = false;
    run(1);
    UI_CHECK_EQ(gu.dialogueVisible().size(), std::size_t(4));
    run(2); // 0.6 s game time total: trail now draining
    UI_CHECK(g::hudBarTrail(ui, bar) < 1.f && g::hudBarTrail(ui, bar) > 0.4f);
    run(5); // 1.1 s: floating text gone
    UI_CHECK_EQ(gu.floatTextActive(), 0u);
    UI_CHECK(gu.dialogueComplete());
    run(12);
    UI_CHECK(g::hudBarTrail(ui, bar) == 0.4f);
    UI_CHECK_EQ(gu.feedLines(), 0u);
    // slow motion scales game time only
    gu.clock().scale = 0.5f;
    const double g0 = gu.clock().gameTime;
    run(2);
    UI_CHECK(std::fabs((gu.clock().gameTime - g0) - 0.1) < 1e-5);
    // skip completes then closes the dialogue
    gu.say("A", "long line", 1.f);
    UI_CHECK(!gu.dialogueComplete());
    gu.skip();
    UI_CHECK(gu.dialogueComplete() && gu.dialogueOpen());
    gu.skip();
    UI_CHECK(!gu.dialogueOpen());
    // locale table: runtime switch re-texts bound nodes
    g::StringTable st;
    st.add("en", "menu.title=Inventory\nmenu.back=Back");
    st.add("de", "menu.title=Inventar\nmenu.back=Zur\xC3\xBC" "ck");
    st.bind(ui, title, "menu.title");
    UI_CHECK(ui.text(title) == "Inventory");
    UI_CHECK(st.setLocale("de"));
    st.refresh(ui);
    UI_CHECK(ui.text(title) == "Inventar");
    UI_CHECK(st.tr("menu.back") == "Zur\xC3\xBC" "ck");
    UI_CHECK(!st.setLocale("fr"));
    UI_CHECK(st.tr("missing.key") == "missing.key");
    (void)fbefore;
    return finish("fuse_ui_pause_time_domain");
}
} // namespace

int main(int argc, char** argv) {
    const char* outDir = nullptr;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) outDir = argv[i] + 6;
    const std::string gate = FUSE_UI_GAME_GATE;
    if (gate == "hud_golden") return hudGolden(outDir);
    if (gate == "gamepad_nav") return gamepadNav();
    if (gate == "world_panel_raycast") return worldPanel();
    if (gate == "input_arbitration") return inputArbitration();
    if (gate == "pause_time_domain") return pauseTimeDomain();
    std::fprintf(stderr, "unknown gate %s\n", gate.c_str());
    return 2;
}
