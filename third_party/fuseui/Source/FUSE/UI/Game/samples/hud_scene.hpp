// The HUD demo scene shared by the hud_demo sample and the fuse_ui_game_hud_golden gate (deterministic: no clocks).
#pragma once

#include "fuse/ui/game/game_ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <cmath>
#include <memory>

namespace fuse::ui::game::demo {

constexpr Size kHudViewport{960.f, 540.f};

struct HudScene {
    std::unique_ptr<GameUi> game;
    NodeHandle health, stamina, compass, radial, menuPanel;
    u32 objective = ~0u, enemy = ~0u, behind = ~0u;
};

inline Mat4 demoCamera(Vec3 eye, f32 yawDeg) {
    const f32 y = yawDeg * 3.14159265f / 180.f;
    const Vec3 fwd{std::sin(y), 0.f, -std::cos(y)};
    return mul(perspective(1.0f, kHudViewport.w / kHudViewport.h, 0.1f, 500.f),
               lookAt(eye, {eye.x + fwd.x, eye.y + fwd.y, eye.z + fwd.z}, {0, 1, 0}));
}

/// Builds the HUD and advances it to a fixed state (all timings in fixed steps).
inline HudScene buildHudScene(UiContext& ui) {
    using namespace widgets;
    HudScene s;
    ui.setCss(ui.root(), "background", "#2E4A5C");
    ui.setCss(ui.root(), "layout", "anchor"); // game screens position by anchors
    ui.setCss(ui.root(), "padding", "0px");
    // fake scene: horizon + ground so the HUD sits over something
    const NodeHandle sky = panel(ui, ui.root(), "scene");
    ui.setCss(sky, "left", "0px");
    ui.setCss(sky, "top", "300px");
    ui.setCss(sky, "width", "100%");
    ui.setCss(sky, "height", "240px");
    ui.setCss(sky, "background", "#3B5A3A");
    ui.setCss(sky, "pointer-events", "none");
    // the HUD layer is created after the scene so it paints on top
    s.game = std::make_unique<GameUi>(ui);
    GameUi& g = *s.game;
    g.setSafeArea({24.f, 16.f, 24.f, 16.f});
    const NodeHandle safe = g.safeArea();

    const NodeHandle vitals = panel(ui, safe, "vitals");
    ui.setCss(vitals, "left", "0px");
    ui.setCss(vitals, "bottom", "0px");
    ui.setCss(vitals, "gap", "6px");
    ui.setCss(vitals, "background", "transparent");
    ui.setCss(vitals, "pointer-events", "none");
    s.health = hudBar(ui, vitals, 1.f, "hp");
    ui.setText(s.health, "HP");
    s.stamina = hudBar(ui, vitals, 0.8f, "stamina");
    ui.setCss(s.stamina, "background-end", "#3D8BFD");
    ui.setCss(s.stamina, "height", "8px");
    const NodeHandle row = panel(ui, vitals, "abilities");
    ui.setCss(row, "flex-direction", "row");
    ui.setCss(row, "gap", "8px");
    ui.setCss(row, "background", "transparent");
    const NodeHandle q = cooldownDial(ui, row, "Q", "cd.q");
    setCooldown(ui, q, 0.f);
    const NodeHandle e = cooldownDial(ui, row, "E", "cd.e");
    setCooldown(ui, e, 0.62f);
    const NodeHandle r = cooldownDial(ui, row, "R", "cd.r");
    setCooldown(ui, r, 0.18f);

    s.compass = compass(ui, safe, "compass");
    ui.setCss(s.compass, "left", "282px");
    ui.setCss(s.compass, "top", "0px");
    setCompass(ui, s.compass, 20.f, 65.f);

    s.radial = radialMenu(ui, safe, "Heal\nShield\nDash\nScan\nTrap\nFlare", "radial");
    ui.setCss(s.radial, "left", "16px");
    ui.setCss(s.radial, "top", "120px");
    ui.setCss(s.radial, "width", "220px");
    ui.setCss(s.radial, "height", "220px");
    radialSelect(ui, s.radial, {0.8f, -0.5f});

    const NodeHandle prompts = panel(ui, safe, "prompts");
    ui.setCss(prompts, "right", "0px");
    ui.setCss(prompts, "bottom", "110px");
    ui.setCss(prompts, "gap", "6px");
    ui.setCss(prompts, "background", "transparent");
    g.prompt(prompts, Action::Interact, "Open crate", "prompt.interact");
    g.prompt(prompts, Action::Back, "Cancel", "prompt.back");

    const Vec3 eye{0.f, 1.7f, 0.f};
    g.setCamera(demoCamera(eye, 0.f), eye);
    s.objective = g.addMarker({8.f, 2.f, -30.f}, "Beacon 30m");
    s.enemy = g.addMarker({-3.f, 1.5f, -90.f}, "");
    s.behind = g.addMarker({-6.f, 1.f, 25.f}, "Camp");

    g.feed("Odin  >  Raider");
    g.feed("Raider  >  Scout");
    g.feed("Odin  >  Drone");
    g.floatText({600.f, 240.f}, "-42", Color::rgb(0xFF5C5C));
    g.floatText({640.f, 210.f}, "CRIT 118", Color::rgb(0xF2C14E));
    g.subtitle("Mira", "The relay is just past the ridge.", 4.f);
    g.say("Mira", "Keep low and follow the beacon. The drones patrol the north road.", 30.f);

    // take damage: 100% -> 55%, trail lingers, then step a fixed amount of game time
    ui.update(0.f);
    setHudBar(ui, s.health, 0.55f);
    for (int i = 0; i < 30; ++i) { // 0.5 s fixed-step
        g.update(1.f / 60.f);
        ui.update(1.f / 60.f);
    }
    return s;
}

} // namespace fuse::ui::game::demo
