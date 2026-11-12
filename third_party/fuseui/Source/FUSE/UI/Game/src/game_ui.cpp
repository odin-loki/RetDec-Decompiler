// FUSE UI7 game UI (plan §4.15). See game_ui.hpp.
#include "fuse/ui/game/game_ui.hpp"
#include "fuse/ui/game/game_extras.hpp"

#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>

namespace fuse::ui::game {
namespace {

constexpr f32 kPi = 3.14159265358979f;

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
void px(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(std::round(v)))); }
void opacity(UiContext& ui, NodeHandle n, f32 a) { ui.set(n, Prop::Opacity, PropValue::ofNumber(std::clamp(a, 0.f, 1.f))); }
void show(UiContext& ui, NodeHandle n, bool on) { ui.set(n, Prop::Display, PropValue::ofBool(on)); }
Color mix(Color a, Color b, f32 t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }
f32 lo32(u64 v) { return std::bit_cast<f32>(u32(v)); }
f32 hi32(u64 v) { return std::bit_cast<f32>(u32(v >> 32)); }
u64 pack(f32 lo, f32 hi) { return u64(std::bit_cast<u32>(lo)) | (u64(std::bit_cast<u32>(hi)) << 32); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void label(const PaintContext& pc, DrawList& dl, std::string_view s, Rect box, Color c, f32 size,
           TextAlignment al = TextAlignment::Center) {
    pc.ui.paintString(dl, s, box, pc.tint(c), size, al, pc.node.index);
}
/// Filled triangle approximated by a fan of lines (the draw list has rects, rounded rects and lines).
void triangle(DrawList& dl, Vec2 a, Vec2 b, Vec2 c, Color col) {
    constexpr int kSteps = 10;
    for (int i = 0; i <= kSteps; ++i) {
        const f32 t = f32(i) / kSteps;
        dl.line(a, {b.x + (c.x - b.x) * t, b.y + (c.y - b.y) * t}, col, 2.f);
    }
}
f32 wrapDeg(f32 d) { d = std::fmod(d + 180.f, 360.f); return (d < 0.f ? d + 360.f : d) - 180.f; }

// ---- widget classes ----------------------------------------------------------------------------------------------
// hudbar: value = current (0..1), userData lo = trail
struct HudBar {
    static constexpr std::string_view kName = "hudbar";
    static constexpr A11yRole kRole = A11yRole::ProgressBar;
    static Size measure(UiContext&, NodeHandle, Size) { return {200.f, 14.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.content;
        const f32 v = std::clamp(pc.ui.value(pc.node), 0.f, 1.f);
        const f32 trail = std::clamp(lo32(pc.ui.userData(pc.node)), v, 1.f);
        const f32 rad = r.h * 0.5f;
        dl.rect(r, pc.tint(Color::rgb(0x111318, 0.75f)), rad);
        if (trail > v) dl.rect({r.x, r.y, r.w * trail, r.h}, pc.tint(Color::rgb(0xF2C14E)), rad);
        const Color full = pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x4CC26A);
        const Color c = v > 0.3f ? full : mix(Color::rgb(0xE5484D), full, v / 0.3f);
        if (v > 0.f) dl.gradientRect({r.x, r.y, std::max(r.w * v, r.h), r.h}, pc.tint(c), pc.tint(mix(c, Color::rgb(0x000000), 0.35f)), true, {rad, rad, rad, rad});
        for (int i = 1; i < 10; ++i) { // segment ticks
            const f32 x = std::round(r.x + r.w * f32(i) / 10.f);
            dl.rect({x, r.y + 2.f, 1.f, r.h - 4.f}, pc.tint(Color::rgb(0x000000, 0.25f)));
        }
        if (!pc.ui.text(pc.node).empty()) label(pc, dl, pc.ui.text(pc.node), r, Color::rgb(0xFFFFFF), std::max(10.f, r.h - 3.f));
    }
};
// cooldown: value = remaining (0..1), text = icon label
struct Cooldown {
    static constexpr std::string_view kName = "cooldown";
    static constexpr A11yRole kRole = A11yRole::ProgressBar;
    static Size measure(UiContext&, NodeHandle, Size) { return {48.f, 48.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.content;
        const f32 s = std::min(r.w, r.h), rad = s * 0.5f;
        const Vec2 c = ctr(r);
        const Rect sq{c.x - rad, c.y - rad, s, s};
        const f32 rem = std::clamp(pc.ui.value(pc.node), 0.f, 1.f);
        const bool ready = rem <= 0.f;
        dl.roundedRect(sq, pc.tint(ready ? Color::rgb(0x2B6CB0) : Color::rgb(0x1A1D24)), {rad, rad, rad, rad},
                       pc.tint(ready ? Color::rgb(0x9CC8FF) : Color::rgb(0x4A4F5C)), 2.f);
        if (!ready) { // clock wipe: remaining fraction darkened, counter-clockwise from 12 o'clock
            const int n = std::max(1, int(std::ceil(rem * 96.f)));
            for (int i = 0; i < n; ++i) {
                const f32 a = -kPi * 0.5f - 2.f * kPi * rem * (f32(i) + 0.5f) / f32(n);
                dl.line(c, {c.x + std::cos(a) * (rad - 2.f), c.y + std::sin(a) * (rad - 2.f)}, pc.tint(Color::rgb(0x000000, 0.45f)), 2.5f);
            }
            const f32 a = -kPi * 0.5f - 2.f * kPi * rem;
            dl.line(c, {c.x + std::cos(a) * (rad - 2.f), c.y + std::sin(a) * (rad - 2.f)}, pc.tint(Color::rgb(0xF2C14E)), 2.f);
        }
        label(pc, dl, pc.ui.text(pc.node), sq, ready ? Color::rgb(0xFFFFFF) : Color::rgb(0xD0D4DC), rad * 0.8f);
    }
};
// compass: userData lo = objective bearing (NaN none), hi = heading
struct Compass {
    static constexpr std::string_view kName = "compass";
    static Size measure(UiContext&, NodeHandle, Size) { return {360.f, 30.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.content;
        const f32 heading = hi32(pc.ui.userData(pc.node)), span = 120.f; // degrees visible
        const f32 ppd = r.w / span, cx = r.x + r.w * 0.5f;
        dl.rect(r, pc.tint(Color::rgb(0x0E1014, 0.6f)), 4.f);
        dl.pushClip(r, 4.f);
        static constexpr std::string_view kCard[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        for (int d = 0; d < 360; d += 15) {
            const f32 off = wrapDeg(f32(d) - heading);
            if (std::fabs(off) > span * 0.5f + 10.f) continue;
            const f32 x = cx + off * ppd;
            if (d % 45 == 0) {
                const Color c = d == 0 ? Color::rgb(0xE5484D) : Color::rgb(0xE6E8EE);
                label(pc, dl, kCard[d / 45], {x - 14.f, r.y, 28.f, r.h - 6.f}, c, 13.f);
            } else dl.rect({std::round(x), r.bottom() - 7.f, 1.f, 5.f}, pc.tint(Color::rgb(0xB0B4BE, 0.8f)));
        }
        const f32 obj = lo32(pc.ui.userData(pc.node));
        if (!std::isnan(obj)) {
            const f32 off = std::clamp(wrapDeg(obj - heading), -span * 0.5f, span * 0.5f);
            const f32 x = cx + off * ppd;
            triangle(dl, {x, r.bottom() - 2.f}, {x - 6.f, r.bottom() - 10.f}, {x + 6.f, r.bottom() - 10.f}, pc.tint(Color::rgb(0xF2C14E)));
        }
        dl.popClip();
        dl.rect({std::round(cx) - 1.f, r.y, 2.f, 5.f}, pc.tint(Color::rgb(0xFFFFFF)));
    }
};
// radialmenu: text = items, userData = selected + 1
int radialCount(std::string_view s) { return s.empty() ? 0 : int(std::count(s.begin(), s.end(), '\n')) + 1; }
std::string_view radialItem(std::string_view s, int i) {
    for (int k = 0; k < i; ++k) { const auto p = s.find('\n'); if (p == s.npos) return {}; s.remove_prefix(p + 1); }
    return s.substr(0, s.find('\n'));
}
struct Radial {
    static constexpr std::string_view kName = "radialmenu";
    static constexpr A11yRole kRole = A11yRole::List;
    static constexpr bool kFocusable = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {240.f, 240.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.content;
        const f32 R = std::min(r.w, r.h) * 0.5f;
        const Vec2 c = ctr(r);
        dl.roundedRect({c.x - R, c.y - R, 2 * R, 2 * R}, pc.tint(Color::rgb(0x0E1014, 0.78f)), {R, R, R, R}, pc.tint(Color::rgb(0x3A3F4B)), 2.f);
        const std::string_view items = pc.ui.text(pc.node);
        const int n = radialCount(items), sel = int(pc.ui.userData(pc.node)) - 1;
        const f32 inner = R * 0.32f;
        for (int i = 0; i < n; ++i) {
            const f32 a0 = -kPi * 0.5f + 2.f * kPi * (f32(i) - 0.5f) / f32(n);
            const f32 a1 = a0 + 2.f * kPi / f32(n);
            if (i == sel) {
                for (int k = 0; k <= 48; ++k) {
                    const f32 a = a0 + (a1 - a0) * f32(k) / 48.f;
                    dl.line({c.x + std::cos(a) * inner, c.y + std::sin(a) * inner}, {c.x + std::cos(a) * (R - 3.f), c.y + std::sin(a) * (R - 3.f)},
                            pc.tint(Color::rgb(0x3D8BFD, 0.85f)), 3.f);
                }
            }
            dl.line({c.x + std::cos(a0) * inner, c.y + std::sin(a0) * inner}, {c.x + std::cos(a0) * (R - 2.f), c.y + std::sin(a0) * (R - 2.f)},
                    pc.tint(Color::rgb(0x3A3F4B)), 1.5f);
            const f32 am = (a0 + a1) * 0.5f, lr = (inner + R) * 0.5f;
            const Vec2 p{c.x + std::cos(am) * lr, c.y + std::sin(am) * lr};
            label(pc, dl, radialItem(items, i), {p.x - 45.f, p.y - 10.f, 90.f, 20.f}, i == sel ? Color::rgb(0xFFFFFF) : Color::rgb(0xC8CCD4), 13.f);
        }
        dl.roundedRect({c.x - inner, c.y - inner, 2 * inner, 2 * inner}, pc.tint(Color::rgb(0x1A1D24)), {inner, inner, inner, inner}, pc.tint(Color::rgb(0x3A3F4B)), 1.5f);
        if (sel >= 0 && sel < n) label(pc, dl, radialItem(items, sel), {c.x - inner, c.y - 10.f, 2 * inner, 20.f}, Color::rgb(0xF2C14E), 12.f);
    }
};
// worldmarker: text = label, userData lo = arrow angle (radians) when the "offscreen" class is set
struct Marker {
    static constexpr std::string_view kName = "worldmarker";
    static Size measure(UiContext&, NodeHandle, Size) { return {28.f, 28.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.rect;
        const Vec2 c = ctr(r);
        const Color col = pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0xF2C14E);
        if (pc.ui.hasClass(pc.node, "offscreen")) {
            const f32 a = lo32(pc.ui.userData(pc.node));
            const Vec2 d{std::cos(a), std::sin(a)}, n{-d.y, d.x};
            triangle(dl, {c.x + d.x * 13.f, c.y + d.y * 13.f}, {c.x - d.x * 5.f + n.x * 9.f, c.y - d.y * 5.f + n.y * 9.f},
                     {c.x - d.x * 5.f - n.x * 9.f, c.y - d.y * 5.f - n.y * 9.f}, pc.tint(col));
        } else {
            for (int i = 0; i <= 9; ++i) { // diamond
                const f32 t = f32(i) / 9.f * 9.f;
                dl.line({c.x - t, c.y - 9.f + t}, {c.x + t, c.y - 9.f + t}, pc.tint(col), 1.5f);
                dl.line({c.x - t, c.y + 9.f - t}, {c.x + t, c.y + 9.f - t}, pc.tint(col), 1.5f);
            }
        }
        if (!pc.ui.text(pc.node).empty())
            label(pc, dl, pc.ui.text(pc.node), {c.x - 60.f, r.bottom(), 120.f, 16.f}, Color::rgb(0xFFFFFF), 12.f);
    }
};
// prompt: text = "glyph\ncaption"
struct Prompt {
    static constexpr std::string_view kName = "prompt";
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const std::string_view t = ui.text(n);
        const auto nl = t.find('\n');
        const f32 g = ui.measureString(t.substr(0, nl), 12.f), cap = nl == t.npos ? 0.f : ui.measureString(t.substr(nl + 1), 14.f);
        return {std::ceil(std::max(g + 12.f, 22.f) + 6.f + cap), 22.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const std::string_view t = pc.ui.text(pc.node);
        const auto nl = t.find('\n');
        const std::string_view glyph = t.substr(0, nl), cap = nl == t.npos ? std::string_view{} : t.substr(nl + 1);
        const Rect r = pc.content;
        const f32 gw = std::max(std::ceil(pc.ui.measureString(glyph, 12.f)) + 12.f, 22.f);
        const Rect badge{r.x, r.y, gw, 22.f};
        dl.roundedRect(badge, pc.tint(Color::rgb(0xE6E8EE)), {6, 6, 6, 6});
        label(pc, dl, glyph, badge, Color::rgb(0x111318), 12.f);
        label(pc, dl, cap, {r.x + gw + 6.f, r.y, r.w - gw - 6.f, 22.f}, pc.style.color, 14.f, TextAlignment::Start);
    }
};

constexpr std::string_view kHudCss = R"(
.hud { layout: anchor; left: 0px; top: 0px; width: 100%; height: 100%; pointer-events: none; background: transparent; padding: 0px; }
.hud-safe { layout: anchor; pointer-events: none; padding: 0px; background: transparent; }
.hud-layer { layout: absolute; left: 0px; top: 0px; width: 100%; height: 100%; pointer-events: none; padding: 0px; background: transparent; }
.killfeed { right: 0px; top: 48px; width: 280px; gap: 4px; pointer-events: none; padding: 0px; background: transparent; }
.feedline { background: #0E1014B0; color: #E6E8EE; padding: 4px 8px; border-radius: 4px; font-size: 14px; pointer-events: none; }
.subtitle { left: 25%; right: 25%; bottom: 150px; color: #FFFFFF; background: #000000A0; padding: 6px 12px; border-radius: 4px; font-size: 18px; pointer-events: none; }
.dialogue { left: 22%; right: 22%; bottom: 0px; height: 84px; background: #10131AE6; border-color: #3A3F4B; border-width: 1px; border-radius: 8px; padding: 10px 16px; gap: 6px; }
.dialogue-speaker { color: #F2C14E; font-size: 15px; }
.dialogue-body { color: #E6E8EE; font-size: 16px; }
floattext { font-size: 18px; color: #FFFFFF; pointer-events: none; }
worldmarker { width: 28px; height: 28px; pointer-events: none; }
)";

// floattext is a label variant painted with a dark outline for contrast over the scene
struct FloatText {
    static constexpr std::string_view kName = "floattext";
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const f32 s = ui.computed(n).fontSize;
        return {std::ceil(ui.measureString(ui.text(n), s)) + 2.f, std::ceil(s * 1.3f)};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.content;
        const std::string_view s = pc.ui.text(pc.node);
        const f32 sz = pc.style.fontSize;
        for (const Vec2 o : {Vec2{-1, 0}, Vec2{1, 0}, Vec2{0, -1}, Vec2{0, 1}})
            label(pc, dl, s, {r.x + o.x, r.y + o.y, r.w, r.h}, Color::rgb(0x000000, 0.85f), sz);
        label(pc, dl, s, r, pc.style.color, sz);
    }
};

} // namespace

// ---- math ----------------------------------------------------------------------------------------------------------
Mat4 perspective(f32 fovY, f32 aspect, f32 n, f32 f) {
    const f32 t = 1.f / std::tan(fovY * 0.5f);
    Mat4 m{};
    m[0] = t / aspect;
    m[5] = t;
    m[10] = (f + n) / (n - f);
    m[11] = -1.f;
    m[14] = 2.f * f * n / (n - f);
    return m;
}
static Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 scale(Vec3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
static f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 norm(Vec3 a) { const f32 l = std::sqrt(dot(a, a)); return l > 0.f ? scale(a, 1.f / l) : a; }
Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = norm(sub(target, eye)), s = norm(cross(f, up)), u = cross(s, f);
    Mat4 m{};
    m[0] = s.x; m[4] = s.y; m[8] = s.z;
    m[1] = u.x; m[5] = u.y; m[9] = u.z;
    m[2] = -f.x; m[6] = -f.y; m[10] = -f.z;
    m[12] = -dot(s, eye); m[13] = -dot(u, eye); m[14] = dot(f, eye); m[15] = 1.f;
    return m;
}
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            f32 s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    return r;
}

// ---- install / widgets -----------------------------------------------------------------------------------------
std::string_view hudThemeCss() { return kHudCss; }
void install(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    r.registerWidget<HudBar>();
    r.registerWidget<Cooldown>();
    r.registerWidget<Compass>();
    r.registerWidget<Radial>();
    r.registerWidget<Marker>();
    r.registerWidget<Prompt>();
    r.registerWidget<FloatText>();
    (void)ui.addStyleSheet(kHudCss);
    installExtras(ui);
}

NodeHandle hudBar(UiContext& ui, NodeHandle parent, f32 v, std::string_view id) {
    const NodeHandle n = make(ui, "hudbar", parent, id);
    ui.setValue(n, v);
    ui.setUserData(n, pack(v, 0.f));
    return n;
}
void setHudBar(UiContext& ui, NodeHandle bar, f32 v) {
    v = std::clamp(v, 0.f, 1.f);
    const f32 trail = lo32(ui.userData(bar));
    // damage leaves a trail that waits briefly then drains (GameUi::update); healing snaps the trail up
    ui.setUserData(bar, pack(std::max(trail, v), v < trail ? 0.35f : 0.f));
    ui.setValue(bar, v);
    ui.invalidate(bar);
}
f32 hudBarTrail(const UiContext& ui, NodeHandle bar) { return lo32(ui.userData(bar)); }

NodeHandle cooldownDial(UiContext& ui, NodeHandle parent, std::string_view lbl, std::string_view id) {
    const NodeHandle n = make(ui, "cooldown", parent, id);
    ui.setText(n, lbl);
    ui.setValue(n, 0.f);
    return n;
}
void setCooldown(UiContext& ui, NodeHandle d, f32 rem) {
    if (ui.value(d) != rem) { ui.setValue(d, std::clamp(rem, 0.f, 1.f)); ui.invalidate(d); }
}
NodeHandle compass(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle n = make(ui, "compass", parent, id);
    ui.setUserData(n, pack(std::numeric_limits<f32>::quiet_NaN(), 0.f));
    return n;
}
void setCompass(UiContext& ui, NodeHandle c, f32 heading, f32 obj) {
    ui.setUserData(c, pack(obj, heading));
    ui.invalidate(c);
}
NodeHandle radialMenu(UiContext& ui, NodeHandle parent, std::string_view items, std::string_view id) {
    const NodeHandle n = make(ui, "radialmenu", parent, id);
    ui.setText(n, items);
    ui.setUserData(n, 0);
    return n;
}
int radialSelect(UiContext& ui, NodeHandle m, Vec2 dir) {
    const int n = radialCount(ui.text(m));
    int sel = -1;
    if (n > 0 && std::hypot(dir.x, dir.y) >= 0.35f) {
        f32 a = std::atan2(dir.y, dir.x) + kPi * 0.5f; // 0 = up, clockwise
        a = std::fmod(a + 2.f * kPi + kPi / f32(n), 2.f * kPi);
        sel = std::min(n - 1, int(a / (2.f * kPi / f32(n))));
    } else if (n > 0) sel = radialSelected(ui, m); // dead zone keeps the last pick (stick released before accept)
    if (sel != radialSelected(ui, m)) { ui.setUserData(m, u64(sel + 1)); ui.invalidate(m); }
    return sel;
}
int radialSelected(const UiContext& ui, NodeHandle m) { return int(ui.userData(m)) - 1; }
bool radialAccept(UiContext& ui, NodeHandle m) {
    const int s = radialSelected(ui, m);
    if (s < 0) return false;
    ui.emit(m, SignalKind::Submitted, {.value = f32(s), .text = radialItem(ui.text(m), s)});
    return true;
}

// ---- prompts / gamepad / arbitration ---------------------------------------------------------------------------
std::string_view promptGlyph(InputDevice d, Action a) {
    static constexpr std::string_view kTable[4][4] = {
        {"Enter", "Esc", "Tab", "E"},
        {"A", "B", "Menu", "X"},
        {"Cross", "Circle", "Options", "Square"},
        {"Tap", "Back", "Menu", "Hold"},
    };
    return kTable[u8(d)][u8(a)];
}

int GamepadNav::update(const GamepadState& s, f32 dt) {
    int dir = -1;
    if (s.up || s.stick.y < -m_cfg.deadZone) dir = int(NavDir::Up);
    else if (s.down || s.stick.y > m_cfg.deadZone) dir = int(NavDir::Down);
    else if (s.left || s.stick.x < -m_cfg.deadZone) dir = int(NavDir::Left);
    else if (s.right || s.stick.x > m_cfg.deadZone) dir = int(NavDir::Right);
    int moves = 0;
    if (dir != m_dir) {
        m_dir = dir;
        m_held = 0.f;
        m_nextRepeat = m_cfg.repeatDelay;
        if (dir >= 0) { m_ui.setSpatialNavigation(true); if (m_ui.navigate(NavDir(dir))) ++moves; }
    } else if (dir >= 0) {
        m_held += dt;
        while (m_held >= m_nextRepeat) {
            m_nextRepeat += m_cfg.repeatInterval;
            if (m_ui.navigate(NavDir(dir))) ++moves;
        }
    }
    if (s.a && !m_a) { m_ui.keyDown(Key::Enter); ++m_accepts; }
    if (!s.a && m_a) m_ui.keyUp(Key::Enter);
    if (s.b && !m_b) { m_ui.keyDown(Key::Escape); ++m_backs; }
    if (!s.b && m_b) m_ui.keyUp(Key::Escape);
    m_a = s.a;
    m_b = s.b;
    return moves;
}

static bool uiTarget(const UiContext& ui, NodeHandle n) {
    for (; ui.alive(n); n = ui.parent(n)) {
        const u32 cls = ui.classOf(n);
        if (cls < ui.registry().size()) {
            const WidgetClass& wc = ui.registry().get(cls);
            if (wc.interactive || wc.focusable) return true;
        }
        if (n == ui.root()) return false;
        if (ui.hasClass(n, "ui-blocker")) return true; // menus / panels that swallow clicks on their background
    }
    return false;
}
InputOwner InputArbiter::pointerOwner(Vec2 p) const {
    if (m_menu || m_uiPress) return InputOwner::Ui;
    return uiTarget(m_ui, m_ui.hitTest(p)) ? InputOwner::Ui : InputOwner::Game;
}
InputOwner InputArbiter::keyboardOwner() const {
    return m_menu || m_ui.wantsTextInput() ? InputOwner::Ui : InputOwner::Game;
}
InputOwner InputArbiter::pointerMove(Vec2 p) {
    m_ui.pointerMove(p); // hover feedback always reaches the UI; ownership decides who acts
    return pointerOwner(p);
}
InputOwner InputArbiter::pointerDown(Vec2 p) {
    const InputOwner o = pointerOwner(p);
    if (o == InputOwner::Ui) { m_ui.pointerDown(p); m_uiPress = true; }
    return o;
}
InputOwner InputArbiter::pointerUp(Vec2 p) {
    if (!m_uiPress) return InputOwner::Game;
    m_ui.pointerUp(p);
    m_uiPress = false;
    return InputOwner::Ui;
}
InputOwner InputArbiter::key(Key k, bool down) {
    const InputOwner o = keyboardOwner();
    if (o == InputOwner::Ui) { if (down) m_ui.keyDown(k); else m_ui.keyUp(k); }
    return o;
}

// ---- locale ----------------------------------------------------------------------------------------------------
void StringTable::add(std::string_view loc, std::string_view kv) {
    while (!kv.empty()) {
        const auto nl = kv.find('\n');
        std::string_view line = kv.substr(0, nl);
        kv = nl == kv.npos ? std::string_view{} : kv.substr(nl + 1);
        const auto eq = line.find('=');
        if (eq == line.npos || line.empty() || line[0] == '#') continue;
        m_entries.push_back({std::string(loc), std::string(line.substr(0, eq)), std::string(line.substr(eq + 1))});
    }
    if (m_locale.empty()) m_locale = loc;
}
bool StringTable::setLocale(std::string_view loc) {
    const bool known = std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.locale == loc; });
    if (known) m_locale = loc;
    return known;
}
std::string_view StringTable::tr(std::string_view key) const {
    for (const Entry& e : m_entries)
        if (e.locale == m_locale && e.key == key) return e.value;
    return key;
}
void StringTable::bind(UiContext& ui, NodeHandle n, std::string_view key) {
    m_bound.emplace_back(n, std::string(key));
    ui.setText(n, tr(key));
}
void StringTable::refresh(UiContext& ui) {
    std::erase_if(m_bound, [&](const auto& b) { return !ui.alive(b.first); });
    for (const auto& [n, k] : m_bound) ui.setText(n, tr(k));
}

// ---- world surfaces --------------------------------------------------------------------------------------------
std::optional<Vec2> surfaceRaycast(const UiSurface& s, Vec3 o, Vec3 d, f32* tOut) {
    const Vec3 nrm = cross(s.right, s.up); // front faces +normal (towards the viewer)
    const f32 den = dot(d, nrm);
    if (den >= -1e-6f) return std::nullopt; // parallel or back-face
    const f32 t = dot(sub(s.center, o), nrm) / den;
    if (t < 0.f) return std::nullopt;
    const Vec3 hit = sub(add(o, scale(d, t)), s.center);
    const f32 u = dot(hit, s.right) / s.width + 0.5f, v = 0.5f - dot(hit, s.up) / s.height;
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return std::nullopt;
    if (tOut) *tOut = t;
    const Size vp = s.ui->viewport();
    return Vec2{u * vp.w, v * vp.h};
}
bool surfacePointer(UiSurface& s, Vec3 o, Vec3 d, bool down, bool up) {
    const auto p = surfaceRaycast(s, o, d);
    if (!p) {
        s.ui->pointerMove({-1.f, -1.f}); // leaving the panel clears hover
        if (up && s.ui->pressed().valid()) s.ui->pointerUp({-1.f, -1.f}); // released off the panel: no click
        return false;
    }
    s.ui->pointerMove(*p);
    if (down) s.ui->pointerDown(*p);
    if (up) s.ui->pointerUp(*p);
    return true;
}
bool surfaceRender(UiSurface& s, f32 dt) {
    UiContext& ui = *s.ui;
    const bool first = s.texture.width != int(ui.viewport().w) || s.texture.height != int(ui.viewport().h);
    if (!first && !ui.needsUpdate()) return false;
    ui.update(dt);
    if (first) s.texture.resize(int(ui.viewport().w), int(ui.viewport().h), 0u);
    else if (ui.damage().empty()) return false;
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(ui.drawList(), s.texture);
    ++s.renders;
    return true;
}

// ---- GameUi ----
static void drainTrails(UiContext& ui, NodeHandle n, u32 barCls, f32 g) {
    for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c)) {
        if (ui.classOf(c) == barCls) {
            const u64 ud = ui.userData(c);
            f32 trail = lo32(ud), wait = hi32(ud);
            const f32 v = ui.value(c);
            if (trail > v) {
                if (wait > 0.f) wait = std::max(0.f, wait - g);
                else trail = std::max(v, trail - g * 0.6f);
                ui.setUserData(c, pack(trail, wait));
                ui.invalidate(c);
            }
        }
        drainTrails(ui, c, barCls, g);
    }
}
GameUi::GameUi(UiContext& ui, NodeHandle parent) : m_ui(ui) {
    if (!ui.alive(parent)) parent = ui.root();
    m_hud = make(ui, "panel", parent, "hud");
    ui.addClass(m_hud, "hud");
    m_safe = make(ui, "panel", m_hud, "hud.safe");
    ui.addClass(m_safe, "hud-safe");
    setSafeArea({});
    m_markers = make(ui, "panel", m_safe, "hud.markers");
    ui.addClass(m_markers, "hud-layer");
    m_feed = make(ui, "panel", m_safe, "hud.killfeed");
    ui.addClass(m_feed, "killfeed");
    for (u32 i = 0; i < kFeedLines; ++i) {
        m_lines[i].node = widgets::label(ui, m_feed, "", {});
        ui.addClass(m_lines[i].node, "feedline");
        show(ui, m_lines[i].node, false);
    }
    m_sub = widgets::label(ui, m_safe, "", "hud.subtitle");
    ui.addClass(m_sub, "subtitle");
    show(ui, m_sub, false);
    m_dlg = make(ui, "panel", m_safe, "hud.dialogue");
    ui.addClass(m_dlg, "dialogue");
    ui.addClass(m_dlg, "ui-blocker");
    m_dlgSpeaker = widgets::label(ui, m_dlg, "", "hud.dialogue.speaker");
    ui.addClass(m_dlgSpeaker, "dialogue-speaker");
    m_dlgBody = widgets::label(ui, m_dlg, "", "hud.dialogue.body");
    ui.addClass(m_dlgBody, "dialogue-body");
    show(ui, m_dlg, false);
    for (u32 i = 0; i < kFloatPool; ++i) {
        m_floats[i].node = make(ui, "floattext", m_safe);
        show(ui, m_floats[i].node, false);
    }
    for (u32 i = 0; i < kMarkerPool; ++i) {
        m_marks[i].node = make(ui, "worldmarker", m_markers);
        show(ui, m_marks[i].node, false);
    }
    m_prompts.reserve(16);
}

void GameUi::setSafeArea(Insets in) {
    m_insets = in;
    px(m_ui, m_safe, Prop::Left, in.left);
    px(m_ui, m_safe, Prop::Top, in.top);
    px(m_ui, m_safe, Prop::Right, in.right);
    px(m_ui, m_safe, Prop::Bottom, in.bottom);
}

void GameUi::update(f32 realDt) {
    const f32 g = m_clock.tick(realDt);
    // bar trails (game time): every hudbar under the HUD
    if (g > 0.f) {
        const u32 barCls = m_ui.registry().find("hudbar");
        drainTrails(m_ui, m_ui.root(), barCls, g);
    }
    for (Float& f : m_floats) {
        if (f.life <= 0.f) continue;
        f.life = std::max(0.f, f.life - g);
        const f32 k = 1.f - f.life / f.total;
        px(m_ui, f.node, Prop::Left, f.at.x);
        px(m_ui, f.node, Prop::Top, f.at.y - 40.f * k);
        opacity(m_ui, f.node, f.life / f.total * 2.f);
        if (f.life <= 0.f) show(m_ui, f.node, false);
    }
    for (FeedLine& l : m_lines) {
        if (l.life <= 0.f) continue;
        l.life = std::max(0.f, l.life - g);
        opacity(m_ui, l.node, std::min(1.f, l.life / 0.5f));
        if (l.life <= 0.f) show(m_ui, l.node, false);
    }
    if (m_subLife > 0.f) { // subtitles follow real time
        m_subLife = std::max(0.f, m_subLife - realDt);
        if (m_subLife <= 0.f) show(m_ui, m_sub, false);
    }
    if (m_dlgOpen && !dialogueComplete()) {
        m_dlgAcc += g * m_dlgCps;
        std::size_t shown = m_dlgShown;
        while (m_dlgAcc >= 1.f && shown < m_dlgText.size()) {
            m_dlgAcc -= 1.f;
            ++shown;
            while (shown < m_dlgText.size() && (u8(m_dlgText[shown]) & 0xC0) == 0x80) ++shown; // whole code points
        }
        if (shown != m_dlgShown) { m_dlgShown = shown; m_ui.setText(m_dlgBody, dialogueVisible()); }
    }
    if (m_focusSounds && m_ui.focused() != m_lastFocus) {
        if (m_ui.alive(m_lastFocus) || m_ui.alive(m_ui.focused())) playSound(UiSound::Move);
        m_lastFocus = m_ui.focused();
    }
    if (m_haveCamera) layoutMarkers();
}

void GameUi::floatText(Vec2 at, std::string_view text, Color c, f32 seconds) {
    Float* slot = &m_floats[0];
    for (Float& f : m_floats) { // free slot, else the oldest
        if (f.life <= 0.f) { slot = &f; break; }
        if (f.life < slot->life) slot = &f;
    }
    slot->life = slot->total = std::max(seconds, 0.05f);
    slot->at = at;
    m_ui.setText(slot->node, text);
    m_ui.set(slot->node, Prop::Color, PropValue::ofColor(c));
    px(m_ui, slot->node, Prop::Left, at.x);
    px(m_ui, slot->node, Prop::Top, at.y);
    opacity(m_ui, slot->node, 1.f);
    show(m_ui, slot->node, true);
}
u32 GameUi::floatTextActive() const {
    return u32(std::count_if(m_floats.begin(), m_floats.end(), [](const Float& f) { return f.life > 0.f; }));
}

void GameUi::feed(std::string_view text, f32 seconds) {
    // shift lines up (oldest drops off) keeping node order: copy text / life downwards
    for (u32 i = 0; i + 1 < kFeedLines; ++i) {
        m_lines[i].life = m_lines[i + 1].life;
        m_ui.setText(m_lines[i].node, m_ui.text(m_lines[i + 1].node));
        show(m_ui, m_lines[i].node, m_lines[i].life > 0.f);
        opacity(m_ui, m_lines[i].node, std::min(1.f, m_lines[i].life / 0.5f));
    }
    FeedLine& l = m_lines[kFeedLines - 1];
    l.life = seconds;
    m_ui.setText(l.node, text);
    opacity(m_ui, l.node, 1.f);
    show(m_ui, l.node, true);
}
u32 GameUi::feedLines() const {
    return u32(std::count_if(m_lines.begin(), m_lines.end(), [](const FeedLine& l) { return l.life > 0.f; }));
}

void GameUi::subtitle(std::string_view speaker, std::string_view text, f32 seconds) {
    std::string s;
    if (!speaker.empty()) { s.append(speaker); s.append(": "); }
    s.append(text);
    m_ui.setText(m_sub, s);
    m_subLife = seconds;
    show(m_ui, m_sub, seconds > 0.f);
}
std::string_view GameUi::subtitleText() const { return m_subLife > 0.f ? m_ui.text(m_sub) : std::string_view{}; }

void GameUi::say(std::string_view speaker, std::string_view text, f32 cps) {
    m_ui.setText(m_dlgSpeaker, speaker);
    m_dlgText.assign(text);
    m_dlgShown = 0;
    m_dlgAcc = 0.f;
    m_dlgCps = cps;
    m_dlgOpen = true;
    m_ui.setText(m_dlgBody, "");
    show(m_ui, m_dlg, true);
    playSound(UiSound::Open);
}
void GameUi::skip() {
    if (!m_dlgOpen) return;
    if (!dialogueComplete()) {
        m_dlgShown = m_dlgText.size();
        m_ui.setText(m_dlgBody, m_dlgText);
        playSound(UiSound::Accept);
        return;
    }
    m_dlgOpen = false;
    show(m_ui, m_dlg, false);
    playSound(UiSound::Close);
}

u32 GameUi::addMarker(Vec3 w, std::string_view lbl) {
    for (u32 i = 0; i < kMarkerPool; ++i)
        if (!m_marks[i].used) {
            m_marks[i] = {m_marks[i].node, w, true, false};
            m_ui.setText(m_marks[i].node, lbl);
            show(m_ui, m_marks[i].node, true);
            if (m_haveCamera) layoutMarkers();
            return i;
        }
    return ~0u;
}
void GameUi::moveMarker(u32 id, Vec3 w) { if (id < kMarkerPool) m_marks[id].world = w; }
void GameUi::removeMarker(u32 id) {
    if (id >= kMarkerPool) return;
    m_marks[id].used = false;
    show(m_ui, m_marks[id].node, false);
}
NodeHandle GameUi::markerNode(u32 id) const { return id < kMarkerPool ? m_marks[id].node : NodeHandle{}; }
bool GameUi::markerOffscreen(u32 id) const { return id < kMarkerPool && m_marks[id].off; }
void GameUi::setCamera(const Mat4& vp, Vec3 eye, f32 fadeNear, f32 fadeFar) {
    m_vp = vp;
    m_eye = eye;
    m_fadeNear = fadeNear;
    m_fadeFar = std::max(fadeFar, fadeNear + 1e-3f);
    m_haveCamera = true;
    layoutMarkers();
}
void GameUi::layoutMarkers() {
    const Size vp = m_ui.viewport();
    const Rect safe{m_insets.left, m_insets.top, vp.w - m_insets.left - m_insets.right, vp.h - m_insets.top - m_insets.bottom};
    constexpr f32 kEdge = 24.f, kHalf = 14.f;
    for (Marker& mk : m_marks) {
        if (!mk.used) continue;
        const Vec3 w = mk.world;
        const f32 cx = m_vp[0] * w.x + m_vp[4] * w.y + m_vp[8] * w.z + m_vp[12];
        const f32 cy = m_vp[1] * w.x + m_vp[5] * w.y + m_vp[9] * w.z + m_vp[13];
        const f32 cw = m_vp[3] * w.x + m_vp[7] * w.y + m_vp[11] * w.z + m_vp[15];
        // NDC -> screen (y down); behind the camera the direction is mirrored so the arrow points the right way
        f32 nx = cx / std::max(std::fabs(cw), 1e-5f), ny = cy / std::max(std::fabs(cw), 1e-5f);
        const bool behind = cw <= 1e-5f;
        Vec2 p{(nx * 0.5f + 0.5f) * vp.w, (0.5f - ny * 0.5f) * vp.h};
        const Rect inner{safe.x + kEdge, safe.y + kEdge, safe.w - 2 * kEdge, safe.h - 2 * kEdge};
        const bool off = behind || !inner.contains(p);
        if (off) {
            const Vec2 c = ctr(inner);
            Vec2 d{p.x - c.x, p.y - c.y};
            if (std::fabs(d.x) < 1e-3f && std::fabs(d.y) < 1e-3f) d = {0.f, 1.f};
            const f32 sx = d.x != 0.f ? (inner.w * 0.5f) / std::fabs(d.x) : 1e9f, sy = d.y != 0.f ? (inner.h * 0.5f) / std::fabs(d.y) : 1e9f;
            const f32 s = std::min(sx, sy);
            p = {c.x + d.x * s, c.y + d.y * s};
            m_ui.setUserData(mk.node, pack(std::atan2(d.y, d.x), 0.f));
        }
        if (off != mk.off || off) {
            if (off) m_ui.addClass(mk.node, "offscreen"); else m_ui.removeClass(mk.node, "offscreen");
            m_ui.invalidate(mk.node);
        }
        mk.off = off;
        const f32 dist = std::sqrt(dot(sub(w, m_eye), sub(w, m_eye)));
        const f32 fade = off ? 1.f : 1.f - std::clamp((dist - m_fadeNear) / (m_fadeFar - m_fadeNear), 0.f, 0.75f);
        opacity(m_ui, mk.node, fade);
        // marker layer is inside the safe area: convert to its coordinates
        px(m_ui, mk.node, Prop::Left, p.x - kHalf - m_insets.left);
        px(m_ui, mk.node, Prop::Top, p.y - kHalf - m_insets.top);
    }
}

NodeHandle GameUi::prompt(NodeHandle parent, Action a, std::string_view caption, std::string_view id) {
    const NodeHandle n = make(m_ui, "prompt", parent, id);
    std::string t(promptGlyph(m_device, a));
    t += '\n';
    t += caption;
    m_ui.setText(n, t);
    m_prompts.emplace_back(n, a);
    return n;
}
void GameUi::noteDevice(InputDevice d) {
    if (d == m_device) return;
    m_device = d;
    std::string t;
    for (const auto& [n, a] : m_prompts) {
        if (!m_ui.alive(n)) continue;
        const std::string_view old = m_ui.text(n);
        const auto nl = old.find('\n');
        t.assign(promptGlyph(d, a));
        t += '\n';
        if (nl != old.npos) t += old.substr(nl + 1);
        m_ui.setText(n, t);
    }
}

} // namespace fuse::ui::game
