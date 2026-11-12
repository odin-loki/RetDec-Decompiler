// fuse_ui_game gates, second part (plan UI7), one executable per gate (FUSE_UI_GAME_GATE):
//   inventory_dnd   — pointer drag between containers (move, merge with remainder, swap, kind filter, cancel),
//                     gamepad pick-and-place, tooltip follows hover / focus, item totals conserved
//   touch_controls  — multi-touch: floating stick + button at once, dead zone, clamp to radius, edges, hide on device
//   loading_screen  — fades in, keeps animating in real time with the game paused, progress eases, tips rotate,
//                     finish waits for 100 % then fades out; fadeTo transition
//   rich_dialogue   — markup parse (spans, pauses, cues, events, errors), typewriter with pauses, voice-synced reveal
//   sound_theme     — @sounds block extraction from a theme sheet, the CSS engine accepts the rest
// `--out=<dir>` writes game_extras.png (inventory mid-drag, touch controls, rich dialogue) and loading.png.
#include "test_util.hpp"

#include "fuse/ui/game/game_extras.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
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
constexpr Size kVp{960, 540};
const char* g_out = nullptr;

void setup(UiContext& ui) {
    w::install(ui);
    g::install(ui);
    ui.setCss(ui.root(), "layout", "anchor");
    ui.setCss(ui.root(), "background", "#1B2230");
}
Vec2 centre(const UiContext& ui, NodeHandle n) { const Rect r = ui.rect(n); return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void savePng(UiContext& ui, const char* name) {
    if (!g_out) return;
    ui.update(0.f);
    Image img;
    img.resize(int(ui.viewport().w), int(ui.viewport().h), 0u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(ui.drawList(), img);
    for (u32& p : img.pixels) p |= 0xFF000000u;
    const std::string path = std::string(g_out) + "/" + name;
    stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    std::printf("wrote %s\n", path.c_str());
}

struct InvScene {
    g::ItemCatalog items;
    u32 potion = 0, sword = 0, gem = 0, helm = 0;
    NodeHandle bagPanel, chestPanel, equipPanel;
};
void buildItems(InvScene& s) {
    s.potion = s.items.add({"Potion", Color::rgb(0xC0392B), 10, 0});
    s.sword = s.items.add({"Sword", Color::rgb(0x7F8C8D), 1, 1});
    s.gem = s.items.add({"Gem", Color::rgb(0x2E86C1), 20, 0});
    s.helm = s.items.add({"Helm", Color::rgb(0xB9770E), 1, 2});
}
NodeHandle anchored(UiContext& ui, std::string_view id, f32 l, f32 t) {
    NodeHandle n = w::panel(ui, ui.root(), id);
    ui.setCss(n, "left", std::to_string(int(l)) + "px");
    ui.setCss(n, "top", std::to_string(int(t)) + "px");
    ui.setCss(n, "background", "transparent");
    ui.setCss(n, "padding", "0px");
    return n;
}

int inventoryDnd() {
    UiContext ui({.viewport = kVp, .font = &font()});
    setup(ui);
    InvScene s;
    buildItems(s);
    const NodeHandle bagHost = anchored(ui, "bag", 40, 60), chestHost = anchored(ui, "chest", 400, 60),
                     equipHost = anchored(ui, "equip", 40, 300);
    const NodeHandle layer = w::panel(ui, ui.root(), "layer");
    ui.setCss(layer, "layout", "absolute");
    ui.setCss(layer, "left", "0px"); ui.setCss(layer, "top", "0px");
    ui.setCss(layer, "width", "100%"); ui.setCss(layer, "height", "100%");
    ui.setCss(layer, "pointer-events", "none"); ui.setCss(layer, "background", "transparent");
    g::Inventory inv(ui, s.items, layer);
    const u32 bag = inv.addGrid(bagHost, 5, 3, "bag.grid");
    const u32 chest = inv.addGrid(chestHost, 4, 2, "chest.grid");
    const u32 equip = inv.addGrid(equipHost, 2, 1, "equip.grid", 1u << 1 | 1u << 2); // weapons + armour only
    inv.set(bag, 0, {s.potion, 7});
    inv.set(bag, 1, {s.sword, 1});
    inv.set(bag, 2, {s.gem, 15});
    inv.set(chest, 0, {s.potion, 6});
    inv.set(chest, 1, {s.helm, 1});
    inv.set(chest, 2, {s.gem, 12});
    ui.update(0.f);
    const u32 potions = inv.total(s.potion), gems = inv.total(s.gem);
    auto drag = [&](NodeHandle from, NodeHandle to) {
        const Vec2 a = centre(ui, from), b = to.valid() ? centre(ui, to) : Vec2{900, 500};
        inv.pointerMove(a);
        UI_CHECK(inv.pointerDown(a));
        inv.pointerMove({a.x + 10, a.y + 6});
        UI_CHECK(inv.holding());
        inv.pointerMove(b);
        inv.update();
        ui.update(0.016f);
        inv.pointerUp(b);
        inv.update();
        ui.update(0.016f);
    };
    // move to an empty slot in another container
    drag(inv.slotNode(bag, 1), inv.slotNode(chest, 5));
    UI_CHECK(inv.get(bag, 1).empty());
    UI_CHECK_EQ(inv.get(chest, 5).item, s.sword);
    // merge with remainder: 7 potions onto 6 (cap 10) -> 10 there, 3 back in the origin
    drag(inv.slotNode(bag, 0), inv.slotNode(chest, 0));
    UI_CHECK_EQ(inv.get(chest, 0).count, 10u);
    UI_CHECK_EQ(inv.get(bag, 0).count, 3u);
    UI_CHECK_EQ(inv.get(bag, 0).item, s.potion);
    // swap different items
    drag(inv.slotNode(bag, 2), inv.slotNode(chest, 1));
    UI_CHECK_EQ(inv.get(chest, 1).item, s.gem);
    UI_CHECK_EQ(inv.get(bag, 2).item, s.helm);
    // kind filter: potions cannot go into equipment -> returned, counted as a reject
    const u32 rejects = inv.rejects();
    drag(inv.slotNode(bag, 0), inv.slotNode(equip, 0));
    UI_CHECK(inv.get(equip, 0).empty());
    UI_CHECK_EQ(inv.get(bag, 0).count, 3u);
    UI_CHECK(inv.rejects() == rejects + 1);
    // helm into equipment is fine
    drag(inv.slotNode(bag, 2), inv.slotNode(equip, 1));
    UI_CHECK_EQ(inv.get(equip, 1).item, s.helm);
    // release outside any slot returns the stack
    drag(inv.slotNode(chest, 2), {});
    UI_CHECK_EQ(inv.get(chest, 2).count, 12u);
    UI_CHECK(!inv.holding());
    // a click without movement does not drag
    const Vec2 c0 = centre(ui, inv.slotNode(chest, 0));
    inv.pointerDown(c0);
    UI_CHECK(!inv.pointerUp(c0));
    UI_CHECK_EQ(inv.get(chest, 0).count, 10u);
    // gamepad / keyboard pick-and-place on the focused slot; the ghost follows focus
    ui.setFocus(inv.slotNode(chest, 5), FocusReason::Keyboard);
    UI_CHECK(inv.activate());
    UI_CHECK(inv.holding());
    ui.setFocus(inv.slotNode(equip, 0), FocusReason::Keyboard);
    inv.update();
    ui.update(0.016f);
    UI_CHECK(ui.computed(inv.ghost()).visible);
    UI_CHECK(inv.activate());
    UI_CHECK_EQ(inv.get(equip, 0).item, s.sword);
    // tooltip follows the focused slot (gamepad) and names the stack
    ui.setFocus(inv.slotNode(chest, 0), FocusReason::Keyboard);
    inv.update();
    ui.update(0.016f);
    UI_CHECK(ui.computed(inv.tooltip()).visible);
    UI_CHECK(std::string(ui.text(inv.tooltip())) == "Potion  x10");
    const Rect tip = ui.rect(inv.tooltip()), sl = ui.rect(inv.slotNode(chest, 0));
    UI_CHECK(tip.x >= sl.right() && std::fabs(tip.y - sl.y) < 1.f);
    // nothing created or lost
    UI_CHECK_EQ(inv.total(s.potion), potions);
    UI_CHECK_EQ(inv.total(s.gem), gems);
    UI_CHECK_EQ(inv.total(s.sword), 1u);
    UI_CHECK(inv.drops() == 5);
    if (g_out) { // screenshot: mid-drag + touch controls + rich dialogue
        g::TouchControls touch(ui, layer);
        touch.addStick({0, 270, 480, 270}, 56.f);
        touch.addButton({880, 460}, 34.f, "A");
        touch.addButton({810, 500}, 28.f, "B");
        touch.touchDown(1, {120, 440});
        touch.touchMove(1, {160, 420});
        touch.touchDown(2, {880, 460});
        g::RichDialogue dlg(ui, ui.root());
        dlg.say("Mira", "The [color=gold]ember gem[/color] is [b]warm[/b]. [shake]Something[/shake] stirs below.", 1000.f);
        dlg.update(1.f);
        const Vec2 a = centre(ui, inv.slotNode(chest, 2));
        inv.pointerMove(a);
        inv.pointerDown(a);
        inv.pointerMove({a.x + 70, a.y + 30});
        inv.update();
        savePng(ui, "game_extras.png");
    }
    return finish("fuse_ui_inventory_dnd");
}

int touchControls() {
    UiContext ui({.viewport = kVp, .font = &font()});
    setup(ui);
    g::TouchControls t(ui, ui.root());
    const u32 st = t.addStick({0, 270, 480, 270}, 50.f, true, 0.2f);
    const u32 fire = t.addButton({880, 460}, 34.f, "A");
    ui.update(0.f);
    // two fingers at once: stick re-centres on its touch, button holds independently
    t.touchDown(7, {200, 400});
    t.touchDown(9, {885, 455});
    UI_CHECK(t.held(fire) && t.pressed(fire));
    UI_CHECK(t.stick(st).x == 0.f && t.stick(st).y == 0.f);
    t.touchMove(7, {205, 400}); // inside the dead zone (5 / 50 = 0.1)
    UI_CHECK(t.stick(st).x == 0.f);
    t.touchMove(7, {300, 400}); // past the radius: clamped to 1
    UI_CHECK(std::fabs(t.stick(st).x - 1.f) < 1e-4f && t.stick(st).y == 0.f);
    t.touchMove(7, {200, 375}); // half way up: (0.5 - 0.2) / 0.8
    UI_CHECK(std::fabs(t.stick(st).y + 0.375f) < 1e-4f);
    const Rect sr = ui.rect(t.stickNode(st));
    ui.update(0.f);
    UI_CHECK(std::fabs(ui.rect(t.stickNode(st)).x - 150.f) < 1.f); // floating base at the touch-down point
    (void)sr;
    t.endFrame();
    UI_CHECK(!t.pressed(fire) && t.held(fire));
    t.touchUp(9);
    UI_CHECK(t.released(fire) && !t.held(fire));
    // a touch outside every zone does nothing
    t.touchDown(11, {700, 100});
    UI_CHECK(!t.held(fire));
    t.touchUp(7);
    UI_CHECK(t.stick(st).x == 0.f && t.stick(st).y == 0.f);
    // the stick's touch stays with it even when the finger leaves the zone
    t.touchDown(3, {100, 500});
    t.touchMove(3, {600, 100});
    UI_CHECK(std::hypot(t.stick(st).x, t.stick(st).y) > 0.99f);
    // hiding (device switched away from touch) releases everything and ignores new touches
    t.setVisible(false);
    UI_CHECK(t.stick(st).x == 0.f);
    t.touchDown(4, {885, 455});
    UI_CHECK(!t.held(fire));
    ui.update(0.f);
    UI_CHECK(!ui.computed(t.buttonNode(fire)).visible);
    // touch controls never take UI pointer input (pointer-events: none)
    t.setVisible(true);
    ui.update(0.f);
    UI_CHECK(!ui.hitTest({880, 460}).valid() || ui.hitTest({880, 460}) == ui.root());
    return finish("fuse_ui_touch_controls");
}

int loadingScreen() {
    UiContext ui({.viewport = kVp, .font = &font()});
    setup(ui);
    g::GameUi game(ui);
    game.clock().paused = true; // the world is streaming: game time stands still
    g::LoadingScreen ls(ui, ui.root());
    ls.show("Ashen Vale", "Tip one\nTip two\nTip three", 0.25f);
    UI_CHECK(ls.visible() && !ls.opaque());
    const f32 dt = 1.f / 60.f;
    for (int i = 0; i < 20; ++i) { ls.update(dt); game.update(dt); ui.update(dt); }
    UI_CHECK(ls.opaque());
    // stalled stream: progress target stays at 0.3, yet the spinner keeps turning and the bar eases up to it
    ls.setProgress(0.3f);
    const f32 a0 = ls.spinnerAngle();
    for (int i = 0; i < 60; ++i) { ls.update(dt); ui.update(dt); }
    UI_CHECK(ls.spinnerAngle() != a0);
    UI_CHECK(std::fabs(ls.shownProgress() - 0.3f) < 1e-3f);
    UI_CHECK(std::string(ui.text(ui.find("loading.percent"))) == "30%");
    UI_CHECK(game.clock().gameTime == 0.0); // game time never moved
    // progress never goes backwards
    ls.setProgress(0.1f);
    ls.update(dt);
    UI_CHECK(ls.shownProgress() >= 0.3f - 1e-3f);
    // tips rotate every 4 s of real time
    for (int i = 0; i < 4 * 60 + 2; ++i) ls.update(dt);
    UI_CHECK_EQ(ls.tipIndex(), 1u);
    ls.setProgress(0.62f);
    for (int i = 0; i < 30; ++i) ls.update(dt);
    savePng(ui, "loading.png");
    // finish: holds until the bar shows 100 %, then fades out and hides
    ls.finish(0.4f);
    ls.update(dt);
    UI_CHECK(ls.visible() && ls.opaque());
    int frames = 0;
    while (ls.visible() && frames < 600) { ls.update(dt); ++frames; }
    UI_CHECK(!ls.visible() && frames > 24 && frames < 200);
    ui.update(dt);
    UI_CHECK(!ui.computed(ls.node()).visible);
    // fadeTo: opaque after 0.2 s, held 0.1 s, clear after 0.3 s more
    ls.fadeTo(0.2f, 0.1f, 0.3f);
    for (int i = 0; i < 13; ++i) ls.update(dt);
    UI_CHECK(ls.opaque());
    for (int i = 0; i < 26; ++i) ls.update(dt);
    UI_CHECK(!ls.visible());
    return finish("fuse_ui_loading_screen");
}

struct EvLog { std::string names; };
void onEvent(void* u, std::string_view n) { static_cast<EvLog*>(u)->names.append(n).append(";"); }

int richDialogue() {
    // parser
    auto r = g::parseRich("Hi [b]bold [color=#FF0000]red[/color][/b] [[x] [pause=0.5]end[event=door]");
    UI_CHECK(r.has_value());
    if (r) {
        UI_CHECK(r->plain == "Hi bold red [x] end");
        UI_CHECK_EQ(r->spans.size(), std::size_t(4));
        UI_CHECK(r->spans[1].bold && !r->spans[0].bold);
        UI_CHECK(r->spans[2].bold && r->spans[2].color.r > 0.99f && r->spans[2].color.g < 0.01f);
        UI_CHECK(r->plain.substr(r->spans[2].begin, r->spans[2].end - r->spans[2].begin) == "red");
        UI_CHECK(r->pauses.size() == 1 && r->pauses[0].first == 16 && r->pauses[0].second == 0.5f);
        UI_CHECK(r->events.size() == 1 && r->events[0].second == "door" && r->events[0].first == 19);
    }
    UI_CHECK(!g::parseRich("[b]open").has_value());
    UI_CHECK(!g::parseRich("[b]x[/i]").has_value());
    UI_CHECK(!g::parseRich("[wiggle]x[/wiggle]").has_value());
    UI_CHECK(!g::parseRich("[pause=abc]").has_value());
    UI_CHECK(!g::parseRich("[t=2][t=1]").has_value());
    UI_CHECK(!g::parseRich("[color=nope]x[/color]").has_value());
    const auto bad = g::parseRich("ok [b]x");
    UI_CHECK(!bad && bad.error().find("byte 7") != std::string::npos);

    UiContext ui({.viewport = kVp, .font = &font()});
    setup(ui);
    g::RichDialogue d(ui, ui.root());
    EvLog log;
    d.setEventHook(&onEvent, &log);
    UI_CHECK(!d.say("A", "[b]broken"));
    UI_CHECK(!d.lastError().empty() && !d.open());
    // typewriter: 10 cps, a 1 s pause after "Wait." and an event on "go"
    UI_CHECK(d.say("Mira", "Wait.[pause=1] [event=step]go", 10.f));
    const f32 dt = 0.05f;
    for (int i = 0; i < 10; ++i) d.update(dt); // 0.5 s -> 5 chars
    UI_CHECK(d.visible() == "Wait.");
    for (int i = 0; i < 10; ++i) d.update(dt); // inside the pause
    UI_CHECK(d.visible() == "Wait.");
    UI_CHECK(log.names.empty());
    for (int i = 0; i < 14; ++i) d.update(dt); // pause ends, " " then 'g'
    UI_CHECK(d.visible().size() >= 7);
    UI_CHECK(log.names == "step;");
    d.update(0.f, -1.f);
    d.skip();
    UI_CHECK(d.complete() && d.open());
    d.skip();
    UI_CHECK(!d.open());
    // voice sync: "Hello there friend" over a 3 s clip with "friend" cued at 2.5 s
    UI_CHECK(d.sayVoiced("Odin", "Hello there [t=2.5]friend", 3.f));
    d.update(0.f, 1.25f); // half way to the cue: 12 bytes before it -> 6
    UI_CHECK_EQ(d.visibleBytes(), 6u);
    d.update(0.f, 2.5f);
    UI_CHECK(d.visible() == "Hello there ");
    d.update(0.f, 2.75f); // half of "friend"
    UI_CHECK_EQ(d.visibleBytes(), 15u);
    d.update(0.f, 3.1f);
    UI_CHECK(d.complete());
    // the voice clock may jump backwards (seek): reveal never shrinks
    d.update(0.f, 0.5f);
    UI_CHECK(d.complete());
    // the a11y name carries the plain text
    std::string a11y;
    ui.update(0.f);
    ui.dumpA11y(a11y);
    UI_CHECK(a11y.find("Hello there friend") != std::string::npos);
    return finish("fuse_ui_rich_dialogue");
}

int soundTheme() {
    const std::string sheet = R"(button { color: #FFFFFF; }
@sounds { move: ui/move.wav; accept: "ui/accept.wav"; back: ui/back.wav; error: ui/error.wav; volume: 0.5; }
label { font-size: 14px; })";
    g::SoundTheme th;
    std::vector<std::string> errs;
    const std::string rest = g::extractSoundTheme(sheet, th, &errs);
    UI_CHECK(errs.empty());
    UI_CHECK(th.clip(g::UiSound::Move) == "ui/move.wav");
    UI_CHECK(th.clip(g::UiSound::Accept) == "ui/accept.wav");
    UI_CHECK(th.clip(g::UiSound::Open).empty());
    UI_CHECK(th.volume == 0.5f);
    UI_CHECK(rest.find("@sounds") == std::string::npos);
    UiContext ui({.viewport = kVp, .font = &font()});
    std::vector<StyleDiagnostic> diags;
    UI_CHECK(ui.addStyleSheet(rest, &diags).has_value());
    UI_CHECK(diags.empty());
    g::SoundTheme bad;
    g::extractSoundTheme("@sounds { hover ui/x.wav; ping: a.wav; }", bad, &errs);
    UI_CHECK_EQ(errs.size(), std::size_t(2));
    return finish("fuse_ui_sound_theme");
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
    const std::string gate = FUSE_UI_GAME_GATE;
    if (gate == "inventory_dnd") return inventoryDnd();
    if (gate == "touch_controls") return touchControls();
    if (gate == "loading_screen") return loadingScreen();
    if (gate == "rich_dialogue") return richDialogue();
    if (gate == "sound_theme") return soundTheme();
    std::fprintf(stderr, "unknown gate %s\n", gate.c_str());
    return 2;
}
